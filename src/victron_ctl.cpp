// Victron per Bluetooth bedienen (Orion Smart / Orion XS) – siehe victron_ctl.h.
// Ablauf je Auftrag (im Bluetooth-Task, Scan pausiert): verbinden, mit PIN koppeln, Kanaele abonnieren, Startfolge
// (wie VictronConnect), Register schreiben bzw. lesen, Antwort abwarten, trennen.
// Nachrichten (CBOR-artig): lesen  05 00 81 19 <Reg hi> <Reg lo>
//                           schreiben 06 00 82 19 <Reg hi> <Reg lo> 41 <u8>  bzw.  42 <u16 LE>
//                           Antwort  … 19 <Reg hi> <Reg lo> 41|42|44 <Wert LE>
#include "victron_ctl.h"

#if WITH_VICTRON_CTL
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <deque>
#include <map>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "i18n/i18n.h"

namespace {

const char *SVC = "306b0001-b081-4037-83dc-e59fcc3cdfd0";
const char *CH_INIT = "306b0002-b081-4037-83dc-e59fcc3cdfd0";   // Start und Lebenszeichen
const char *CH_CMD = "306b0003-b081-4037-83dc-e59fcc3cdfd0";    // Befehle und Registerwerte
const char *CH_BULK = "306b0004-b081-4037-83dc-e59fcc3cdfd0";   // gesammelte Registerwerte

const uint16_t REG_MODE = 0x0200;    // 1 an, 4 aus (0/4 aus, 1/3 an)
const uint16_t REG_IMAX = 0xEDF0;    // maximaler Ladestrom, 0,1 A
const uint32_t REFRESH_MS = 60000;   // Stand hoechstens so oft lesen
const uint32_t OPT_MS = 90000;       // eigener Schaltbefehl gilt so lange vor dem Werbepaket

struct St {
  uint8_t type = 0;          // Adresstyp aus dem Scan
  int mode = -1;             // zuletzt gelesen/geschrieben
  uint32_t mode_ms = 0;
  float limit = NAN;         // A
  uint32_t read_ms = 0;      // letzter Lesevorgang (auch fehlgeschlagen)
  std::string status;
};
struct Job {
  std::string mac;
  int mode = -1;
  float limit = NAN;
  bool read = false;
};

SemaphoreHandle_t s_mtx = nullptr;
struct Lock {
  Lock() {
    if (!s_mtx) s_mtx = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGiveRecursive(s_mtx); }
};

std::map<std::string, St> s_st;
std::deque<Job> s_jobs;
std::map<uint16_t, uint32_t> s_rx;   // Antworten der laufenden Verbindung: Register -> Wert
uint32_t s_next_try = 0;
std::string s_pin_now;               // PIN der laufenden Verbindung
std::string s_cur_mac;

void logf(const char *fmt, ...) {
  char b[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  log_printf("[victron-ctl] %s\n", b);
}

std::string nvs_key(const std::string &mac) {
  std::string k = "p";
  for (char c : mac)
    if (c != ':') k += (char)tolower((unsigned char)c);
  return k.substr(0, 15);
}

// Antworten auswerten: jede Stelle "19 hi lo 41|42|44 <Wert>" ist ein Registerwert
void parse(const uint8_t *d, size_t n) {
  Lock l;
  for (size_t i = 0; i + 4 < n; i++) {
    if (d[i] != 0x19) continue;
    uint8_t t = d[i + 3];
    size_t len = t == 0x41 ? 1 : t == 0x42 ? 2 : t == 0x44 ? 4 : 0;
    if (!len || i + 4 + len > n) continue;
    uint16_t reg = (uint16_t)(d[i + 1] << 8 | d[i + 2]);
    uint32_t v = 0;
    for (size_t k = 0; k < len; k++) v |= (uint32_t)d[i + 4 + k] << (8 * k);
    s_rx[reg] = v;
    i += 3 + len;
  }
}

void on_notify(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { parse(d, n); }

class CB : public NimBLEClientCallbacks {
  void onPassKeyEntry(NimBLEConnInfo &ci) override {
    std::string pin;
    {
      Lock l;
      pin = s_pin_now;
    }
    bool ok = NimBLEDevice::injectPassKey(ci, (uint32_t)strtoul(pin.c_str(), nullptr, 10));
    logf("Geraet verlangt PIN – gebe sie ein (%s)", ok ? "angenommen" : "abgelehnt");
  }
  void onConfirmPasskey(NimBLEConnInfo &ci, uint32_t) override { NimBLEDevice::injectConfirmPasskey(ci, true); }
  void onDisconnect(NimBLEClient *, int reason) override { logf("getrennt (Grund %d)", reason); }
};
CB s_cb;

// Waehrend der Verbindung PIN-Eingabe erlauben (VictronConnect-PIN), danach wieder wie ueberall
struct SecGuard {
  SecGuard() {
    NimBLEDevice::setSecurityAuth(true, true, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  }
  ~SecGuard() {
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
    NimBLEDevice::setSecurityAuth(true, false, true);
  }
};

bool wr(NimBLERemoteCharacteristic *c, const uint8_t *d, size_t n) {
  if (!c) return false;
  bool rsp = !c->canWriteNoResponse();
  return c->writeValue(d, n, rsp);
}

bool wait_reg(uint16_t reg, uint32_t &val, uint32_t ms, NimBLERemoteCharacteristic *init) {
  static const uint8_t KEEP[] = {0xF9, 0x41};
  uint32_t t0 = millis(), last_keep = millis();
  while (millis() - t0 < ms) {
    {
      Lock l;
      auto it = s_rx.find(reg);
      if (it != s_rx.end()) {
        val = it->second;
        return true;
      }
    }
    if (millis() - last_keep > 900) {   // Lebenszeichen, sonst trennt das Geraet
      wr(init, KEEP, sizeof(KEEP));
      last_keep = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return false;
}

void set_status(const std::string &mac, const std::string &t) {
  Lock l;
  s_st[mac].status = t;
}

}   // namespace

// ---------------------------------------------------------------- oeffentlich
bool vctl_supported(uint8_t record) { return record == 0x04 || record == 0x0F; }

void vctl_on_adv(const std::string &mac, uint8_t addr_type) {
  Lock l;
  auto it = s_st.find(mac);
  if (it != s_st.end()) it->second.type = addr_type;
  else if (s_st.size() < 12) s_st[mac].type = addr_type;
}

void vctl_fill(VictronInfo &v) {
  if (!vctl_supported(v.record) || v.gx || v.bms) return;
  Lock l;
  const St *s = nullptr;
  auto it = s_st.find(v.mac);
  if (it != s_st.end()) s = &it->second;
  v.can_mode = true;
  v.mode_set = 1;   // Ein (1) / Aus (4)
  if (s && s->mode >= 0 && millis() - s->mode_ms < OPT_MS) v.mode = s->mode;
  else if (v.has) v.mode = (v.off_reason & 0x04) ? 4 : 1;   // Abschaltgrund "Soft power switch" = ausgeschaltet
  v.can_limit = true;
  v.ac_limit = s ? s->limit : NAN;
  if (s && !s->status.empty() && v.status == "OK") v.status = s->status;
}

static bool queue(const Job &j) {
  Lock l;
  for (auto &q : s_jobs)
    if (q.mac == j.mac) {   // gleiches Geraet: zusammenfassen
      if (j.mode >= 0) q.mode = j.mode;
      if (!isnan(j.limit)) q.limit = j.limit;
      q.read |= j.read;
      return true;
    }
  if (s_jobs.size() >= 6) return false;
  s_jobs.push_back(j);
  s_next_try = 0;
  return true;
}

bool vctl_set_mode(const std::string &mac, int mode) {
  if (mode != 1 && mode != 4) return false;
  Job j;
  j.mac = mac;
  j.mode = mode;
  {
    Lock l;
    St &s = s_st[mac];
    s.mode = mode;   // gleich anzeigen, die Rueckmeldung bestaetigt
    s.mode_ms = millis();
    s.status = tr("Wird geschaltet …");
  }
  return queue(j);
}

bool vctl_set_limit(const std::string &mac, float amps) {
  if (isnan(amps) || amps < 1 || amps > 100) return false;
  Job j;
  j.mac = mac;
  j.limit = roundf(amps * 10) / 10;
  {
    Lock l;
    St &s = s_st[mac];
    s.limit = j.limit;
    s.status = tr("Wird eingestellt …");
  }
  return queue(j);
}

void vctl_refresh(const std::string &mac) {
  {
    Lock l;
    St &s = s_st[mac];
    if (s.read_ms && millis() - s.read_ms < REFRESH_MS) return;
    s.read_ms = millis() | 1;
  }
  Job j;
  j.mac = mac;
  j.read = true;
  queue(j);
}

std::string vctl_pin(const std::string &mac) {
  Preferences p;
  if (!p.begin("vicctl", true)) return VCTL_PIN_DEFAULT;
  std::string r = p.getString(nvs_key(mac).c_str(), VCTL_PIN_DEFAULT).c_str();
  p.end();
  return r;
}

void vctl_set_pin(const std::string &mac, const std::string &pin) {
  Preferences p;
  if (!p.begin("vicctl", false)) return;
  if (pin.empty() || pin == VCTL_PIN_DEFAULT) p.remove(nvs_key(mac).c_str());
  else p.putString(nvs_key(mac).c_str(), pin.substr(0, 6).c_str());
  p.end();
  if (!NimBLEDevice::isInitialized()) return;
  NimBLEAddress a(mac, 0), b(mac, 1);   // neue PIN: alte Kopplung loeschen, damit neu gekoppelt wird
  if (NimBLEDevice::isBonded(a)) NimBLEDevice::deleteBond(a);
  if (NimBLEDevice::isBonded(b)) NimBLEDevice::deleteBond(b);
}

void vctl_print_debug() {
  Lock l;
  log_printf("Victron-Steuerung: %u Auftraege offen\n", (unsigned)s_jobs.size());
  for (auto &kv : s_st)
    log_printf("  %s Typ %u Modus %d Strom %.1f A – %s\n", kv.first.c_str(), kv.second.type, kv.second.mode, kv.second.limit,
               kv.second.status.c_str());
}

bool vctl_ble_needed() {
  Lock l;
  return !s_jobs.empty();
}

bool vctl_want_connect() {
  Lock l;
  return !s_jobs.empty() && (int32_t)(millis() - s_next_try) >= 0;
}

void vctl_connect_now() {
  Job j;
  uint8_t type = 0;
  {
    Lock l;
    if (s_jobs.empty()) return;
    j = s_jobs.front();
    s_jobs.pop_front();
    auto it = s_st.find(j.mac);
    if (it != s_st.end()) type = it->second.type;
    s_rx.clear();
    s_cur_mac = j.mac;
  }
  {
    std::string pin = vctl_pin(j.mac);
    Lock l;
    s_pin_now = pin;
  }
  logf("%s: verbinde (Modus %d, Strom %.1f, lesen %d)", j.mac.c_str(), j.mode, j.limit, j.read);
  NimBLEClient *c = NimBLEDevice::createClient();
  c->setClientCallbacks(&s_cb, false);
  c->setConnectTimeout(8000);
  bool ok;
  std::string err;
  {
    SecGuard sg;
    ok = c->connect(NimBLEAddress(j.mac, type), true);
    if (ok && !c->getConnInfo().isEncrypted() && !c->secureConnection()) {
      int e = c->getLastError();
      logf("%s: Kopplung fehlgeschlagen, Fehler %d", j.mac.c_str(), e);
      err = tr("Kopplung abgelehnt – PIN aus VictronConnect prüfen");
      ok = false;
    }
  }
  NimBLERemoteCharacteristic *ci = nullptr, *cc = nullptr, *cb = nullptr;
  if (ok) {
    NimBLERemoteService *s = c->getService(NimBLEUUID(SVC));
    if (s) {
      ci = s->getCharacteristic(NimBLEUUID(CH_INIT));
      cc = s->getCharacteristic(NimBLEUUID(CH_CMD));
      cb = s->getCharacteristic(NimBLEUUID(CH_BULK));
    }
    if (!ci || !cc) {
      err = tr("Gerät antwortet nicht wie erwartet");
      ok = false;
    }
  } else if (err.empty()) {
    err = tr("Nicht erreichbar – VictronConnect getrennt? In Reichweite?");
  }
  bool done = false;
  if (ok) {
    for (auto *ch : {ci, cc, cb})
      if (ch && ch->canNotify()) ch->subscribe(true, on_notify, true);
    // Startfolge wie VictronConnect (Mitschnitt Orion)
    static const uint8_t I1[] = {0xFA, 0x80, 0xFF}, I2[] = {0xF9, 0x80}, C1[] = {0x01}, C2[] = {0x03, 0x00},
                         C3[] = {0x06, 0x00, 0x82, 0x18, 0x93, 0x42, 0x10, 0x27};
    wr(ci, I1, sizeof(I1));
    vTaskDelay(pdMS_TO_TICKS(300));
    wr(ci, I2, sizeof(I2));
    vTaskDelay(pdMS_TO_TICKS(300));
    wr(cc, C1, sizeof(C1));
    vTaskDelay(pdMS_TO_TICKS(200));
    wr(cc, C2, sizeof(C2));
    vTaskDelay(pdMS_TO_TICKS(200));
    wr(cc, C3, sizeof(C3));
    vTaskDelay(pdMS_TO_TICKS(300));
    done = true;
    uint32_t v;
    if (j.mode >= 0) {
      uint8_t m[] = {0x06, 0x00, 0x82, 0x19, REG_MODE >> 8, REG_MODE & 0xFF, 0x41, (uint8_t)j.mode};
      {
        Lock l;
        s_rx.erase(REG_MODE);
      }
      wr(cc, m, sizeof(m));
      if (wait_reg(REG_MODE, v, 2500, ci)) {
        logf("%s: Modus %lu bestaetigt", j.mac.c_str(), (unsigned long)v);
        Lock l;
        s_st[j.mac].mode = (v == 0 || v == 4) ? 4 : 1;
        s_st[j.mac].mode_ms = millis();
      } else {
        done = false;
        err = tr("Gerät hat den Schalter nicht bestätigt");
      }
    }
    if (!isnan(j.limit)) {
      uint16_t raw = (uint16_t)lroundf(j.limit * 10);
      uint8_t m[] = {0x06, 0x00, 0x82, 0x19, REG_IMAX >> 8, REG_IMAX & 0xFF, 0x42, (uint8_t)(raw & 0xFF), (uint8_t)(raw >> 8)};
      {
        Lock l;
        s_rx.erase(REG_IMAX);
      }
      wr(cc, m, sizeof(m));
      if (wait_reg(REG_IMAX, v, 2500, ci)) {
        logf("%s: Ladestrom %.1f A bestaetigt", j.mac.c_str(), (v & 0xFFFF) / 10.0f);
        Lock l;
        s_st[j.mac].limit = (v & 0xFFFF) / 10.0f;
      } else {
        done = false;
        err = tr("Gerät hat den Ladestrom nicht bestätigt");
      }
    }
    // Stand lesen (immer: zeigt danach den echten Wert)
    static const uint8_t RM[] = {0x05, 0x00, 0x81, 0x19, REG_MODE >> 8, REG_MODE & 0xFF};
    static const uint8_t RI[] = {0x05, 0x00, 0x81, 0x19, REG_IMAX >> 8, REG_IMAX & 0xFF};
    {
      Lock l;
      s_rx.erase(REG_MODE);
      s_rx.erase(REG_IMAX);
    }
    wr(cc, RM, sizeof(RM));
    vTaskDelay(pdMS_TO_TICKS(150));
    wr(cc, RI, sizeof(RI));
    uint32_t vm = 0, vi = 0;
    bool gm = wait_reg(REG_MODE, vm, 2000, ci);
    bool gi = wait_reg(REG_IMAX, vi, 1500, ci);
    {
      Lock l;
      St &s = s_st[j.mac];
      if (gm) {
        s.mode = (vm == 0 || vm == 4) ? 4 : 1;
        s.mode_ms = millis();
      }
      if (gi) s.limit = (vi & 0xFFFF) / 10.0f;
      s.read_ms = millis() | 1;
    }
    if (j.read && !gm && !gi) {
      done = false;
      if (err.empty()) err = tr("Gerät liefert keine Werte – PIN/Firmware prüfen");
    }
  }
  if (c->isConnected()) c->disconnect();
  NimBLEDevice::deleteClient(c);
  set_status(j.mac, done ? "" : err);
  {
    Lock l;
    s_cur_mac.clear();
    if (!done) s_next_try = millis() + 5000;
  }
  logf("%s: %s", j.mac.c_str(), done ? "fertig" : err.c_str());
}

#endif
