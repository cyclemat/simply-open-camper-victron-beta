#include "truma.h"
#include "i18n/i18n.h"
#include "app.h"
#include "victron.h"
#include "mopeka.h"
#include "truma_inetx.h"
#include "hymer.h"
#include "web_server.h"
// Satelliten haben keinen Webserver: dort gilt dieser Ersatz (das Panel nimmt den aus webserver.cpp)
__attribute__((weak)) bool web_busy() { return false; }
#include "settings.h"
#include "sat_proto.h"

#include <Arduino.h>
#include <atomic>
#include <algorithm>
#include <stdarg.h>
#include <NimBLEDevice.h>
#include "esp_heap_caps.h"
#include "bms.h"
#include "panelsat_pair.h"
#if CONFIG_ESP_HOSTED_ENABLED || BOARD_P4
#include "esp32-hal-hosted.h"
#endif
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *UUID_SERVICE = "22bfa601-6afe-4cc5-b3ff-3e140baedeb6";
static const char *UUID_MEASUREMENT = "22bfa610-6afe-4cc5-b3ff-3e140baedeb6";  // lesen
static const char *UUID_PASSWORD = "22bfa619-6afe-4cc5-b3ff-3e140baedeb6";     // lesen, 5 Byte
static const char *UUID_STORE_NV = "22bfa617-6afe-4cc5-b3ff-3e140baedeb6";     // schreiben: Ausloeser
static const uint16_t MANUFACTURER_ID = 0x5000;
static const uint16_t VICTRON_MANUFACTURER_ID = 0x02E1;
static const uint8_t TRIGGER_INIT = 0x03, TRIGGER_GO = 0x02;
static const uint32_t MEASURE_WAIT_MS = 3000;
static const uint32_t WAIT_FOR_ADV_MS = 6 * 60 * 1000;  // der Sensor sendet nur alle paar Minuten

namespace {   // dateilokal: gleichnamige Typen anderer Dateien duerfen sich nicht vermischen
struct Sensor {
  std::string mac;
  uint8_t type = 0;
  std::string name;
  // letzte Messung
  bool has = false;
  int percent = 0, grams = 0, battery = 0, temp = 0, quality = 0;
  uint32_t when_ms = 0;   // millis() der Messung (0 = vor dem Neustart)
  time_t when_epoch = 0;  // Uhrzeit der Messung, falls bekannt
  // Ablauf
  bool due = true, busy = false;
  uint32_t due_since = 0, next_ms = 0;
  char status[64] = "Warte auf erste Messung …";
};
}  // namespace

static SemaphoreHandle_t s_mtx;
static std::vector<Sensor> s_sensors;
static uint16_t s_interval_min = 10;
static std::atomic<uint32_t> s_ver{1};
static Preferences s_prefs;

static std::vector<TrumaFound> s_found;
static volatile bool s_setup_scan = false;
static uint32_t s_setup_until = 0;
static volatile int s_adv_hit = -1;
static volatile uint8_t s_adv_type = 0;
static bool s_ble_ready = false;
static bool s_scan_passive = false;   // laufender Scan ist der passive (Messen/Victron)

enum CmdType : uint8_t { CMD_MEASURE = 1, CMD_SCAN, CMD_RELOAD };
struct Cmd { CmdType t; char mac[18]; };
static QueueHandle_t s_cmd;

namespace {   // dateilokal: gleichnamige Typen anderer Dateien duerfen sich nicht vermischen
struct Lock {
  Lock() { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(s_mtx); }
};
}  // namespace

static std::string lower(std::string s) {
  for (auto &c : s) c = tolower(c);
  return s;
}

// ---------------------------------------------------------------- Speicher
struct __attribute__((packed)) StoredReading {
  uint8_t percent;
  uint16_t grams_div100;
  uint8_t battery;
  int8_t temp;
  uint8_t quality;
  uint32_t epoch;
};

static void save_config() {
  Lock l;
  s_prefs.begin("truma", false);
  s_prefs.putUChar("n", s_sensors.size());
  s_prefs.putUShort("iv", s_interval_min);
  for (size_t i = 0; i < TRUMA_MAX_SENSORS; i++) {
    char k[4];
    if (i < s_sensors.size()) {
      snprintf(k, sizeof(k), "m%u", (unsigned)i); s_prefs.putString(k, s_sensors[i].mac.c_str());
      snprintf(k, sizeof(k), "t%u", (unsigned)i); s_prefs.putUChar(k, s_sensors[i].type);
      snprintf(k, sizeof(k), "a%u", (unsigned)i); s_prefs.putString(k, s_sensors[i].name.c_str());
    }
  }
  s_prefs.end();
}

static void save_reading(size_t i) {
  Lock l;
  if (i >= s_sensors.size()) return;
  const Sensor &s = s_sensors[i];
  StoredReading r = {(uint8_t)s.percent, (uint16_t)(s.grams / 100), (uint8_t)s.battery, (int8_t)s.temp,
                     (uint8_t)s.quality, (uint32_t)s.when_epoch};
  char k[4];
  snprintf(k, sizeof(k), "r%u", (unsigned)i);
  s_prefs.begin("truma", false);
  s_prefs.putBytes(k, &r, sizeof(r));
  s_prefs.end();
}

static void load_config() {
  Lock l;
  s_sensors.clear();
  s_prefs.begin("truma", true);
  int n = s_prefs.getUChar("n", 0);
  s_interval_min = s_prefs.getUShort("iv", 10);
  for (int i = 0; i < n && i < TRUMA_MAX_SENSORS; i++) {
    char k[4];
    Sensor s;
    snprintf(k, sizeof(k), "m%d", i); s.mac = s_prefs.getString(k, "").c_str();
    snprintf(k, sizeof(k), "t%d", i); s.type = s_prefs.getUChar(k, 0);
    snprintf(k, sizeof(k), "a%d", i); s.name = s_prefs.getString(k, "Gasflasche").c_str();
    snprintf(k, sizeof(k), "r%d", i);
    StoredReading r;
    if (s_prefs.getBytes(k, &r, sizeof(r)) == sizeof(r)) {
      s.has = true;
      s.percent = r.percent; s.grams = r.grams_div100 * 100; s.battery = r.battery;
      s.temp = r.temp; s.quality = r.quality; s.when_epoch = r.epoch; s.when_ms = 0;
    }
    if (!s.mac.empty()) s_sensors.push_back(s);
  }
  s_prefs.end();
  if (s_interval_min < 2) s_interval_min = 10;
}

// ---------------------------------------------------------------- Protokoll
static void rc4(const uint8_t *key, size_t klen, const uint8_t *in, uint8_t *out, size_t len) {
  uint8_t S[256];
  for (int i = 0; i < 256; i++) S[i] = i;
  uint8_t j = 0;
  for (int i = 0; i < 256; i++) {
    j = j + S[i] + key[i % klen];
    uint8_t t = S[i]; S[i] = S[j]; S[j] = t;
  }
  uint8_t a = 0, b = 0;
  for (size_t n = 0; n < len; n++) {
    a++;
    b += S[a];
    uint8_t t = S[a]; S[a] = S[b]; S[b] = t;
    out[n] = in[n] ^ S[(uint8_t)(S[a] + S[b])];
  }
}

static void set_status(size_t i, const char *fmt, ...) {
  Lock l;
  if (i >= s_sensors.size()) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(s_sensors[i].status, sizeof(s_sensors[i].status), fmt, ap);
  va_end(ap);
  s_ver++;
}

static bool measure(size_t i) {
  std::string mac;
  uint8_t type;
  {
    Lock l;
    if (i >= s_sensors.size()) return false;
    mac = s_sensors[i].mac;
    type = s_sensors[i].type;
    s_sensors[i].busy = true;
  }
  set_status(i, "Verbinde …");
  NimBLEClient *c = NimBLEDevice::createClient();
  bool ok = false;
  uint8_t dec[16] = {0};
  size_t dlen = 0;
  c->setConnectTimeout(8000);
  if (!c->connect(NimBLEAddress(mac, type), true)) {
    set_status(i, "Verbindung fehlgeschlagen – ist die App verbunden?");
  } else {
    NimBLERemoteService *svc = c->getService(NimBLEUUID(UUID_SERVICE));
    NimBLERemoteCharacteristic *ch_pw = svc ? svc->getCharacteristic(NimBLEUUID(UUID_PASSWORD)) : nullptr;
    NimBLERemoteCharacteristic *ch_nv = svc ? svc->getCharacteristic(NimBLEUUID(UUID_STORE_NV)) : nullptr;
    NimBLERemoteCharacteristic *ch_ms = svc ? svc->getCharacteristic(NimBLEUUID(UUID_MEASUREMENT)) : nullptr;
    if (!ch_pw || !ch_ms) {
      set_status(i, "Unbekanntes Gerät (Dienst fehlt)");
    } else {
      NimBLEAttValue pw = ch_pw->readValue();
      if (pw.size() != 5) {
        set_status(i, "Passwort nicht lesbar");
      } else {
        uint8_t key[5];
        const char *truma = "TRUMA";
        for (int k = 0; k < 5; k++) key[k] = pw.data()[k] ^ (uint8_t)truma[k];
        set_status(i, "Messung läuft …");
        if (ch_nv) {
          uint8_t v = TRIGGER_INIT;
          ch_nv->writeValue(&v, 1, false);
          delay(400);
          v = TRIGGER_GO;
          ch_nv->writeValue(&v, 1, false);
          delay(400);
        }
        delay(MEASURE_WAIT_MS);
        NimBLEAttValue raw = ch_ms->readValue();
        dlen = raw.size() < sizeof(dec) ? raw.size() : sizeof(dec);
        if (dlen >= 5) {
          rc4(key, 5, raw.data(), dec, dlen);
          ok = dec[0] <= 100;
          if (!ok) set_status(i, "Unplausibler Messwert");
        } else {
          set_status(i, "Kein Messwert erhalten");
        }
      }
    }
    c->disconnect();
  }
  NimBLEDevice::deleteClient(c);

  Lock l;
  if (i >= s_sensors.size()) return false;
  Sensor &s = s_sensors[i];
  s.busy = false;
  if (ok) {
    s.has = true;
    s.percent = dec[0];
    s.grams = (dec[1] | (dec[2] << 8)) * 100;
    s.battery = dec[3];
    s.temp = (int8_t)dec[4];  // vermutlich vorzeichenbehaftet; bei Minusgraden pruefen
    s.quality = dlen > 6 ? (dec[6] >> 6) & 0x03 : 0;
    s.when_ms = millis();
    s.when_epoch = time(nullptr) > 1700000000 ? time(nullptr) : 0;
    snprintf(s.status, sizeof(s.status), "OK");
    log_i("Truma %s: %d %%, %d g, Akku %d %%", s.mac.c_str(), s.percent, s.grams, s.battery);
  }
  s_ver++;
  if (ok) save_reading(i);
  return ok;
}

// ---------------------------------------------------------------- Scannen
// Diagnose beim Einrichten: jedes neue Geraet einmal loggen (Name, Hersteller-Kennung, Dienste) – nur waehrend
// "Sensor suchen" laeuft, hoechstens 60 Geraete
static std::vector<std::string> s_diag_seen;
static void diag_adv(const NimBLEAdvertisedDevice *dev) {
  std::string mac = dev->getAddress().toString();
  for (auto &m : s_diag_seen)
    if (m == mac) return;
  if (s_diag_seen.size() >= 60) return;
  s_diag_seen.push_back(mac);
  char md[40] = "-";
  if (dev->haveManufacturerData()) {
    std::string d = dev->getManufacturerData();
    int k = 0;
    for (size_t i = 0; i < d.size() && i < 12; i++) k += snprintf(md + k, sizeof(md) - k, "%02x", (uint8_t)d[i]);
  }
  std::string svc;
  for (int i = 0; i < (int)dev->getServiceUUIDCount() && i < 3; i++) svc += dev->getServiceUUID(i).toString() + " ";
  log_i("BLE %s %4d dBm  Name \"%s\"  Hersteller %s  Dienste %s", mac.c_str(), dev->getRSSI(),
        dev->haveName() ? dev->getName().c_str() : "", md, svc.c_str());
}

// ---- Zaehlung aller Bluetooth-Geraete in Reichweite (serieller Befehl "ble", nur auf Anfrage)
struct CensusEntry {
  char mac[18];
  char name[22];
  int8_t rssi;
  uint16_t mfg;   // Hersteller-Kennung (0xFFFF = keine)
  uint32_t n;     // empfangene Pakete
};
static const int CEN_MAX = 250;
static CensusEntry *s_cen = nullptr;   // nur waehrend der Zaehlung, im PSRAM
static int s_cen_n = 0;
static uint32_t s_cen_pkts = 0, s_cen_start = 0;
static volatile uint32_t s_cen_until = 0;

static void census_add(const NimBLEAdvertisedDevice *dev) {
  if (!s_cen) return;
  s_cen_pkts++;
  std::string mac = dev->getAddress().toString();
  CensusEntry *e = nullptr;
  for (int i = 0; i < s_cen_n; i++)
    if (!strcmp(s_cen[i].mac, mac.c_str())) { e = &s_cen[i]; break; }
  if (!e) {
    if (s_cen_n >= CEN_MAX) return;
    e = &s_cen[s_cen_n++];
    strncpy(e->mac, mac.c_str(), sizeof(e->mac) - 1);
    e->rssi = -127;
    e->mfg = 0xFFFF;
  }
  e->n++;
  if (dev->getRSSI() > e->rssi) e->rssi = dev->getRSSI();
  if (dev->haveName() && !e->name[0]) strncpy(e->name, dev->getName().c_str(), sizeof(e->name) - 1);
  if (dev->haveManufacturerData() && e->mfg == 0xFFFF) {
    std::string md = dev->getManufacturerData();
    if (md.size() >= 2) e->mfg = (uint8_t)md[0] | ((uint8_t)md[1] << 8);
  }
}

static const char *mfg_name(uint16_t id) {
  switch (id) {
    case 0x004C: return "Apple";
    case 0x0006: return "Microsoft";
    case 0x0075: return "Samsung";
    case 0x00E0: return "Google";
    case 0x02E1: return "Victron";
    case 0x0059: return "Nordic";
    case 0x0087: return "Garmin";
    case 0x0157: return "Huami";
    case 0xFFFF: return "-";
    default: return nullptr;
  }
}

static void census_report() {
  uint32_t secs = (millis() - s_cen_start) / 1000;
  log_printf("\n===== Bluetooth in Reichweite (%lu s, aktive Suche) =====\n", (unsigned long)secs);
  log_printf("%d Geraete, %lu Pakete (%lu pro Sekunde)%s\n", s_cen_n, (unsigned long)s_cen_pkts,
             (unsigned long)(secs ? s_cen_pkts / secs : 0), s_cen_n >= CEN_MAX ? " – Liste voll" : "");
  // nach Hersteller zaehlen
  struct G { uint16_t id; int n; } g[24];
  int ng = 0;
  for (int i = 0; i < s_cen_n; i++) {
    int k = 0;
    while (k < ng && g[k].id != s_cen[i].mfg) k++;
    if (k == ng && ng < 24) g[ng++] = {s_cen[i].mfg, 0};
    if (k < ng) g[k].n++;
  }
  std::sort(g, g + ng, [](const G &a, const G &b) { return a.n > b.n; });
  log_printf("Nach Hersteller:");
  for (int k = 0; k < ng; k++) {
    const char *nm = mfg_name(g[k].id);
    if (nm) log_printf(" %s %d,", nm, g[k].n);
    else log_printf(" 0x%04X %d,", g[k].id, g[k].n);
  }
  log_printf("\nStaerkste Sender:\n");
  std::sort(s_cen, s_cen + s_cen_n, [](const CensusEntry &a, const CensusEntry &b) { return a.rssi > b.rssi; });
  for (int i = 0; i < s_cen_n && i < 30; i++) {
    const char *nm = mfg_name(s_cen[i].mfg);
    char id[8];
    if (!nm) { snprintf(id, sizeof(id), "0x%04X", s_cen[i].mfg); nm = id; }
    log_printf("  %s %4d dBm %5lu Pakete  %-9s %s\n", s_cen[i].mac, s_cen[i].rssi, (unsigned long)s_cen[i].n, nm, s_cen[i].name);
  }
  log_printf("==========================================\n\n");
  heap_caps_free(s_cen);
  s_cen = nullptr;
}

void truma_ble_census(uint16_t seconds) {
  Lock l;
  if (s_cen_until) return;   // laeuft schon
  s_cen = (CensusEntry *)heap_caps_calloc(CEN_MAX, sizeof(CensusEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_cen) {
    log_printf("Kein Speicher fuer die Zaehlung\n");
    return;
  }
  s_cen_n = 0;
  s_cen_pkts = 0;
  s_cen_start = millis();
  s_cen_until = (millis() + seconds * 1000UL) | 1;
}

// Satelliten im Einrichtungsmodus: nur das Panel wertet sie aus (panelsat_pair.cpp); Satellit und Victron-Display
// bauen diese Datei ohne Panel-Teil – dort gelten die leeren Fassungen.
__attribute__((weak)) bool satpair_searching() { return false; }
__attribute__((weak)) bool satpair_on_adv(const NimBLEAdvertisedDevice *) { return false; }
__attribute__((weak)) bool satpair_want_connect() { return false; }
__attribute__((weak)) void satpair_connect_now() {}

class ScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *dev) override {
    if (s_cen_until) {
      Lock l;
      census_add(dev);
    }
    if (s_setup_scan) {
      Lock l;
      diag_adv(dev);
    }
    if (satpair_on_adv(dev)) return;   // Satellit wartet aufs Einrichten (nur waehrend der Suche am Panel)
    // Nur die in den Einstellungen gewaehlten Protokolle auswerten
#if WITH_BMS
    if (ble_proto_on(BLE_P_BMS) && bms_on_adv(dev)) return;
#endif
    if (inetx_enabled() && ble_proto_on(BLE_P_INETX) && inetx_on_adv(dev)) return;
    if (hymer_enabled() && ble_proto_on(BLE_P_HYMER) && hymer_on_adv(dev)) return;
    bool truma = false;
    std::string mac = lower(dev->getAddress().toString());
    if (dev->haveManufacturerData()) {
      std::string md = dev->getManufacturerData();
      uint16_t mid = md.size() >= 2 ? ((uint8_t)md[0] | ((uint8_t)md[1] << 8)) : 0;
      if (mid == SAT_ADV_COMPANY) {   // Satellit sendet Werte per Bluetooth
        if (ble_proto_on(BLE_P_SAT)) panelsat_on_adv(mac, (const uint8_t *)md.data(), md.size(), dev->getRSSI());
        return;
      }
      if (mid == VICTRON_MANUFACTURER_ID) {
        if (ble_proto_on(BLE_P_VICTRON))
          victron_on_adv(mac, (const uint8_t *)md.data(), md.size(), dev->getRSSI(), dev->haveName() ? dev->getName() : "");
        return;
      }
      if (ble_proto_on(BLE_P_MOPEKA) && mopeka_on_adv(mac, (const uint8_t *)md.data(), md.size(), dev->getRSSI())) return;
      truma = mid == MANUFACTURER_ID;
    }
    if (!ble_proto_on(BLE_P_TRUMA)) return;
    if (!truma && dev->isAdvertisingService(NimBLEUUID(UUID_SERVICE))) truma = true;
    // Neuere LevelControl: keine feste Hersteller-Kennung, wirbt mit dem kurzen Dienst 0x0A00 (Name "T…<Flaschenart>")
    if (!truma && dev->isAdvertisingService(NimBLEUUID((uint16_t)0x0A00))) truma = true;
    Lock l;
    if (s_setup_scan && truma) {
      bool known = false;
      for (auto &f : s_found)
        if (f.mac == mac) { f.rssi = dev->getRSSI(); known = true; }
      if (!known) {
        TrumaFound f;
        f.mac = mac;
        f.addr_type = dev->getAddress().getType();
        f.rssi = dev->getRSSI();
        for (auto &s : s_sensors) f.configured |= s.mac == mac;
        s_found.push_back(f);
        s_ver++;
      }
    }
    for (size_t i = 0; i < s_sensors.size(); i++)
      if (s_sensors[i].due && s_sensors[i].mac == mac && dev->isConnectable()) {
        s_adv_hit = i;
        s_adv_type = dev->getAddress().getType();
      }
  }
};
static ScanCB s_scan_cb;

#ifndef BLE_DEVICE_NAME
#define BLE_DEVICE_NAME "SOC-Panel"
#endif

static void ble_init() {
  if (s_ble_ready) return;
#if CONFIG_ESP_HOSTED_ENABLED || BOARD_P4
  // ESP32-P4: der Funk sitzt im ESP32-C6 (esp-hosted). Dessen Bluetooth-Controller erst einschalten – die
  // Arduino-BLE-Bibliothek macht das selbst, esp-nimble-cpp nicht.
  if (!hostedInitBLE()) log_e("Bluetooth ueber den C6 nicht verfuegbar (hostedInitBLE)");
#endif
  NimBLEDevice::init(BLE_DEVICE_NAME);
  if (inetx_enabled()) inetx_ble_setup();
  if (hymer_enabled()) hymer_ble_setup();
#if WITH_BMS
  // Super B fordert selbst eine Kopplung an: Just Works mit Bonding zulassen (wie Hymer/iNet X)
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
#endif
  NimBLEScan *sc = NimBLEDevice::getScan();
  sc->setScanCallbacks(&s_scan_cb, true);  // Duplikate: jede Aussendung zaehlt
  sc->setMaxResults(0);                     // nichts speichern, nur Rueckrufe
  s_ble_ready = true;
}

static void scan_start(bool setup) {
  ble_init();
  NimBLEScan *sc = NimBLEDevice::getScan();
  if (sc->isScanning()) sc->stop();
  // Passiv mit geringer Last, damit WLAN und Display ungestoert bleiben; beim Einrichten aktiv.
  // Victron sendet laufend neue Werte – dafuer etwas haeufiger hinhoeren (ca. 30 % statt 12 %).
  // Mopeka sendet etwa jede Sekunde ein Paket, ein Treffer pro Minute genuegt – dafuer reicht die geringe Last.
  bool vic = victron_has_devices();
  sc->setActiveScan(setup);
  sc->setInterval(setup ? 100 : (vic ? 160 : 320));
  sc->setWindow(setup ? 90 : (vic ? 48 : 40));
  sc->start(0, false, true);
}

static void scan_stop() {
  if (!s_ble_ready) return;
  NimBLEScan *sc = NimBLEDevice::getScan();
  if (sc->isScanning()) sc->stop();
}

// ---------------------------------------------------------------- Ablauf
// Wird Bluetooth gleich gebraucht (eingerichtete Geraete)? Dann frueh starten: Der Controller braucht beim Start einen
// zusammenhaengenden internen Block (~36 KB). 20 s nach dem Start (erste Gasmessung) hatten WLAN, MQTT und Cerbo den
// internen Speicher schon zerstueckelt -> "BLE_INIT: Malloc failed", Haenger im Controller, Watchdog-Neustart in
// Schleife (Waveshare, 2026-11-17). Frueh gestartet belegt Bluetooth dasselbe, nur bevor der Speicher zerfaellt.
static bool ble_needed_soon() {
  bool truma;
  {
    Lock l;
    truma = ble_proto_on(BLE_P_TRUMA) && !s_sensors.empty();
  }
  return truma || (ble_proto_on(BLE_P_VICTRON) && victron_has_devices()) ||
         (ble_proto_on(BLE_P_MOPEKA) && mopeka_has_devices()) || (ble_proto_on(BLE_P_BMS) && bms_has_devices()) ||
         (inetx_enabled() && ble_proto_on(BLE_P_INETX)) || (hymer_enabled() && ble_proto_on(BLE_P_HYMER)) ||
         ble_proto_on(BLE_P_SAT);
}

static void worker(void *) {
  bool early_checked = false;
  for (;;) {
    if (!early_checked && millis() > 1500) {   // nach dem Laden der Geraetelisten (main: *_begin)
      early_checked = true;
      if (!s_ble_ready && ble_needed_soon()) {
        size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL), lb = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ble_init();
        log_i("Bluetooth frueh gestartet (intern vorher frei %u, groesster Block %u, danach frei %u)", (unsigned)f,
              (unsigned)lb, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
      }
    }
    Cmd cmd;
    if (xQueueReceive(s_cmd, &cmd, pdMS_TO_TICKS(200)) == pdTRUE) {
      Lock l;
      if (cmd.t == CMD_MEASURE) {
        for (auto &s : s_sensors)
          if (!cmd.mac[0] || s.mac == cmd.mac) {
            s.due = true;
            s.due_since = millis();
            snprintf(s.status, sizeof(s.status), "Warte auf Sensor …");
          }
        s_ver++;
      } else if (cmd.t == CMD_SCAN) {
        Lock l;
        s_found.clear();
        s_diag_seen.clear();
        s_setup_scan = true;
        s_setup_until = millis() + 180000;  // der Sensor sendet nur alle paar Minuten
        s_ver++;
      }
    }
    uint32_t now = millis();
    bool any_due = false;
    {
      Lock l;
      for (auto &s : s_sensors) {
        if (!s.due && (int32_t)(now - s.next_ms) >= 0) {
          s.due = true;
          s.due_since = now;
        }
        if (s.due && now - s.due_since > WAIT_FOR_ADV_MS) {
          // Sensor hat sich nicht gemeldet: in ein paar Minuten erneut
          s.due = false;
          s.next_ms = now + 3 * 60 * 1000;
          snprintf(s.status, sizeof(s.status), "Nicht gefunden – außer Reichweite?");
          s_ver++;
        }
        any_due |= s.due;
      }
      if (s_setup_scan && (int32_t)(now - s_setup_until) >= 0) {
        s_setup_scan = false;
        s_ver++;
      }
      if (s_cen_until && (int32_t)(now - s_cen_until) >= 0) {
        s_cen_until = 0;
        census_report();
      }
    }
    // Einrichten hat Vorrang (aktiv, kurz); sonst passiv lauschen, solange eine Messung faellig ist
    // oder Victron-Geraete eingerichtet sind
    bool scanning = s_ble_ready && NimBLEDevice::getScan()->isScanning();
    bool p_truma = ble_proto_on(BLE_P_TRUMA), p_vic = ble_proto_on(BLE_P_VICTRON), p_mop = ble_proto_on(BLE_P_MOPEKA);
    bool want_active = (p_truma && s_setup_scan) || (p_vic && victron_discovery_running()) ||
                       (p_mop && mopeka_discovery_running()) || s_cen_until != 0;
    bool want_passive = (p_truma && any_due) || (p_vic && victron_has_devices()) || (p_mop && mopeka_has_devices()) ||
                        ble_proto_on(BLE_P_SAT) || satpair_searching();   // Panel: "Neuen Satelliten hinzufuegen"
    if (inetx_enabled() && ble_proto_on(BLE_P_INETX)) {
      inetx_scan_wanted(want_passive, want_active);
      // iNet X gefunden: Verbindung hier im Scan-Task aufbauen, solange der Scan pausiert
      if (inetx_want_connect()) {
        scan_stop();
        inetx_connect_now();
      }
    }
    if (hymer_enabled() && ble_proto_on(BLE_P_HYMER)) {
      hymer_scan_wanted(want_passive, want_active);
      // SCU gefunden: Verbindung (Kopplung, Dienste) hier aufbauen, solange der Scan pausiert; TLS laeuft im Hymer-Task
      if (hymer_want_connect()) {
        scan_stop();
        hymer_connect_now();
      }
    }
    // Satellit einrichten (Knopf am Panel): verbinden und WLAN-Daten uebertragen, solange der Scan pausiert
    if (satpair_want_connect()) {
      ble_init();
      scan_stop();
      satpair_connect_now();
      scanning = s_ble_ready && NimBLEDevice::getScan()->isScanning();
    }
#if WITH_BMS
    // Akkus (BMS): bleiben verbunden; Verbindung hier aufbauen, solange der Scan pausiert
    if (ble_proto_on(BLE_P_BMS)) {
      bms_scan_wanted(want_passive, want_active);
      if (bms_want_connect()) {
        ble_init();
        scan_stop();
        bms_connect_now();
        scanning = s_ble_ready && NimBLEDevice::getScan()->isScanning();
      }
      if (bms_want_poll()) bms_poll_now();   // Akkus, die abgefragt werden (Power Quarry)
    }
#endif
    // Waehrend das Webinterface benutzt wird, das Dauer-Lauschen pausieren: WLAN und Bluetooth teilen sich die
    // Funkeinheit, der Editor laedt sonst zaeh oder gar nicht. Einrichten (aktive Suche) laeuft weiter.
    if (want_passive && !want_active && web_busy()) want_passive = false;
    if (want_active) {
      if (!scanning || s_scan_passive) {
        scan_start(true);
        s_scan_passive = false;
      }
    } else if (want_passive) {
      if (!scanning || !s_scan_passive) {
        scan_start(false);
        s_scan_passive = true;
      }
    } else if (scanning) {
      scan_stop();
    }
    int hit = s_adv_hit;
    if (hit >= 0 && !s_setup_scan) {
      s_adv_hit = -1;
      {
        Lock l;
        if (hit < (int)s_sensors.size()) s_sensors[hit].type = s_adv_type;
      }
      scan_stop();
      bool ok = measure(hit);
      Lock l;
      if (hit < (int)s_sensors.size()) {
        s_sensors[hit].due = false;
        s_sensors[hit].next_ms = millis() + (ok ? s_interval_min * 60UL * 1000 : 60UL * 1000);
      }
    }
  }
}

// ---------------------------------------------------------------- oeffentliche API
void truma_ble_init() { ble_init(); }

void truma_begin() {
  s_mtx = xSemaphoreCreateRecursiveMutex();
  s_cmd = xQueueCreate(6, sizeof(Cmd));
  load_config();
  uint32_t now = millis();
  for (auto &s : s_sensors) {
    s.due = false;
    s.next_ms = now + 20000;  // erst nach dem Start von WLAN und Display messen
  }
  // Auf Kern 0 neben WLAN; Bluetooth wird erst initialisiert, wenn ein Sensor eingerichtet ist
  xTaskCreatePinnedToCore(worker, "truma", 6144, nullptr, 3, nullptr, 0);
}

uint32_t truma_version() { return s_ver; }

static TrumaInfo info_of(const Sensor &s) {
  TrumaInfo t;
  t.key = TRUMA_KEY_PREFIX + s.mac;
  t.mac = s.mac;
  t.name = s.name;
  t.has_reading = s.has;
  t.percent = s.percent;
  t.kg = s.grams / 1000.0f;
  t.battery = s.battery;
  t.temperature = s.temp;
  t.quality = s.quality;
  if (s.has) {
    if (s.when_ms) t.age_s = (millis() - s.when_ms) / 1000;
    else if (s.when_epoch && time(nullptr) > 1700000000) t.age_s = time(nullptr) - s.when_epoch;
  }
  t.busy = s.busy || s.due;
  t.status = s.status;
  return t;
}

std::vector<TrumaInfo> truma_list() {
  std::vector<TrumaInfo> out;
  Lock l;
  for (auto &s : s_sensors) out.push_back(info_of(s));
  return out;
}

bool truma_get(const std::string &key, TrumaInfo &out) {
  if (!truma_is_key(key)) return false;
  std::string mac = key.substr(strlen(TRUMA_KEY_PREFIX));
  Lock l;
  for (auto &s : s_sensors)
    if (s.mac == mac) {
      out = info_of(s);
      return true;
    }
  return false;
}

void truma_measure(const std::string &mac) {
  Cmd c;
  memset(&c, 0, sizeof(c));
  c.t = CMD_MEASURE;
  strncpy(c.mac, mac.c_str(), sizeof(c.mac) - 1);
  xQueueSend(s_cmd, &c, 0);
}

void truma_scan_start() {
  Cmd c;
  memset(&c, 0, sizeof(c));
  c.t = CMD_SCAN;
  xQueueSend(s_cmd, &c, 0);
}

bool truma_scan_running() { return s_setup_scan; }

std::vector<TrumaFound> truma_scan_results() {
  Lock l;
  std::vector<TrumaFound> out = s_found;
  for (auto &f : out) {
    f.configured = false;
    for (auto &s : s_sensors) f.configured |= s.mac == f.mac;
  }
  return out;
}

std::string truma_add(const std::string &mac_in, uint8_t addr_type, const std::string &name) {
  std::string mac = lower(mac_in);
  if (mac.size() != 17) return "Ungültige Bluetooth-Adresse";
  {
    Lock l;
    for (auto &s : s_sensors)
      if (s.mac == mac) return "Dieser Sensor ist schon eingerichtet";
    if (s_sensors.size() >= TRUMA_MAX_SENSORS) return "Es sind höchstens vier Gasflaschen möglich";
    Sensor s;
    s.mac = mac;
    s.type = addr_type;
    s.name = name.empty() ? tr("Gasflasche ") + std::to_string(s_sensors.size() + 1) : name.substr(0, 32);
    s.due = true;
    s.due_since = millis();
    snprintf(s.status, sizeof(s.status), "Warte auf Sensor …");
    s_sensors.push_back(s);
    s_ver++;
  }
  save_config();
  return "";
}

void truma_remove(const std::string &mac) {
  {
    Lock l;
    for (size_t i = 0; i < s_sensors.size(); i++)
      if (s_sensors[i].mac == mac) {
        s_sensors.erase(s_sensors.begin() + i);
        break;
      }
    s_ver++;
  }
  // gespeicherte Messwerte neu ordnen
  s_prefs.begin("truma", false);
  s_prefs.remove("r0");
  s_prefs.remove("r1");
  s_prefs.end();
  save_config();
  for (size_t i = 0; i < s_sensors.size(); i++) save_reading(i);
}

void truma_rename(const std::string &mac, const std::string &name) {
  {
    Lock l;
    for (auto &s : s_sensors)
      if (s.mac == mac && !name.empty()) s.name = name.substr(0, 32);
    s_ver++;
  }
  save_config();
}

void truma_set_interval(uint16_t minutes) {
  {
    Lock l;
    s_interval_min = minutes < 2 ? 2 : (minutes > 240 ? 240 : minutes);
    uint32_t now = millis();
    for (auto &s : s_sensors)
      if (!s.due) s.next_ms = now + s_interval_min * 60UL * 1000;
    s_ver++;
  }
  save_config();
}

uint16_t truma_interval() { return s_interval_min; }
