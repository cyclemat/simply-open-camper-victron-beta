#include "victron.h"
#include "cerbo.h"
#include "bms.h"
#include "victron_ctl.h"
#include "i18n/i18n.h"

#include <Arduino.h>
#include <atomic>
#include <math.h>
#include <Preferences.h>
#include "mbedtls/aes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Aufbau der Herstellerdaten (Victron "Extra Manufacturer Data"):
//  [0..1] Hersteller-ID 0x02E1   [2] 0x10 Produktwerbung   [3] (reserviert)   [4..5] Modell-ID
//  [6] Datensatztyp   [7..8] Nonce/Datenzaehler   [9] erstes Byte des Schluessels   [10..] verschluesselte Daten
static const size_t HDR_LEN = 10;
static const uint32_t DISCOVERY_MS = 3 * 60 * 1000;
static const size_t MAX_FOUND = 20;   // voll: ein staerkeres Signal verdraengt das schwaechste

struct VicDev {
  std::string mac, name;
  uint8_t key[16] = {0};
  uint16_t last_iv = 0;
  bool have_iv = false;
  uint32_t when_ms = 0;
  VictronInfo v;  // letzte Werte (key/mac/name werden in info_of gesetzt)
};

static SemaphoreHandle_t s_mtx;
static std::vector<VicDev> s_devs;
static std::vector<VictronFound> s_found;
static std::atomic<uint32_t> s_ver{1};
static volatile uint32_t s_disc_until = 0;
static volatile bool s_disc = false;
static Preferences s_prefs;

namespace {   // dateilokal: gleichnamige Typen anderer Dateien duerfen sich nicht vermischen
struct Lock {
  Lock() { xSemaphoreTakeRecursive(s_mtx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(s_mtx); }
};
}  // namespace

static std::string lower(std::string s) {
  for (auto &c : s) c = tolower((unsigned char)c);
  return s;
}

static bool parse_key(const std::string &hex_in, uint8_t out[16]) {
  std::string hex;
  for (char c : hex_in)
    if (!isspace((unsigned char)c) && c != ':' && c != '-') hex += (char)tolower((unsigned char)c);
  if (hex.size() != 32) return false;
  for (int i = 0; i < 16; i++) {
    int v = 0;
    for (int k = 0; k < 2; k++) {
      char c = hex[i * 2 + k];
      int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
      if (d < 0) return false;
      v = v * 16 + d;
    }
    out[i] = (uint8_t)v;
  }
  return true;
}

static std::string key_hex(const uint8_t k[16]) {
  char b[33];
  for (int i = 0; i < 16; i++) snprintf(b + i * 2, 3, "%02x", k[i]);
  return b;
}

VicKind victron_kind_of_record(uint8_t record) {
  switch (record) {
    case 0x01: return VIC_SOLAR;
    case 0x02: return VIC_SHUNT;
    case 0x08: return VIC_CHARGER;   // Blue Smart IP22/IP65/IP67, Phoenix Smart Charger
    case 0x0D: return VIC_DCSYS;     // SmartShunt als DC-Energiezaehler (Verbraucher oder Quelle)
    case 0x04:
    case 0x0F: return VIC_DCDC;
    case 0x05: return VIC_LITHIUM;   // Smart Lithium
    case 0x09: return VIC_PROTECT;   // Smart BatteryProtect
    default: return VIC_UNKNOWN;
  }
}

// ---------------------------------------------------------------- Speicher
static void save_config() {
  Lock l;
  s_prefs.begin("victron", false);
  s_prefs.clear();
  s_prefs.putUChar("n", s_devs.size());
  for (size_t i = 0; i < s_devs.size(); i++) {
    char k[4];
    snprintf(k, sizeof(k), "m%u", (unsigned)i); s_prefs.putString(k, s_devs[i].mac.c_str());
    snprintf(k, sizeof(k), "k%u", (unsigned)i); s_prefs.putString(k, key_hex(s_devs[i].key).c_str());
    snprintf(k, sizeof(k), "a%u", (unsigned)i); s_prefs.putString(k, s_devs[i].name.c_str());
  }
  s_prefs.end();
}

static void load_config() {
  Lock l;
  s_devs.clear();
  s_prefs.begin("victron", true);
  int n = s_prefs.getUChar("n", 0);
  for (int i = 0; i < n && i < VICTRON_MAX_DEVICES; i++) {
    char k[4];
    VicDev d;
    snprintf(k, sizeof(k), "m%d", i); d.mac = s_prefs.getString(k, "").c_str();
    snprintf(k, sizeof(k), "k%d", i); std::string kh = s_prefs.getString(k, "").c_str();
    snprintf(k, sizeof(k), "a%d", i); d.name = s_prefs.getString(k, "Victron").c_str();
    if (d.mac.empty() || !parse_key(kh, d.key)) continue;
    d.v.status = "Warte auf Daten …";
    s_devs.push_back(d);
  }
  s_prefs.end();
}

// ---------------------------------------------------------------- Entschluesseln und Auswerten
// AES-128-CTR; Zaehlerblock = Nonce little-endian, Rest 0
static bool decrypt(const uint8_t key[16], uint16_t iv, const uint8_t *in, uint8_t *out, size_t n) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
    mbedtls_aes_free(&ctx);
    return false;
  }
  uint8_t ctr[16] = {0}, ks[16];
  ctr[0] = iv & 0xFF;
  ctr[1] = iv >> 8;
  for (size_t off = 0; off < n; off += 16) {
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, ctr, ks);
    for (size_t i = 0; i < 16 && off + i < n; i++) out[off + i] = in[off + i] ^ ks[i];
    for (int i = 0; i < 16; i++)
      if (++ctr[i]) break;
  }
  mbedtls_aes_free(&ctx);
  return true;
}

// Liest Bitfelder, niederwertigstes Bit zuerst (wie Victron sie packt)
struct BitReader {
  const uint8_t *d;
  size_t n, pos = 0;
  BitReader(const uint8_t *data, size_t len) : d(data), n(len) {}
  uint32_t u(int bits) {
    uint32_t v = 0;
    for (int i = 0; i < bits; i++, pos++) {
      size_t byte = pos >> 3;
      if (byte < n && ((d[byte] >> (pos & 7)) & 1)) v |= 1u << i;
    }
    return v;
  }
  int32_t s(int bits) {
    uint32_t v = u(bits);
    if (v & (1u << (bits - 1))) v |= ~((1u << bits) - 1);
    return (int32_t)v;
  }
};

static float s16(int32_t raw, float scale) { return raw == 0x7FFF ? NAN : raw * scale; }
static float u16(uint32_t raw, float scale) { return raw == 0xFFFF ? NAN : raw * scale; }

static bool parse_record(uint8_t record, const uint8_t *p, size_t n, VictronInfo &v) {
  BitReader b(p, n);
  switch (record) {
    case 0x01: {  // Solarregler
      if (n < 12) return false;
      v.state = b.u(8);
      v.error = b.u(8);
      v.bat_v = s16(b.s(16), 0.01f);
      v.bat_i = s16(b.s(16), 0.1f);
      v.yield_kwh = u16(b.u(16), 0.01f);
      v.pv_w = u16(b.u(16), 1.0f);
      uint32_t load = b.u(9);
      v.load_i = load == 0x1FF ? NAN : load * 0.1f;
      return true;
    }
    case 0x02: {  // Batteriemonitor (SmartShunt, BMV)
      if (n < 15) return false;
      uint32_t rem = b.u(16);
      v.remaining_min = rem == 0xFFFF ? -1 : (int)rem;
      v.bat_v = s16(b.s(16), 0.01f);
      v.alarm = b.u(16);
      uint32_t aux = b.u(16);
      v.aux_mode = b.u(2);
      int32_t cur = b.s(22);
      v.bat_i = cur == 0x1FFFFF ? NAN : cur * 0.001f;
      uint32_t cons = b.u(20);
      v.consumed_ah = cons == 0xFFFFF ? NAN : cons * 0.1f;
      uint32_t soc = b.u(10);
      v.soc = soc == 0x3FF ? NAN : soc * 0.1f;
      if (v.aux_mode == 0) v.aux = s16((int16_t)aux, 0.01f);
      else if (v.aux_mode == 1) v.aux = u16(aux, 0.01f);
      else if (v.aux_mode == 2) v.aux = aux == 0xFFFF ? NAN : aux * 0.01f - 273.15f;
      else v.aux = NAN;
      return true;
    }
    case 0x04: {  // Orion Smart (DC/DC)
      if (n < 6) return false;
      v.state = b.u(8);
      v.error = b.u(8);
      v.in_v = u16(b.u(16), 0.01f);
      v.bat_v = s16(b.s(16), 0.01f);  // Ausgang
      v.off_reason = b.u(32);
      v.bat_i = NAN;
      v.in_i = NAN;
      return true;
    }
    case 0x08: {  // AC-Ladegeraet (Blue Smart / Phoenix Smart Charger): bis zu 3 Ausgaenge
      if (n < 13) return false;
      v.state = b.u(8);
      v.error = b.u(8);
      float sum_i = NAN;
      for (int k = 0; k < 3; k++) {
        uint32_t ov = b.u(13), oi = b.u(11);
        float fv = ov == 0x1FFF ? NAN : ov * 0.01f, fi = oi == 0x7FF ? NAN : oi * 0.1f;
        if (k == 0) v.bat_v = fv;
        if (!isnan(fi)) sum_i = isnan(sum_i) ? fi : sum_i + fi;
      }
      v.bat_i = sum_i;
      uint32_t t = b.u(7);
      v.aux_mode = t == 0x7F ? 3 : 2;
      v.aux = t == 0x7F ? NAN : (float)t - 40.0f;
      uint32_t ac = b.u(9);
      v.ac_i = ac == 0x1FF ? NAN : ac * 0.1f;
      v.power_w = (!isnan(v.bat_v) && !isnan(v.bat_i)) ? v.bat_v * v.bat_i : NAN;
      return true;
    }
    case 0x0D: {  // DC-Energiezaehler (SmartShunt im Modus "DC-Energiezaehler")
      if (n < 13) return false;
      v.meter = (int16_t)b.u(16);
      v.bat_v = s16(b.s(16), 0.01f);
      v.alarm = b.u(16);
      uint32_t aux = b.u(16);
      v.aux_mode = b.u(2);
      int32_t cur = b.s(22);
      v.bat_i = cur == 0x1FFFFF ? NAN : cur * 0.001f;
      if (v.aux_mode == 0) v.aux = s16((int16_t)aux, 0.01f);
      else if (v.aux_mode == 2) v.aux = aux == 0xFFFF ? NAN : aux * 0.01f - 273.15f;
      else v.aux = NAN;
      v.power_w = (!isnan(v.bat_v) && !isnan(v.bat_i)) ? v.bat_v * v.bat_i : NAN;
      return true;
    }
    case 0x05: {  // Smart Lithium (nach keshavdv/victron-ble, Unlicense): BMS-Bits, Fehler, 8 Zellen je 7 Bit, Spannung, Balancer, Temp.
      if (n < 16) return false;
      b.u(32);   // BMS-Flags
      v.alarm = b.u(16);
      for (int k = 0; k < 8; k++) {
        uint32_t c = b.u(7);   // 0 = unter 2,61 V, 0x7E = ueber 3,85 V, 0x7F = keine Zelle
        v.cell_v[k] = c == 0x7F ? NAN : 2.60f + c * 0.01f;
      }
      uint32_t bv = b.u(12);
      v.bat_v = bv == 0xFFF ? NAN : bv * 0.01f;
      v.balancer = b.u(4);
      uint32_t t = b.u(7);
      v.aux_mode = t == 0x7F ? 3 : 2;
      v.aux = t == 0x7F ? NAN : (float)t - 40.0f;
      v.bat_i = NAN;
      v.soc = NAN;
      return true;
    }
    case 0x09: {  // Smart BatteryProtect (nach keshavdv/victron-ble, Unlicense): Zustand, Ausgang, Fehler, Alarm, Warnung, Ein-/Ausgang V
      if (n < 15) return false;
      v.state = b.u(8);
      uint32_t out = b.u(8);
      v.output = out == 1 ? 1 : out == 4 ? 0 : -1;
      uint32_t err = b.u(8);
      v.error = err == 0xFF ? -1 : (int)err;
      v.alarm = b.u(16);
      b.u(16);   // Warnung
      v.in_v = s16(b.s(16), 0.01f);
      v.bat_v = u16(b.u(16), 0.01f);   // Ausgang
      v.off_reason = b.u(32);
      v.bat_i = NAN;
      return true;
    }
    case 0x0F: {  // Orion XS
      if (n < 10) return false;
      v.state = b.u(8);
      v.error = b.u(8);
      v.bat_v = s16(b.s(16), 0.01f);  // Ausgang
      v.bat_i = s16(b.s(16), 0.1f);
      v.in_v = u16(b.u(16), 0.01f);
      v.in_i = u16(b.u(16), 0.1f);
      v.off_reason = b.u(32);
      return true;
    }
  }
  return false;
}

void victron_on_adv(const std::string &mac_in, const uint8_t *md, size_t len, int rssi, const std::string &adv_name) {
  if (len < HDR_LEN + 1 || md[0] != 0xE1 || md[1] != 0x02 || md[2] != 0x10) return;
  uint16_t model = md[4] | (md[5] << 8);
  uint8_t record = md[6];
  uint16_t iv = md[7] | (md[8] << 8);
  std::string mac = lower(mac_in);
  Lock l;
  if (s_disc) {
    if ((int32_t)(millis() - s_disc_until) >= 0) {
      s_disc = false;
      s_ver++;
    } else {
      bool known = false;
      for (auto &f : s_found)
        if (f.mac == mac) {
          known = true;
          f.rssi = rssi;
          if (f.name.empty() && !adv_name.empty()) {
            f.name = adv_name;
            s_ver++;
          }
        }
      if (!known && s_found.size() >= MAX_FOUND) {   // voll: schwaechstes Signal ersetzen, wenn das neue staerker ist
        size_t w = 0;
        for (size_t i = 1; i < s_found.size(); i++)
          if (s_found[i].rssi < s_found[w].rssi) w = i;
        if (rssi > s_found[w].rssi) s_found.erase(s_found.begin() + w);
      }
      if (!known && s_found.size() < MAX_FOUND) {
        VictronFound f;
        f.mac = mac;
        f.name = adv_name;
        f.model = model;
        f.record = record;
        f.rssi = rssi;
        s_found.push_back(f);
        s_ver++;
      }
    }
  }
  for (auto &d : s_devs) {
    if (d.mac != mac) continue;
    d.v.rssi = rssi;
    d.v.model = model;
    d.v.record = record;
    d.v.kind = victron_kind_of_record(record);
    if (d.have_iv && iv == d.last_iv && d.v.has) {
      d.when_ms = millis();  // gleiche Daten wie zuletzt – nur "lebt noch" merken
      return;
    }
    if (md[9] != d.key[0]) {
      if (d.v.status != "Schlüssel passt nicht") {
        d.v.status = "Schlüssel passt nicht";
        s_ver++;
      }
      return;
    }
    uint8_t dec[32];
    size_t n = len - HDR_LEN;
    if (n > sizeof(dec)) n = sizeof(dec);
    if (!decrypt(d.key, iv, md + HDR_LEN, dec, n)) return;
    VictronInfo nv = d.v;
    if (!parse_record(record, dec, n, nv)) {
      char b[64];
      snprintf(b, sizeof(b), tr("Gerätetyp 0x%02X wird nicht unterstützt"), record);
      if (d.v.status != b) {
        d.v.status = b;
        s_ver++;
      }
      return;
    }
    nv.has = true;
    nv.status = "OK";
    d.v = nv;
    d.last_iv = iv;
    d.have_iv = true;
    d.when_ms = millis();
    s_ver++;
    return;
  }
}

// ---------------------------------------------------------------- oeffentliche API
void victron_begin() {
  s_mtx = xSemaphoreCreateRecursiveMutex();
  load_config();
  log_i("Victron: %u Gerät(e) eingerichtet", (unsigned)s_devs.size());
}

uint32_t victron_version() { return s_ver + cerbo_version(); }

bool victron_has_devices() {
  Lock l;
  return !s_devs.empty();
}

static VictronInfo info_of(const VicDev &d) {
  VictronInfo v = d.v;
  v.key = VICTRON_KEY_PREFIX + d.mac;
  v.mac = d.mac;
  v.name = d.name;
  v.age_s = d.when_ms ? (int32_t)((millis() - d.when_ms) / 1000) : -1;
  vctl_fill(v);   // Orion: per Bluetooth schaltbar (victron_ctl.cpp, nur Panels)
  return v;
}

std::vector<VictronInfo> victron_list() {
  std::vector<VictronInfo> out;
  Lock l;
  for (auto &d : s_devs) out.push_back(info_of(d));
  return out;
}

// Geraet aus bms.cpp als Victron-Kachel darstellen (Kachel, Detailfenster, Editor): Akku/Shunt wie ein SmartShunt,
// Renogy-Laderegler als Solarregler, DC-DC-Lader als Ladebooster, Wechselrichter als Wechselrichter
bool victron_from_bms(const BmsInfo &b, VictronInfo &v) {
  v = VictronInfo();
  v.key = b.key;
  v.mac = b.mac;
  v.name = b.name;
  v.bms = true;
  v.has = b.has;
  v.age_s = b.age_s;
  v.status = b.status;
  v.bat_v = b.volt;
  v.bat_i = b.amp;
  v.power_w = isfinite(b.volt) && isfinite(b.amp) ? b.volt * b.amp : NAN;
  v.state = b.chg_state;
  switch (b.dev) {
    case BMS_DEV_SOLAR:
      v.kind = VIC_SOLAR;
      v.record = 0x01;
      v.pv_w = b.pv_w;
      v.yield_kwh = b.yield_wh >= 0 ? b.yield_wh / 1000.0f : NAN;
      v.load_i = b.load_a;
      return true;
    case BMS_DEV_DCDC:
      v.kind = VIC_DCDC;
      v.record = 0x04;
      v.in_v = b.in_v;
      v.in_i = b.in_a;
      v.pv_w = b.pv_w;
      v.yield_kwh = b.yield_wh >= 0 ? b.yield_wh / 1000.0f : NAN;
      return true;
    case BMS_DEV_PSTATION:
      v.kind = VIC_PSTATION;
      v.soc = b.soc >= 0 ? (float)b.soc : NAN;
      v.ps_in_w = b.ps_in_w;
      v.ps_out_w = b.ps_out_w;
      v.ps_ac_in_w = b.ps_ac_in_w;
      v.ps_dc_in_w = b.ps_dc_in_w;
      v.ps_ac_out_w = b.ps_ac_out_w;
      v.ps_dc_out_w = b.ps_dc_out_w;
      v.ac_on = b.ac_on;
      v.dc_on = b.dc_on;
      v.can_ac = b.can_ac;
      v.can_dc = b.can_dc;
      v.remaining_min = b.remain_min;
      v.power_w = isfinite(b.ps_in_w) && isfinite(b.ps_out_w) ? b.ps_in_w - b.ps_out_w : NAN;
      return true;
    case BMS_DEV_INVERTER:
      v.kind = VIC_INVERTER;
      v.ac_v = b.ac_v;
      v.ac_i = b.ac_a;
      v.ac_w = b.ac_w;
      v.power_w = b.ac_w;
      return true;
    case BMS_DEV_CLIMATE:   // Ruuvi
      v.kind = VIC_CLIMATE;
      v.aux = b.temp;
      v.aux_mode = 2;
      v.hum = b.hum;
      v.press = b.press;
      v.pm25 = b.pm25;
      v.co2 = b.co2;
      v.soc = b.soc >= 0 ? (float)b.soc : NAN;   // Batterie % (SwitchBot, BTHome)
      v.note = b.note;
      v.power_w = NAN;
      return true;
    case BMS_DEV_HEATER:
      v.kind = VIC_HEATER;
      v.aux = b.temp;
      v.ht_on = b.ht_on; v.ht_step = b.ht_step; v.ht_mode = b.ht_mode; v.ht_level = b.ht_level;
      v.ht_set = b.ht_set; v.ht_err = b.ht_err; v.ht_case = b.ht_case;
      v.power_w = NAN;
      return true;
    case BMS_DEV_FRIDGE:
      v.kind = VIC_FRIDGE;
      v.fr_on = b.fr_on; v.fr_eco = b.fr_eco; v.fr_zones = b.fr_zones; v.fr_min = b.fr_min; v.fr_max = b.fr_max;
      for (int z = 0; z < 2; z++) v.fr_set[z] = b.fr_set[z], v.fr_cur[z] = b.fr_cur[z];
      v.aux = b.fr_zones ? (float)b.fr_cur[0] : NAN;
      v.soc = b.soc >= 0 ? (float)b.soc : NAN;
      v.power_w = NAN;
      return true;
    case BMS_DEV_TANK:   // SuperSense-Tankmodul
      v.kind = VIC_TANK;
      for (int k = 0; k < 4; k++) {
        v.tank_pct[k] = b.tank_pct[k];
        v.tank_temp[k] = b.tank_temp[k];
      }
      return true;
    default:
      break;
  }
  v.kind = VIC_SHUNT;
  v.record = 0x02;
  v.soc = b.soc >= 0 ? (float)b.soc : NAN;
  v.consumed_ah = NAN;
  v.remaining_min = b.ttg_s > 0 ? b.ttg_s / 60 : -1;
  if (isfinite(b.starter_v) && b.starter_v > 1) {   // Renogy Smart Shunt: Starterbatterie
    v.aux_mode = 0;
    v.aux = b.starter_v;
  } else {
    v.aux_mode = isfinite(b.temp) ? 2 : 3;
    v.aux = b.temp;
  }
  v.alarm = b.ok ? 0 : 1;
  return true;
}

bool victron_get(const std::string &key, VictronInfo &out) {
  if (cerbo_is_key(key)) return cerbo_get(key, out);
  if (key.rfind(BMS_KEY_PREFIX, 0) == 0) {
    BmsInfo b;
    return bms_get(key, b) && victron_from_bms(b, out);
  }
  if (!victron_is_key(key)) return false;
  std::string mac = key.substr(strlen(VICTRON_KEY_PREFIX));
  Lock l;
  for (auto &d : s_devs)
    if (d.mac == mac) {
      out = info_of(d);
      return true;
    }
  return false;
}

void victron_discovery_start() {
  Lock l;
  s_found.clear();
  s_disc_until = millis() + DISCOVERY_MS;
  s_disc = true;
  s_ver++;
}

bool victron_discovery_running() {
  if (s_disc && (int32_t)(millis() - s_disc_until) >= 0) {
    s_disc = false;
    s_ver++;
  }
  return s_disc;
}

std::vector<VictronFound> victron_found() {
  Lock l;
  std::vector<VictronFound> out = s_found;
  for (auto &f : out) {
    f.configured = false;
    for (auto &d : s_devs) f.configured |= d.mac == f.mac;
  }
  return out;
}

std::string victron_add(const std::string &mac_in, const std::string &kh, const std::string &name) {
  std::string mac = lower(mac_in);
  if (mac.size() != 17) return "Ungültige Bluetooth-Adresse";
  VicDev d;
  if (!parse_key(kh, d.key)) return "Der Schlüssel muss 32 Zeichen lang sein (0–9, a–f)";
  {
    Lock l;
    for (auto &o : s_devs)
      if (o.mac == mac) return "Dieses Gerät ist schon eingerichtet";
    if (s_devs.size() >= VICTRON_MAX_DEVICES) return "Es sind höchstens sechs Victron-Geräte möglich";
    d.mac = mac;
    for (auto &f : s_found)
      if (f.mac == mac) {
        d.v.record = f.record;
        d.v.kind = victron_kind_of_record(f.record);
        d.v.model = f.model;
      }
    d.name = name.empty() ? (d.v.kind != VIC_UNKNOWN ? victron_kind_text(d.v.kind) : "Victron") : name.substr(0, 32);
    d.v.status = "Warte auf Daten …";
    s_devs.push_back(d);
    s_ver++;
  }
  save_config();
  return "";
}

std::string victron_set_key(const std::string &mac, const std::string &kh) {
  uint8_t k[16];
  if (!parse_key(kh, k)) return "Der Schlüssel muss 32 Zeichen lang sein (0–9, a–f)";
  {
    Lock l;
    for (auto &d : s_devs)
      if (d.mac == mac) {
        memcpy(d.key, k, 16);
        d.have_iv = false;
        d.v.has = false;
        d.v.status = "Warte auf Daten …";
      }
    s_ver++;
  }
  save_config();
  return "";
}

void victron_remove(const std::string &mac) {
  {
    Lock l;
    for (size_t i = 0; i < s_devs.size(); i++)
      if (s_devs[i].mac == mac) {
        s_devs.erase(s_devs.begin() + i);
        break;
      }
    s_ver++;
  }
  save_config();
}

void victron_rename(const std::string &mac, const std::string &name) {
  if (name.empty()) return;
  {
    Lock l;
    for (auto &d : s_devs)
      if (d.mac == mac) d.name = name.substr(0, 32);
    s_ver++;
  }
  save_config();
}

// Art des DC-Energiezaehlers (VictronConnect: Einstellungen > Allgemein > Monitor-Modus)
const char *victron_meter_text(int m) {
  switch (m) {
    case -9: return "Solarregler";
    case -8: return "Windgenerator";
    case -7: return "Wellengenerator";
    case -6: return "Lichtmaschine";
    case -5: return "Brennstoffzelle";
    case -4: return "Wassergenerator";
    case -3: return "DC/DC-Ladegerät";
    case -2: return "AC-Ladegerät";
    case -1: return "Quelle";
    case 1: return "Verbraucher";
    case 2: return "Elektroantrieb";
    case 3: return "Kühlschrank";
    case 4: return "Wasserpumpe";
    case 5: return "Bilgepumpe";
    case 6: return "DC-System";
    case 7: return "Wechselrichter";
    case 8: return "Warmwasser";
    default: return "";
  }
}

const char *victron_kind_text(VicKind k) {
  switch (k) {
    case VIC_SOLAR: return "Solarregler";
    case VIC_SHUNT: return "Batteriemonitor";
    case VIC_DCDC: return "Ladebooster";
    case VIC_GRID: return "Netz";
    case VIC_CHARGER: return "Ladegerät";
    case VIC_DCSYS: return "DC-System";
    case VIC_INVERTER: return "Wechselrichter";
    case VIC_PSTATION: return "Power Station";
    case VIC_PROTECT: return "BatteryProtect";
    case VIC_LITHIUM: return "Lithium-Akku";
    default: return "Victron-Gerät";
  }
}

const char *victron_state_text(int s) {
  switch (s) {
    case 0: return "Aus";
    case 1: return "Sparbetrieb";
    case 2: return "Fehler";
    case 3: return "Bulk";
    case 4: return "Absorption";
    case 5: return "Erhaltung";
    case 6: return "Lagerung";
    case 7: return "Ausgleichsladung";
    case 8: return "Durchleitung";
    case 9: return "Wechselrichter";
    case 10: return "Power Assist";
    case 11: return "Netzteil";
    case 245: return "Startet";
    case 246: return "Wiederholte Absorption";
    case 247: return "Auto-Ausgleich";
    case 248: return "BatterySafe";
    case 252: return "Externe Steuerung";
    default: return s < 0 ? "–" : "Unbekannt";
  }
}
