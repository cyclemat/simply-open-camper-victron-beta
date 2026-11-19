// Akkus mit BMS per Bluetooth – siehe bms.h
#include "bms.h"
// Abgeschaltete Akku-Typen (BMS_*_ON = 0) lassen ihre Hilfsfunktionen ungenutzt zurueck – das ist gewollt
#pragma GCC diagnostic ignored "-Wunused-function"

// Name fuer die Liste "andere Geraete": nur druckbare Zeichen und gueltiges UTF-8 (Truma-Sensoren senden z. B. ein
// Steuerbyte vor dem Namen und Nullen dahinter)
static std::string seen_name(const std::string &in) {
  std::string out;
  for (size_t i = 0; i < in.size(); i++) {
    uint8_t c = (uint8_t)in[i];
    if (c >= 0x20 && c < 0x7F) out += (char)c;
    else if (c >= 0xC2 && c <= 0xDF && i + 1 < in.size() && ((uint8_t)in[i + 1] & 0xC0) == 0x80) {
      out += in.substr(i, 2);
      i++;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

const char *bms_model_text(BmsModel m) {
  switch (m) {
    case BMS_SUPERB: return "Super B";
    case BMS_PQ_HEX:
    case BMS_PQ_ASCII: return "Power Quarry / Shentec";   // CBT-BMS, App "Power Quarry" (auch Shentec-Akkus)
    case BMS_BULLTRON: return "Daly/Bulltron";
    case BMS_RENOGY: return "Renogy";
    case BMS_RENOGY_PRO: return "Renogy Pro";
    case BMS_RENOGY_BT: return "Renogy BT-1/BT-2";
    case BMS_RENOGY_SOLAR: return "Renogy Laderegler";
    case BMS_RENOGY_DCC: return "Renogy DC-DC-Lader";
    case BMS_RENOGY_INV: return "Renogy Wechselrichter";
    case BMS_RENOGY_SHUNT: return "Renogy Smart Shunt";
    case BMS_BLUETTI: return "Bluetti";
    case BMS_ALLPOWERS: return "Allpowers";
    case BMS_ANKER: return "Anker SOLIX";
    case BMS_ANKER_C300: return "Anker SOLIX C300";
    case BMS_ANKER_G2: return "Anker SOLIX Gen 2";
    case BMS_ANKER_F3800: return "Anker SOLIX F3800";
    case BMS_ECOFLOW: return "EcoFlow Delta 2/River 2";
    case BMS_ECOFLOW_PB: return "EcoFlow River 3/Delta 3";
    case BMS_ECOWORTHY: return "Eco-Worthy";
    case BMS_ANT: return "ANT";
    case BMS_VATRER: return "Vatrer";
    case BMS_JBD: return "JBD";
    case BMS_JK: return "JK-BMS";
    case BMS_LITIME: return "LiTime";
    case BMS_SS_TANK: return "SuperSense Tankmodul";
    default: return "Akku";
  }
}

BmsDev bms_dev_of(BmsModel m) {
  switch (m) {
    case BMS_RENOGY_BT:   // noch nicht erkannt: meist ein Laderegler
    case BMS_RENOGY_SOLAR: return BMS_DEV_SOLAR;
    case BMS_RENOGY_DCC: return BMS_DEV_DCDC;
    case BMS_RENOGY_INV: return BMS_DEV_INVERTER;
    case BMS_RENOGY_SHUNT: return BMS_DEV_SHUNT;
    case BMS_BLUETTI:
    case BMS_ALLPOWERS:
    case BMS_ANKER:
    case BMS_ANKER_C300:
    case BMS_ANKER_G2:
    case BMS_ANKER_F3800:
    case BMS_ECOFLOW:
    case BMS_ECOFLOW_PB: return BMS_DEV_PSTATION;
    case BMS_SS_TANK: return BMS_DEV_TANK;
    default: return BMS_DEV_BATTERY;
  }
}

#if WITH_BMS
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <algorithm>
#include <atomic>
#include <ctype.h>
#include <deque>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "victron.h"   // victron_state_text (Ladephase der Renogy-Geraete)
#include "i18n/i18n.h"   // tr(), UI_DECIMAL_COMMA

namespace {   // dateilokal: gleichnamige Typen anderer Dateien duerfen sich nicht vermischen

// ---------------------------------------------------------------- Super B (App "Be in Charge" 2.1.1)
const char *SB_SERVICE = "74b9c2d1-dc6d-42cf-a2e9-7398b8fc2e70";
const char *SB_STATUS = "6edadbe4-4f53-4a5a-96ed-02f93db93790";   // Notify: Statuspaket
const char *SB_RPC = "93618162-0634-49ad-8510-798a91645056";      // Textkonsole (nur zum Entsperren benutzt)
const char *SB_UNLOCK = "SuperB";
const char *SB_HINTS[] = {"epsilon", "super b", "superb"};
const size_t SB_CHUNK = 20;
// Super B "Epsilon V2" (neuere Akkus, Hersteller-ID 0x50BE): anderer Dienst; nach aiobmsble superb_v2_bms (Apache-2.0)
// Anfrage 21 54 00 -> 24-Byte-Rahmen per Notify (Typ = Byte 0, Big Endian):
// Typ 0: [1] SoC, [18] Zyklen, [19] SoH, [20..23] Restzeit s (FFFFFFFF = keine); Typ 2: [6..9] mA (mit Vorzeichen), [10..11] mV
const char *SB2_SERVICE = "cf9ccdf7-eee9-43ce-87a5-82b54af5324e";
const char *SB2_RXTX = "cf9ccdfa-eee9-43ce-87a5-82b54af5324e";
const char *SB2_NOTIFY = "e0fef452-9d2b-4005-a1e3-69fe1102b436";     // bei aiobmsble als Kanal; am Akku ist es der Dienst
const char *SB2_NOTIFY_CH = "e0fef453-9d2b-4005-a1e3-69fe1102b436";  // Meldekanal darin (Datenrahmen, gesehen 2026-11-16)
const uint32_t SB2_POLL_MS = 10000;

// ---------------------------------------------------------------- Power Quarry (App "Power Quarry" 3.5.2, CBT-BMS)
// Schreiben 0xFFE5/0xFFE9 (ohne Antwort, 20-Byte-Stuecke, 30 ms Abstand), Empfang 0xFFE0/0xFFE4 (Notify).
// Kein Pairing. Nur Messwert-Abfragen, keine Parameter-Befehle.
const uint32_t PQ_POLL_MS = 20000;   // Spannung/Strom; Ladezustand und Seltenes jede 3. Abfrage (1 min)
struct PqProduct {
  const char *code, *name;
  uint16_t ah;
  uint8_t cells;
};
const PqProduct PQ_PRODUCTS[] = {
    {"0A", "VB101", 172, 4}, {"0B", "VB201", 172, 4}, {"0C", "VB001", 172, 4}, {"0D", "VB002", 258, 4},
    {"0E", "VB007", 120, 4}, {"0F", "VB102", 258, 4}, {"09", "VB016", 200, 4}, {"08", "VB103", 240, 4},
    {"07", "VB104", 480, 4}, {"10", "VB029", 40, 4},  {"11", "VB030", 100, 4}, {"12", "VB031", 750, 4},
    {"16", "VB028", 300, 4}, {"18", "VB107", 135, 4}, {"19", "VB038", 50, 4},  {"1C", "VB042", 60, 4},
    {"1A", "VB043", 150, 4}, {"CBT001", "CBT001", 480, 4}, {"VB106", "VB106", 42, 4}, {"13", "VB032", 100, 8},
    {"A0", "VB032", 100, 8}, {"A1", "VB017", 135, 8}, {"A2", "VB033", 200, 8}, {"C0", "VB039", 50, 16},
    {"20", "VB046", 175, 4}, {"17", "VB024", 200, 4}, {"14", "VB037", 135, 4}, {"15", "VB035", 270, 4},
};

// ---------------------------------------------------------------- SuperSense (comworks), App 2.13.121 – BETA, ungetestet
// Tankmodul "SSBT…": Dienst a0eeb190…, Befehl 8d81b2f7… (Write), Fuellstaende 8215dd0f… (Read/Notify),
// Temperaturen 0bd4c5d4… (Read/Notify). Nach dem Verbinden 1 s warten, Init 0E 01 02 03 04 05 06 schreiben (mit
// Bestaetigung), dann 8 Byte = 4 Werte BCD (12 34 -> 1234, 9999 = kein Sensor). Fuellstand % = roh / 2475 * 100,
// Temperatur °C = roh * 5 / 1023 * 100 - 50. Kein Pairing. Hier: kurz verbinden, beide Werte lesen, trennen.
const char *SS_SERVICE = "a0eeb190-6b2a-45fb-966e-d8e49323691f";
const char *SS_CMD = "8d81b2f7-e56f-44bf-94ee-94083f5ea6d7";
const char *SS_DATA = "8215dd0f-cc20-4d2a-8b64-73907ec4bd16";
const char *SS_TEMP = "0bd4c5d4-62e3-4946-82ea-d7643834d368";
const uint8_t SS_INIT[7] = {0x0E, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
const int SS_FULL_RAW = 2475;

const uint32_t DISCOVERY_MS = 60000;
const uint32_t RETRY_MS = 20000;        // nach fehlgeschlagener Verbindung
const uint32_t RECONNECT_MS = 8000;     // nach Abbruch einer bestehenden Verbindung
const uint32_t STALE_MS = 90000;        // so lange ohne Statuspaket: Verbindung neu aufbauen

// Renogy-Geraete: Werte ausser Akku (NAN/-1 = unbekannt), siehe BmsInfo
struct Extra {
  float pv_v = NAN, pv_w = NAN, in_v = NAN, in_a = NAN, in_w = NAN, load_a = NAN, load_w = NAN;
  float ac_v = NAN, ac_a = NAN, ac_w = NAN, ac_hz = NAN, grid_v = NAN, starter_v = NAN;
  int yield_wh = -1, state = -1;
  // Power Stations
  float ps_ac_in = NAN, ps_dc_in = NAN, ps_ac_out = NAN, ps_dc_out = NAN, ps_in = NAN, ps_out = NAN;
  int8_t ac_on = -1, dc_on = -1;
  int remain = -1;
};

// Anker SOLIX: Sitzung der Dauerverbindung (bms_ps.inc)
struct AkSess {
  bool enc_outer = false, keyed = false, authorized = false, replied = false;
  uint8_t key[16] = {0}, iv[16] = {0};
  uint16_t mtu = 0;
  int auth = -1;
};
// EcoFlow: Sitzung der Dauerverbindung (bms_ps.inc)
struct EfSess {
  uint8_t enc = 7;
  bool keyed = false, authed = false;
  uint8_t key[16] = {0}, iv[16] = {0};
};
static bool is_ecoflow(BmsModel m) { return m == BMS_ECOFLOW || m == BMS_ECOFLOW_PB; }
static bool is_anker(BmsModel m) {
  return m == BMS_ANKER || m == BMS_ANKER_C300 || m == BMS_ANKER_G2 || m == BMS_ANKER_F3800;
}

struct Bat {
  std::string mac, name;
  uint8_t type = 0;
  BmsModel model = BMS_SUPERB;
  NimBLEClient *client = nullptr;
  volatile bool connected = false;
  uint32_t next_try = 0, last_ms = 0, conn_ms = 0;
  bool has = false;
  int soc = -1, soh = -1;
  float volt = NAN, amp = NAN;
  int32_t ttg = -1;
  uint8_t flags = 0;
  std::string model_text, serial, firmware, status = "Warte …";
  bool remove = false;
  // Power Quarry
  int cap_ah = 0;
  uint8_t cells = 4;
  NimBLERemoteCharacteristic *wr = nullptr;
  uint32_t next_poll = 0;
  float temp = NAN;
  int cycles = -1;
  std::vector<uint16_t> cell_mv;
  uint32_t alarms = 0;
  bool chg_on = true, dis_on = true;
  std::string rx;   // Empfangspuffer (Notify)
  bool wr_resp = false;   // mit Bestaetigung schreiben (falls ohne nichts ankommt)
  int mos_raw = -1;       // Power Quarry: Rohwert MOS-Status (0x0A, Byte 2) – Bedeutung noch ungeklaert
  uint32_t polls = 0;     // Zaehler: nur jede 3. Abfrage (1 min) auch Ladezustand/Temperatur/Kapazitaet/Alarme
  int rssi = 0;           // Signal zuletzt (fuers Protokoll bei Trennung)
  uint8_t silent = 0;     // Abfragen hintereinander ganz ohne Empfang
  std::string pin;        // Kopplungs-PIN (Super B Epsilon V2: steht auf dem Akku bzw. in der Anleitung)
  bool pin_need = false;  // Akku hat beim Koppeln eine PIN verlangt
  bool auth_fail = false; // Trennung mit "Authentication Failure" (0x205/0x206): alte Kopplung vor dem naechsten Versuch loeschen
  // Abgefragte Akkus (Power Quarry, Bulltron) nur kurz verbinden: verbinden, abfragen, trennen. Die Dauer-
  // verbindung riss beim Power Quarry immer nach ~13 s ab ("Grund 520", Funkverbindung verloren).
  bool polled() const { return model != BMS_SUPERB && model != BMS_UNKNOWN && !is_anker(model) && !is_ecoflow(model); }
  int8_t daly_x81 = -1;    // Daly: -1 noch unbekannt, 0 Protokoll D2, 1 Protokoll 0x81 (Antwort 0x51)
  int jk_sw = -1;          // JK: Firmware-Hauptversion (ab 11 Aufbau 32S)
  std::string eco_a1, eco_a2;   // Eco-Worthy: letzte Rahmen A1/A2 (je ein Notify)
  uint8_t rid = 0;              // Renogy: Modbus-Adresse, die geantwortet hat (0 = noch unbekannt)
  Extra ex;                     // Renogy-Geraete, Power Stations
  int8_t bl_enc = -1;           // Bluetti: -1 unbekannt, 0 Klartext, 1 verschluesselt (rid = Register-Generation 1/2)
  int8_t want_ac = -1, want_dc = -1;   // Power Station: Ausgang schalten (bei der naechsten Verbindung)
  std::string ap_frame;         // Allpowers: letzter Datensatz
  AkSess ak;                    // Anker: Sitzung (Dauerverbindung)
  int8_t ak_outer = -1;         // Anker: -1 unbekannt (offen probieren), 0 offen, 1 verschluesselt
  std::deque<std::string> ak_q; // Anker: empfangene Pakete
  std::string ak_frag;          // Anker: Bruchstuecke
  EfSess ef;                    // EcoFlow: Sitzung (Dauerverbindung)
  uint8_t ef_enc = 7;           // EcoFlow: Verschluesselungsart (aus den Werbedaten, gespeichert)
  std::string ef_rx;            // EcoFlow: Empfangspuffer
  Extra ef_ex;                  // EcoFlow: gesammelte Werte (Meldungen bringen nur Teile)
  float ef_soc = NAN, ef_temp = NAN, ef_volt = NAN;
  int ss_raw[BMS_TANKS] = {-1, -1, -1, -1};    // SuperSense: Rohwerte Fuellstand (-1 = kein Sensor)
  int ss_traw[BMS_TANKS] = {-1, -1, -1, -1};   // SuperSense: Rohwerte Temperatur
};

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient *c, int reason) override;
  void onAuthenticationComplete(NimBLEConnInfo &ci) override;   // Kopplung: Ergebnis ins Protokoll
  void onPassKeyEntry(NimBLEConnInfo &ci) override;
  void onConfirmPasskey(NimBLEConnInfo &ci, uint32_t pin) override;
};

}  // namespace

static SemaphoreHandle_t s_mtx = nullptr;
static std::vector<Bat> s_bats;
static std::vector<BmsFound> s_found;
static std::vector<BmsSeen> s_seen;   // nicht zugeordnet (nur waehrend/nach der Suche)
static uint32_t s_disc_until = 0;
static std::atomic<uint32_t> s_ver{1};
static std::deque<std::string> s_log;
static ClientCB s_ccb;
static std::string s_rpc;               // Antwort der Textkonsole (Notify), nur waehrend des Entsperrens

namespace {
struct Lock {
  Lock() { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(s_mtx); }
};
}  // namespace

static std::string lower(std::string s) {
  for (auto &c : s) c = tolower((unsigned char)c);
  return s;
}

static void logf(const char *fmt, ...) {
  char b[240];
  int n = snprintf(b, sizeof(b), "%6lus ", (unsigned long)(millis() / 1000));
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b + n, sizeof(b) - n, fmt, ap);
  va_end(ap);
  log_i("BMS %s", b);
  Lock l;
  s_log.push_back(b);
  while (s_log.size() > 14) s_log.pop_front();
  s_ver++;
}

static Bat *find_mac(const std::string &mac) {
  for (auto &b : s_bats)
    if (b.mac == mac) return &b;
  return nullptr;
}

static void set_status(Bat &b, const char *s) {
  b.status = s;
  s_ver++;
}

// ---------------------------------------------------------------- Speichern
static void save() {
  Preferences p;
  p.begin("bms", false);
  p.clear();
  uint8_t n = 0;
  for (auto &b : s_bats) {
    if (b.remove) continue;
    char k[4];
    snprintf(k, sizeof(k), "m%u", n); p.putString(k, b.mac.c_str());
    snprintf(k, sizeof(k), "t%u", n); p.putUChar(k, b.type);
    snprintf(k, sizeof(k), "k%u", n); p.putUChar(k, b.model);
    snprintf(k, sizeof(k), "a%u", n); p.putString(k, b.name.c_str());
    snprintf(k, sizeof(k), "c%u", n); p.putUShort(k, b.cap_ah);
    snprintf(k, sizeof(k), "z%u", n); p.putUChar(k, b.cells);
    if (!b.pin.empty()) {
      snprintf(k, sizeof(k), "p%u", n); p.putString(k, b.pin.c_str());
    }
    if (!b.serial.empty()) {
      snprintf(k, sizeof(k), "s%u", n); p.putString(k, b.serial.c_str());
      snprintf(k, sizeof(k), "e%u", n); p.putUChar(k, b.ef_enc);
    }
    n++;
  }
  p.putUChar("n", n);
  p.end();
}

static void load() {
  Preferences p;
  if (!p.begin("bms", true)) return;
  uint8_t n = p.getUChar("n", 0);
  for (uint8_t i = 0; i < n && i < BMS_MAX; i++) {
    char k[4];
    Bat b;
    snprintf(k, sizeof(k), "m%u", i); b.mac = p.getString(k, "").c_str();
    snprintf(k, sizeof(k), "t%u", i); b.type = p.getUChar(k, 0);
    snprintf(k, sizeof(k), "k%u", i); b.model = (BmsModel)p.getUChar(k, BMS_SUPERB);
    snprintf(k, sizeof(k), "a%u", i); b.name = p.getString(k, "Akku").c_str();
    snprintf(k, sizeof(k), "c%u", i); b.cap_ah = p.getUShort(k, 0);
    snprintf(k, sizeof(k), "z%u", i); b.cells = p.getUChar(k, 4);
    snprintf(k, sizeof(k), "s%u", i); b.serial = p.getString(k, "").c_str();
    snprintf(k, sizeof(k), "e%u", i); b.ef_enc = p.getUChar(k, 7);
    snprintf(k, sizeof(k), "p%u", i); if (p.isKey(k)) b.pin = p.getString(k, "").c_str();
    if (!b.mac.empty()) s_bats.push_back(b);
  }
  p.end();
}

// ---------------------------------------------------------------- Super B: Statuspaket
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static float bef(const uint8_t *p) {
  uint32_t u = be32(p);
  float f;
  memcpy(&f, &u, 4);
  return f;
}

// [1] Flags, [2] SoC, [3] SoH, [4..7] Restzeit s, [8..11] Strom A, [12..15] Spannung V (Big Endian)
static void sb_status(const std::string &mac, const uint8_t *d, size_t n) {
  if (n < 16) return;
  Lock l;
  Bat *b = find_mac(mac);
  if (!b) return;
  b->flags = d[1];
  b->soc = d[2];
  b->soh = d[3];
  b->ttg = (int32_t)be32(d + 4);
  float a = bef(d + 8), v = bef(d + 12);
  b->amp = isfinite(a) ? a : NAN;
  b->volt = isfinite(v) ? v : NAN;
  if (!b->has) {
    char raw[3 * 16 + 1] = "";
    for (int i = 0; i < 16; i++) snprintf(raw + 3 * i, 4, "%02X ", d[i]);
    logf("%s: erstes Statuspaket %s", b->name.c_str(), raw);
  }
  b->has = true;
  b->last_ms = millis();
  b->status = "Verbunden";
  s_ver++;
}

// Epsilon V2: Rahmen Typ 0 (Ladezustand usw.) und Typ 2 (Strom/Spannung)
static int s_sb2_dbg = 0;   // erste Rahmen ins Protokoll (Fehlersuche)
static void sb2_rx(const std::string &mac, const uint8_t *d, size_t n) {
  Lock l;
  Bat *b = find_mac(mac);
  if (!b) return;
  if (!b->has && s_sb2_dbg < 6) {
    s_sb2_dbg++;
    char raw[3 * 24 + 1] = "";
    for (size_t i = 0; i < n && i < 24; i++) snprintf(raw + 3 * i, 4, "%02X ", d[i]);
    logf("%s: empfangen %u Byte: %s", b->name.c_str(), (unsigned)n, raw);
  }
  if (n != 24) return;
  if (d[0] == 0x00) {
    b->soc = d[1];
    b->cycles = d[18];
    b->soh = d[19];
    uint32_t rt = be32(d + 20);
    b->ttg = rt == 0xFFFFFFFFu ? -1 : (int32_t)rt;
  } else if (d[0] == 0x01 || d[0] == 0x02) {   // Akku des Nutzers schickt nur 01 und 02 (gleicher Aufbau)
    b->amp = (int32_t)be32(d + 6) / 1000.0f;
    b->volt = ((uint16_t)d[10] << 8 | d[11]) / 1000.0f;
    int16_t t = (int16_t)((uint16_t)d[14] << 8 | d[15]);   // 0x0963 = 24,03 °C (vermutlich 0,01 °C)
    if (t > -4000 && t < 9000) b->temp = t / 100.0f;
    // keine Statusbits bekannt: "in Ordnung", Laden/Entladen erlaubt, Richtung aus dem Strom
    b->flags = 0x01 | 0x10 | 0x20 | (b->amp > 0.1f ? 0x04 : b->amp < -0.1f ? 0x02 : 0);
  } else {
    return;
  }
  if (!b->has && !isnan(b->volt)) {
    logf("%s: erste Werte (Epsilon V2) %d %%, %.2f V, %.2f A", b->name.c_str(), b->soc, b->volt, b->amp);
    b->has = true;
  }
  if (b->has) {
    b->last_ms = millis();
    b->status = "Verbunden";
  }
  s_ver++;
}

// Ladezustand ueber den Standard-Dienst Batteriestand (0x180F/0x2A19) – die Epsilon V2 schickt keinen Rahmen 00
static void sb2_soc(const std::string &mac, const uint8_t *d, size_t n) {
  if (n < 1 || d[0] > 100) return;
  Lock l;
  Bat *b = find_mac(mac);
  if (!b) return;
  if (b->soc != d[0]) s_ver++;
  b->soc = d[0];
}

static bool sb2_connect(Bat &b, NimBLEClient *c) {
  // Erst alle Dienste einlesen (getServices(true) loescht vorher geholte Dienste/Kanaele - danach erst Zeiger holen).
  // Der zweite Meldekanal kann in einem anderen Dienst liegen.
  const std::vector<NimBLERemoteService *> &all = c->getServices(true);
  NimBLERemoteService *svc = c->getService(NimBLEUUID(SB2_SERVICE));
  NimBLERemoteCharacteristic *rx = svc ? svc->getCharacteristic(NimBLEUUID(SB2_RXTX)) : nullptr;
  NimBLERemoteCharacteristic *nt = nullptr;
  if (NimBLERemoteService *ns = c->getService(NimBLEUUID(SB2_NOTIFY))) nt = ns->getCharacteristic(NimBLEUUID(SB2_NOTIFY_CH));
  if (!nt)
    for (auto *s : all)
      if ((nt = s->getCharacteristic(NimBLEUUID(SB2_NOTIFY)))) break;
  if (!rx) {
    logf("%s: Epsilon V2 ohne Datenkanal", b.name.c_str());
    return false;
  }
  auto props = [](NimBLERemoteCharacteristic *ch) -> std::string {
    if (!ch) return "fehlt";
    std::string p;
    if (ch->canRead()) p += "lesen ";
    if (ch->canWrite()) p += "schreiben ";
    if (ch->canWriteNoResponse()) p += "ohne-Antw ";
    if (ch->canNotify()) p += "notify ";
    if (ch->canIndicate()) p += "indicate";
    return p;
  };
  logf("%s: Epsilon V2 cf9ccdfa: %s / e0fef453: %s", b.name.c_str(), props(rx).c_str(), props(nt).c_str());

  s_sb2_dbg = 0;
  std::string mac = b.mac;
  auto cb = [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { sb2_rx(mac, d, n); };
  auto sub = [&](NimBLERemoteCharacteristic *ch) -> bool {
    if (!ch || !(ch->canNotify() || ch->canIndicate())) return false;
    return ch->subscribe(ch->canNotify(), cb, true);
  };
  bool a = sub(rx), n2 = sub(nt);
  if (!a && !n2 && c->isConnected()) {
    // Akku verlangt evtl. eine verschluesselte Verbindung (Kopplung "Just Works" wie mit der App)
    logf("%s: Abonnieren abgelehnt – koppele und versuche erneut", b.name.c_str());
    bool sec = c->secureConnection();
    logf("%s: Kopplung %s", b.name.c_str(), sec ? "ok" : "fehlgeschlagen");
    a = sub(rx);
    n2 = sub(nt);
  }
  if (!a && !n2) {
    logf("%s: Epsilon V2 nicht abonnierbar", b.name.c_str());
    return false;
  }
  logf("%s: abonniert cf9ccdfa %s, e0fef453 %s", b.name.c_str(), a ? "ja" : "nein", n2 ? "ja" : "nein");
  if (NimBLERemoteService *bs = c->getService(NimBLEUUID((uint16_t)0x180F)))
    if (NimBLERemoteCharacteristic *bl = bs->getCharacteristic(NimBLEUUID((uint16_t)0x2A19))) {
      if (bl->canRead()) {
        NimBLEAttValue v = bl->readValue();
        sb2_soc(mac, v.data(), v.length());
      }
      if (bl->canNotify())
        bl->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { sb2_soc(mac, d, n); }, true);
    }
  {
    Lock l;
    b.model_text = "Super B Epsilon V2";
  }
  b.wr = rx;
  b.wr_resp = !rx->canWriteNoResponse() && rx->canWrite();
  b.next_poll = millis() + 300;
  logf("%s: verbunden (Epsilon V2)", b.name.c_str());
  return true;
}

// Textkonsole: alles einsammeln (nur fuers Entsperren)
static void sb_rpc(const uint8_t *d, size_t n) {
  Lock l;
  if (s_rpc.size() < 256) s_rpc.append((const char *)d, n);
}

static bool sb_write(NimBLERemoteCharacteristic *c, const char *txt) {
  size_t len = strlen(txt);
  for (size_t off = 0; off < len; off += SB_CHUNK) {
    size_t k = len - off < SB_CHUNK ? len - off : SB_CHUNK;
    if (!c->writeValue((const uint8_t *)txt + off, k, false)) return false;   // ohne Antwort, wie die App
    delay(20);
  }
  return true;
}

// Ablauf wie die App: verbinden, Konsole mit "SuperB" entsperren, Geraete-Info lesen, Status abonnieren
static bool sb_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteService *svc = c->getService(NimBLEUUID(SB_SERVICE));
  if (!svc) {
    if (c->getService(NimBLEUUID(SB2_SERVICE))) return sb2_connect(b, c);
  }
  NimBLERemoteCharacteristic *st = svc ? svc->getCharacteristic(NimBLEUUID(SB_STATUS)) : nullptr;
  NimBLERemoteCharacteristic *rpc = svc ? svc->getCharacteristic(NimBLEUUID(SB_RPC)) : nullptr;
  if (!svc || !st || !rpc) {
    logf("%s: kein Super-B-Dienst gefunden", b.name.c_str());
    return false;
  }
  {
    Lock l;
    s_rpc.clear();
  }
  if (!rpc->subscribe(true, [](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { sb_rpc(d, n); }, true))
    logf("%s: Konsole nicht abonnierbar", b.name.c_str());
  bool unlocked = false;
  for (int attempt = 1; attempt <= 3 && !unlocked && c->isConnected(); attempt++) {
    delay(500);
    sb_write(rpc, "\n");   // Konsole zuruecksetzen
    delay(150);
    {
      Lock l;
      s_rpc.clear();
    }
    std::string line = std::string(SB_UNLOCK) + "\n";
    if (!sb_write(rpc, line.c_str())) {
      logf("%s: Entsperren nicht gesendet (Versuch %d)", b.name.c_str(), attempt);
      continue;
    }
    uint32_t t0 = millis();
    while (millis() - t0 < 2000) {
      {
        Lock l;
        if (s_rpc.find('\n') != std::string::npos) break;
      }
      delay(20);
    }
    Lock l;
    std::string ans = s_rpc.substr(0, s_rpc.find('\n'));
    while (!ans.empty() && (ans.back() == '\r' || ans.back() == '\n')) ans.pop_back();
    if (s_rpc.find('\n') != std::string::npos) {
      unlocked = true;
      logf("%s: entsperrt, Antwort \"%.40s\"", b.name.c_str(), ans.c_str());
    } else {
      logf("%s: keine Antwort beim Entsperren (Versuch %d)", b.name.c_str(), attempt);
    }
  }
  delay(500);
  rpc->unsubscribe();   // Konsole wird danach nicht mehr gebraucht
  // Geraete-Info (0x180A): Modell, Seriennummer, Firmware
  NimBLERemoteService *dis = c->getService(NimBLEUUID((uint16_t)0x180A));
  auto rd = [&](uint16_t u) -> std::string {
    NimBLERemoteCharacteristic *ch = dis ? dis->getCharacteristic(NimBLEUUID(u)) : nullptr;
    if (!ch || !ch->canRead()) return "";
    NimBLEAttValue v = ch->readValue();
    std::string s((const char *)v.data(), v.length());
    while (!s.empty() && (s.back() == 0 || s.back() == ' ')) s.pop_back();
    return s;
  };
  std::string model = rd(0x2A24), serial = rd(0x2A25), fw = rd(0x2A26);
  {
    Lock l;
    b.model_text = model.empty() ? std::string("Super B") : "Super B " + model;
    b.serial = serial;
    b.firmware = fw;
  }
  logf("%s: %s, Seriennr. %s, Firmware %s", b.name.c_str(), model.c_str(), serial.c_str(), fw.c_str());
  std::string mac = b.mac;
  if (!st->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { sb_status(mac, d, n); },
                     true)) {
    logf("%s: Statusmeldungen nicht abonnierbar", b.name.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- Power Quarry
static const PqProduct *pq_by_code(const std::string &code) {
  for (auto &p : PQ_PRODUCTS)
    if (code == p.code) return &p;
  return nullptr;
}
static const PqProduct *pq_by_prefix(const std::string &name) {
  for (auto &p : PQ_PRODUCTS)
    if (name.compare(0, strlen(p.code), p.code) == 0) return &p;
  for (auto &p : PQ_PRODUCTS)
    if (name.compare(0, strlen(p.name), p.name) == 0) return &p;
  return nullptr;
}
static std::string upper(std::string s) {
  for (auto &c : s) c = toupper((unsigned char)c);
  return s;
}

// Erkennung wie die App (FilterDev): Herstellerdaten "CBT^" (Version > 1, 2 = ASCII), sonst Name mit Produktcode
// (10 Zeichen) oder Name beginnt mit "VB"/"CBT" (binaer). model = BMS_UNKNOWN: kein Power-Quarry-Akku.
static BmsModel pq_detect(const std::string &raw_name, const std::string &mfr, const PqProduct **prod) {
  *prod = nullptr;
  if (raw_name.empty()) return BMS_UNKNOWN;
  std::string name = upper(raw_name);
  while (!name.empty() && name.back() == ' ') name.pop_back();
  if (mfr.size() >= 6 && mfr.compare(0, 4, "CBT^") == 0 && (uint8_t)mfr[4] > 1) {
    *prod = pq_by_prefix(name);
    return (uint8_t)mfr[4] == 2 ? BMS_PQ_ASCII : BMS_UNKNOWN;   // andere Versionen kennt auch die App nicht
  }
  if (name.size() == 10 && (*prod = pq_by_code(name.substr(0, 2))) != nullptr) return BMS_PQ_HEX;
  if (name.compare(0, 2, "VB") == 0 || name.compare(0, 3, "CBT") == 0) {
    *prod = pq_by_prefix(name);
    return BMS_PQ_HEX;
  }
  return BMS_UNKNOWN;
}

static void pq_rx(const std::string &mac, const uint8_t *d, size_t n) {
  Lock l;
  Bat *b = find_mac(mac);
  if (b && b->rx.size() < 600) b->rx.append((const char *)d, n);
}

static bool pq_write(NimBLERemoteCharacteristic *c, const uint8_t *p, size_t n, bool resp) {
  for (size_t off = 0; off < n; off += 20) {
    size_t k = n - off < 20 ? n - off : 20;
    if (!c->writeValue(p + off, k, resp)) return false;
    delay(30);
  }
  return true;
}

// Diagnose der letzten fehlgeschlagenen Abfrage (fuers Protokoll): gesendet, empfangen (Laenge + Hex/Text)
static std::string s_pq_diag;
static size_t s_pq_rxlen = 0;
static void pq_note(const uint8_t *req, size_t n, bool wrote, const std::string &rx) {
  char b[200];
  int k = snprintf(b, sizeof(b), "gesendet");
  for (size_t i = 0; i < n && i < 8; i++) k += snprintf(b + k, sizeof(b) - k, " %02X", req[i]);
  if (!wrote) {
    snprintf(b + k, sizeof(b) - k, " – Schreiben abgelehnt");
  } else if (rx.empty()) {
    snprintf(b + k, sizeof(b) - k, " – nichts empfangen");
  } else {
    k += snprintf(b + k, sizeof(b) - k, " – empfangen %u B:", (unsigned)rx.size());
    for (size_t i = 0; i < rx.size() && i < 24 && k < (int)sizeof(b) - 4; i++)
      k += snprintf(b + k, sizeof(b) - k, " %02X", (uint8_t)rx[i]);
  }
  s_pq_diag = b;
  s_pq_rxlen = rx.size();
}

// Binaer: AA 55 CMD LEN DATA SUM 0A 0D (Antwort: 0D 0A), SUM = (CMD + LEN + DATA) & 0xFF
static bool pq_hex_frame(const std::string &rx, uint8_t cmd, std::string &data) {
  const uint8_t *b = (const uint8_t *)rx.data();
  size_t n = rx.size();
  for (size_t i = 0; i + 4 <= n; i++) {
    if (b[i] != 0xAA || b[i + 1] != 0x55) continue;
    size_t total = (size_t)b[i + 3] + 7;
    if (i + total > n) return false;
    unsigned sum = 0;
    for (size_t k = 2; k < total - 3; k++) sum += b[i + k];
    // Ende laut App 0A 0D – der Akku (VB037, getestet 2026-11-13) schickt 0D 0A: beides zulassen
    bool tail = (b[i + total - 2] == 0x0A && b[i + total - 1] == 0x0D) || (b[i + total - 2] == 0x0D && b[i + total - 1] == 0x0A);
    if ((sum & 0xFF) == b[i + total - 3] && tail && b[i + 2] == cmd) {
      data.assign((const char *)b + i + 4, b[i + 3]);
      return true;
    }
  }
  return false;
}

// ASCII: "~" … "\r", Pruefsumme und Laenge pruefen, INFO als Bytes
static bool pq_ascii_frame(const std::string &rx, std::string &info) {
  size_t a = rx.find('~');
  if (a == std::string::npos) return false;
  size_t e = rx.find('\r', a);
  if (e == std::string::npos) return false;
  std::string s = rx.substr(a + 1, e - a - 1);
  if (s.size() < 16 || s.size() % 2) return false;
  unsigned sum = 0;
  for (size_t i = 0; i + 4 < s.size(); i++) sum += (unsigned char)s[i];
  std::string b;
  for (size_t i = 0; i < s.size(); i += 2) {
    int h = isxdigit((unsigned char)s[i]) ? (isdigit((unsigned char)s[i]) ? s[i] - '0' : (toupper(s[i]) - 'A' + 10)) : -1;
    int l = isxdigit((unsigned char)s[i + 1]) ? (isdigit((unsigned char)s[i + 1]) ? s[i + 1] - '0' : (toupper(s[i + 1]) - 'A' + 10)) : -1;
    if (h < 0 || l < 0) return false;
    b.push_back((char)(h << 4 | l));
  }
  unsigned cs = (uint8_t)b[b.size() - 2] << 8 | (uint8_t)b[b.size() - 1];
  if (((sum + cs) & 0xFFFF) != 0) return false;
  unsigned lenid = ((uint8_t)b[4] << 8 | (uint8_t)b[5]) & 0xFFF;
  if (lenid != s.size() - 16) return false;
  info = b.substr(6, b.size() - 8);
  return true;
}

// Modbus-RTU (Bulltron): D2 03 LEN DATEN CRClo CRChi, CRC-16/Modbus (0xFFFF, 0xA001)
static uint16_t mb_crc(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1);
  }
  return c;
}
static uint8_t s_mb_addr = 0xD2;   // erwartete Antwortadresse (Daly D2: 0xD2, Daly 0x81: 0x51, Renogy: 0x30; 0 = jede)
static bool mb_frame(const std::string &rx, uint8_t len, std::string &data) {
  const uint8_t *b = (const uint8_t *)rx.data();
  size_t n = rx.size();
  for (size_t i = 0; i + 2 < n; i++) {
    if ((s_mb_addr && b[i] != s_mb_addr) || b[i + 1] != 0x03 || b[i + 2] != len) continue;
    size_t total = (size_t)len + 5;
    if (i + total > n) continue;
    uint16_t c = mb_crc(b + i, total - 2);
    if (b[i + total - 2] == (c & 0xFF) && b[i + total - 1] == (c >> 8)) {
      data.assign((const char *)b + i + 3, len);
      return true;
    }
  }
  return false;
}

// JBD: DD CMD STATUS LEN DATEN SUM(2, BE) 77, SUM = 0x10000 - Summe(STATUS..DATEN)
static bool jbd_frame(const std::string &rx, uint8_t cmd, std::string &data) {
  const uint8_t *b = (const uint8_t *)rx.data();
  size_t n = rx.size();
  for (size_t i = 0; i + 4 <= n; i++) {
    if (b[i] != 0xDD || b[i + 1] != cmd) continue;
    size_t len = b[i + 3], total = len + 7;
    if (i + total > n) return false;
    if (b[i + total - 1] != 0x77 || b[i + 2] != 0) continue;
    unsigned sum = 0;
    for (size_t k = 2; k < 4 + len; k++) sum += b[i + k];
    if (((0x10000 - sum) & 0xFFFF) != (unsigned)(b[i + 4 + len] << 8 | b[i + 5 + len])) continue;
    data.assign((const char *)b + i + 4, len);
    return true;
  }
  return false;
}

// JK02: 55 AA EB 90 TYP …, 300 Byte, Byte 299 = Summe der Bytes 0..298 (unteres Byte).
// TYP 2 = Zelldaten, 3 = Geraeteinfo (Firmware ab Byte 30)
static uint8_t s_jk_type = 0x02;
static bool jk_frame(const std::string &rx, std::string &data) {
  const uint8_t *b = (const uint8_t *)rx.data();
  size_t n = rx.size();
  for (size_t i = 0; i + 300 <= n; i++) {
    if (b[i] != 0x55 || b[i + 1] != 0xAA || b[i + 2] != 0xEB || b[i + 3] != 0x90 || b[i + 4] != s_jk_type) continue;
    uint8_t sum = 0;
    for (size_t k = 0; k < 299; k++) sum += b[i + k];
    if (sum != b[i + 299]) continue;
    data.assign((const char *)b + i, 300);
    return true;
  }
  return false;
}

// ANT: 7E A1 TYP ADR(2) LEN DATEN CRC(2, Modbus ueber TYP..DATEN, LE) AA 55; Antwort auf Status: TYP 0x11
static bool ant_frame(const std::string &rx, std::string &data) {
  const uint8_t *b = (const uint8_t *)rx.data();
  size_t n = rx.size();
  for (size_t i = 0; i + 10 <= n; i++) {
    if (b[i] != 0x7E || b[i + 1] != 0xA1 || b[i + 2] != 0x11) continue;
    size_t len = (size_t)b[i + 5] + 10;
    if (i + len > n) return false;
    if (b[i + len - 2] != 0xAA || b[i + len - 1] != 0x55) continue;
    uint16_t c = mb_crc(b + i + 1, len - 5);
    if (b[i + len - 4] != (c & 0xFF) || b[i + len - 3] != (c >> 8)) continue;
    data.assign((const char *)b + i, len);
    return true;
  }
  return false;
}

// Anfrage senden, auf passende Antwort warten (Puffer gehoert dem Akku, gefuellt per Notify).
// kind: 0 Power Quarry binaer (cmd = Befehl), 1 Power Quarry ASCII, 2 Modbus (cmd = erwartete Datenlaenge),
//       3 JBD (cmd), 4 JK (Zelldaten-Rahmen), 5 LiTime (Antwort ab cmd*2 Byte, dann kurz ruhig)
static bool pq_ask(const std::string &mac, NimBLERemoteCharacteristic *wr, const uint8_t *req, size_t n, int kind,
                   uint8_t cmd, std::string &out, uint32_t timeout_ms) {
  {
    Lock l;
    Bat *b = find_mac(mac);
    if (!b) return false;
    b->rx.clear();
  }
  bool resp = false;
  {
    Lock l;
    Bat *b = find_mac(mac);
    resp = b && b->wr_resp;
  }
  if (!pq_write(wr, req, n, resp)) {
    pq_note(req, n, false, "");
    return false;
  }
  uint32_t t0 = millis();
  while (millis() - t0 < timeout_ms) {
    delay(20);
    Lock l;
    Bat *b = find_mac(mac);
    if (!b) return false;
    bool got = false;
    switch (kind) {
      case 1: got = pq_ascii_frame(b->rx, out); break;
      case 2: got = mb_frame(b->rx, cmd, out); break;
      case 3: got = jbd_frame(b->rx, cmd, out); break;
      case 4: got = jk_frame(b->rx, out); break;
      case 7: got = ant_frame(b->rx, out); break;
      case 5: {   // LiTime: kein Rahmen, feste Laenge – warten bis genug da ist und 150 ms nichts mehr kommt
        static size_t last_n = 0;
        static uint32_t last_ms = 0;
        if (b->rx.size() != last_n) {
          last_n = b->rx.size();
          last_ms = millis();
        } else if (last_n >= (size_t)cmd * 2 && millis() - last_ms > 150) {
          out = b->rx;
          last_n = 0;
          got = true;
        }
        break;
      }
      default: got = pq_hex_frame(b->rx, cmd, out);
    }
    if (got) return true;
  }
  Lock l;
  Bat *b = find_mac(mac);
  pq_note(req, n, true, b ? b->rx : std::string());
  return false;
}

static bool pq_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteService *ws = c->getService(NimBLEUUID((uint16_t)0xFFE5));
  NimBLERemoteService *ns = c->getService(NimBLEUUID((uint16_t)0xFFE0));
  NimBLERemoteCharacteristic *wr = ws ? ws->getCharacteristic(NimBLEUUID((uint16_t)0xFFE9)) : nullptr;
  NimBLERemoteCharacteristic *nt = ns ? ns->getCharacteristic(NimBLEUUID((uint16_t)0xFFE4)) : nullptr;
  if (!wr || !nt) {
    logf("%s: Dienste FFE5/FFE9 bzw. FFE0/FFE4 fehlen", b.name.c_str());
    return false;
  }
  if (!b.has)
    logf("%s: FFE9 %s%s, FFE4 %s%s", b.name.c_str(), wr->canWriteNoResponse() ? "ohne-Antw " : "",
         wr->canWrite() ? "mit-Antw" : "", nt->canNotify() ? "notify " : "", nt->canIndicate() ? "indicate" : "");
  b.wr_resp = !wr->canWriteNoResponse() && wr->canWrite();
  std::string mac = b.mac;
  bool use_notify = nt->canNotify() || !nt->canIndicate();
  if (!nt->subscribe(use_notify, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { pq_rx(mac, d, n); }, true)) {
    logf("%s: Notify nicht abonnierbar", b.name.c_str());
    return false;
  }
  b.wr = wr;
  b.next_poll = millis() + 300;
  if (!b.has) logf("%s: verbunden, Protokoll %s", b.name.c_str(), b.model == BMS_PQ_ASCII ? "ASCII" : "binaer");
  return true;
}

// Bulltron: Dienst 0xFFF0, Antworten 0xFFF1 (Notify), Anfragen 0xFFF2 (die App schreibt mit Bestaetigung und
// abonniert auch dort Notify). Ohne Pairing.
// Gemeinsam fuer JBD/JK/LiTime: Dienst + Notify-/Schreib-Characteristic suchen, Notify abonnieren
static bool generic_connect(Bat &b, NimBLEClient *c, uint16_t svc_id, uint16_t rx_id, uint16_t tx_id, const char *what) {
  NimBLERemoteService *svc = c->getService(NimBLEUUID(svc_id));
  NimBLERemoteCharacteristic *nt = svc ? svc->getCharacteristic(NimBLEUUID(rx_id)) : nullptr;
  NimBLERemoteCharacteristic *wr = svc ? svc->getCharacteristic(NimBLEUUID(tx_id)) : nullptr;
  if (!nt || !wr) return false;
  std::string mac = b.mac;
  if (!nt->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { pq_rx(mac, d, n); }, true)) {
    logf("%s: Notify nicht abonnierbar", b.name.c_str());
    return false;
  }
  b.wr = wr;
  b.wr_resp = !wr->canWriteNoResponse() && wr->canWrite();
  b.next_poll = millis() + 300;
  if (!b.has) logf("%s: verbunden (%s)", b.name.c_str(), what);
  return true;
}

static bool jbd_connect(Bat &b, NimBLEClient *c) {
  if (generic_connect(b, c, 0xFF00, 0xFF01, 0xFF02, "JBD")) return true;
  if (generic_connect(b, c, 0xFFF0, 0xFFF1, 0xFFF2, "JBD, Variante FFF0")) return true;   // z. B. DH04
  logf("%s: JBD-Dienst FF00/FFF0 fehlt", b.name.c_str());
  return false;
}

static bool jk_connect(Bat &b, NimBLEClient *c) {
  if (generic_connect(b, c, 0xFFE0, 0xFFE1, 0xFFE1, "JK02")) return true;
  logf("%s: JK-Dienst FFE0/FFE1 fehlt", b.name.c_str());
  return false;
}

// Renogy: Antworten auf 0xFFF1 (Dienst 0xFFF0), Anfragen auf 0xFFD1 (Dienst 0xFFD0)
static bool renogy_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteService *rs = c->getService(NimBLEUUID((uint16_t)0xFFF0));
  NimBLERemoteService *ts = c->getService(NimBLEUUID((uint16_t)0xFFD0));
  NimBLERemoteCharacteristic *nt = rs ? rs->getCharacteristic(NimBLEUUID((uint16_t)0xFFF1)) : nullptr;
  NimBLERemoteCharacteristic *wr = ts ? ts->getCharacteristic(NimBLEUUID((uint16_t)0xFFD1)) : nullptr;
  if (!nt || !wr) {
    logf("%s: Renogy-Dienste FFF0/FFF1 bzw. FFD0/FFD1 fehlen", b.name.c_str());
    return false;
  }
  std::string mac = b.mac;
  if (!nt->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { pq_rx(mac, d, n); }, true))
    return false;
  b.wr = wr;
  b.wr_resp = !wr->canWriteNoResponse() && wr->canWrite();
  b.next_poll = millis() + 300;
  if (b.model == BMS_RENOGY_INV) {   // Wechselrichter: wie renogy-ble zuerst 0xFFD4 lesen (weckt die Schnittstelle)
    NimBLERemoteCharacteristic *ini = ts->getCharacteristic(NimBLEUUID((uint16_t)0xFFD4));
    delay(300);
    if (ini && ini->canRead()) ini->readValue();
  }
  if (!b.has) logf("%s: verbunden (%s)", b.name.c_str(), bms_model_text(b.model));
  return true;
}

// Renogy Smart Shunt 300: schickt seine Werte von selbst (Notify 0xC411), geschrieben wird nichts
static bool rshunt_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteCharacteristic *nt = nullptr;
  for (auto *s : c->getServices(true)) {
    nt = s->getCharacteristic(NimBLEUUID((uint16_t)0xC411));
    if (nt) break;
  }
  if (!nt) {
    logf("%s: Smart-Shunt-Kanal C411 fehlt", b.name.c_str());
    return false;
  }
  std::string mac = b.mac;
  if (!nt->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { pq_rx(mac, d, n); }, true))
    return false;
  b.wr = nt;   // nur Merker "bereit zur Abfrage"
  b.next_poll = millis() + 300;
  if (!b.has) logf("%s: verbunden (Renogy Smart Shunt)", b.name.c_str());
  return true;
}

#include "bms_ps.inc"   // Power Stations (Bluetti, Allpowers, Anker, EcoFlow)

// Eco-Worthy: jeder Notify ist ein ganzer Rahmen, beginnend mit A1/A2 (Version 1) oder MAC + A1/A2 (Version 2),
// am Ende CRC-16/Modbus (Little Endian). Version 2 wird um 2 Byte vorne aufgefuellt, dann gelten dieselben Positionen.
static void eco_rx(const std::string &mac, const uint8_t *d, size_t n) {
  if (n < 10) return;
  uint16_t c = mb_crc(d, n - 2);
  if (d[n - 2] != (c & 0xFF) || d[n - 1] != (c >> 8)) return;
  uint8_t m[6];
  if (sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) return;
  bool v2 = n > 7 && memcmp(d, m, 6) == 0 && (d[6] == 0xA1 || d[6] == 0xA2);
  bool v1 = d[0] == 0xA1 || d[0] == 0xA2;
  if (!v1 && !v2) return;
  std::string f = v2 ? std::string(2, '\0') + std::string((const char *)d, n) : std::string((const char *)d, n);
  uint8_t typ = v2 ? d[6] : d[0];
  Lock l;
  Bat *b = find_mac(mac);
  if (!b) return;
  (typ == 0xA1 ? b->eco_a1 : b->eco_a2) = f;
}

static bool eco_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteService *svc = c->getService(NimBLEUUID((uint16_t)0xFFF0));
  NimBLERemoteCharacteristic *nt = svc ? svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFF1)) : nullptr;
  NimBLERemoteCharacteristic *wr = svc ? svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFF2)) : nullptr;
  if (!nt || !wr) {
    logf("%s: Eco-Worthy-Dienst FFF0 mit FFF1/FFF2 fehlt", b.name.c_str());
    return false;
  }
  std::string mac = b.mac;
  if (!nt->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { eco_rx(mac, d, n); }, true))
    return false;
  b.wr = wr;
  b.wr_resp = !wr->canWriteNoResponse() && wr->canWrite();
  b.next_poll = millis() + 300;
  if (!b.has) logf("%s: verbunden (Eco-Worthy)", b.name.c_str());
  return true;
}

static bool ant_connect(Bat &b, NimBLEClient *c) {
  if (generic_connect(b, c, 0xFFE0, 0xFFE1, 0xFFE1, "ANT")) return true;
  logf("%s: ANT-Dienst FFE0/FFE1 fehlt", b.name.c_str());
  return false;
}

// Vatrer: Nordic UART (6e400001 / Notify 6e400003 / Schreiben 6e400002)
static bool vatrer_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteService *svc = c->getService(NimBLEUUID("6e400001-b5a3-f393-e0a9-e50e24dcca9e"));
  NimBLERemoteCharacteristic *nt = svc ? svc->getCharacteristic(NimBLEUUID("6e400003-b5a3-f393-e0a9-e50e24dcca9e")) : nullptr;
  NimBLERemoteCharacteristic *wr = svc ? svc->getCharacteristic(NimBLEUUID("6e400002-b5a3-f393-e0a9-e50e24dcca9e")) : nullptr;
  if (!nt || !wr) {
    logf("%s: Vatrer-Dienst (Nordic UART) fehlt", b.name.c_str());
    return false;
  }
  std::string mac = b.mac;
  if (!nt->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { pq_rx(mac, d, n); }, true))
    return false;
  b.wr = wr;
  b.wr_resp = !wr->canWriteNoResponse() && wr->canWrite();
  b.next_poll = millis() + 300;
  if (!b.has) logf("%s: verbunden (Vatrer)", b.name.c_str());
  return true;
}

// SuperSense Tankmodul: Dienst und Kanaele pruefen, Befehlskanal merken (Abfrage in bms_poll_now)
static bool ss_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteService *s = c->getService(SS_SERVICE);
  NimBLERemoteCharacteristic *cmd = s ? s->getCharacteristic(SS_CMD) : nullptr;
  NimBLERemoteCharacteristic *dat = s ? s->getCharacteristic(SS_DATA) : nullptr;
  if (!cmd || !dat) {
    logf("%s: SuperSense-Dienst bzw. Kanaele fehlen – kein Tankmodul?", b.name.c_str());
    return false;
  }
  b.wr = cmd;
  b.wr_resp = true;
  b.next_poll = millis();
  if (b.model_text.empty()) b.model_text = bms_model_text(b.model);
  return true;
}

// 8 Byte BCD -> 4 Werte (9999 = kein Sensor -> -1); false = Laenge falsch oder keine BCD-Ziffern
static bool ss_bcd4(const std::string &d, int out[BMS_TANKS]) {
  if (d.size() != 8) return false;
  for (int i = 0; i < BMS_TANKS; i++) {
    int v = 0;
    for (int k = 0; k < 2; k++) {
      uint8_t x = (uint8_t)d[2 * i + k];
      if ((x >> 4) > 9 || (x & 0x0F) > 9) return false;
      v = v * 100 + (x >> 4) * 10 + (x & 0x0F);
    }
    out[i] = v == 9999 ? -1 : v;
  }
  return true;
}

static bool litime_connect(Bat &b, NimBLEClient *c) {
  if (generic_connect(b, c, 0xFFE0, 0xFFE1, 0xFFE2, "LiTime")) return true;
  logf("%s: LiTime-Dienst FFE0 mit FFE1/FFE2 fehlt", b.name.c_str());
  return false;
}

static bool bt_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteService *svc = c->getService(NimBLEUUID((uint16_t)0xFFF0));
  NimBLERemoteCharacteristic *nt = svc ? svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFF1)) : nullptr;
  NimBLERemoteCharacteristic *wr = svc ? svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFF2)) : nullptr;
  if (!nt || !wr) {
    logf("%s: Dienst FFF0 mit FFF1/FFF2 fehlt", b.name.c_str());
    return false;
  }
  std::string mac = b.mac;
  auto cb = [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { pq_rx(mac, d, n); };
  if (!nt->subscribe(true, cb, true)) {
    logf("%s: Notify auf FFF1 nicht abonnierbar", b.name.c_str());
    return false;
  }
  if (wr->canNotify()) wr->subscribe(true, cb, true);
  b.wr = wr;
  b.wr_resp = wr->canWrite();
  b.next_poll = millis() + 300;
  if (!b.has) logf("%s: verbunden (Modbus)", b.name.c_str());
  return true;
}

static int16_t le16s(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static int16_t be16s(const uint8_t *p) { return (int16_t)(p[0] << 8 | p[1]); }

void ClientCB::onDisconnect(NimBLEClient *c, int reason) {
  Lock l;
  for (auto &b : s_bats)
    if (b.client == c) {
      b.connected = false;
      b.next_try = millis() + RECONNECT_MS;
      b.status = "Verbindung getrennt";
      s_ver++;
      if (reason == 0x205 || reason == 0x206) b.auth_fail = true;   // Kopplung passt nicht (mehr)
      logf("%s: getrennt (Grund %d) nach %lu s, Signal zuletzt %d dBm", b.name.c_str(), reason,
           (unsigned long)((millis() - b.conn_ms) / 1000), b.rssi);
      // Kopplung abgelehnt und keine PIN eingetragen: Akku verlangt vermutlich eine (gilt fuer alle Typen). Ab dem
      // naechsten Versuch PIN-Eingabe erlauben (wants_pin_io) – fragt der Akku dann wirklich, meldet onPassKeyEntry.
      if (b.auth_fail && b.pin.empty()) {
        b.pin_need = true;
        b.status = "Kopplung abgelehnt – braucht der Akku eine PIN?";
      }
    }
}

void ClientCB::onAuthenticationComplete(NimBLEConnInfo &ci) {
  logf("%s: Kopplung %s (verschluesselt %d, authentifiziert %d, gespeichert %d, Schluessel %d Byte)",
       ci.getAddress().toString().c_str(), ci.isEncrypted() ? "ok" : "FEHLGESCHLAGEN", ci.isEncrypted(),
       ci.isAuthenticated(), ci.isBonded(), ci.getSecKeySize());
}
void ClientCB::onPassKeyEntry(NimBLEConnInfo &ci) {
  std::string pin, name;
  {
    Lock l;
    for (auto &b : s_bats)
      if (b.client && b.client->getConnHandle() == ci.getConnHandle()) {
        b.pin_need = true;
        pin = b.pin;
        name = b.name;
        if (pin.empty()) b.status = "Akku verlangt eine Kopplungs-PIN – bitte eintragen";
        s_ver++;
      }
  }
  if (pin.empty()) {
    logf("%s: Akku verlangt eine PIN – bitte beim Akku eintragen (steht auf dem Akku bzw. in der Anleitung)", name.c_str());
    NimBLEDevice::injectPassKey(ci, 0);   // ohne PIN schlaegt die Kopplung fehl
    return;
  }
  bool ok = NimBLEDevice::injectPassKey(ci, (uint32_t)strtoul(pin.c_str(), nullptr, 10));
  logf("%s: gebe Kopplungs-PIN ein (%s)", name.c_str(), ok ? "angenommen" : "vom Bluetooth-Stack abgelehnt");
}
void ClientCB::onConfirmPasskey(NimBLEConnInfo &ci, uint32_t pin) {
  logf("%s: Geraet will Zahlenvergleich %06lu", ci.getAddress().toString().c_str(), (unsigned long)pin);
  NimBLEDevice::injectConfirmPasskey(ci, true);
}

// Waehrend der Verbindung zu einem Akku mit PIN darf NimBLE eine PIN abfragen (onPassKeyEntry); sonst Just Works
struct IoCapGuard {
  bool on;
  explicit IoCapGuard(bool o) : on(o) {
    if (on) NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  }
  ~IoCapGuard() {
    if (on) NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  }
};
// PIN-Eingabe erlauben: Akku mit eingetragener PIN, Super B (Epsilon V2 verlangt immer eine) oder ein Akku, der die
// Kopplung ohne PIN abgelehnt hat (JBD, JK, Daly … mit gesetzter Bluetooth-PIN)
static bool wants_pin_io(const std::string &mac) {
  Lock l;
  Bat *b = find_mac(mac);
  return b && (!b->pin.empty() || b->pin_need || b->model == BMS_SUPERB);
}

// Client vom Akku loesen (unter der Sperre) und erst danach ausserhalb der Sperre trennen/loeschen:
// die Trennmeldung von NimBLE (onDisconnect) braucht dieselbe Sperre.
static NimBLEClient *detach(Bat &b) {
  NimBLEClient *c = b.client;
  b.client = nullptr;
  b.connected = false;
  b.wr = nullptr;
  return c;
}
static void kill(NimBLEClient *c) {
  if (!c) return;
  if (c->isConnected()) c->disconnect();
  NimBLEDevice::deleteClient(c);
}

// Renogy (Merkmale nach IAmTheMitchell/renogy-ble): Name bzw. Hersteller-ID -> Geraeteart
static BmsModel renogy_detect(const std::string &un, uint16_t mid) {
  if (un.rfind("RNGRIU", 0) == 0 || un.rfind("BTRIC", 0) == 0) return BMS_RENOGY_INV;
  if (un.rfind("RTMSHUNT", 0) == 0) return BMS_RENOGY_SHUNT;
  if (un.rfind("RNGPRO", 0) == 0) return BMS_RENOGY;   // Strom in 0,01 A wie der alte Akku (Adresse 0xFF probiert er)
  if (un.rfind("RNGRBP", 0) == 0 || un.rfind("RNGC", 0) == 0 || mid == 0xE14C) return BMS_RENOGY_PRO;
  if (un.rfind("BT-TH-", 0) == 0) return un.find("BATT", 6) != std::string::npos ? BMS_RENOGY : BMS_RENOGY_BT;
  return BMS_UNKNOWN;
}

// ---------------------------------------------------------------- Bluetooth-Ablauf (Scan-Task von truma.cpp)
bool bms_on_adv(const NimBLEAdvertisedDevice *dev) {
  if (!s_disc_until) return false;
  bool svc = dev->isAdvertisingService(NimBLEUUID(SB_SERVICE)) || dev->isAdvertisingService(NimBLEUUID(SB2_SERVICE));
  if (!svc && dev->haveManufacturerData()) {   // Epsilon V2: Hersteller-ID 0x50BE
    std::string md = dev->getManufacturerData();
    svc = md.size() >= 2 && (uint8_t)md[0] == 0xBE && (uint8_t)md[1] == 0x50;
  }
  std::string name = dev->haveName() ? dev->getName() : "";
  std::string ln = lower(name);
  bool hint = false;
  for (const char *h : SB_HINTS) hint |= ln.find(h) != std::string::npos;
  BmsModel model = BMS_SUPERB;
  const PqProduct *prod = nullptr;
  std::string ef_sn;   // EcoFlow: Seriennummer und Verschluesselungsart aus den Werbedaten
  uint8_t ef_enc = 7;
  bool ss = dev->isAdvertisingService(NimBLEUUID(SS_SERVICE)) || lower(name).rfind("ssbt", 0) == 0;
  if (ss) model = BMS_SS_TANK;   // SuperSense Tankmodul (Name "SSBT…" bzw. Dienst)
  if (!svc && !hint && !ss) {
    model = pq_detect(name, dev->haveManufacturerData() ? dev->getManufacturerData() : std::string(), &prod);
    std::string un = name;
    for (auto &ch : un) ch = toupper((unsigned char)ch);
    bool ff00 = dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFF00));
    // eindeutige Merkmale nach aiobmsble zuerst: Hersteller-IDs und Namensanfaenge
    uint16_t mid = 0;
    if (dev->haveManufacturerData()) {
      std::string md = dev->getManufacturerData();
      if (md.size() >= 2) mid = (uint8_t)md[0] | (uint8_t)md[1] << 8;
    }
    if (model == BMS_UNKNOWN && (un.rfind("DL-", 0) == 0 || mid == 0x102 || mid == 0x104 || mid == 0x107 ||
                                 mid == 0x302 || mid == 0x303 || mid == 0x402))
      model = BMS_BULLTRON;   // Daly
    if (model == BMS_UNKNOWN && mid == 0x585A) model = BMS_LITIME;   // LiTime/Redodo
    if (model == BMS_UNKNOWN) model = renogy_detect(un, mid);
    if (model == BMS_UNKNOWN && bluetti_name(un)) model = BMS_BLUETTI;   // vor JBD (gleicher Dienst 0xFF00)
    if (model == BMS_UNKNOWN && un.find("ALLPOWERS") != std::string::npos) model = BMS_ALLPOWERS;
    if (model == BMS_UNKNOWN) model = anker_detect(un, dev->haveManufacturerData() ? dev->getManufacturerData() : std::string());
#if BMS_ECOFLOW_ON
    if (model == BMS_UNKNOWN && mid == 0xB5B5) model = ecoflow_detect(dev->getManufacturerData(), ef_sn, ef_enc);
#endif
    if (model == BMS_UNKNOWN && (un.rfind("ECO-WORTHY", 0) == 0 || un.rfind("DCHOUSE", 0) == 0)) model = BMS_ECOWORTHY;
    if (model == BMS_UNKNOWN && un.rfind("JK", 0) == 0) model = BMS_JK;   // "JK-B2A24S…", "JK_…"
    // ANT: "ANT-BLE24…", "ANT-BLE3…" (Zeichen 4 beliebig)
    if (model == BMS_UNKNOWN && un.size() >= 8 && un.rfind("ANT", 0) == 0 && un.compare(4, 3, "BLE") == 0) model = BMS_ANT;
    // Vatrer: 15 Zeichen JJMMTTVVVAAAAxx, z. B. "2403151280100A1" – Muster [2-9]???[0-3]?512??00?? + Nordic UART
    if (model == BMS_UNKNOWN && un.size() == 15 && un[0] >= '2' && un[0] <= '9' && un[4] >= '0' && un[4] <= '3' &&
        un.compare(6, 3, "512") == 0 && un.compare(11, 2, "00") == 0)
      model = BMS_VATRER;
    // LiTime / Redodo / PowerQueen: "L-12100…", "R-12…", "P-12…"
    if (model == BMS_UNKNOWN && un.size() >= 4 && (un[0] == 'L' || un[0] == 'R' || un[0] == 'P') && un[1] == '-' &&
        isdigit((unsigned char)un[2]) && isdigit((unsigned char)un[3]))
      model = BMS_LITIME;
    // JBD / Xiaoxiang: Dienst 0xFF00 oder typische Namen
    if (model == BMS_UNKNOWN && (ff00 || un.rfind("JBD", 0) == 0 || un.find("XIAOXIANG") != std::string::npos ||
                                 ((un.rfind("SP", 0) == 0 || un.rfind("DP", 0) == 0) && un.size() > 2 &&
                                  isdigit((unsigned char)un[2]))))
      model = BMS_JBD;
    if (model == BMS_UNKNOWN) {
      // Bulltron: "DL"/"JHB" im Namen; in den rohen Werbedaten "JHB" oder "DL" zusammen mit Dienst 0xFFF0
      // (zwei Buchstaben allein treffen zufaellig auch fremde Geraete)
      const std::vector<uint8_t> &pl = dev->getPayload();
      std::string raw(pl.begin(), pl.end());
      bool fff0 = dev->isAdvertisingService(NimBLEUUID((uint16_t)0xFFF0));
      if (name.find("DL") != std::string::npos || name.find("JHB") != std::string::npos ||
          raw.find("JHB") != std::string::npos || (fff0 && raw.find("DL") != std::string::npos))
        model = BMS_BULLTRON;
      else {
        // unbekannt: merken, damit es von Hand einem Typ zugeordnet werden kann (nur verbindbare Geraete mit Name
        // oder mit einem der ueblichen Akku-Dienste)
        std::string svcs;
        for (int i = 0; i < dev->getServiceUUIDCount() && i < 4; i++) {
          std::string u = dev->getServiceUUID(i).toString();
          if (u.rfind("0x", 0) == 0) u = u.substr(2);
          svcs += (svcs.empty() ? "" : " ") + u.substr(0, 8);
        }
        if (!dev->isConnectable() || (name.empty() && svcs.empty())) return false;
        std::string mac = lower(dev->getAddress().toString());
        name = seen_name(name);
        Lock l;
        for (auto &x : s_seen)
          if (x.mac == mac) {
            x.rssi = dev->getRSSI();
            if (!name.empty()) x.name = name;
            return false;
          }
        if (s_seen.size() < 24) {
          BmsSeen x;
          x.mac = mac;
          x.name = name;
          x.svc = svcs;
          x.addr_type = dev->getAddress().getType();
          x.rssi = dev->getRSSI();
          x.mid = mid;
          s_seen.push_back(x);
          s_ver++;
        }
        return false;
      }
    }
  }
  {   // in dieser Firmware nicht eingebaute Typen nicht anbieten
    bool on = true;
    switch (model) {
      case BMS_SUPERB: on = BMS_SUPERB_ON; break;
      case BMS_PQ_HEX:
      case BMS_PQ_ASCII: on = BMS_PQ_ON; break;
      case BMS_BULLTRON: on = BMS_DALY_ON; break;
      case BMS_JBD: on = BMS_JBD_ON; break;
      case BMS_JK: on = BMS_JK_ON; break;
      case BMS_LITIME: on = BMS_LITIME_ON; break;
      case BMS_RENOGY:
      case BMS_RENOGY_PRO: on = BMS_RENOGY_ON; break;
      case BMS_RENOGY_BT: on = BMS_RENOGY_ON || BMS_RENOGY_DEV_ON; break;
      case BMS_RENOGY_SOLAR:
      case BMS_RENOGY_DCC:
      case BMS_RENOGY_INV:
      case BMS_RENOGY_SHUNT: on = BMS_RENOGY_DEV_ON; break;
      case BMS_BLUETTI: on = BMS_BLUETTI_ON; break;
      case BMS_ALLPOWERS: on = BMS_ALLPOWERS_ON; break;
      case BMS_ANKER:
      case BMS_ANKER_C300:
      case BMS_ANKER_G2:
      case BMS_ANKER_F3800: on = BMS_ANKER_ON; break;
      case BMS_ECOFLOW:
      case BMS_ECOFLOW_PB: on = BMS_ECOFLOW_ON; break;
      case BMS_ECOWORTHY: on = BMS_ECOWORTHY_ON; break;
      case BMS_ANT: on = BMS_ANT_ON; break;
      case BMS_VATRER: on = BMS_VATRER_ON; break;
      case BMS_SS_TANK: on = BMS_SUPERSENSE_ON; break;
      default: break;
    }
    if (!on) return false;
  }
  std::string mac = lower(dev->getAddress().toString());
  Lock l;
  for (auto &f : s_found)
    if (f.mac == mac) {
      f.rssi = dev->getRSSI();
      if (!name.empty()) f.name = name;
      f.service |= svc;
      return true;
    }
  if (s_found.size() >= 12) return true;
  BmsFound f;
  f.mac = mac;
  f.name = name;
  f.addr_type = dev->getAddress().getType();
  f.rssi = dev->getRSSI();
  f.model = model;
  f.service = svc;
  f.serial = ef_sn;
  f.enc = ef_enc;
  if (prod) {
    f.product = prod->name;
    f.capacity_ah = prod->ah;
    f.cells = prod->cells;
  }
  f.configured = find_mac(mac) != nullptr;
  s_found.push_back(f);
  s_ver++;
  logf("Gefunden: %s \"%s\" %d dBm, %s%s%s", mac.c_str(), name.c_str(), f.rssi, bms_model_text(model),
       svc ? " (Super-B-Dienst)" : "", model == BMS_PQ_ASCII ? " ASCII" : model == BMS_PQ_HEX ? " binaer" : "");
  if (model == BMS_BULLTRON) f.service = false;
  return true;
}

void bms_scan_wanted(bool &, bool &active) {
  if (!s_disc_until) return;
  if ((int32_t)(millis() - s_disc_until) >= 0) {
    s_disc_until = 0;
    s_ver++;
    return;
  }
  active = true;
}

bool bms_want_connect() {
  std::vector<NimBLEClient *> dead;
  bool want = false;
  {
    Lock l;
    uint32_t now = millis();
    // Entfernte Akkus hier aufraeumen (Clients gehoeren dem Scan-Task)
    for (size_t i = 0; i < s_bats.size();) {
      if (s_bats[i].remove) {
        dead.push_back(detach(s_bats[i]));
        s_bats.erase(s_bats.begin() + i);
        s_ver++;
      } else {
        i++;
      }
    }
    for (auto &b : s_bats) {
      bool stale = b.connected && (b.last_ms ? now - b.last_ms > STALE_MS : now - b.conn_ms > STALE_MS);
      if (stale) {   // verbunden, aber stumm
        logf("%s: keine Werte mehr – verbinde neu", b.name.c_str());
        dead.push_back(detach(b));
        b.next_try = now;
      }
      if (!b.connected && (int32_t)(now - b.next_try) >= 0) want = true;
    }
    if (s_disc_until) want = false;   // waehrend der Suche nicht verbinden
  }
  for (auto *c : dead) kill(c);
  return want;
}

void bms_connect_now() {
  std::string mac;
  uint8_t type = 0;
  NimBLEClient *old = nullptr;
  {
    Lock l;
    uint32_t now = millis();
    for (auto &b : s_bats)
      if (!b.connected && (int32_t)(now - b.next_try) >= 0) {
        mac = b.mac;
        type = b.type;
        b.next_try = now + RETRY_MS;   // bis zum Ergebnis nicht erneut
        set_status(b, "Verbinde …");
        old = detach(b);
        break;
      }
  }
  kill(old);
  if (mac.empty()) return;
  NimBLEClient *c = NimBLEDevice::createClient();
  c->setClientCallbacks(&s_ccb, false);
  c->setConnectTimeout(8000);
  // Dauerverbindung neben WLAN (gemeinsames Funkteil): Takt 30–50 ms, Zeitgrenze 6 s statt knapper Vorgabe
  // (Power Quarry trennte sonst nach ~10 s mit "Grund 520" = Connection Timeout)
  c->setConnectionParams(24, 40, 0, 600);
  {
    Lock l;
    Bat *b = find_mac(mac);
    if (!b) {
      NimBLEDevice::deleteClient(c);
      return;
    }
    b->client = c;
  }
  bool quiet = false;
  {
    Lock l;
    Bat *b = find_mac(mac);
    quiet = b && b->polled() && b->has;   // Kurzverbindungen: nur beim ersten Mal und bei Fehlern ins Protokoll
  }
  bool reset_bond = false;
  {
    Lock l;
    Bat *b = find_mac(mac);
    if (b && b->auth_fail) {
      b->auth_fail = false;
      reset_bond = true;
    }
  }
  NimBLEAddress addr(mac, type);
  bool bonded = NimBLEDevice::isBonded(addr);
  if (reset_bond && bonded) {
    bool del = NimBLEDevice::deleteBond(addr);
    logf("%s: Kopplung passte nicht – gespeicherte Kopplung %s, koppele neu", mac.c_str(), del ? "geloescht" : "nicht loeschbar");
    bonded = false;
  }
  if (!quiet) logf("Verbinde %s …%s", mac.c_str(), bonded ? " (gekoppelt)" : "");
  // Kopplung mit PIN-Eingabe erlauben (Akku mit PIN, Super B); danach wieder "ohne Ein-/Ausgabe"
  IoCapGuard io_guard(wants_pin_io(mac));
  bool ok = c->connect(addr, true);
  if (!ok) {
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) {
        detach(*b);
        b->next_try = millis() + RETRY_MS;
        set_status(*b, "Nicht erreichbar – Akku in Reichweite? App geschlossen?");
      }
    }
    kill(c);
    logf("%s: Verbindung fehlgeschlagen", mac.c_str());
    return;
  }
  Bat copy;
  {
    Lock l;
    Bat *b = find_mac(mac);
    if (!b) return;
    b->connected = true;
    b->conn_ms = millis();
    b->last_ms = 0;
    copy = *b;
  }
  // Akku mit PIN: selbst koppeln und auf das Ergebnis warten, bevor Kanaele abonniert werden. esp-nimble-cpp (P4) meldet
  // eine fehlgeschlagene Kopplung nicht an onAuthenticationComplete – so steht der Fehlercode im Protokoll.
  if (!copy.pin.empty() && !c->getConnInfo().isEncrypted()) {
    bool sec = c->secureConnection();
    NimBLEConnInfo ci = c->getConnInfo();
    if (sec)
      logf("%s: mit PIN gekoppelt (authentifiziert %d, gespeichert %d)", copy.name.c_str(), ci.isAuthenticated(), ci.isBonded());
    else
    {
      int e = c->getLastError();
      logf("%s: Kopplung mit PIN fehlgeschlagen, Fehler %d (%s)%s", copy.name.c_str(), e, NimBLEUtils::returnCodeToString(e),
           e == 0x504 || e == 0x50B ? " – PIN falsch?" : "");
      Lock l;
      if (Bat *bb = find_mac(copy.mac)) bb->status = e == 0x504 || e == 0x50B ? "Kopplung abgelehnt – PIN pruefen" : "Kopplung fehlgeschlagen";
    }
  }
  bool good = false;
  switch (copy.model) {
#if BMS_SUPERB_ON
    case BMS_SUPERB: good = sb_connect(copy, c); break;
#endif
#if BMS_PQ_ON
    case BMS_PQ_HEX:
    case BMS_PQ_ASCII: good = pq_connect(copy, c); break;
#endif
#if BMS_DALY_ON
    case BMS_BULLTRON: good = bt_connect(copy, c); break;
#endif
#if BMS_JBD_ON
    case BMS_JBD: good = jbd_connect(copy, c); break;
#endif
#if BMS_JK_ON
    case BMS_JK: good = jk_connect(copy, c); break;
#endif
#if BMS_LITIME_ON
    case BMS_LITIME: good = litime_connect(copy, c); break;
#endif
#if BMS_RENOGY_ON
    case BMS_RENOGY:
    case BMS_RENOGY_PRO:
#endif
#if BMS_RENOGY_DEV_ON
    case BMS_RENOGY_SOLAR:
    case BMS_RENOGY_DCC:
    case BMS_RENOGY_INV:
#endif
#if BMS_RENOGY_ON || BMS_RENOGY_DEV_ON
    case BMS_RENOGY_BT: good = renogy_connect(copy, c); break;
#endif
#if BMS_RENOGY_DEV_ON
    case BMS_RENOGY_SHUNT: good = rshunt_connect(copy, c); break;
#endif
#if BMS_BLUETTI_ON
    case BMS_BLUETTI: good = bluetti_connect(copy, c); break;
#endif
#if BMS_ALLPOWERS_ON
    case BMS_ALLPOWERS: good = allpowers_connect(copy, c); break;
#endif
#if BMS_ANKER_ON
    case BMS_ANKER:
    case BMS_ANKER_C300:
    case BMS_ANKER_G2:
    case BMS_ANKER_F3800: good = anker_connect(copy, c); break;
#endif
#if BMS_ECOFLOW_ON
    case BMS_ECOFLOW:
    case BMS_ECOFLOW_PB: good = ecoflow_connect(copy, c); break;
#endif
#if BMS_ECOWORTHY_ON
    case BMS_ECOWORTHY: good = eco_connect(copy, c); break;
#endif
#if BMS_ANT_ON
    case BMS_ANT: good = ant_connect(copy, c); break;
#endif
#if BMS_VATRER_ON
    case BMS_VATRER: good = vatrer_connect(copy, c); break;
#endif
#if BMS_SUPERSENSE_ON
    case BMS_SS_TANK: good = ss_connect(copy, c); break;
#endif
    default: logf("%s: Akku-Typ %s ist in dieser Firmware nicht eingebaut", copy.name.c_str(), bms_model_text(copy.model));
  }
  NimBLEClient *dead = nullptr;
  {
    Lock l;
    Bat *b = find_mac(mac);
    if (!b) {
      dead = c;   // waehrenddessen entfernt
    } else {
      b->model_text = copy.model_text;
      b->serial = copy.serial;
      b->firmware = copy.firmware;
      b->wr = copy.wr;
      b->next_poll = copy.next_poll;
      b->wr_resp = copy.wr_resp;
      if (!good || !c->isConnected()) {
        dead = detach(*b);
        b->next_try = millis() + RETRY_MS;
        set_status(*b, "Verbunden, aber keine Werte – siehe Protokoll");
      } else if (!b->has) {
        set_status(*b, "Verbunden, warte auf Werte …");
      }
    }
  }
  kill(dead);
}

// Renogy-Modbus: Register lesen; Adresse 0xFF = Rundruf (das Geraet antwortet mit seiner eigenen Adresse)
static bool renogy_rd(const std::string &mac, NimBLERemoteCharacteristic *wr, uint8_t dev, uint16_t reg, uint16_t cnt,
                      std::string &d, int tries = 2) {
  s_mb_addr = dev == 0xFF ? 0 : dev;
  uint8_t req[8] = {dev, 0x03, (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(cnt >> 8), (uint8_t)cnt, 0, 0};
  uint16_t c = mb_crc(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;
  for (int attempt = 0; attempt < tries; attempt++)
    if (pq_ask(mac, wr, req, 8, 2, (uint8_t)(cnt * 2), d, 2500)) return true;
  return false;
}

static std::string renogy_text(const std::string &d) {   // Modellname aus Registern (Leerzeichen/Nullen weg)
  std::string s;
  for (char ch : d)
    if (ch > 0x20 && ch < 0x7F) s += ch;
    else if (ch == ' ' && !s.empty()) s += ch;
  while (!s.empty() && s.back() == ' ') s.pop_back();
  return s;
}

// Ladephase Renogy -> Victron-Nummerierung (victron_state_text)
static int renogy_state(uint8_t s) {
  switch (s) {
    case 0: return 0;   // aus
    case 1:             // aktiv
    case 2:             // MPPT
    case 6:             // Strombegrenzung
    case 8: return 3;   // Lichtmaschine direkt -> Bulk
    case 3: return 7;   // Ausgleichsladung
    case 4: return 4;   // Boost -> Absorption
    case 5: return 5;   // Erhaltung
    default: return -1;
  }
}

static bool model_unread(const std::string &mac, BmsModel m) {   // Modellname noch nicht aus dem Geraet gelesen
  Lock l;
  Bat *b = find_mac(mac);
  return b && (b->model_text.empty() || b->model_text == bms_model_text(m));
}
static void set_model_text(const std::string &mac, const std::string &t) {
  if (t.empty()) return;
  Lock l;
  Bat *b = find_mac(mac);
  if (b) b->model_text = "Renogy " + t;
}

// BT-1/BT-2 ohne eindeutigen Namen: Modell lesen (Register 12) -> Laderegler bzw. DC-DC-Lader (DCC…/RBC<Zahl>D…);
// keine Antwort, aber Akku-Register 0x13B2 an Adresse 0x30 -> Akku. Ergebnis wird gespeichert.
static BmsModel renogy_resolve(const std::string &mac, NimBLERemoteCharacteristic *wr, std::string &d) {
  BmsModel m = BMS_RENOGY_BT;
  std::string txt;
  if (renogy_rd(mac, wr, 0xFF, 12, 8, d) && d.size() >= 16) {
    txt = renogy_text(d.substr(0, 16));
    std::string u = upper(txt);
    size_t k = 3;
    while (k < u.size() && isdigit((unsigned char)u[k])) k++;
    bool dcc = u.rfind("DCC", 0) == 0 || (u.rfind("RBC", 0) == 0 && k > 3 && k < u.size() && u[k] == 'D');
    m = dcc ? BMS_RENOGY_DCC : BMS_RENOGY_SOLAR;
  } else if (renogy_rd(mac, wr, 0x30, 0x13B2, 7, d, 1)) {
    m = BMS_RENOGY;
  }
  if (m == BMS_RENOGY_BT) return m;
  Lock l;
  Bat *b = find_mac(mac);
  if (b) {
    b->model = m;
    b->model_text = txt.empty() ? std::string(bms_model_text(m)) : "Renogy " + txt;
    save();
    s_ver++;
    logf("%s: erkannt als %s%s%s", b->name.c_str(), bms_model_text(m), txt.empty() ? "" : ", Modell ", txt.c_str());
  }
  return m;
}

bool bms_want_poll() {
  if (!s_mtx) return false;
  Lock l;
  uint32_t now = millis();
  for (auto &b : s_bats)
    if (b.connected && b.wr && (int32_t)(now - b.next_poll) >= 0) return true;
  return false;
}

void bms_poll_now() {
  std::string mac;
  NimBLERemoteCharacteristic *wr = nullptr;
  BmsModel model = BMS_UNKNOWN;
  uint8_t cells = 4;
  bool full = true;   // auch die selten noetigen Werte (Temperatur, Kapazitaet, Zyklen, Alarme)
  NimBLEClient *cl = nullptr;
  {
    Lock l;
    uint32_t now = millis();
    for (auto &b : s_bats)
      if (b.connected && b.wr && (int32_t)(now - b.next_poll) >= 0) {
        mac = b.mac;
        wr = b.wr;
        model = b.model;
        cells = b.cells ? b.cells : 4;
        b.next_poll = now + PQ_POLL_MS;
        full = !b.has || b.polls % 3 == 0;
        b.polls++;
        cl = b.client;
        break;
      }
  }
  if (mac.empty()) return;
  if (model == BMS_SUPERB) {   // Epsilon V2: Dauerverbindung, nur Anfrage senden, Antwort kommt per Notify
    static const uint8_t REQ[3] = {0x21, 0x54, 0x00};
    bool resp;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (!b) return;
      b->next_poll = millis() + SB2_POLL_MS;
      resp = b->wr_resp;
    }
    if (!wr->writeValue(REQ, 3, resp)) logf("Super B: Anfrage nicht gesendet");
    return;
  }
  // Abgefragte Akkus nach der Abfrage trennen (auch bei Fehlern), naechste Verbindung in PQ_POLL_MS
  struct AfterPoll {
    std::string mac;
    ~AfterPoll() {
      NimBLEClient *c = nullptr;
      {
        Lock l;
        Bat *b = find_mac(mac);
        if (!b || !b->polled()) return;
        c = detach(*b);
        b->next_try = millis() + PQ_POLL_MS;
      }
      kill(c);
    }
  } after_poll{mac};
  bool ok = false;
  float soc = NAN, v = NAN, a = NAN, temp = NAN;
  int cyc = -1, cap = 0;
  std::vector<uint16_t> cmv;
  uint32_t alarms = 0;
  bool chg_on = true, dis_on = true;
  int mos = -1;
  std::string d;
  if (cl && cl->isConnected()) {
    int r = cl->getRssi();
    Lock l;
    Bat *b = find_mac(mac);
    if (b && r) b->rssi = r;
  }
  int state = 0;   // Bulltron: 1 laedt, 2 entlaedt
  float rem_ah = NAN;
  Extra ex;   // Renogy-Geraete
#if BMS_RENOGY_ON || BMS_RENOGY_DEV_ON
  if (model == BMS_RENOGY_BT) model = renogy_resolve(mac, wr, d);
#endif
  int ss_raw[BMS_TANKS] = {-1, -1, -1, -1}, ss_traw[BMS_TANKS] = {-1, -1, -1, -1};
  if (false) {
#if BMS_SUPERSENSE_ON
  } else if (model == BMS_SS_TANK) {
    // wie die App: 1 s nach dem Verbinden Init schreiben, 0,5 s warten, Fuellstaende und Temperaturen lesen
    NimBLERemoteService *s = cl && cl->isConnected() ? cl->getService(SS_SERVICE) : nullptr;
    if (s) {
      uint32_t since = 0;
      {
        Lock l;
        Bat *b = find_mac(mac);
        if (b) since = millis() - b->conn_ms;
      }
      if (since < 1000) delay(1000 - since);
      if (!wr->writeValue(SS_INIT, sizeof(SS_INIT), true)) logf("SuperSense: Init nicht bestaetigt");
      delay(500);
      NimBLERemoteCharacteristic *dc = s->getCharacteristic(SS_DATA), *tc = s->getCharacteristic(SS_TEMP);
      auto rd = [](NimBLERemoteCharacteristic *ch) {   // NimBLEAttValue -> std::string
        std::string r;
        if (ch) {
          NimBLEAttValue v = ch->readValue();
          r.assign((const char *)v.data(), v.size());
        }
        return r;
      };
      std::string dv = rd(dc);
      ok = ss_bcd4(dv, ss_raw);
      if (!ok) {
        char hx[40] = "";
        for (size_t k = 0; k < dv.size() && k < 12; k++) snprintf(hx + 3 * k, 4, "%02X ", (uint8_t)dv[k]);
        s_pq_diag = "Fuellstand: " + std::to_string(dv.size()) + " Byte " + hx;
        s_pq_rxlen = dv.size();
      }
      if (ok && tc) ss_bcd4(rd(tc), ss_traw);   // Temperatur ist freiwillig
    } else {
      s_pq_diag = "keine Verbindung";
      s_pq_rxlen = 0;
    }
#endif
#if BMS_JBD_ON
  } else if (model == BMS_JBD) {
    // Grunddaten: DD A5 03 00 FF FD 77 -> V u16/100, I s16/100 (+ laden), Rest u16/100 Ah, Nenn u16/100 Ah, Zyklen,
    // Datum, Balance(2x2), Schutz u16, Version, SoC, FET (Bit0 Laden, Bit1 Entladen), Zellen, NTC, NTC u16 0,1 K
    static const uint8_t REQ[7] = {0xDD, 0xA5, 0x03, 0x00, 0xFF, 0xFD, 0x77};
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
      if (!pq_ask(mac, wr, REQ, 7, 3, 0x03, d, 2000) || d.size() < 23) continue;
      const uint8_t *q = (const uint8_t *)d.data();
      auto be = [&](int o) -> uint16_t { return q[o] << 8 | q[o + 1]; };
      v = be(0) / 100.0f;
      a = (int16_t)be(2) / 100.0f;
      cap = (int)lroundf(be(6) / 100.0f);
      cyc = be(8);
      alarms = be(16);
      soc = q[19];
      chg_on = q[20] & 0x01;
      dis_on = q[20] & 0x02;
      int nt = q[22];
      float sum = 0;
      int cnt = 0;
      for (int t = 0; t < nt && 23 + 2 * t + 1 < (int)d.size(); t++) {
        sum += be(23 + 2 * t) / 10.0f - 273.15f;
        cnt++;
      }
      if (cnt) temp = sum / cnt;
      ok = true;
    }
#endif
#if BMS_JK_ON
  } else if (model == BMS_JK) {
    // JK02: Geraete-Info (0x97) und Zelldaten (0x96) anfordern, dann kommt der 300-Byte-Rahmen Typ 2
    auto jk_req = [&](uint8_t cmd) {
      uint8_t f[20] = {0xAA, 0x55, 0x90, 0xEB, cmd, 0x00};
      uint8_t sum = 0;
      for (int i = 0; i < 19; i++) sum += f[i];
      f[19] = sum;
      return std::string((const char *)f, 20);
    };
    std::string r97 = jk_req(0x97), r96 = jk_req(0x96);
    int sw = -1;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) sw = b->jk_sw;
    }
    // Geraeteinfo (Typ 3): Firmware-Version ab Byte 30 ("11.XW" …) – ab Version 11 Aufbau 32S (wie aiobmsble)
    s_jk_type = 0x03;
    if (pq_ask(mac, wr, (const uint8_t *)r97.data(), 20, 4, 0, d, 3000) && d.size() >= 38) {
      sw = atoi(d.substr(30, 8).c_str());
      Lock l;
      Bat *b = find_mac(mac);
      if (b && b->jk_sw != sw) {
        b->jk_sw = sw;
        logf("%s: JK-Firmware %d, Aufbau %s", b->name.c_str(), sw, sw >= 11 ? "32S" : "24S");
      }
    }
    s_jk_type = 0x02;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
      if (!pq_ask(mac, wr, (const uint8_t *)r96.data(), 20, 4, 0, d, 5000) || d.size() < 300) continue;
      const uint8_t *q = (const uint8_t *)d.data();
      // 24S-/32S-Aufbau: nach Firmware (ab 11 = 32S); ohne Info wie bisher an Byte 287 erkennen
      int o = sw >= 0 ? (sw >= 11 ? 32 : 0) : (q[287] > 0 ? 32 : 0);
      auto u16 = [&](int x) -> uint16_t { return q[x] | q[x + 1] << 8; };
      auto u32 = [&](int x) -> uint32_t { return q[x] | q[x + 1] << 8 | q[x + 2] << 16 | (uint32_t)q[x + 3] << 24; };
      v = u16(118 + o) / 1000.0f;
      a = (int32_t)u32(126 + o) / 1000.0f;
      float t1 = (int16_t)u16(130 + o) / 10.0f, t2 = (int16_t)u16(132 + o) / 10.0f;
      temp = (t1 + t2) / 2;
      soc = q[141 + o];
      cap = (int)lroundf(u32(146 + o) / 1000.0f);
      cyc = (int)u32(150 + o);
      chg_on = q[166 + o];
      dis_on = q[167 + o];
      alarms = o ? (u32(166) >> 16) : (u32(134) & 0xFFFF);   // wie aiobmsble
      ok = true;
    }
#endif
#if BMS_LITIME_ON
  } else if (model == BMS_LITIME) {
    // LiTime: 00 00 04 01 13 55 AA 17 -> Antwort (Little Endian): Gesamt-mV u32 @8, Strom mA i32 @48, Zelltemp i16 @52,
    // Rest u16/100 Ah @62, Voll u16/100 Ah @64, Schutz u32 @76, Zustand u16 @88, SoC u16 @90, Entladungen u32 @96
    static const uint8_t REQ[8] = {0x00, 0x00, 0x04, 0x01, 0x13, 0x55, 0xAA, 0x17};
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
      if (!pq_ask(mac, wr, REQ, 8, 5, 52, d, 3000) || d.size() < 100) continue;   // 52*2 = 104 Byte
      const uint8_t *q = (const uint8_t *)d.data();
      auto u16 = [&](int x) -> uint16_t { return q[x] | q[x + 1] << 8; };
      auto u32 = [&](int x) -> uint32_t { return q[x] | q[x + 1] << 8 | q[x + 2] << 16 | (uint32_t)q[x + 3] << 24; };
      v = u32(8) / 1000.0f;
      a = (int32_t)u32(48) / 1000.0f;
      temp = (int16_t)u16(52);
      cap = (int)lroundf(u16(64) / 100.0f);
      uint32_t prot = u32(76);
      uint16_t st = u16(88);
      soc = u16(90);
      cyc = (int)u32(96);
      chg_on = st != 4;
      dis_on = !(u32(68) == 0x80 || prot == 0x20 || prot == 0x80);
      alarms = prot;
      ok = true;
    }
#endif
#if BMS_DALY_ON
  } else if (model == BMS_BULLTRON) {
    int8_t x81 = -1;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) x81 = b->daly_x81;
    }
    uint8_t dev = 0xD2;
    auto rd = [&](uint16_t reg, uint16_t cnt) -> bool {
      s_mb_addr = dev == 0x81 ? 0x51 : dev;   // Daly 0x81 antwortet mit 0x51
      uint8_t req[8] = {dev, 0x03, (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(cnt >> 8), (uint8_t)cnt, 0, 0};
      uint16_t c = mb_crc(req, 6);
      req[6] = c & 0xFF;
      req[7] = c >> 8;
      for (int attempt = 0; attempt < 2; attempt++)
        if (pq_ask(mac, wr, req, 8, 2, (uint8_t)(cnt * 2), d, 2500)) return true;
      return false;
    };
    int nominal = 0;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) nominal = b->cap_ah;
    }
    if (x81 != 1 && nominal <= 0 && rd(0x0080, 41) && d.size() >= 2)   // Einstellungen: S0 = Nennkapazitaet x 0,1 Ah
      cap = (int)lroundf(((uint8_t)d[0] << 8 | (uint8_t)d[1]) / 10.0f);
    bool d2ok = x81 != 1 && rd(0x0000, 62) && d.size() >= 124;   // Laufdaten D2
    if (!d2ok && x81 != 0) {
      // Daly-Variante 0x81 (Antwort 0x51): 64 Register ab 0 und 62 ab 0x41 (Positionen nach aiobmsble)
      dev = 0x81;
      std::string m64;
      if (rd(0x0000, 64) && d.size() >= 128) {
        m64 = d;
        if (rd(0x0041, 62) && d.size() >= 124) {
          auto r64 = [&](int x) -> uint16_t { return (uint8_t)m64[x] << 8 | (uint8_t)m64[x + 1]; };
          auto r62 = [&](int x) -> uint16_t { return (uint8_t)d[x] << 8 | (uint8_t)d[x + 1]; };
          v = r64(112) / 10.0f;
          a = ((int)r64(114) - 30000) / 10.0f;
          soc = r64(116) / 10.0f;
          int nt = r64(122) > 8 ? 8 : r64(122), sum = 0;
          for (int i = 0; i < nt; i++) sum += (int)r64(96 + 2 * i) - 40;
          if (nt) temp = (float)sum / nt;
          cyc = r62(22);
          alarms = (uint32_t)r62(88) << 16 | r62(90);
          chg_on = r62(34);
          dis_on = r62(36);
          state = a > 0.05f ? 1 : a < -0.05f ? 2 : 0;
          ok = true;
          if (x81 != 1) {
            Lock l;
            Bat *b = find_mac(mac);
            if (b) b->daly_x81 = 1;
            logf("%s: Daly-Protokoll 0x81", mac.c_str());
          }
        }
      }
    } else if (d2ok) {   // Laufdaten D2, 62 Register Big Endian
      if (x81 == -1) {
        Lock l;
        Bat *b = find_mac(mac);
        if (b) b->daly_x81 = 0;
      }
      auto r = [&](int i) -> uint16_t { return (uint8_t)d[2 * i] << 8 | (uint8_t)d[2 * i + 1]; };
      for (int i = 0; i < 32; i++)
        if (r(i)) cmv.push_back(r(i));
      int nt = r(50) > 8 ? 8 : r(50), sum = 0;
      for (int i = 0; i < nt; i++) sum += (int)r(32 + i) - 40;
      if (nt) temp = (float)sum / nt;
      v = r(40) * 0.1f;
      a = ((int)r(41) - 30000) * 0.1f;
      soc = r(42) / 10.0f;
      state = r(47);
      rem_ah = r(48) * 0.1f;
      cyc = r(51);
      chg_on = r(53) == 1;
      dis_on = r(54) == 1;
      alarms = (r(58) | r(59) | r(60) | r(61)) ? (uint32_t)(r(58) | r(59) << 16) | 1u : 0;
      ok = true;
    }
#endif
#if BMS_RENOGY_ON
  } else if (model == BMS_RENOGY || model == BMS_RENOGY_PRO) {
    // Renogy-Akku (Modbus): alt an Adresse 0x30, "Pro" an 0xFF (sonst die andere probieren). 0x13B2 x7 = Strom/Spannung/
    // Ladung/Kapazitaet/Zyklen, 0x1388 x34 = Zellen/Temperaturen, 0x13EC x8 = Alarme und Schalter (Positionen nach
    // aiobmsble bzw. renogy-ble, ohne den 3-Byte-Kopf)
    uint8_t rid = 0;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) rid = b->rid;
    }
    uint8_t dev = rid ? rid : (model == BMS_RENOGY_PRO ? 0xFF : 0x30);
    auto rd = [&](uint16_t reg, uint16_t cnt) -> bool { return renogy_rd(mac, wr, dev, reg, cnt, d); };
    auto be16 = [&](int x) -> uint16_t { return (uint8_t)d[x] << 8 | (uint8_t)d[x + 1]; };
    auto be32 = [&](int x) -> uint32_t { return (uint32_t)be16(x) << 16 | be16(x + 2); };
    bool got = rd(0x13B2, 7) && d.size() >= 14;
    if (!got && !rid) {
      dev = dev == 0x30 ? 0xFF : 0x30;
      got = rd(0x13B2, 7) && d.size() >= 14;
    }
    if (got && rid != dev) {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) b->rid = dev;
    }
    if (got) {
      a = (int16_t)be16(0) / (model == BMS_RENOGY_PRO ? 10.0f : 100.0f);
      v = be16(2) / 10.0f;
      float ah = be32(4) / 1000.0f, full = be32(8) / 1000.0f;
      cap = (int)lroundf(full);
      cyc = be16(12);
      if (full > 0) soc = ah / full * 100.0f;
      ok = !isnan(soc);
      if (rd(0x1388, 0x22) && d.size() >= 38 && be16(34) > 0) temp = (int16_t)be16(36) / 10.0f;
      if (rd(0x13EC, 8) && d.size() >= 14) {
        uint8_t st = (uint8_t)d[13];
        chg_on = st & 0x02;
        dis_on = st & 0x04;
        alarms = 0;
        for (int i = 0; i < 13; i++) alarms |= (uint8_t)d[i];
        alarms |= st & ~0x0E & 0xFF;
      }
    }
#endif
#if BMS_RENOGY_DEV_ON
  } else if (model == BMS_RENOGY_SOLAR || model == BMS_RENOGY_DCC) {
    // Laderegler (Rover/Wanderer/Adventurer) und DC-DC-Lader (DCC/RBC) am BT-1/BT-2, Rundruf-Adresse 0xFF (Einzelgeraet).
    // 0x0100 x34: SoC, Batterie V/A, Temperaturen, Last bzw. Lichtmaschine, Solar, Tageswerte, 0x0120 Ladephase
    // (nach cyrils/renogy-bt und renogy-ble). Nur lesen.
    bool dcc = model == BMS_RENOGY_DCC;
    if (model_unread(mac, model) && renogy_rd(mac, wr, 0xFF, 12, 8, d, 1) && d.size() >= 16)
      set_model_text(mac, renogy_text(d.substr(0, 16)));
    bool got = renogy_rd(mac, wr, 0xFF, 0x0100, 34, d) && d.size() >= 68;
    if (!got) got = renogy_rd(mac, wr, 0xFF, 0x0100, 30, d, 1) && d.size() >= 60;   // aeltere Geraete: ohne Ladephase
    if (got) {
      auto be = [&](int x) -> uint16_t { return (uint8_t)d[x] << 8 | (uint8_t)d[x + 1]; };
      auto tc = [](uint8_t r) -> float { return r & 0x80 ? -(float)(r & 0x7F) : (float)r; };   // Bit 7 = minus
      soc = be(0);
      v = be(2) / 10.0f;
      a = be(4) / 100.0f;
      temp = tc((uint8_t)d[7]);   // Batterie (d[6] = Regler)
      if (dcc) {
        ex.in_v = be(8) / 10.0f;
        ex.in_a = be(10) / 100.0f;
        ex.in_w = be(12);
      } else {
        ex.load_a = be(10) / 100.0f;
        ex.load_w = be(12);
      }
      ex.pv_v = be(14) / 10.0f;
      ex.pv_w = be(18);
      ex.yield_wh = be(38);
      if (d.size() >= 66) ex.state = renogy_state((uint8_t)d[65]);
      ok = true;
    }
  } else if (model == BMS_RENOGY_INV) {
    // Wechselrichter (RNGRIU…/BTRIC…): Adresse 0x20 (sonst 0xFF). 4000 x10 Netz/Ausgang/Batterie/Temperatur, 4408 x6 Last,
    // 4327 x7 Laden/Solar (nach renogy-ble). Nur lesen.
    uint8_t rid = 0;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) rid = b->rid;
    }
    uint8_t dev = rid ? rid : 0x20;
    bool got = renogy_rd(mac, wr, dev, 4000, 10, d) && d.size() >= 20;
    if (!got && !rid) {
      dev = 0xFF;
      got = renogy_rd(mac, wr, dev, 4000, 10, d) && d.size() >= 20;
    }
    if (got && rid != dev) {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) b->rid = dev;
    }
    if (got) {
      auto be = [&](int x) -> uint16_t { return (uint8_t)d[x] << 8 | (uint8_t)d[x + 1]; };
      ex.grid_v = be(0) / 10.0f;
      ex.ac_v = be(4) / 10.0f;
      ex.ac_a = be(6) / 100.0f;
      ex.ac_hz = be(8) / 100.0f;
      v = be(10) / 10.0f;
      temp = be(12) / 10.0f;
      ok = true;
      if (renogy_rd(mac, wr, dev, 4408, 6, d, 1) && d.size() >= 6) ex.ac_w = be(2);
      if (renogy_rd(mac, wr, dev, 4327, 7, d, 1) && d.size() >= 14) {
        soc = be(0);
        a = (int16_t)be(2) / 10.0f;
        ex.pv_v = be(4) / 10.0f;
        ex.pv_w = be(8);
      }
      ex.state = ex.grid_v > 50 ? 8 : ex.ac_v > 50 ? 9 : 0;   // Durchleitung, Wechselrichter, aus
      if (model_unread(mac, model) && renogy_rd(mac, wr, dev, 4311, 8, d, 1) && d.size() >= 16)
        set_model_text(mac, renogy_text(d.substr(0, 16)));
    }
  } else if (model == BMS_RENOGY_SHUNT) {
    // Smart Shunt 300: Werte kommen von selbst (Notify), Rahmen ab 42 57 01 19 (~110 Byte, ohne Pruefsumme): Strom s24 @21
    // mA, Spannung u24 @25 mV, Starterbatterie u16 @30 mV, SoC u16 @34 x0,1 %, Temperatur u16 @66 x0,1 °C (nach
    // renogy-ble, dort auch die Plausibilitaetsgrenzen)
    static const char HDR[4] = {0x42, 0x57, 0x01, 0x19};
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) b->rx.clear();
    }
    std::string f;
    for (uint32_t t0 = millis(); millis() - t0 < 5000 && f.empty();) {
      delay(50);
      Lock l;
      Bat *b = find_mac(mac);
      if (!b) break;
      size_t at = b->rx.find(std::string(HDR, 4));
      if (at != std::string::npos && b->rx.size() >= at + 68) f = b->rx.substr(at, 68);
      else if (b->rx.size() > 400) b->rx.erase(0, 200);
    }
    if (f.size() >= 68) {
      auto u = [&](int x, int n) -> uint32_t {
        uint32_t r = 0;
        for (int i = 0; i < n; i++) r = r << 8 | (uint8_t)f[x + i];
        return r;
      };
      int32_t ma = (int32_t)(u(21, 3) << 8) >> 8;   // 24 Bit mit Vorzeichen
      float sv = u(25, 3) / 1000.0f, sa = ma / 1000.0f;
      if (sv >= 6 && sv <= 80 && fabsf(sa) <= 500 && fabsf(sv * sa) <= 10000) {
        v = sv;
        a = sa;
        ex.starter_v = u(30, 2) / 1000.0f;
        float s = u(34, 2) / 10.0f;
        if (s <= 100) soc = s;
        float t = u(66, 2) / 10.0f;
        if (t >= -40 && t <= 100) temp = t;
        ok = true;
      }
    }
    if (!ok) pq_note(nullptr, 0, true, f);
#endif
#if BMS_BLUETTI_ON
  } else if (model == BMS_BLUETTI) {
    // Bluetti (bms_ps.inc): Schluesseltausch falls noetig, Generation erkennen, Leistungen und Schalter lesen
    int8_t enc = -1, wac = -1, wdc = -1;
    uint8_t iot = 0;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) {
        enc = b->bl_enc;
        iot = b->rid;
        wac = b->want_ac;
        wdc = b->want_dc;
      }
    }
    BlSess bs;
    if (bl_handshake(mac, wr, bs, enc)) {
      if (enc < 0) {
        logf(bs.enc ? "%s: Bluetti verschluesselt, Schluesseltausch ok" : "%s: Bluetti ohne Verschluesselung", mac.c_str());
        Lock l;
        Bat *b = find_mac(mac);
        if (b) b->bl_enc = bs.enc ? 1 : 0;
      }
      auto be = [&](int x) -> uint16_t { return (uint8_t)d[x] << 8 | (uint8_t)d[x + 1]; };
      std::string type;
      bool got = false;
      if (iot != 1 && bl_ask(mac, wr, bs, 0x03, 100, 48, d) && d.size() >= 96) {   // Generation 2: Register 100..147
        type = bl_text(d.substr(20, 12), true);
        if (iot == 2 || bluetti_type_ok(type)) {
          iot = 2;
          got = true;
          soc = be(4);
          ex.ps_dc_out = be(80);
          ex.ps_ac_out = be(84);
          ex.ps_dc_in = be(88);
          ex.ps_ac_in = be(92);
        }
      }
      if (!got && iot != 2 && bl_ask(mac, wr, bs, 0x03, 10, 34, d) && d.size() >= 68) {   // Generation 1: 10..43
        type = bl_text(d.substr(0, 12), false);
        if (iot == 1 || bluetti_type_ok(type)) {
          iot = 1;
          got = true;
          soc = be(66);
          ex.ps_dc_in = be(52);
          ex.ps_ac_in = be(54);
          ex.ps_ac_out = be(56);
          ex.ps_dc_out = be(58);
        }
      }
      if (got) {
        if (soc > 100) soc = NAN;
        uint16_t sw = iot == 2 ? 2011 : 3007;   // AC-Ausgang, danach DC-Ausgang
        if (wac >= 0) bl_ask(mac, wr, bs, 0x06, sw, wac ? 1 : 0, d);
        if (wdc >= 0) bl_ask(mac, wr, bs, 0x06, sw + 1, wdc ? 1 : 0, d);
        if (bl_ask(mac, wr, bs, 0x03, sw, 2, d) && d.size() >= 4) {
          ex.ac_on = be(0) != 0;
          ex.dc_on = be(2) != 0;
        } else {
          if (wac >= 0) ex.ac_on = wac;
          if (wdc >= 0) ex.dc_on = wdc;
        }
        ok = true;
        Lock l;
        Bat *b = find_mac(mac);
        if (b) {
          if (wac >= 0 || wdc >= 0) logf("%s: Ausgang geschaltet (AC %d, DC %d)", b->name.c_str(), wac, wdc);
          if (b->want_ac == wac) b->want_ac = -1;
          if (b->want_dc == wdc) b->want_dc = -1;
          if (b->rid != iot) logf("%s: Bluetti %s, Register-Generation %u", b->name.c_str(), type.c_str(), iot);
          b->rid = iot;
          if (!type.empty()) b->model_text = "Bluetti " + type;
        }
      }
    }
#endif
#if BMS_ALLPOWERS_ON
  } else if (model == BMS_ALLPOWERS) {
    // Allpowers (bms_ps.inc): Datensatz abwarten, ggf. schalten
    int8_t wac = -1, wdc = -1;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) {
        b->ap_frame.clear();
        wac = b->want_ac;
        wdc = b->want_dc;
      }
    }
    std::string f;
    for (uint32_t t0 = millis(); millis() - t0 < 6000 && f.empty();) {
      delay(50);
      Lock l;
      Bat *b = find_mac(mac);
      if (!b) break;
      f = b->ap_frame;
    }
    if (f.size() >= 15) {
      const uint8_t *q = (const uint8_t *)f.data();
      bool dc = q[7] & 0x01, ac = q[7] & 0x02, light = q[7] & 0x10;
      if (wac >= 0 || wdc >= 0) {
        if (wac >= 0) ac = wac;
        if (wdc >= 0) dc = wdc;
        ap_switch(wr, ac, dc, light);
        Lock l;
        Bat *b = find_mac(mac);
        if (b) {
          logf("%s: Ausgang geschaltet (AC %d, DC %d)", b->name.c_str(), wac, wdc);
          if (b->want_ac == wac) b->want_ac = -1;
          if (b->want_dc == wdc) b->want_dc = -1;
        }
      }
      soc = q[8] <= 100 ? q[8] : NAN;
      ex.ps_in = q[9] << 8 | q[10];
      ex.ps_out = q[11] << 8 | q[12];
      ex.remain = q[13] << 8 | q[14];
      ex.ac_on = ac;
      ex.dc_on = dc;
      ok = true;
    } else {
      pq_note(nullptr, 0, true, f);
    }
#endif
#if BMS_ANKER_ON
  } else if (is_anker(model)) {
    // Anker SOLIX (bms_ps.inc): Dauerverbindung – beim ersten Mal anmelden, dann schalten, Werte anfordern und auswerten
    AkSess s;
    int8_t wac = -1, wdc = -1;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) {
        s = b->ak;
        wac = b->want_ac;
        wdc = b->want_dc;
      }
    }
    if (!s.authorized) {
      bool okn = ak_negotiate(mac, wr, s);
      Lock l;
      Bat *b = find_mac(mac);
      if (b) {
        b->ak = s;
        if (okn) {
          b->ak_outer = s.enc_outer ? 1 : 0;
          logf("%s: Anker angemeldet (%s)", b->name.c_str(), s.enc_outer ? "verschluesselt" : "offen");
        } else if (!s.replied) {
          b->ak_outer = s.enc_outer ? 0 : 1;   // abgelehnt: beim naechsten Verbinden die andere Huelle
        }
      }
    }
    if (s.authorized) {
      const AkLayout &L = ak_layout(model);
      if (wac >= 0 && L.cmd_ac) ak_command(mac, wr, s, L.cmd_ac, wac);
      if (wdc >= 0 && L.cmd_dc) ak_command(mac, wr, s, L.cmd_dc, wdc);
      if (L.cmd_sub) ak_command(mac, wr, s, L.cmd_sub, -1);   // Werte anfordern bzw. abonnieren
      ok = ak_telemetry(mac, s, model, soc, temp, ex, 8000);
      if (ok && (wac >= 0 || wdc >= 0)) {
        Lock l;
        Bat *b = find_mac(mac);
        if (b) {
          logf("%s: Ausgang geschaltet (AC %d, DC %d)", b->name.c_str(), wac, wdc);
          if (b->want_ac == wac) b->want_ac = -1;
          if (b->want_dc == wdc) b->want_dc = -1;
        }
      }
    }
    if (!ok) pq_note(nullptr, 0, true, s.authorized ? "x" : "");
#endif
#if BMS_ECOFLOW_ON
  } else if (is_ecoflow(model)) {
    // EcoFlow (bms_ps.inc): Dauerverbindung – beim ersten Mal anmelden, dann schalten und Meldungen auswerten
    EfSess s;
    std::string sn;
    int8_t wac = -1, wdc = -1;
    Extra acc;
    float esoc = NAN, etemp = NAN, evolt = NAN;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) {
        s = b->ef;
        sn = b->serial;
        wac = b->want_ac;
        wdc = b->want_dc;
        acc = b->ef_ex;
        esoc = b->ef_soc;
        etemp = b->ef_temp;
        evolt = b->ef_volt;
      }
    }
    bool fresh = false;
    if (s_ef_uid.empty() || sn.size() < 8) {
      static uint32_t last_hint = 0;
      if (!last_hint || millis() - last_hint > 300000) {
        last_hint = millis();
        logf("%s: %s", mac.c_str(), sn.size() < 8 ? "EcoFlow-Seriennummer fehlt – bitte ueber die Suche neu hinzufuegen"
                                                   : "EcoFlow-Benutzer-ID fehlt – im Editor (Power Stations) anmelden");
      }
    } else {
      if (!s.authed) {
        bool oka = ef_auth(mac, wr, s, model, sn, acc, esoc, etemp, evolt);
        if (oka) logf("%s: EcoFlow angemeldet", mac.c_str());
        fresh = oka && !isnan(esoc);
      }
      if (s.authed) {
        if (wac >= 0) ef_switch(wr, s, model, sn, true, wac);
        if (wdc >= 0) ef_switch(wr, s, model, sn, false, wdc);
        std::vector<EfPkt> pk;
        for (uint32_t t0 = millis(); millis() - t0 < 5000;) {
          pk.clear();
          ef_drain(mac, s, model == BMS_ECOFLOW_PB, nullptr, pk);
          for (auto &p : pk) fresh |= ef_values(model, p, acc, esoc, etemp, evolt);
          if (fresh && millis() - t0 > 1500) break;
          delay(100);
        }
        if (wac >= 0) acc.ac_on = wac;
        if (wdc >= 0) acc.dc_on = wdc;
      }
    }
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) {
        b->ef = s;
        b->ef_ex = acc;
        b->ef_soc = esoc;
        b->ef_temp = etemp;
        b->ef_volt = evolt;
        if (s.authed && (wac >= 0 || wdc >= 0)) {
          logf("%s: Ausgang geschaltet (AC %d, DC %d)", b->name.c_str(), wac, wdc);
          if (b->want_ac == wac) b->want_ac = -1;
          if (b->want_dc == wdc) b->want_dc = -1;
        }
      }
    }
    if (fresh && !isnan(esoc)) {
      ok = true;
      soc = esoc;
      temp = etemp;
      v = evolt;
      ex = acc;
    } else if (s.authed) {
      pq_note(nullptr, 0, true, "x");
    }
#endif
#if BMS_ECOWORTHY_ON
  } else if (model == BMS_ECOWORTHY) {
    // Eco-Worthy: zwei Startbefehle, danach schickt der Akku die Rahmen A1 (Werte) und A2 (Zellen, Temperaturen)
    static const uint8_t INIT1[18] = {0xFF, 0x08, 0x02, 0x00, 0x0B, 0x01, 0x00, 0x64, 0x01, 0xFF,
                                      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x2D};
    static const uint8_t INIT2[18] = {0xFF, 0x08, 0x02, 0x00, 0x0B, 0x01, 0x00, 0x14, 0x01, 0xFF,
                                      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x65, 0xEF};
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) b->eco_a1.clear(), b->eco_a2.clear();
    }
    pq_write(wr, INIT1, 18, false);
    delay(150);
    pq_write(wr, INIT2, 18, false);
    std::string f1, f2;
    for (uint32_t t0 = millis(); millis() - t0 < 4000 && (f1.empty() || f2.empty());) {
      delay(50);
      Lock l;
      Bat *b = find_mac(mac);
      if (!b) break;
      f1 = b->eco_a1;
      f2 = b->eco_a2;
    }
    if (f1.size() >= 53) {
      const uint8_t *q = (const uint8_t *)f1.data();
      auto be = [&](int x) -> uint16_t { return q[x] << 8 | q[x + 1]; };
      bool v1 = q[0] == 0xA1;   // Version 1: Strom in 0,01 A, Version 2: 0,1 A
      soc = be(16);
      v = be(20) / 100.0f;
      a = (int16_t)be(22) / (v1 ? 100.0f : 10.0f);
      cap = be(26) / 100;
      alarms = be(51);
      ok = true;
      if (f2.size() >= 84) {
        const uint8_t *t = (const uint8_t *)f2.data();
        int nt = t[80] << 8 | t[81];
        if (nt > 0 && f2.size() >= 84) temp = (int16_t)(t[82] << 8 | t[83]) / 10.0f;
      }
    } else {
      pq_note(INIT2, 18, true, f1.empty() ? std::string() : f1);
    }
#endif
#if BMS_ANT_ON
  } else if (model == BMS_ANT) {
    // ANT: Status anfordern (7E A1 01 0000 BE CRC AA 55), Antwort Typ 0x11; Felder hinter Zellen und Temperaturen
    uint8_t req[10] = {0x7E, 0xA1, 0x01, 0x00, 0x00, 0xBE, 0, 0, 0xAA, 0x55};
    uint16_t c = mb_crc(req + 1, 5);
    req[6] = c & 0xFF;
    req[7] = c >> 8;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
      if (!pq_ask(mac, wr, req, 10, 7, 0, d, 3000) || d.size() < 90) continue;
      const uint8_t *q = (const uint8_t *)d.data();
      int cells = q[9] > 32 ? 32 : q[9], tc = q[8] > 6 ? 6 : q[8];
      int sh = (tc + cells) * 2;
      if ((size_t)(62 + sh) > d.size()) continue;
      auto u16 = [&](int x) -> uint16_t { return q[x] | q[x + 1] << 8; };
      auto u32 = [&](int x) -> uint32_t { return q[x] | q[x + 1] << 8 | q[x + 2] << 16 | (uint32_t)q[x + 3] << 24; };
      v = u16(38 + sh) / 100.0f;
      a = (int16_t)u16(40 + sh) / 10.0f;
      soc = u16(42 + sh);
      uint16_t pr = u16(46 + sh);
      alarms = (((pr >> 8) != 0x1 && (pr >> 8) != 0x4 && (pr >> 8) != 0xF) ? (pr & 0xF00) : 0) |
               (((pr & 0xF) != 0x1 && (pr & 0xF) != 0x4 && (pr & 0xF) != 0xB && (pr & 0xF) != 0xF) ? (pr & 0xF) : 0);
      chg_on = q[46 + sh] == 0x1;
      dis_on = q[47 + sh] == 0x1;
      cap = (int)(u32(50 + sh) / 1000000UL);
      if (tc > 0) {
        float sum = 0;
        for (int t = 0; t < tc; t++) sum += (int16_t)u16(34 + cells * 2 + 2 * t);
        temp = sum / tc;
      }
      ok = true;
    }
#endif
#if BMS_VATRER_ON
  } else if (model == BMS_VATRER) {
    // Vatrer (Modbus, Geraet 0x02): 0x00 x20 = Werte, 0x34 x18 = Temperaturen/Schalter/Alarme (Positionen ohne Kopf)
    auto rd = [&](uint16_t reg, uint16_t cnt) -> bool {
      s_mb_addr = 0x02;
      uint8_t req[8] = {0x02, 0x03, (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(cnt >> 8), (uint8_t)cnt, 0, 0};
      uint16_t c = mb_crc(req, 6);
      req[6] = c & 0xFF;
      req[7] = c >> 8;
      for (int attempt = 0; attempt < 2; attempt++)
        if (pq_ask(mac, wr, req, 8, 2, (uint8_t)(cnt * 2), d, 2500)) return true;
      return false;
    };
    auto be16 = [&](int x) -> uint16_t { return (uint8_t)d[x] << 8 | (uint8_t)d[x + 1]; };
    if (rd(0x0000, 0x14) && d.size() >= 16) {
      v = be16(0) / 100.0f;
      a = (int32_t)((uint32_t)be16(2) << 16 | be16(4)) / 100.0f;
      soc = be16(6);
      cyc = be16(12);
      ok = true;
      if (rd(0x0034, 0x12) && d.size() >= 30) {
        int tc = be16(0) > 6 ? 6 : be16(0);
        if (tc > 0) temp = (int16_t)be16(2);
        alarms = 0;
        for (int i = 14; i < 29; i++) alarms |= (uint8_t)d[i];
        chg_on = d[29] & 0x10;
        dis_on = d[29] & 0x20;
      }
    }
#endif
#if BMS_PQ_ON
  } else if (model == BMS_PQ_ASCII) {
    // ~11014642E00201FD35\r : Messwerte, Akku 01
    static const char REQ[] = "~11014642E00201FD35\r";
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
      if (!pq_ask(mac, wr, (const uint8_t *)REQ, sizeof(REQ) - 1, 1, 0, d, 2500)) continue;
      const uint8_t *p = (const uint8_t *)d.data();
      size_t n = d.size(), i = 0;
      if (n < 1) continue;
      uint8_t m = p[i++];
      if (n < 1 + 2u * m + 1) continue;
      uint8_t tc = p[1 + 2 * m], nmos = tc >> 4, nct = tc & 0x0F;
      if (n != 2u * m + 2u * (nmos + nct) + 23) {
        logf("%s: ASCII-Antwort hat unerwartete Laenge %u", mac.c_str(), (unsigned)n);
        continue;
      }
      for (uint8_t k = 0; k < m; k++, i += 2) cmv.push_back((uint16_t)be16s(p + i));
      i++;
      i += 2 * nmos;
      for (uint8_t k = 0; k < nct; k++, i += 2) {
        float t = be16s(p + i) / 10.0f;
        if (isnan(temp) || t > temp) temp = t;
      }
      a = be16s(p + i) / 10.0f;
      i += 2;
      v = (uint16_t)be16s(p + i) / 10.0f;
      i += 2;
      soc = be16s(p + i);
      i += 3;
      cyc = (uint16_t)be16s(p + i);
      ok = true;
    }
  } else if (model == BMS_PQ_HEX) {
    auto ask = [&](uint8_t cmd) -> bool {
      uint8_t req[7] = {0xAA, 0x55, cmd, 0x00, cmd, 0x0A, 0x0D};
      for (int attempt = 0; attempt < 2; attempt++)
        if (pq_ask(mac, wr, req, 7, 0, cmd, d, 1500)) return true;
      return false;
    };
    if (full && ask(0x15) && d.size() >= 4) {   // Kapazitaet Ah, Zyklen
      const uint8_t *p = (const uint8_t *)d.data();
      cap = (uint16_t)le16s(p);
      cyc = (uint16_t)le16s(p + 2);
    }
    bool s1 = !full || (ask(0x0A) && d.size() >= 2);   // SoC, MOS-Status – nur jede Minute
    if (s1 && full) {
      soc = (uint8_t)d[0];
      mos = (uint8_t)d[1];
      // Die Deutung der App (oberes Nibble 0 = Laden an, unteres 2 = Entladen an) passt nicht zum Akku (zeigte
      // bei 98 % in Ruhe beides aus) – vorerst nicht auswerten, Rohwert steht im Protokoll.
    }
    bool s2 = ask(0x0B) && d.size() >= 8;   // Spannung mV, Strom mA (+ laden)
    if (s2) {
      const uint8_t *p = (const uint8_t *)d.data();
      v = le32(p) / 1000.0f;
      a = (int32_t)le32(p + 4) / 1000.0f;
    }
    if (full && ask(0x09) && d.size() >= 2) temp = le16s((const uint8_t *)d.data());
    for (uint8_t g = 0; full && g < 4 && cmv.size() < cells; g++) {
      if (!ask(0x05 + g)) break;
      const uint8_t *p = (const uint8_t *)d.data();
      for (size_t k = 0; k + 1 < d.size() && k < 10; k += 2) cmv.push_back((uint16_t)le16s(p + k));
    }
    if (cmv.size() > cells) cmv.resize(cells);
    if (full && ask(0x21) && d.size() >= 4) {
      const uint8_t *p = (const uint8_t *)d.data();
      alarms = p[1] | p[2] << 8 | (uint32_t)p[3] << 16;
    }
    ok = s1 && s2;
#endif
  }
  Lock l;
  Bat *b = find_mac(mac);
  if (!b) return;
  if (!ok) {
    logf("%s: keine gueltige Antwort – %s", b->name.c_str(), s_pq_diag.c_str());
    if (s_pq_rxlen == 0 && ++b->silent >= 2) {   // gar nichts zurueck: andere Schreibart probieren
      b->silent = 0;
      b->wr_resp = !b->wr_resp;
      logf("%s: schreibe jetzt %s Bestaetigung", b->name.c_str(), b->wr_resp ? "mit" : "ohne");
    }
    return;
  }
  b->silent = 0;
  if (model == BMS_SS_TANK) {   // Tankmodul: nur Fuellstaende und Temperaturen
    bool first = !b->has;
    for (int k = 0; k < BMS_TANKS; k++) {
      b->ss_raw[k] = ss_raw[k];
      b->ss_traw[k] = ss_traw[k];
    }
    if (first)
      logf("%s: Rohwerte %d / %d / %d / %d, Temperatur %d / %d / %d / %d", b->name.c_str(), ss_raw[0], ss_raw[1], ss_raw[2],
           ss_raw[3], ss_traw[0], ss_traw[1], ss_traw[2], ss_traw[3]);
    b->has = true;
    b->last_ms = millis();
    b->flags = 0x01;
    b->status = "Verbunden";
    s_ver++;
    return;
  }
  if (!b->has || (mos >= 0 && mos != b->mos_raw))
    logf(mos >= 0 ? "%s: %.0f %% %.2f V %.2f A, MOS-Status 0x%02X" : "%s: %.0f %% %.2f V %.2f A", b->name.c_str(), soc,
         v, a, mos);
  if (mos >= 0) b->mos_raw = mos;
  b->has = true;
  b->last_ms = millis();
  if (!isnan(soc)) b->soc = (int)lroundf(soc);
  soc = b->soc;   // ohne neue Abfrage: letzter Ladezustand (fuer Restzeit und Zustand)
  b->volt = v;
  b->amp = a;
  if (!isnan(temp)) b->temp = temp;   // seltene Werte nur bei der vollen Abfrage
  if (cyc >= 0) b->cycles = cyc;
  if (cap > 0) b->cap_ah = cap;
  if (!cmv.empty()) b->cell_mv = cmv;
  if (full || model != BMS_PQ_HEX) b->alarms = alarms;
  alarms = b->alarms;
  b->chg_on = chg_on;
  b->dis_on = dis_on;
  b->ex = ex;
  // Restzeit wie die App: ab 0,5 A, mit Kapazitaet
  b->ttg = -1;
  if (b->cap_ah > 0 && isfinite(a) && a <= -0.5f) b->ttg = (int32_t)(soc / 100.0f * b->cap_ah / -a * 3600.0f);
  if (model == BMS_BULLTRON) {
    // Zustandsregister des Akkus statt Stromschwelle; Lade-/Entlade-MOSFET sind hier eindeutig dokumentiert
    if (state == 1) a = fabsf(a);
    if (state == 2) a = -fabsf(a);
    b->amp = a;
    b->flags = (alarms ? 0 : 0x01) | (state == 2 ? 0x02 : 0) | (state == 1 ? 0x04 : 0) | (soc <= 10 ? 0x08 : 0) |
               (chg_on ? 0x10 : 0) | (dis_on ? 0x20 : 0);
    b->ttg = -1;
    if (b->cap_ah > 0 && state == 2 && fabsf(a) > 0.05f) b->ttg = (int32_t)(soc / 100.0f * b->cap_ah / fabsf(a) * 3600.0f);
    (void)rem_ah;
    b->status = "Verbunden";
    s_ver++;
    return;
  }
  // Zustand in die gemeinsamen Flags (wie Super B): ok, entlaedt, laedt, niedrig, Laden erlaubt, Entladen erlaubt
  b->flags = (alarms ? 0 : 0x01) | (a < -0.2f ? 0x02 : 0) | (a > 0.2f ? 0x04 : 0) | (soc <= 10 ? 0x08 : 0) |
             (chg_on ? 0x10 : 0) | (dis_on ? 0x20 : 0);
  b->status = "Verbunden";
  s_ver++;
}

// ---------------------------------------------------------------- oeffentliche API
void bms_begin() {
  if (s_mtx) return;
  s_mtx = xSemaphoreCreateRecursiveMutex();
  load();
#if BMS_ECOFLOW_ON
  {
    Preferences p;
    if (p.begin("bms", true)) {
      s_ef_uid = p.getString("efuid", "").c_str();
      p.end();
    }
  }
#endif
  uint32_t now = millis();
  for (auto &b : s_bats) b.next_try = now + 15000;   // erst nach dem Start von WLAN und Display
}

uint32_t bms_version() { return s_ver; }

bool bms_has_devices() {
  if (!s_mtx) return false;
  Lock l;
  return !s_bats.empty();
}

static BmsInfo info_of(const Bat &b) {
  BmsInfo i;
  i.key = BMS_KEY_PREFIX + b.mac;
  i.mac = b.mac;
  i.name = b.name;
  i.model = b.model;
  i.model_text = b.model_text.empty() ? bms_model_text(b.model) : b.model_text;
  i.serial = b.serial;
  i.firmware = b.firmware;
  i.connected = b.connected || (b.polled() && b.last_ms && millis() - b.last_ms < 3 * PQ_POLL_MS);
  i.has = b.has;
  i.age_s = b.last_ms ? (int32_t)((millis() - b.last_ms) / 1000) : -1;
  i.soc = b.soc;
  i.soh = b.soh;
  i.volt = b.volt;
  i.amp = b.amp;
  i.ttg_s = b.ttg > 0 ? b.ttg : -1;
  i.ok = b.flags & 0x01;
  i.discharging = b.flags & 0x02;
  i.charging = b.flags & 0x04;
  i.low = b.flags & 0x08;
  i.charge_allowed = b.flags & 0x10;
  i.discharge_allowed = b.flags & 0x20;
  i.temp = b.temp;
  i.cycles = b.cycles;
  i.pin_set = !b.pin.empty();
  i.pin_need = b.pin_need;
  i.capacity_ah = b.cap_ah;
  i.cells_mv = b.cell_mv;
  i.alarms = b.alarms;
  i.status = b.status;
  i.dev = bms_dev_of(b.model);
  for (int k = 0; k < BMS_TANKS; k++) {   // SuperSense: % = roh / 2475 (ohne Kalibrierung), °C aus 10-Bit-Wert
    i.tank_raw[k] = b.ss_raw[k];
    if (b.ss_raw[k] >= 0) i.tank_pct[k] = std::min(100, std::max(0, (int)lroundf(b.ss_raw[k] * 100.0f / SS_FULL_RAW)));
    if (b.ss_traw[k] >= 0) i.tank_temp[k] = b.ss_traw[k] * 5.0f / 1023.0f * 100.0f - 50.0f;
  }
  i.pv_v = b.ex.pv_v;
  i.pv_w = b.ex.pv_w;
  i.in_v = b.ex.in_v;
  i.in_a = b.ex.in_a;
  i.in_w = b.ex.in_w;
  i.load_a = b.ex.load_a;
  i.load_w = b.ex.load_w;
  i.ac_v = b.ex.ac_v;
  i.ac_a = b.ex.ac_a;
  i.ac_w = b.ex.ac_w;
  i.ac_hz = b.ex.ac_hz;
  i.grid_v = b.ex.grid_v;
  i.starter_v = b.ex.starter_v;
  i.yield_wh = b.ex.yield_wh;
  i.chg_state = b.ex.state;
  i.ps_ac_in_w = b.ex.ps_ac_in;
  i.ps_dc_in_w = b.ex.ps_dc_in;
  i.ps_ac_out_w = b.ex.ps_ac_out;
  i.ps_dc_out_w = b.ex.ps_dc_out;
  i.ps_in_w = isfinite(b.ex.ps_in) ? b.ex.ps_in
              : isfinite(b.ex.ps_ac_in) || isfinite(b.ex.ps_dc_in)
                  ? (isfinite(b.ex.ps_ac_in) ? b.ex.ps_ac_in : 0) + (isfinite(b.ex.ps_dc_in) ? b.ex.ps_dc_in : 0)
                  : NAN;
  i.ps_out_w = isfinite(b.ex.ps_out) ? b.ex.ps_out
               : isfinite(b.ex.ps_ac_out) || isfinite(b.ex.ps_dc_out)
                   ? (isfinite(b.ex.ps_ac_out) ? b.ex.ps_ac_out : 0) + (isfinite(b.ex.ps_dc_out) ? b.ex.ps_dc_out : 0)
                   : NAN;
  i.ac_on = b.want_ac >= 0 ? b.want_ac : b.ex.ac_on;   // gewuenschter Zustand sofort anzeigen
  i.dc_on = b.want_dc >= 0 ? b.want_dc : b.ex.dc_on;
  i.can_ac = i.can_dc =
      (b.model == BMS_BLUETTI && b.rid) || ((b.model == BMS_ALLPOWERS || is_anker(b.model) || is_ecoflow(b.model)) && b.has);
  i.remain_min = b.ex.remain;
  return i;
}

std::vector<BmsInfo> bms_list() {
  std::vector<BmsInfo> out;
  if (!s_mtx) return out;
  Lock l;
  for (auto &b : s_bats)
    if (!b.remove) out.push_back(info_of(b));
  return out;
}

bool bms_get(const std::string &key, BmsInfo &out) {
  if (!s_mtx) return false;
  Lock l;
  for (auto &b : s_bats)
    if (!b.remove && BMS_KEY_PREFIX + b.mac == key) {
      out = info_of(b);
      return true;
    }
  return false;
}

void bms_discovery_start() {
  Lock l;
  s_found.clear();
  s_seen.clear();
  s_disc_until = (millis() + DISCOVERY_MS) | 1;
  s_ver++;
  logf("Suche gestartet (60 s)");
}

bool bms_discovery_running() { return s_disc_until != 0; }

std::vector<BmsSeen> bms_seen() {
  Lock l;
  std::vector<BmsSeen> out = s_seen;
  std::sort(out.begin(), out.end(), [](const BmsSeen &a, const BmsSeen &b) { return a.rssi > b.rssi; });
  return out;
}

std::vector<BmsFound> bms_found() {
  Lock l;
  std::vector<BmsFound> out = s_found;
  for (auto &f : out) f.configured = find_mac(f.mac) != nullptr;
  return out;
}

std::string bms_add(const std::string &mac_in, uint8_t addr_type, const std::string &name, BmsModel model,
                    int capacity_ah, int cells) {
  std::string mac = lower(mac_in);
  if (mac.size() != 17) return "Ungültige Adresse";
  Lock l;
  if (find_mac(mac)) return "Dieses Gerät ist schon eingerichtet";
  size_t n = 0;
  for (auto &b : s_bats) n += !b.remove;
  if (n >= BMS_MAX) return "Es sind höchstens sechs Geräte möglich";
  Bat b;
  b.mac = mac;
  b.type = addr_type;
  b.model = model == BMS_UNKNOWN ? BMS_SUPERB : model;
  b.name = name.empty() ? std::string("Akku") : name.substr(0, 32);
  b.cap_ah = capacity_ah > 0 ? capacity_ah : 0;
  b.cells = cells > 0 && cells <= 32 ? cells : 4;
  b.model_text = bms_model_text(b.model);
  if (is_ecoflow(b.model)) {   // Seriennummer/Verschluesselungsart aus der Suche (Werbedaten)
    for (auto &f : s_found)
      if (f.mac == mac) {
        b.serial = f.serial;
        b.ef_enc = f.enc;
      }
    if (b.serial.size() < 8) return "EcoFlow bitte über „Power Stations suchen“ hinzufügen (Seriennummer nötig)";
  }
  b.next_try = millis() + 2000;
  s_bats.push_back(b);
  save();
  s_disc_until = 0;   // Suche beenden, gleich verbinden
  s_ver++;
  logf("Eingerichtet: %s \"%s\"", mac.c_str(), b.name.c_str());
  return "";
}

// ---------------------------------------------------------------- EcoFlow-Benutzer-ID
bool bms_ef_uid_set() {
#if BMS_ECOFLOW_ON
  return !s_ef_uid.empty();
#else
  return false;
#endif
}
std::string bms_ef_uid_hint() {
#if BMS_ECOFLOW_ON
  return s_ef_uid.size() > 4 ? "…" + s_ef_uid.substr(s_ef_uid.size() - 4) : s_ef_uid;
#else
  return "";
#endif
}
void bms_ef_set_uid(const std::string &uid_in) {
#if BMS_ECOFLOW_ON
  std::string uid;
  for (char c : uid_in)
    if (c != ' ') uid += c;
  {
    Lock l;
    s_ef_uid = uid;
    for (auto &b : s_bats) b.ef.authed = false;   // neu anmelden
  }
  Preferences p;
  p.begin("bms", false);
  if (uid.empty()) p.remove("efuid");
  else p.putString("efuid", uid.c_str());
  p.end();
  s_ver++;
#else
  (void)uid_in;
#endif
}
std::string bms_ef_login_start(const std::string &email, const std::string &password) {
#if BMS_ECOFLOW_ON
  if (email.empty() || password.empty()) return "E-Mail und Passwort eingeben";
  if (s_ef_login_state == 1) return "Anmeldung läuft schon";
  auto *r = new EfLoginReq{email, password};
  s_ef_login_state = 1;
  s_ef_login_text = "Melde an …";
  if (xTaskCreatePinnedToCore(ef_login_task, "ef_login", 8192, r, 2, nullptr, 1) != pdPASS) {
    delete r;
    s_ef_login_state = 3;
    s_ef_login_text = "Zu wenig Speicher für die Anmeldung";
    return s_ef_login_text;
  }
  return "";
#else
  (void)email;
  (void)password;
  return "EcoFlow ist in dieser Firmware nicht eingebaut";
#endif
}
int bms_ef_login_state() {
#if BMS_ECOFLOW_ON
  return s_ef_login_state;
#else
  return 0;
#endif
}
std::string bms_ef_login_text() {
#if BMS_ECOFLOW_ON
  Lock l;
  return s_ef_login_text;
#else
  return "";
#endif
}

bool bms_set_output(const std::string &mac, bool ac, bool on) {
  if (!s_mtx) return false;
  Lock l;
  Bat *b = find_mac(lower(mac));
  if (!b || bms_dev_of(b->model) != BMS_DEV_PSTATION) return false;
  (ac ? b->want_ac : b->want_dc) = on ? 1 : 0;
  uint32_t now = millis();
  if (b->connected) b->next_poll = now;
  else b->next_try = now;   // gleich verbinden statt bis zur naechsten Abfrage zu warten
  s_ver++;
  logf("%s: %s-Ausgang %s angefordert", b->name.c_str(), ac ? "AC" : "DC", on ? "ein" : "aus");
  return true;
}

void bms_remove(const std::string &mac) {
  Lock l;
  Bat *b = find_mac(lower(mac));
  if (!b) return;
  b->remove = true;   // aufraeumen im Scan-Task (Client)
  save();
  s_ver++;
}

void bms_set_pin(const std::string &mac, const std::string &pin) {
  std::string digits;
  for (char ch : pin)
    if (isdigit((unsigned char)ch)) digits += ch;
  Lock l;
  Bat *b = find_mac(lower(mac));
  if (!b) return;
  b->pin = digits.substr(0, 6);
  if (!b->pin.empty()) b->auth_fail = true;   // alte Kopplung (ohne PIN) vor dem naechsten Versuch loeschen
  b->next_try = millis();   // gleich neu versuchen
  save();
  s_ver++;
  logf("%s: PIN %s", b->name.c_str(), b->pin.empty() ? "geloescht" : "gespeichert");
}

void bms_rename(const std::string &mac, const std::string &name) {
  Lock l;
  Bat *b = find_mac(lower(mac));
  if (!b || name.empty()) return;
  b->name = name.substr(0, 32);
  save();
  s_ver++;
}

std::vector<std::string> bms_log() {
  Lock l;
  return std::vector<std::string>(s_log.begin(), s_log.end());
}

// ---------------------------------------------------------------- Texte (Panel-Einstellungen, Mini-Display)
static std::string fnum(float v, int dec, const char *unit) {
  if (!isfinite(v)) return "–";
  char t[32];
  snprintf(t, sizeof(t), "%.*f %s", dec, v, unit);
#if UI_DECIMAL_COMMA
  for (char *q = t; *q; q++)
    if (*q == '.') *q = ',';
#endif
  return t;
}
static std::string both(const std::string &a, const std::string &b) {
  if (a == "–") return b;
  if (b == "–") return a;
  return a + " · " + b;
}

std::string bms_value_text(const BmsInfo &b) {
  if (!b.has) return b.status.empty() ? std::string("–") : b.status;
  const char *dot = "  ·  ";
  switch (b.dev) {
    case BMS_DEV_SOLAR: return fnum(b.pv_w, 0, "W") + dot + fnum(b.volt, 1, "V") + dot + victron_state_text(b.chg_state);
    case BMS_DEV_DCDC: return fnum(b.amp, 1, "A") + dot + fnum(b.volt, 1, "V") + dot + victron_state_text(b.chg_state);
    case BMS_DEV_INVERTER: return fnum(b.ac_w, 0, "W") + dot + fnum(b.ac_v, 0, "V") + dot + fnum(b.volt, 1, "V");
    case BMS_DEV_PSTATION:
      return (b.soc >= 0 ? std::to_string(b.soc) + " %" : std::string("–")) + dot + tr("rein ") + fnum(b.ps_in_w, 0, "W") + dot +
             tr("raus ") + fnum(b.ps_out_w, 0, "W");
    case BMS_DEV_TANK: {   // "T1 63 %  ·  T2 12 %"
      std::string s;
      for (int k = 0; k < BMS_TANKS; k++)
        if (b.tank_pct[k] >= 0) s += (s.empty() ? "" : dot) + std::string("T") + std::to_string(k + 1) + " " + std::to_string(b.tank_pct[k]) + " %";
      return s.empty() ? std::string(tr("kein Tanksensor angeschlossen")) : s;
    }
    default: {
      std::string s = (b.soc >= 0 ? std::to_string(b.soc) + " %" : std::string("–")) + dot + fnum(b.volt, 2, "V");
      if (isfinite(b.volt) && isfinite(b.amp)) s += dot + fnum(b.volt * b.amp, 0, "W");
      return s;
    }
  }
}

std::vector<std::pair<std::string, std::string>> bms_rows(const BmsInfo &b) {
  std::vector<std::pair<std::string, std::string>> r;
  bool h = b.has;
  auto add = [&](const char *k, const std::string &v) { r.emplace_back(k, h ? v : std::string("–")); };
  std::string soc = b.soc >= 0 ? std::to_string(b.soc) + " %" : "–";
  std::string pw = isfinite(b.volt) && isfinite(b.amp) ? fnum(b.volt * b.amp, 0, "W") : "–";
  std::string temp = fnum(b.temp, 0, "°C");
  std::string today = b.yield_wh >= 0 ? fnum((float)b.yield_wh, 0, "Wh") : "–";
  bool bat = b.dev == BMS_DEV_BATTERY || b.dev == BMS_DEV_SHUNT;
  if (b.dev == BMS_DEV_TANK) {   // je Kanal Fuellstand (Rohwert) und Temperatur
    static const char *T[BMS_TANKS] = {"Tank 1", "Tank 2", "Tank 3", "Tank 4"};
    for (int k = 0; k < BMS_TANKS; k++)
      if (b.tank_pct[k] >= 0)
        r.emplace_back(T[k], std::to_string(b.tank_pct[k]) + " %" + (isfinite(b.tank_temp[k]) ? "  ·  " + fnum(b.tank_temp[k], 1, "°C") : std::string()));
    if (r.empty()) r.emplace_back("Tanks", h ? std::string(tr("kein Tanksensor angeschlossen")) : std::string("–"));
    return r;
  }
  switch (b.dev) {
    case BMS_DEV_SOLAR:
      add("Solar", both(fnum(b.pv_w, 0, "W"), fnum(b.pv_v, 1, "V")));
      add("Batterie", both(fnum(b.volt, 2, "V"), fnum(b.amp, 1, "A")));
      add("Ladephase", victron_state_text(b.chg_state));
      add("Heute", today);
      add("Last", both(fnum(b.load_w, 0, "W"), fnum(b.load_a, 1, "A")));
      add("Temperatur", temp);
      break;
    case BMS_DEV_DCDC:
      add("Ladestrom", fnum(b.amp, 1, "A"));
      add("Batterie", fnum(b.volt, 2, "V"));
      add("Eingang", both(fnum(b.in_v, 1, "V"), fnum(b.in_w, 0, "W")));
      add("Solar", both(fnum(b.pv_w, 0, "W"), fnum(b.pv_v, 1, "V")));
      add("Ladephase", victron_state_text(b.chg_state));
      add("Heute", today);
      add("Temperatur", temp);
      break;
    case BMS_DEV_INVERTER:
      add("Leistung", fnum(b.ac_w, 0, "W"));
      add("Ausgang", both(fnum(b.ac_v, 0, "V"), fnum(b.ac_hz, 1, "Hz")));
      add("Netz", fnum(b.grid_v, 0, "V"));
      add("Batterie", both(fnum(b.volt, 2, "V"), fnum(b.amp, 1, "A")));
      add("Solar", fnum(b.pv_w, 0, "W"));
      add("Ladezustand", soc);
      add("Temperatur", temp);
      break;
    case BMS_DEV_PSTATION: {
      auto sw = [](int8_t on) { return on < 0 ? std::string() : on ? std::string("ein") : std::string("aus"); };
      add("Ladezustand", soc);
      add("Eingang", fnum(b.ps_in_w, 0, "W"));
      add("Ausgang", fnum(b.ps_out_w, 0, "W"));
      add("AC-Ausgang", both(fnum(b.ps_ac_out_w, 0, "W"), b.ac_on < 0 ? std::string("–") : sw(b.ac_on)));
      add("DC-Ausgang", both(fnum(b.ps_dc_out_w, 0, "W"), b.dc_on < 0 ? std::string("–") : sw(b.dc_on)));
      add("AC-Eingang", fnum(b.ps_ac_in_w, 0, "W"));
      add("DC-Eingang", fnum(b.ps_dc_in_w, 0, "W"));
      if (b.remain_min > 0) {
        char t[24];
        snprintf(t, sizeof(t), "%d h %02d min", b.remain_min / 60, b.remain_min % 60);
        add("Restzeit", t);
      }
      break;
    }
    case BMS_DEV_SHUNT:
      add("Ladezustand", soc);
      add("Spannung", fnum(b.volt, 2, "V"));
      add("Strom", fnum(b.amp, 2, "A"));
      add("Leistung", pw);
      add("Starter", fnum(b.starter_v, 2, "V"));
      add("Temperatur", temp);
      break;
    default: {   // Akku: Super B ohne Temperatur/Zyklen, die anderen ohne Gesundheit (SoH)
      bool sb = b.model == BMS_SUPERB;
      add("Ladezustand", soc);
      add("Spannung", fnum(b.volt, 2, "V"));
      add("Strom", fnum(b.amp, 2, "A"));
      add("Leistung", pw);
      if (!sb) add("Temperatur", temp);
      std::string rest = "–";
      if (b.ttg_s > 0) {
        char t[24];
        snprintf(t, sizeof(t), "%ld h %02ld min", (long)(b.ttg_s / 3600), (long)(b.ttg_s % 3600 / 60));
        rest = t;
      } else if (isfinite(b.amp) && b.amp > 0.2f) {
        rest = "lädt";
      }
      add("Restzeit", rest);
      if (!sb || b.cycles >= 0) add("Zyklen", b.cycles >= 0 ? std::to_string(b.cycles) : "–");
      if (sb) add("Gesundheit", b.soh >= 0 ? std::to_string(b.soh) + " %" : "–");
    }
  }
  std::string z;
  if (!b.connected) {
    z = b.status.empty() ? std::string("getrennt") : b.status;
  } else if (!h) {
    z = "–";
  } else if (bat) {
    z = !b.ok ? "Alarm" : b.charging ? "lädt" : b.discharging ? "entlädt" : "Ruhe";
    if (b.low) z += ", niedrig";
    if (!b.charge_allowed) z += ", Laden aus";
    if (!b.discharge_allowed) z += ", Entladen aus";
  } else {
    z = b.ok ? "in Ordnung" : "Fehler";
  }
  r.emplace_back("Zustand", z);
  return r;
}
#endif
