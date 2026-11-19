// Truma iNet X per Bluetooth LE (siehe truma_inetx.h)
//
// GATT (Basis xxxxxxxx-f3b2-11e8-8eb2-f2801f1b9fd1):
//   fc314001 CMD    – Benachrichtigung + Schreiben mit Antwort (Steuerung der Uebertragung)
//   fc314002 DATA_W – Schreiben ohne Antwort (unsere Rahmen)
//   fc314003 DATA_R – Benachrichtigung (Rahmen vom Bedienteil)
// Senden von L Byte:   CMD <- 01 lo hi, warten auf CMD 81, DATA_W <- Rahmen, warten auf CMD F0 01 (F0 00 = abgelehnt)
// Empfangen:           CMD 83 lo hi kuendigt L Byte an, wir CMD <- 03 00, DATA_R sammeln, dann CMD <- F0 01
// Rahmen: [0-1] Ziel, [2-3] Quelle, [4-5] Laenge-7, [6] Steuerung, [7-15] 0, [16] Untertyp, [17] Korrelation, [18..] CBOR
// Ablauf nach dem Verbinden: anmelden (Adresse holen), Themen abonnieren, Identitaet schreiben, Parameter abfragen.
#include "truma_inetx.h"
#include "hymer.h"
#include "i18n/i18n.h"

#if !WITH_TRUMA_INETX
void inetx_begin() {}
uint32_t inetx_version() { return 0; }
bool inetx_get(TrumaHeatInfo &) { return false; }
void inetx_set_heat(bool, float, uint8_t) {}
void inetx_set_water(TrumaWater) {}
void inetx_set_energy(TrumaEnergy, uint16_t) {}
void inetx_pair_start() {}
bool inetx_pairing() { return false; }
bool inetx_paired() { return false; }
void inetx_forget() {}
void inetx_disconnect() {}
void inetx_ble_setup() {}
bool inetx_on_adv(const NimBLEAdvertisedDevice *) { return false; }
void inetx_scan_wanted(bool &, bool &) {}
bool inetx_want_connect() { return false; }
void inetx_connect_now() {}
#else

#include <Arduino.h>
#include <atomic>
#include <map>
#include <vector>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *UUID_SUFFIX = "-f3b2-11e8-8eb2-f2801f1b9fd1";
static const char *UUID_CMD = "fc314001-f3b2-11e8-8eb2-f2801f1b9fd1";
static const char *UUID_DATA_W = "fc314002-f3b2-11e8-8eb2-f2801f1b9fd1";
static const char *UUID_DATA_R = "fc314003-f3b2-11e8-8eb2-f2801f1b9fd1";
static const uint16_t MANUFACTURER = 0x0C73;
static const uint16_t ADDR_BROADCAST = 0xFFFF, ADDR_BROKER = 0x0000, ADDR_PANEL = 0x0101, ADDR_HEATER = 0x0201,
                      ADDR_APP = 0x0500;
static const uint8_t C_REG = 0x01, C_MBP = 0x03;
static const uint8_t S_INFO = 0x00, S_WRITE = 0x01, S_SUBSCRIBE = 0x02, S_PDISC = 0x04, S_REG_RESP = 0x02, S_PDISC_RESP = 0x84;
static const uint32_t PAIR_MS = 3 * 60 * 1000;
static const size_t RX_MAX = 16384;

// ---------------------------------------------------------------- Parameter, die uns interessieren
enum Param {
  P_ROOM_CUR, P_ROOM_TGT, P_HEAT_ACTIVE, P_HEAT_MODE, P_RC_MODE, P_W_ACTIVE, P_W_MODE, P_W_TEMP, P_W_FAST, P_EL_LEVEL,
  P_GAS_LEVEL, P_VBAT, P_TEMP_INT, P_COUNT
};
static const struct { const char *tn, *pn; } PARAMS[P_COUNT] = {
    {"AirHeating", "Temp"},      {"AirHeating", "TgtTemp"},     {"AirHeating", "Active"},
    {"AirHeating", "Mode"},      {"RoomClimate", "Mode"},       {"WaterHeating", "Active"},
    {"WaterHeating", "Mode"},    {"WaterHeating", "Temp"},      {"WaterHeating", "FasterHeatingMode"},
    {"EnergySrc", "ElectricLevel"}, {"EnergySrc", "GasLevel"},  {"VBat", "Voltage"},
    {"Temperature", "Internal"},
};
static const char *TOPICS[] = {"AirCirculation", "AirHeating", "EnergySrc", "ErrorReset", "RoomClimate", "WaterHeating",
                               "VBat", "System", "Temperature", "GasBtl", "SystemTime", "MobileIdentity"};

// ---------------------------------------------------------------- Zustand
static SemaphoreHandle_t s_mtx;
namespace {   // dateilokal: gleichnamige Typen anderer Dateien duerfen sich nicht vermischen
struct Lock {
  Lock() { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(s_mtx); }
};
}  // namespace
static std::atomic<uint32_t> s_ver{1};
static Preferences s_prefs;

static double s_val[P_COUNT];
static bool s_has[P_COUNT];
static int s_err = 0;
static std::map<std::string, uint16_t> s_owner;     // Thema -> Adresse des Geraets, das es veroeffentlicht
static uint32_t s_last_rx_ms = 0;

static bool s_paired = false;
static std::string s_peer_id;                      // Identitaetsadresse des gekoppelten Bedienteils
static std::string s_muid, s_uuid;
static volatile bool s_pairing = false;
static uint32_t s_pair_until = 0;

static std::string s_cand;                         // Bedienteil aus dem letzten Werbepaket
static uint8_t s_cand_type = 0;
static uint32_t s_cand_ms = 0;
static std::map<std::string, uint32_t> s_block;   // Adressen, die gerade nicht klappen -> bis wann meiden
static uint32_t s_next_try_ms = 0, s_backoff_ms = 15000;

static NimBLEClient *s_client = nullptr;
static NimBLERemoteCharacteristic *s_ch_cmd = nullptr, *s_ch_w = nullptr, *s_ch_r = nullptr;
static volatile bool s_connected = false, s_ready = false;
static uint16_t s_addr = 0;                        // vom Bedienteil zugewiesene Adresse
static char s_status[64] = "Nicht gekoppelt";

// Empfang (Benachrichtigungen laufen im NimBLE-Task, Auswertung im eigenen Task)
enum EvKind : uint8_t { EV_CMD = 1, EV_FRAME, EV_UP, EV_DOWN };
struct Ev { EvKind k; uint8_t n; uint8_t d[6]; };
static QueueHandle_t s_ev;
static uint8_t *s_rx = nullptr;                    // PSRAM
static size_t s_rx_len = 0, s_rx_need = 0;
static bool s_rx_announced = false;
static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_frame = nullptr;                 // fertiger Rahmen zur Auswertung (PSRAM)
static size_t s_frame_len = 0;
static volatile bool s_frame_busy = false;

// Schreibauftraege der Oberflaeche
namespace {   // dateilokal: gleichnamige Typen anderer Dateien duerfen sich nicht vermischen
struct WriteReq { char tn[20]; char pn[24]; int32_t v; };
}  // namespace
static QueueHandle_t s_wq;

static void set_status(const char *s) {
  Lock l;
  if (strcmp(s_status, s)) {
    strncpy(s_status, s, sizeof(s_status) - 1);
    s_ver++;
  }
}

// ---------------------------------------------------------------- CBOR (nur was gebraucht wird)
static void cw_head(std::vector<uint8_t> &o, uint8_t maj, uint64_t v) {
  maj <<= 5;
  if (v < 24) o.push_back(maj | v);
  else if (v < 0x100) { o.push_back(maj | 24); o.push_back(v); }
  else if (v < 0x10000) { o.push_back(maj | 25); o.push_back(v >> 8); o.push_back(v); }
  else if (v <= 0xFFFFFFFFULL) { o.push_back(maj | 26); for (int i = 3; i >= 0; i--) o.push_back(v >> (8 * i)); }
  else { o.push_back(maj | 27); for (int i = 7; i >= 0; i--) o.push_back(v >> (8 * i)); }
}
static void cw_int(std::vector<uint8_t> &o, int64_t v) {
  if (v >= 0) cw_head(o, 0, v);
  else cw_head(o, 1, (uint64_t)(-1 - v));
}
static void cw_str(std::vector<uint8_t> &o, const char *s) {
  size_t n = strlen(s);
  cw_head(o, 3, n);
  o.insert(o.end(), s, s + n);
}

struct CR {
  const uint8_t *p, *e;
  bool ok = true;
};
static bool cr_head(CR &r, uint8_t &maj, uint8_t &ai, uint64_t &v) {
  if (!r.ok || r.p >= r.e) { r.ok = false; return false; }
  uint8_t b = *r.p++;
  maj = b >> 5;
  ai = b & 31;
  v = 0;
  if (ai < 24) v = ai;
  else if (ai <= 27) {
    int n = 1 << (ai - 24);
    if (r.e - r.p < n) { r.ok = false; return false; }
    for (int i = 0; i < n; i++) v = (v << 8) | *r.p++;
  } else if (ai != 31) {
    r.ok = false;
    return false;
  }
  return true;
}
static bool cr_at_break(CR &r) { return r.p < r.e && *r.p == 0xFF; }
static void cr_skip(CR &r, int depth = 0) {
  uint8_t maj, ai;
  uint64_t v;
  if (depth > 16 || !cr_head(r, maj, ai, v)) { r.ok = false; return; }
  bool indef = ai == 31;
  switch (maj) {
    case 2: case 3:
      if (indef) { while (r.ok && !cr_at_break(r)) cr_skip(r, depth + 1); r.p++; }
      else if ((uint64_t)(r.e - r.p) < v) r.ok = false;
      else r.p += v;
      break;
    case 4: case 5: {
      uint64_t n = maj == 5 ? v * 2 : v;
      if (indef) { while (r.ok && !cr_at_break(r)) cr_skip(r, depth + 1); r.p++; }
      else for (uint64_t i = 0; i < n && r.ok; i++) cr_skip(r, depth + 1);
      break;
    }
    case 6: cr_skip(r, depth + 1); break;
    default: break;   // Zahlen, einfache Werte, Gleitkomma: Kopf ist schon gelesen
  }
}
static float half_to_float(uint16_t h) {
  int e = (h >> 10) & 0x1F, m = h & 0x3FF;
  float v = e == 0 ? ldexpf(m, -24) : e == 31 ? INFINITY : ldexpf(m + 1024, e - 25);
  return (h & 0x8000) ? -v : v;
}
// Zahl lesen (Ganzzahl, true/false, Gleitkomma); sonst ueberspringen und false
static bool cr_num(CR &r, double &out) {
  const uint8_t *start = r.p;
  uint8_t maj, ai;
  uint64_t v;
  if (!cr_head(r, maj, ai, v)) return false;
  if (maj == 0) { out = (double)v; return true; }
  if (maj == 1) { out = -1.0 - (double)v; return true; }
  if (maj == 7) {
    if (ai == 20 || ai == 21) { out = ai == 21; return true; }
    if (ai == 25) { out = half_to_float(v); return true; }
    if (ai == 26) { uint32_t u = v; float f; memcpy(&f, &u, 4); out = f; return true; }
    if (ai == 27) { double d; memcpy(&d, &v, 8); out = d; return true; }
    return false;
  }
  r.p = start;
  cr_skip(r);
  return false;
}
static bool cr_str(CR &r, std::string &out) {
  if (r.p < r.e && (*r.p >> 5) == 3 && (*r.p & 31) != 31) {
    uint8_t maj, ai;
    uint64_t v;
    if (!cr_head(r, maj, ai, v) || (uint64_t)(r.e - r.p) < v) { r.ok = false; return false; }
    out.assign((const char *)r.p, v);
    r.p += v;
    return true;
  }
  cr_skip(r);
  return false;
}
// Container oeffnen: liefert Anzahl (bzw. -1 = unbestimmt)
static bool cr_open(CR &r, uint8_t want_maj, int64_t &n) {
  if (r.p >= r.e || (*r.p >> 5) != want_maj) { cr_skip(r); return false; }
  uint8_t maj, ai;
  uint64_t v;
  if (!cr_head(r, maj, ai, v)) return false;
  n = ai == 31 ? -1 : (int64_t)v;
  return true;
}
static bool cr_more(CR &r, int64_t n, int64_t i) {
  if (!r.ok) return false;
  if (n < 0) {
    if (cr_at_break(r)) { r.p++; return false; }
    return r.p < r.e;
  }
  return i < n;
}

// ---------------------------------------------------------------- Werte ablegen
static void store(uint16_t src, const std::string &tn, const std::string &pn, double v) {
  Lock l;
  if (src && src != ADDR_BROKER) s_owner[tn] = src;
  for (int i = 0; i < P_COUNT; i++)
    if (tn == PARAMS[i].tn && pn == PARAMS[i].pn) {
      if (!s_has[i] || s_val[i] != v) {
        s_val[i] = v;
        s_has[i] = true;
        s_ver++;
      }
      return;
    }
}

// ErrorReset.ErrCode: Liste von {"sev","code","resettable"} – leer = keine Stoerung
static void store_errors(CR &r) {
  int64_t n;
  int first = 0;
  if (!cr_open(r, 4, n)) return;
  for (int64_t i = 0; cr_more(r, n, i); i++) {
    int64_t m;
    if (!cr_open(r, 5, m)) continue;
    for (int64_t j = 0; cr_more(r, m, j); j++) {
      std::string k;
      cr_str(r, k);
      double d;
      if (k == "code" && cr_num(r, d)) { if (!first) first = (int)d; }
      else if (k != "code") cr_skip(r);
    }
  }
  Lock l;
  if (s_err != first) {
    s_err = first;
    s_ver++;
  }
}

// Ein Wert {tn, pn, v, ...} – Schluessel in beliebiger Reihenfolge
static void parse_value_map(CR &r, uint16_t src, const std::string &tn_outer) {
  int64_t n;
  if (!cr_open(r, 5, n)) return;
  std::string tn = tn_outer, pn;
  const uint8_t *vpos = nullptr;
  for (int64_t i = 0; cr_more(r, n, i); i++) {
    std::string k;
    cr_str(r, k);
    if (k == "tn") cr_str(r, tn);
    else if (k == "pn") cr_str(r, pn);
    else if (k == "v") { vpos = r.p; cr_skip(r); }
    else cr_skip(r);
  }
  if (!vpos || tn.empty() || pn.empty()) return;
  CR v{vpos, r.e};
  if (tn == "ErrorReset" && pn == "ErrCode") { store_errors(v); return; }
  double d;
  if (cr_num(v, d)) store(src, tn, pn, d);
}

// Antwort auf die Parameterabfrage: {"topics":[{"tn":..,"parameters":[{pn,v,..},..]},..]}
static void parse_discovery(CR &r, uint16_t src) {
  int64_t n;
  if (!cr_open(r, 5, n)) return;
  for (int64_t i = 0; cr_more(r, n, i); i++) {
    std::string k;
    cr_str(r, k);
    if (k != "topics") { cr_skip(r); continue; }
    int64_t nt;
    if (!cr_open(r, 4, nt)) continue;
    for (int64_t t = 0; cr_more(r, nt, t); t++) {
      int64_t m;
      if (!cr_open(r, 5, m)) continue;
      std::string tn;
      const uint8_t *ppos = nullptr;
      for (int64_t j = 0; cr_more(r, m, j); j++) {
        std::string k2;
        cr_str(r, k2);
        if (k2 == "tn") cr_str(r, tn);
        else if (k2 == "parameters") { ppos = r.p; cr_skip(r); }
        else cr_skip(r);
      }
      if (!ppos || tn.empty()) continue;
      CR pr{ppos, r.e};
      int64_t np;
      if (!cr_open(pr, 4, np)) continue;
      for (int64_t p = 0; cr_more(pr, np, p); p++) parse_value_map(pr, src, tn);
    }
  }
}

static void handle_frame(const uint8_t *f, size_t n) {
  if (n < 18) return;
  uint16_t src = f[2] | (f[3] << 8);
  uint8_t ctrl = f[6], sub = f[16];
  CR r{f + 18, f + n};
  {
    Lock l;
    s_last_rx_ms = millis();
  }
  if (ctrl == C_REG && sub == S_REG_RESP) {
    int64_t m;
    if (!cr_open(r, 5, m)) return;
    for (int64_t i = 0; cr_more(r, m, i); i++) {
      std::string k;
      cr_str(r, k);
      double d;
      if (k == "addr" && cr_num(r, d)) s_addr = (uint16_t)d;
      else if (k != "addr") cr_skip(r);
    }
    log_i("iNet X: Adresse 0x%04X zugewiesen", s_addr);
  } else if (ctrl == C_MBP && sub == S_INFO) {
    parse_value_map(r, src, "");
  } else if (ctrl == C_MBP && sub == S_PDISC_RESP) {
    parse_discovery(r, src);
  }
}

// ---------------------------------------------------------------- Uebertragung
static void on_cmd(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) {
  if (!n) return;
  if (d[0] == 0x83 && n >= 3) {
    portENTER_CRITICAL(&s_rx_mux);
    s_rx_need = d[1] | (d[2] << 8);
    if (s_rx_need > RX_MAX) s_rx_need = 0;
    s_rx_len = 0;
    s_rx_announced = s_rx_need > 0;
    portEXIT_CRITICAL(&s_rx_mux);
  }
  Ev e{EV_CMD, (uint8_t)(n < 6 ? n : 6), {0}};
  memcpy(e.d, d, e.n);
  xQueueSend(s_ev, &e, 0);
}

static void deliver(const uint8_t *d, size_t n) {
  if (s_frame_busy) return;   // vorheriger Rahmen wird noch ausgewertet – sehr selten, dann verwerfen
  memcpy(s_frame, d, n);
  s_frame_len = n;
  s_frame_busy = true;
  Ev e{EV_FRAME, 0, {0}};
  xQueueSend(s_ev, &e, 0);
}

static void on_data(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) {
  portENTER_CRITICAL(&s_rx_mux);
  bool ann = s_rx_announced;
  if (ann) {
    size_t k = s_rx_len + n > s_rx_need ? s_rx_need - s_rx_len : n;
    memcpy(s_rx + s_rx_len, d, k);
    s_rx_len += k;
  }
  bool done = ann && s_rx_len >= s_rx_need;
  if (done) s_rx_announced = false;
  portEXIT_CRITICAL(&s_rx_mux);
  if (!ann) {
    if (n <= RX_MAX) deliver(d, n);   // ohne Ankuendigung: eine Benachrichtigung = ein Rahmen
  } else if (done) {
    deliver(s_rx, s_rx_need);
  }
}

namespace {   // dateilokal: gleichnamige Typen anderer Dateien duerfen sich nicht vermischen
class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *, int reason) override {
    s_connected = false;
    s_ready = false;
    Ev e{EV_DOWN, 1, {(uint8_t)reason}};
    xQueueSend(s_ev, &e, 0);
  }
};
}  // namespace
static ClientCB s_ccb;

static bool write_cmd(const uint8_t *d, size_t n) { return s_ch_cmd && s_ch_cmd->writeValue(d, n, true); }

// Ein Ereignis abarbeiten; Rueckgabe: Inhalt einer CMD-Meldung (fuer Warteschleifen)
static void process(const Ev &e) {
  if (e.k == EV_CMD && e.n >= 1 && e.d[0] == 0x83) {
    uint8_t ok[2] = {0x03, 0x00};
    write_cmd(ok, 2);
  } else if (e.k == EV_FRAME) {
    uint8_t ack[2] = {0xF0, 0x01};
    write_cmd(ack, 2);
    handle_frame(s_frame, s_frame_len);
    s_frame_busy = false;
  }
}

// Warten auf eine CMD-Meldung mit erstem Byte b0; andere Ereignisse werden nebenbei abgearbeitet
static bool wait_cmd(uint8_t b0, uint32_t ms, uint8_t *b1 = nullptr) {
  uint32_t until = millis() + ms;
  while ((int32_t)(until - millis()) > 0) {
    Ev e;
    if (xQueueReceive(s_ev, &e, pdMS_TO_TICKS(50)) != pdTRUE) {
      if (!s_connected) return false;
      continue;
    }
    if (e.k == EV_DOWN) {
      xQueueSendToFront(s_ev, &e, 0);
      return false;
    }
    if (e.k == EV_CMD && e.n >= 1 && e.d[0] == b0) {
      if (b1) *b1 = e.n >= 2 ? e.d[1] : 0;
      return true;
    }
    process(e);
  }
  return false;
}

static bool tx(const std::vector<uint8_t> &f) {
  if (!s_connected || !s_ch_cmd || !s_ch_w) return false;
  uint8_t init[3] = {0x01, (uint8_t)(f.size() & 0xFF), (uint8_t)(f.size() >> 8)};
  if (!write_cmd(init, 3)) return false;
  if (!wait_cmd(0x81, 3000)) return false;
  size_t chunk = s_client->getMTU() > 3 ? s_client->getMTU() - 3 : 20;
  for (size_t pos = 0; pos < f.size(); pos += chunk) {
    size_t k = f.size() - pos < chunk ? f.size() - pos : chunk;
    if (!s_ch_w->writeValue(f.data() + pos, k, false)) return false;
  }
  uint8_t res = 0;
  if (!wait_cmd(0xF0, 3000, &res) || res != 0x01) return false;
  vTaskDelay(pdMS_TO_TICKS(200));
  return true;
}

static std::vector<uint8_t> frame(uint16_t dest, uint8_t ctrl, uint8_t sub, uint8_t corr, const std::vector<uint8_t> &cbor,
                                  uint16_t src) {
  std::vector<uint8_t> f(18 + cbor.size(), 0);
  f[0] = dest & 0xFF; f[1] = dest >> 8;
  f[2] = src & 0xFF; f[3] = src >> 8;
  uint16_t ps = f.size() - 7;
  f[4] = ps & 0xFF; f[5] = ps >> 8;
  f[6] = ctrl;
  f[16] = sub;
  f[17] = corr;
  memcpy(f.data() + 18, cbor.data(), cbor.size());
  return f;
}

static bool write_param(uint16_t dest, const char *tn, const char *pn, int64_t v, const char *sv = nullptr) {
  std::vector<uint8_t> c;
  cw_head(c, 5, 4);
  cw_str(c, "tn"); cw_str(c, tn);
  cw_str(c, "pn"); cw_str(c, pn);
  cw_str(c, "v");
  if (sv) cw_str(c, sv);
  else cw_int(c, v);
  cw_str(c, "id"); cw_int(c, 0);
  return tx(frame(dest, C_MBP, S_WRITE, 0, c, s_addr));
}

static bool discover(uint16_t dest) { return tx(frame(dest, C_MBP, S_PDISC, 0, {}, s_addr)); }

static uint16_t owner_of(const char *tn) {
  if (!strcmp(tn, "RoomClimate") || !strcmp(tn, "SystemTime") || !strcmp(tn, "MobileIdentity")) return ADDR_PANEL;
  Lock l;
  auto it = s_owner.find(tn);
  return it != s_owner.end() ? it->second : ADDR_HEATER;
}

// ---------------------------------------------------------------- Sitzung
static bool startup() {
  set_status("Melde an …");
  s_addr = 0;
  std::vector<uint8_t> c;
  cw_head(c, 5, 1);
  cw_str(c, "pv");
  cw_head(c, 4, 2); cw_int(c, 5); cw_int(c, 1);
  if (!tx(frame(ADDR_BROADCAST, C_REG, S_WRITE, 0x42, c, ADDR_APP))) return false;
  uint32_t until = millis() + 20000;
  while (!s_addr && (int32_t)(until - millis()) > 0) {
    Ev e;
    if (xQueueReceive(s_ev, &e, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (e.k == EV_DOWN) return false;
    process(e);
  }
  if (!s_addr) return false;

  set_status("Abonniere Werte …");
  c.clear();
  cw_head(c, 5, 1);
  cw_str(c, "tn");
  cw_head(c, 4, sizeof(TOPICS) / sizeof(TOPICS[0]));
  for (auto t : TOPICS) cw_str(c, t);
  if (!tx(frame(ADDR_BROKER, C_MBP, S_SUBSCRIBE, 0, c, s_addr))) return false;
  vTaskDelay(pdMS_TO_TICKS(500));

  time_t now = time(nullptr);
  if (now > 1700000000) write_param(ADDR_PANEL, "SystemTime", "Time", now);
  write_param(ADDR_PANEL, "SystemTime", "Lot", 0);
  write_param(ADDR_PANEL, "MobileIdentity", "UserName", 0, "Zigbee-Panel");
  write_param(ADDR_PANEL, "MobileIdentity", "Muid", 0, s_muid.c_str());
  write_param(ADDR_PANEL, "MobileIdentity", "Uuid", 0, s_uuid.c_str());
  write_param(ADDR_PANEL, "MobileIdentity", "LastMessage", 1);

  set_status("Lese Werte …");
  discover(ADDR_BROADCAST);
  static const uint16_t SEED[] = {0x0101, 0x0201, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406,
                                  0x0601, 0x0602, 0x0603, 0x0604};
  for (uint16_t a : SEED) {
    if (!s_connected) return false;
    discover(a);
    vTaskDelay(pdMS_TO_TICKS(150));
  }
  return s_connected;
}

static std::string make_uuid() {
  uint8_t b[16];
  esp_fill_random(b, 16);
  b[6] = (b[6] & 0x0F) | 0x40;
  b[8] = (b[8] & 0x3F) | 0x80;
  char s[40];
  snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
           b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return s;
}

static void drop_client() {
  if (s_client) {
    if (s_client->isConnected()) s_client->disconnect();
    NimBLEDevice::deleteClient(s_client);
  }
  s_client = nullptr;
  s_ch_cmd = s_ch_w = s_ch_r = nullptr;
  s_connected = false;
  s_ready = false;
}

static void task(void *) {
  for (;;) {
    Ev e;
    if (xQueueReceive(s_ev, &e, pdMS_TO_TICKS(500)) == pdTRUE) {
      if (e.k == EV_UP) {
        bool ok = startup();
        s_ready = ok && s_connected;
        if (s_ready) {
          set_status("OK");
          s_backoff_ms = 15000;
        } else if (s_connected) {
          set_status("Anmeldung am Bedienteil fehlgeschlagen");
          s_client->disconnect();
        }
        s_ver++;
      } else if (e.k == EV_DOWN) {
        set_status("Verbindung getrennt");
        s_ver++;
      } else {
        process(e);
      }
    }
    // Schreibauftraege
    WriteReq w;
    while (s_ready && xQueueReceive(s_wq, &w, 0) == pdTRUE) {
      if (!write_param(owner_of(w.tn), w.tn, w.pn, w.v)) set_status("Befehl wurde nicht bestätigt");
      s_ver++;
    }
    // Lange still? Mit einer Abfrage pruefen, ob die Verbindung noch lebt
    if (s_ready) {
      uint32_t last;
      {
        Lock l;
        last = s_last_rx_ms;
      }
      if (millis() - last > 10UL * 60 * 1000) {
        if (!discover(ADDR_HEATER)) s_client->disconnect();
        Lock l;
        s_last_rx_ms = millis();
      }
    }
  }
}

// ---------------------------------------------------------------- Bluetooth-Anbindung (aus truma.cpp)
void inetx_ble_setup() {
  NimBLEDevice::setSecurityAuth(true, false, true);    // Bonding, Just Works, Secure Connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setMTU(251);
}

static bool looks_like_inetx(const NimBLEAdvertisedDevice *dev, bool &add_mode) {
  add_mode = false;
  bool hit = false;
  if (dev->haveManufacturerData()) {
    std::string md = dev->getManufacturerData();
    if (md.size() >= 2 && ((uint8_t)md[0] | ((uint8_t)md[1] << 8)) == MANUFACTURER) {
      hit = true;
      add_mode = md.size() >= 4 && (uint8_t)md[3] == 0x01;
    }
  }
  for (int i = 0; !hit && i < (int)dev->getServiceUUIDCount(); i++) {
    std::string u = dev->getServiceUUID(i).toString();
    hit = u.size() > 8 && u.rfind("fc31", 0) == 0 && u.find(UUID_SUFFIX) != std::string::npos;
  }
  if (!hit && dev->haveName()) hit = dev->getName().rfind("Truma iNetX", 0) == 0;
  return hit;
}

bool inetx_on_adv(const NimBLEAdvertisedDevice *dev) {
  bool add_mode;
  if (!looks_like_inetx(dev, add_mode)) return false;
  std::string mac = dev->getAddress().toString();
  Lock l;
  auto b = s_block.find(mac);
  if (b != s_block.end() && (int32_t)(millis() - b->second) < 0) return true;
  // Beim Koppeln nur ein Bedienteil im Hinzufuegen-Modus; sonst bevorzugt das gekoppelte
  if (s_pairing && !add_mode && dev->haveManufacturerData()) return true;
  if (!s_pairing && !s_paired) return true;
  if (s_cand.empty() || mac == s_peer_id || millis() - s_cand_ms > 5000) {
    s_cand = mac;
    s_cand_type = dev->getAddress().getType();
    s_cand_ms = millis();
  }
  return true;
}

static void ensure_task();

void inetx_scan_wanted(bool &passive, bool &active) {
  ensure_task();   // wird nur aufgerufen, wenn das iNet X in den Einstellungen eingeschaltet ist
  if (s_pairing && (int32_t)(millis() - s_pair_until) >= 0) {
    s_pairing = false;
    set_status(s_paired ? "Getrennt" : "Nicht gekoppelt");
  }
  if (s_pairing) active = true;
  else if (s_paired && !s_connected) passive = true;
}

bool inetx_want_connect() {
  if (s_connected || (!s_paired && !s_pairing)) return false;
  if ((int32_t)(millis() - s_next_try_ms) < 0) return false;
  Lock l;
  return !s_cand.empty() && millis() - s_cand_ms < 10000;
}

void inetx_connect_now() {
  std::string mac;
  uint8_t type;
  bool pairing = s_pairing;
  {
    Lock l;
    mac = s_cand;
    type = s_cand_type;
    s_cand.clear();
  }
  if (mac.empty()) return;
  set_status(pairing ? "Kopple …" : "Verbinde …");
  drop_client();
  s_client = NimBLEDevice::createClient();
  s_client->setClientCallbacks(&s_ccb, false);
  s_client->setConnectTimeout(10000);
  auto fail = [&](const char *why, uint32_t block_ms) {
    log_w("iNet X %s: %s", mac.c_str(), why);
    drop_client();
    {
      Lock l;
      s_block[mac] = millis() + block_ms;
    }
    s_next_try_ms = millis() + s_backoff_ms;
    if (s_backoff_ms < 45000) s_backoff_ms *= 2;
    set_status(why);
  };
  if (!s_client->connect(NimBLEAddress(mac, type), true)) return fail("Verbindung fehlgeschlagen", 20000);
  // Ohne Verschluesselung laesst das Bedienteil nichts zu; beim ersten Mal wird dabei gekoppelt
  if (!s_client->secureConnection()) return fail(pairing ? "Kopplung abgelehnt – am iNet X „Gerät hinzufügen“ öffnen" : "Verschlüsselung fehlgeschlagen – neu koppeln?", 20000);
  for (auto *svc : s_client->getServices(true)) {
    if (!s_ch_cmd) s_ch_cmd = svc->getCharacteristic(NimBLEUUID(UUID_CMD));
    if (!s_ch_w) s_ch_w = svc->getCharacteristic(NimBLEUUID(UUID_DATA_W));
    if (!s_ch_r) s_ch_r = svc->getCharacteristic(NimBLEUUID(UUID_DATA_R));
  }
  if (!s_ch_cmd || !s_ch_w || !s_ch_r) return fail("Unbekanntes Gerät (Dienst fehlt)", 60000);
  if (!s_ch_cmd->subscribe(true, on_cmd, true) || !s_ch_r->subscribe(true, on_data, true))
    return fail("Abonnieren abgelehnt – neu koppeln?", 20000);
  s_connected = true;
  if (pairing) {
    std::string id = s_client->getConnInfo().getIdAddress().toString();
    {
      Lock l;
      s_paired = true;
      s_pairing = false;
      s_peer_id = id;
    }
    s_prefs.begin("inetx", false);
    s_prefs.putBool("paired", true);
    s_prefs.putString("peer", id.c_str());
    s_prefs.end();
    log_i("iNet X gekoppelt: %s", id.c_str());
  }
  Ev e{EV_UP, 0, {0}};
  xQueueSend(s_ev, &e, 0);
}

// ---------------------------------------------------------------- oeffentliche API
void inetx_begin() {
  s_mtx = xSemaphoreCreateRecursiveMutex();
  s_ev = xQueueCreate(24, sizeof(Ev));
  s_wq = xQueueCreate(8, sizeof(WriteReq));
  s_rx = (uint8_t *)heap_caps_malloc(RX_MAX, MALLOC_CAP_SPIRAM);
  s_frame = (uint8_t *)heap_caps_malloc(RX_MAX, MALLOC_CAP_SPIRAM);
  s_prefs.begin("inetx", false);
  s_paired = s_prefs.getBool("paired", false);
  s_peer_id = s_prefs.getString("peer", "").c_str();
  s_muid = s_prefs.getString("muid", "").c_str();
  s_uuid = s_prefs.getString("uuid", "").c_str();
  if (s_muid.empty()) { s_muid = make_uuid(); s_prefs.putString("muid", s_muid.c_str()); }
  if (s_uuid.empty()) { s_uuid = make_uuid(); s_prefs.putString("uuid", s_uuid.c_str()); }
  s_prefs.end();
  strncpy(s_status, s_paired ? "Suche Bedienteil …" : "Nicht gekoppelt", sizeof(s_status) - 1);
  // Der Task (6 KB Stack im knappen internen RAM) entsteht erst, wenn das iNet X eingeschaltet ist
  log_i("iNet X: %s", s_paired ? "gekoppelt" : "nicht gekoppelt");
}

static bool s_task_started = false;
static void ensure_task() {
  if (s_task_started) return;
  s_task_started = true;
  xTaskCreatePinnedToCore(task, "inetx", 6144, nullptr, 3, nullptr, 0);
}

uint32_t inetx_version() { return s_ver; }

bool inetx_get(TrumaHeatInfo &o) {
  o = TrumaHeatInfo();
  o.key = TRUMA_HEAT_KEY_INETX;
  o.source = "Truma iNet X";
  o.mode_names[0] = "Komfort";
  o.mode_names[1] = "Schnell";
  o.mode_count = 2;
  o.water_boost = true;
  o.energy_writable = false;
  o.energy_style = HES_INETX;
  Lock l;
  o.connected = s_ready;
  auto has = [](Param p) { return s_has[p]; };
  auto val = [](Param p) { return s_val[p]; };
  o.has = has(P_ROOM_TGT) || has(P_RC_MODE) || has(P_W_ACTIVE);
  if (has(P_RC_MODE)) o.heat_on = val(P_RC_MODE) == 3 || val(P_RC_MODE) == 4;
  else if (has(P_HEAT_ACTIVE)) o.heat_on = val(P_HEAT_ACTIVE) != 0;
  if (has(P_ROOM_TGT)) o.room_target = val(P_ROOM_TGT) / 10.0f;
  if (has(P_ROOM_CUR)) o.room_actual = val(P_ROOM_CUR) / 10.0f;
  else if (has(P_TEMP_INT)) o.room_actual = val(P_TEMP_INT) / 10.0f;
  o.heat_mode = has(P_HEAT_MODE) && val(P_HEAT_MODE) == 0 ? 1 : 0;   // 0 = schnell, 1 = komfort
  o.running = has(P_HEAT_ACTIVE) && val(P_HEAT_ACTIVE) == 1;
  if (has(P_W_ACTIVE) && val(P_W_ACTIVE) == 0) o.water = TW_OFF;
  else if (has(P_W_FAST) && val(P_W_FAST) == 1) o.water = TW_BOOST;
  else if (has(P_W_MODE)) o.water = val(P_W_MODE) >= 1 ? TW_HOT : TW_ECO;
  if (has(P_W_TEMP)) o.water_actual = val(P_W_TEMP) / 10.0f;
  int el = has(P_EL_LEVEL) ? (int)val(P_EL_LEVEL) : 0;
  o.el_power = el == 2 ? 1800 : el == 1 ? 900 : 0;
  bool gas = !has(P_GAS_LEVEL) || val(P_GAS_LEVEL) != 0;
  o.energy = el ? (gas ? TE_MIX : TE_ELECTRIC) : TE_GAS;
  if (has(P_VBAT)) o.battery_v = val(P_VBAT) / 10.0f;
  o.error = s_err;
  o.age_s = s_last_rx_ms ? (int32_t)((millis() - s_last_rx_ms) / 1000) : -1;
  o.busy = uxQueueMessagesWaiting(s_wq) > 0;
  char b[64];
  if (s_ready && s_err) { snprintf(b, sizeof(b), tr("Störung, Code %d"), s_err); o.status = b; }
  else if (s_pairing && !s_connected) o.status = "Kopplung läuft – am iNet X „Gerät hinzufügen“ wählen";
  else o.status = s_status;
  return true;
}

static void queue_write(const char *tn, const char *pn, int32_t v) {
  WriteReq w;
  memset(&w, 0, sizeof(w));
  strncpy(w.tn, tn, sizeof(w.tn) - 1);
  strncpy(w.pn, pn, sizeof(w.pn) - 1);
  w.v = v;
  xQueueSend(s_wq, &w, 0);
  s_ver++;
}

void inetx_set_heat(bool on, float t, uint8_t mode) {
  if (!on) {
    queue_write("RoomClimate", "Mode", 0);
    return;
  }
  t = t < 5 ? 5 : t > 30 ? 30 : t;
  queue_write("AirHeating", "TgtTemp", (int32_t)lroundf(t * 10));
  queue_write("AirHeating", "Mode", mode == 1 ? 0 : 1);
  queue_write("RoomClimate", "Mode", 3);
}

void inetx_set_water(TrumaWater w) {
  if (w == TW_OFF) {
    queue_write("WaterHeating", "Active", 0);
    return;
  }
  queue_write("WaterHeating", "Active", 1);
  queue_write("WaterHeating", "Mode", w == TW_ECO ? 0 : w == TW_HOT ? 1 : 2);
  queue_write("WaterHeating", "FasterHeatingMode", w == TW_BOOST ? 1 : 0);
}

void inetx_set_energy(TrumaEnergy e, uint16_t p) {
  queue_write("EnergySrc", "ElectricLevel", e == TE_GAS || e == TE_NONE ? 0 : p >= 1800 ? 2 : 1);
}

void inetx_pair_start() {
  {
    Lock l;
    s_pairing = true;
    s_pair_until = millis() + PAIR_MS;
    s_block.clear();
    s_cand.clear();
  }
  s_next_try_ms = 0;
  set_status("Kopplung läuft – am iNet X „Gerät hinzufügen“ wählen");
}

bool inetx_pairing() { return s_pairing; }
bool inetx_paired() { return s_paired; }

void inetx_disconnect() {
  s_pairing = false;
  if (s_client && s_client->isConnected()) s_client->disconnect();
  set_status("Ausgeschaltet");
}

void inetx_forget() {
  std::string id;
  {
    Lock l;
    id = s_peer_id;
    s_paired = false;
    s_pairing = false;
    s_peer_id.clear();
    for (auto &v : s_has) v = false;
    s_owner.clear();
  }
  if (s_client && s_client->isConnected()) s_client->disconnect();
  // Nur die Kopplung mit diesem iNet X loeschen – Hymer Connect ist ggf. ebenfalls gekoppelt
  bool found = false;
  for (int i = NimBLEDevice::getNumBonds() - 1; i >= 0; i--) {
    NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
    if (!id.empty() && a.toString() == id) {
      NimBLEDevice::deleteBond(a);
      found = true;
    }
  }
  if (!found && !hymer_paired()) NimBLEDevice::deleteAllBonds();   // alte Kopplung ohne gemerkte Adresse
  s_prefs.begin("inetx", false);
  s_prefs.putBool("paired", false);
  s_prefs.remove("peer");
  s_prefs.end();
  set_status("Nicht gekoppelt");
}

#endif
