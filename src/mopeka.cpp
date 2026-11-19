#include "mopeka.h"

#include <Arduino.h>
#include <atomic>
#include <math.h>
#include <Preferences.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ---------------------------------------------------------------- Protokoll
// Pro-Familie: Herstellerdaten = 0x59 0x00 + 10 Byte
//   [0] Hardware-ID   [1] Bit 0-6 Batterie (V = raw/32)   [2] Bit 7 Sync-Taste, Bit 0-6 Temperatur (raw - 40 °C)
//   [3..4] Bit 0-13 Laufzeit des Echos, Bit 14-15 Qualitaet 0..3   [5..7] Teil der MAC   [8..9] Lage (Beschleunigung)
//   Fuellhoehe mm = Laufzeit * (c0 + c1*T + c2*T^2) mit T = Rohwert der Temperatur, Koeffizienten je Medium
// Standard-Familie: Herstellerdaten = 0x0D 0x00 + 23 Byte
//   [1] & 0xCF Hardware-ID   [2] Batterie (V = raw/256*2 + 1,5)   [3] Bit 0-5 Temperatur, Bit 7 Sync-Taste
//   [4..18] 3 x 40 Bit mit je 4 Paaren (Zeit 5 Bit, Amplitude 5 Bit): das staerkste Echo gibt die Laufzeit
static const uint16_t MID_PRO = 0x0059;
static const uint16_t MID_STD = 0x000D;
static const size_t PRO_LEN = 10, STD_LEN = 23;
static const uint32_t DISCOVERY_MS = 3 * 60 * 1000;
static const size_t MAX_FOUND = 12;
static const uint32_t STALE_MS = 10 * 60 * 1000;   // so lange ohne Paket: "kein Empfang"
// Senso4s (Gasflaschen-Waage, BETA, ungetestet): Herstellerdaten 0x0059 (Nordic) bzw. 0x09CC, 12 Byte nach der ID:
//   [0] oben 4 Bit: 8 = Basic, sonst Plus mit Warnbits (1 Temperatur, 2 Neigung, 4 Bewegung); unten 4 Bit Modus 1..5
//   [1] Fuellstand 0..100 % bzw. 0xFF nicht bereit (Nullabgleich), 0xFC Einstellfehler, 0xFE Batterien leer,
//       0xF1..0xF7 Warnung beim Messbeginn; [4] Batterie %; [6..11] MAC (Reihenfolge offen: vorwaerts oder umgedreht).
// Messung alle 15 min. Nach ksanislo/senso4s_ble (Apache-2.0), eigene Umsetzung.
static const uint16_t MID_SENSO = 0x09CC;
static const uint16_t MODEL_SENSO = 0x200;   // | 1 = Plus
static const uint32_t SENSO_STALE_MS = 60 * 60 * 1000;

static const MopekaTank TANKS[] = {
    {"11 kg (Europa)", 38, 366, 11},
    {"6 kg (Europa)", 38, 336, 6},
    {"14 kg (Europa)", 38, 467, 14},
    {"20 lb (USA)", 38, 254, 9.1f},
    {"30 lb (USA)", 38, 381, 13.6f},
    {"40 lb (USA)", 38, 508, 18.1f},
};
static const size_t TANK_N = sizeof(TANKS) / sizeof(TANKS[0]);

size_t mopeka_tank_count() { return TANK_N; }
const MopekaTank &mopeka_tank(size_t i) { return TANKS[i < TANK_N ? i : 0]; }

const char *mopeka_model_text(uint16_t m) {
  switch (m) {
    case 0x03: return "Pro Check";
    case 0x04: return "Pro-200";
    case 0x05: return "Pro Check H2O";
    case 0x06: return "Lippert BottleCheck";
    case 0x08: return "Pro Plus";
    case 0x09: return "Pro Plus (Mobilfunk)";
    case 0x0A: return "TD40/TD200";
    case 0x0B: return "TD40/TD200 (Mobilfunk)";
    case 0x0C: return "Pro Check Universal";
    case 0x12: return "Pro-200B";
    case 0x102: return "Check Standard";
    case 0x103: return "Check XL";
    case 0x144: return "Check Standard";
    case 0x146: return "Check eTrailer";
    case 0x200: return "Senso4s Basic";
    case 0x201: return "Senso4s Plus";
    default: return "Mopeka";
  }
}

static bool pro_model_known(uint8_t m) {
  return m == 0x03 || m == 0x04 || m == 0x05 || m == 0x06 || m == 0x08 || m == 0x09 || m == 0x0A || m == 0x0B ||
         m == 0x0C || m == 0x12;
}

// Medium ergibt sich aus der Bauart: H2O misst Wasser von unten, Pro-200/TD misst von oben durch Luft
static MopekaMedium medium_of(uint16_t m) {
  if (m == 0x05) return MOP_WATER;
  if (m == 0x04 || m == 0x0A || m == 0x0B || m == 0x12) return MOP_AIR;
  return MOP_PROPANE;
}

struct Coef { float c0, c1, c2; };
static const Coef COEF[] = {
    {0.573045f, -0.002822f, -0.00000535f},   // Propan (Fluessiggas)
    {0.153096f, 0.000327f, -0.000000294f},   // Luft (Sensor oben im Tank)
    {0.600592f, 0.003124f, -0.00001368f},    // Wasser
};

struct Reading {
  bool ok = false;      // Laufzeit brauchbar
  float mm = 0;
  int quality = 0;
  int temp = 0;
  float volt = 0;
  bool button = false;
};

static bool parse_pro(const uint8_t *p, MopekaMedium med, Reading &r) {
  r.volt = (p[1] & 0x7F) / 32.0f;
  int traw = p[2] & 0x7F;
  r.temp = traw - 40;
  r.button = p[2] & 0x80;
  uint16_t raw = (p[3] | (p[4] << 8)) & 0x3FFF;
  r.quality = p[4] >> 6;
  const Coef &c = COEF[med];
  r.mm = raw * (c.c0 + c.c1 * traw + c.c2 * traw * traw);
  r.ok = r.quality >= 1 && raw > 0;
  return true;
}

static bool parse_std(const uint8_t *p, Reading &r) {
  r.volt = p[2] / 256.0f * 2.0f + 1.5f;
  int traw = p[3] & 0x3F;
  r.temp = traw == 0 ? -40 : (int)((traw - 25.0f) * 1.776964f);
  r.button = p[3] & 0x80;
  // 12 Echos: Zeitabstand seit dem vorigen Echo und Staerke; das staerkste Echo zaehlt
  int usable = 0, best_val = 0, best_time = 0, acc = 0;
  for (int g = 0; g < 3; g++) {
    uint64_t v = 0;
    for (int b = 0; b < 5; b++) v |= (uint64_t)p[4 + g * 5 + b] << (8 * b);
    for (int j = 0; j < 4; j++) {
      int t = (int)((v >> (10 * j)) & 0x1F) + 1;
      int a = (int)((v >> (10 * j + 5)) & 0x1F);
      acc += t;
      if (a) {
        usable++;
        if (a > best_val) { best_val = a; best_time = acc; }
        acc = 0;
      }
    }
  }
  r.ok = usable >= 1 && best_val >= 2 && best_time >= 2;
  r.quality = !r.ok ? 0 : best_val >= 12 ? 3 : best_val >= 6 ? 2 : 1;   // eigene Einteilung, das Protokoll kennt keine
  // Schallgeschwindigkeit in Fluessiggas (100 % Propan) in m/s, Zeit in 10-µs-Schritten
  float T = r.temp, mix = 1.0f;
  float speed = 1040.71f - 4.87f * T - 137.5f * mix - 0.0107f * T * T - 1.63f * T * mix;
  r.mm = speed * best_time / 100.0f;
  return true;
}

// ---------------------------------------------------------------- Zustand
namespace {   // dateilokal: gleichnamige Typen anderer Dateien duerfen sich nicht vermischen
struct Sensor {
  std::string mac, name;
  uint8_t tank = 0;
  uint16_t empty_mm = 38, full_mm = 366;
  float capacity = 11;
  uint16_t model = 0;
  // Werte
  bool has = false;
  float mm = 0;              // geglaettet
  int quality = 0, temp = 0, rssi = 0;
  float volt = 0;
  uint32_t when_ms = 0;      // letztes Paket
  uint32_t good_ms = 0;      // letzter brauchbarer Messwert
  bool echo_lost = false;
  int shown_pct = -1, shown_batt = -1, shown_temp = -999, shown_q = -1;
  // Senso4s: Fuellstand und Batterie direkt in %, Fehler-/Warncode (0 = keiner)
  int s4_pct = -1, s4_batt = -1;
  uint8_t s4_code = 0;
};
}  // namespace

static SemaphoreHandle_t s_mtx;
static std::vector<Sensor> s_sens;
static std::vector<MopekaFound> s_found;
static std::atomic<uint32_t> s_ver{1};
static volatile uint32_t s_disc_until = 0;
static volatile bool s_disc = false;
static Preferences s_prefs;

namespace {   // dateilokal: gleichnamige Typen anderer Dateien duerfen sich nicht vermischen
struct Lock {
  Lock() { if (s_mtx) xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
  ~Lock() { if (s_mtx) xSemaphoreGiveRecursive(s_mtx); }
};
}  // namespace

static std::string lower(std::string s) {
  for (auto &c : s) c = tolower((unsigned char)c);
  return s;
}

static int batt_pct(float v) {
  float p = (v - 2.2f) / 0.65f * 100.0f;
  return p < 0 ? 0 : p > 100 ? 100 : (int)p;
}

static bool is_senso(uint16_t model) { return (model & 0xFF00) == MODEL_SENSO; }

static int pct_of(const Sensor &s) {
  if (is_senso(s.model)) return s.s4_pct < 0 ? 0 : s.s4_pct;
  // Gilt fuer beide Richtungen: von unten (leer < voll) und von oben (leer > voll)
  if (s.full_mm == s.empty_mm) return 0;
  float p = (s.mm - s.empty_mm) * 100.0f / ((float)s.full_mm - s.empty_mm);
  return p < 0 ? 0 : p > 100 ? 100 : (int)lroundf(p);
}

// ---------------------------------------------------------------- Speicher
struct __attribute__((packed)) StoredTank {
  uint8_t tank;
  uint16_t empty_mm, full_mm;
  uint16_t cap_x10;
  uint16_t model;
};

static void save_config() {
  Lock l;
  s_prefs.begin("mopeka", false);
  s_prefs.putUChar("n", s_sens.size());
  for (size_t i = 0; i < s_sens.size() && i < MOPEKA_MAX_SENSORS; i++) {
    const Sensor &s = s_sens[i];
    char k[4];
    snprintf(k, sizeof(k), "m%u", (unsigned)i); s_prefs.putString(k, s.mac.c_str());
    snprintf(k, sizeof(k), "a%u", (unsigned)i); s_prefs.putString(k, s.name.c_str());
    StoredTank t = {s.tank, s.empty_mm, s.full_mm, (uint16_t)lroundf(s.capacity * 10), s.model};
    snprintf(k, sizeof(k), "t%u", (unsigned)i); s_prefs.putBytes(k, &t, sizeof(t));
  }
  s_prefs.end();
}

static void load_config() {
  Lock l;
  s_sens.clear();
  s_prefs.begin("mopeka", true);
  int n = s_prefs.getUChar("n", 0);
  for (int i = 0; i < n && i < MOPEKA_MAX_SENSORS; i++) {
    Sensor s;
    char k[4];
    snprintf(k, sizeof(k), "m%d", i); s.mac = s_prefs.getString(k, "").c_str();
    snprintf(k, sizeof(k), "a%d", i); s.name = s_prefs.getString(k, "Gasflasche").c_str();
    StoredTank t;
    snprintf(k, sizeof(k), "t%d", i);
    if (s_prefs.getBytes(k, &t, sizeof(t)) == sizeof(t)) {
      s.tank = t.tank;
      s.empty_mm = t.empty_mm;
      s.full_mm = t.full_mm;
      s.capacity = t.cap_x10 / 10.0f;
      s.model = t.model;
    }
    if (!s.mac.empty()) s_sens.push_back(s);
  }
  s_prefs.end();
}

// ---------------------------------------------------------------- Empfang
static void note_found(const std::string &mac, uint16_t model, int rssi, bool button) {
  if (!s_disc) return;
  if ((int32_t)(millis() - s_disc_until) >= 0) {
    s_disc = false;
    s_ver++;
    return;
  }
  for (auto &f : s_found)
    if (f.mac == mac) {
      f.rssi = rssi;
      if (button && !f.button) {
        f.button = true;
        s_ver++;
      }
      return;
    }
  if (s_found.size() >= MAX_FOUND) return;
  MopekaFound f;
  f.mac = mac;
  f.model = model;
  f.model_text = mopeka_model_text(model);
  f.rssi = rssi;
  f.button = button;
  s_found.push_back(f);
  s_ver++;
}

// Senso4s erkennen: alle festen Felder plausibel und die MAC im Paket passt zur Funkadresse
static bool senso_check(const std::string &mac, const uint8_t *p) {
  uint8_t lv = p[1];
  bool lv_ok = lv <= 100 || (lv >= 0xF1 && lv <= 0xF7) || lv == 0xFC || lv == 0xFE || lv == 0xFF;
  if ((p[0] & 0x0F) < 1 || (p[0] & 0x0F) > 5 || !lv_ok || p[4] > 100) return false;
  unsigned a[6];
  if (sscanf(mac.c_str(), "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) != 6) return false;
  bool fwd = true, rev = true;
  for (int i = 0; i < 6; i++) {
    fwd &= p[6 + i] == a[i];
    rev &= p[6 + i] == a[5 - i];
  }
  return fwd || rev;
}

static bool senso_adv(const std::string &mac, const uint8_t *p, int rssi) {
  uint16_t model = MODEL_SENSO | ((p[0] >> 4) < 0x8 ? 1 : 0);
  Lock l;
  note_found(mac, model, rssi, false);
  for (auto &s : s_sens) {
    if (s.mac != mac) continue;
    uint32_t now = millis();
    s.rssi = rssi;
    s.when_ms = now;
    if (s.model != model) {
      s.model = model;
      s_ver++;
    }
    uint8_t lv = p[1], code = 0;
    if (lv <= 100) {
      s.s4_pct = lv;
      s.has = true;
      s.good_ms = now;
    } else {
      code = lv;   // Wert bleibt der letzte gemessene
    }
    if (model & 1) {   // Plus: dauerhafte Warnbits im oberen Halbbyte
      uint8_t w = p[0] >> 4;
      if (!code && w) code = 0xF0 | w;
    }
    s.s4_batt = p[4];
    if (s.s4_pct != s.shown_pct || s.s4_batt != s.shown_batt || code != s.s4_code) {
      s.shown_pct = s.s4_pct;
      s.shown_batt = s.s4_batt;
      s.s4_code = code;
      s_ver++;
    }
    break;
  }
  return true;
}

bool mopeka_on_adv(const std::string &mac_in, const uint8_t *md, size_t len, int rssi) {
  if (len < 2) return false;
  uint16_t mid = md[0] | (md[1] << 8);
  const uint8_t *p = md + 2;
  size_t n = len - 2;
  if ((mid == MID_PRO || mid == MID_SENSO) && n == 12) {   // Senso4s (gleiche Nordic-ID wie Mopeka Pro)
    std::string mac = lower(mac_in);
    if (!senso_check(mac, p)) return false;
    if (!s_mtx) return true;
    return senso_adv(mac, p, rssi);
  }
  uint16_t model;
  bool pro;
  if (mid == MID_PRO && n == PRO_LEN && pro_model_known(p[0])) {
    model = p[0];
    pro = true;
  } else if (mid == MID_STD && n == STD_LEN) {
    uint8_t hw = p[1] & 0xCF;
    if (hw != 0x02 && hw != 0x03 && hw != 0x44 && hw != 0x46) return false;
    model = 0x100 | hw;
    pro = false;
  } else {
    return false;
  }
  if (!s_mtx) return true;  // noch nicht gestartet
  std::string mac = lower(mac_in);
  Reading r;
  if (pro) parse_pro(p, medium_of(model), r);
  else parse_std(p, r);

  Lock l;
  note_found(mac, model, rssi, r.button);
  for (auto &s : s_sens) {
    if (s.mac != mac) continue;
    uint32_t now = millis();
    s.rssi = rssi;
    s.when_ms = now;
    if (s.model != model) {
      s.model = model;
      s_ver++;
    }
    s.volt = r.volt;
    s.temp = r.temp;
    s.quality = r.quality;
    if (r.ok) {
      // Glaetten: Schwappen und schlechte Echos sollen den Wert nicht springen lassen.
      // Nach laengerer Pause oder beim ersten Wert direkt uebernehmen.
      float a = r.quality >= 3 ? 0.30f : r.quality == 2 ? 0.15f : 0.05f;
      if (!s.has || now - s.good_ms > 5 * 60 * 1000) a = 1.0f;
      s.mm += a * (r.mm - s.mm);
      s.has = true;
      s.good_ms = now;
      if (s.echo_lost) { s.echo_lost = false; s_ver++; }
    } else if (!s.echo_lost && (!s.has || now - s.good_ms > 60 * 1000)) {
      s.echo_lost = true;  // erst nach einer Minute ohne Echo melden – einzelne Aussetzer sind normal
      s_ver++;
    }
    // Neue Version nur, wenn sich etwas Sichtbares aendert (Pakete kommen jede Sekunde)
    int pct = s.has ? pct_of(s) : -1, bat = batt_pct(s.volt);
    if (pct != s.shown_pct || bat != s.shown_batt || s.temp != s.shown_temp || s.quality != s.shown_q) {
      s.shown_pct = pct;
      s.shown_batt = bat;
      s.shown_temp = s.temp;
      s.shown_q = s.quality;
      s_ver++;
    }
    break;
  }
  return true;
}

// ---------------------------------------------------------------- oeffentliche API
void mopeka_begin() {
  s_mtx = xSemaphoreCreateRecursiveMutex();
  load_config();
  log_i("Mopeka: %u Sensor(en) eingerichtet", (unsigned)s_sens.size());
}

uint32_t mopeka_version() { return s_ver; }

bool mopeka_has_devices() {
  Lock l;
  return !s_sens.empty();
}

static MopekaInfo info_of(const Sensor &s) {
  MopekaInfo m;
  m.key = MOPEKA_KEY_PREFIX + s.mac;
  m.mac = s.mac;
  m.name = s.name;
  m.model = s.model;
  m.model_text = mopeka_model_text(s.model);
  m.medium = (s.model & 0x100) ? MOP_PROPANE : medium_of(s.model);
  m.tank = s.tank;
  m.empty_mm = s.empty_mm;
  m.full_mm = s.full_mm;
  m.capacity = s.capacity;
  m.unit = m.medium == MOP_PROPANE ? "kg" : "l";
  m.has = s.has;
  m.percent = s.has ? pct_of(s) : 0;
  m.level_mm = (int)lroundf(s.mm);
  m.amount = s.capacity * m.percent / 100.0f;
  m.battery = batt_pct(s.volt);
  m.battery_v = s.volt;
  m.temperature = s.temp;
  m.quality = s.quality;
  m.rssi = s.rssi;
  m.age_s = s.when_ms ? (int32_t)((millis() - s.when_ms) / 1000) : -1;
  if (is_senso(s.model)) {   // Waage: keine Temperatur, kein Echo, keine Fuellhoehe
    m.medium = MOP_PROPANE;
    m.unit = "kg";
    m.battery = s.s4_batt < 0 ? 0 : s.s4_batt;
    m.battery_v = 0;
    m.temperature = MOPEKA_NO_VALUE;
    m.quality = MOPEKA_NO_VALUE;
    m.level_mm = MOPEKA_NO_VALUE;
    if (!s.when_ms) m.status = "Warte auf Sensor … (misst alle 15 Minuten)";
    else if (millis() - s.when_ms > SENSO_STALE_MS) m.status = "Kein Empfang – außer Reichweite?";
    else if (s.s4_code == 0xFF) m.status = "Nicht bereit – Nullabgleich in der Senso4s-App";
    else if (s.s4_code == 0xFC) m.status = "Einstellfehler – Flaschengewicht/Inhalt in der Senso4s-App prüfen";
    else if (s.s4_code == 0xFE) m.status = "Batterien leer";
    else if (s.s4_code) {
      uint8_t w = s.s4_code >= 0xF1 && s.s4_code <= 0xF7 ? s.s4_code - 0xF0 : s.s4_code & 0x0F;
      m.status = std::string("Warnung:") + (w & 1 ? " Temperatur" : "") + (w & 2 ? " Neigung" : "") + (w & 4 ? " Bewegung" : "");
    } else m.status = "OK";
    return m;
  }
  if (!s.when_ms) m.status = "Warte auf Sensor …";
  else if (millis() - s.when_ms > STALE_MS) m.status = "Kein Empfang – außer Reichweite?";
  else if (s.echo_lost) m.status = "Kein Echo – Sensor richtig angebracht?";
  else m.status = "OK";
  return m;
}

std::vector<MopekaInfo> mopeka_list() {
  std::vector<MopekaInfo> out;
  Lock l;
  for (auto &s : s_sens) out.push_back(info_of(s));
  return out;
}

bool mopeka_get(const std::string &key, MopekaInfo &out) {
  if (!mopeka_is_key(key)) return false;
  std::string mac = key.substr(strlen(MOPEKA_KEY_PREFIX));
  Lock l;
  for (auto &s : s_sens)
    if (s.mac == mac) {
      out = info_of(s);
      return true;
    }
  return false;
}

void mopeka_discovery_start() {
  Lock l;
  s_found.clear();
  s_disc_until = millis() + DISCOVERY_MS;
  s_disc = true;
  s_ver++;
}

bool mopeka_discovery_running() {
  if (s_disc && (int32_t)(millis() - s_disc_until) >= 0) {
    s_disc = false;
    s_ver++;
  }
  return s_disc;
}

std::vector<MopekaFound> mopeka_found() {
  Lock l;
  std::vector<MopekaFound> out = s_found;
  for (auto &f : out) {
    f.configured = false;
    for (auto &s : s_sens) f.configured |= s.mac == f.mac;
  }
  return out;
}

static void apply_tank(Sensor &s, int tank, int empty_mm, int full_mm, float capacity) {
  if (tank >= 0 && tank < (int)TANK_N) {
    s.tank = tank;
    s.empty_mm = TANKS[tank].empty_mm;
    s.full_mm = TANKS[tank].full_mm;
    s.capacity = TANKS[tank].capacity;
  } else {
    s.tank = MOPEKA_TANK_CUSTOM;
    s.empty_mm = empty_mm;
    s.full_mm = full_mm;
    s.capacity = capacity;
  }
  s.shown_pct = -1;
}

std::string mopeka_add(const std::string &mac_in, const std::string &name, int tank) {
  std::string mac = lower(mac_in);
  if (mac.size() != 17) return "Ungültige Bluetooth-Adresse";
  {
    Lock l;
    for (auto &s : s_sens)
      if (s.mac == mac) return "Dieser Sensor ist schon eingerichtet";
    if (s_sens.size() >= MOPEKA_MAX_SENSORS) return "Es sind höchstens vier Mopeka-Sensoren möglich";
    Sensor s;
    s.mac = mac;
    for (auto &f : s_found)
      if (f.mac == mac) s.model = f.model;
    MopekaMedium med = (s.model & 0x100) ? MOP_PROPANE : medium_of(s.model);
    if (med == MOP_PROPANE) apply_tank(s, tank >= 0 && tank < (int)TANK_N ? tank : 0, 0, 0, 0);
    // Wasser und Messung von oben: keine passende Vorgabe – Startwerte, die man unter "Tank" anpasst
    else if (med == MOP_WATER) apply_tank(s, MOPEKA_TANK_CUSTOM, 20, 400, 0);
    else apply_tank(s, MOPEKA_TANK_CUSTOM, 500, 50, 0);
    s.name = name.empty() ? (med == MOP_PROPANE ? "Gasflasche" : "Tank") : name.substr(0, 32);
    s_sens.push_back(s);
    s_ver++;
  }
  save_config();
  return "";
}

void mopeka_remove(const std::string &mac) {
  {
    Lock l;
    for (size_t i = 0; i < s_sens.size(); i++)
      if (s_sens[i].mac == mac) {
        s_sens.erase(s_sens.begin() + i);
        break;
      }
    s_ver++;
  }
  save_config();
}

void mopeka_rename(const std::string &mac, const std::string &name) {
  {
    Lock l;
    for (auto &s : s_sens)
      if (s.mac == mac && !name.empty()) s.name = name.substr(0, 32);
    s_ver++;
  }
  save_config();
}

std::string mopeka_set_tank(const std::string &mac, int tank, int empty_mm, int full_mm, float capacity) {
  if (tank < 0 || tank >= (int)TANK_N) {
    if (empty_mm < 0 || full_mm < 0 || empty_mm > 3000 || full_mm > 3000) return "Höhe muss zwischen 0 und 3000 mm liegen";
    if (abs(full_mm - empty_mm) < 20) return "Leer und voll liegen zu dicht beieinander";
    if (capacity < 0 || capacity > 2000) return "Ungültiger Inhalt";
  }
  {
    Lock l;
    bool found = false;
    for (auto &s : s_sens)
      if (s.mac == mac) {
        apply_tank(s, tank, empty_mm, full_mm, capacity);
        found = true;
      }
    if (!found) return "Sensor nicht gefunden";
    s_ver++;
  }
  save_config();
  return "";
}
