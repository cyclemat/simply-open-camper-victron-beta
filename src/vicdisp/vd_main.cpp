// Victron-Display – siehe vd_main.h
#include "vd_main.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include "../updater_link.h"
#include <WebServer.h>
#include <WiFi.h>
#include <algorithm>
#include <math.h>
#include <vector>
// Weboberflaeche: Deutsch aus vd_page.h, Englisch als erzeugte Kopie (tools/i18n_page.py, Woerterbuch
// tools/i18n/vicdisp_en.json) – Umgebung esp32-cyd-victron-en setzt VD_PAGE_EN
#if VD_PAGE_EN
#include "../generated/vd_page_en.h"
#else
#include "vd_page.h"
#endif
#include "../i18n/i18n.h"
#include "../cerbo.h"
#include "../victron.h"
#include "../truma.h"
#include "../mopeka.h"
#include "../sat/sat_display.h"
#include "../devsetup.h"
#include "../gx_find.h"
#include "../bms.h"

#include "../soc_version.h"
#define VD_FW_VERSION SOC_VERSION   // gemeinsame Versionsnummer (soc_version.h)

static const uint32_t AP_AFTER_MS = 30000;   // so lange ohne WLAN: Einrichtungs-WLAN dazuschalten
static const uint32_t AP_OFF_MS = 120000;    // verbunden und niemand im Einrichtungs-WLAN: abschalten

namespace {
struct VdConfig {
  String ssid, pass, name;
  bool cerbo_on = false;    // Cerbo GX benutzen – ab Werk aus (vielleicht gibt es nur Bluetooth-Geraete)
  bool cerbo_auto = true;   // Cerbo GX automatisch suchen (172.24.24.1, Gateway, venus.local / einstein.local)
  String chost;             // Adresse des GX (eingetragen oder zuletzt gefunden)
  uint8_t led = 1;          // RGB-LED Rueckseite: 0 aus, 1 automatisch (Batterie, sonst Gas), 2 Batterie, 3 Gas
};
VdConfig s_cfg;
String s_id;   // "SOC-Victron-AABBCC"
WebServer s_srv(80);
uint32_t s_lost_ms = 0, s_conn_ms = 0, s_probe_ms = 0;
bool s_mdns = false;
bool s_upl_ok = false;          // Firmware-Upload nur angemeldet

void load_cfg() {
  Preferences p;
  p.begin("vicdisp", true);
  s_cfg.ssid = p.getString("ssid", "");
  s_cfg.pass = p.getString("pass", "");
  s_cfg.name = p.getString("name", "");
  s_cfg.cerbo_on = p.getBool("con", false);
  s_cfg.cerbo_auto = p.getBool("cauto", true);
  s_cfg.chost = p.getString("chost", "");
  s_cfg.led = p.getUChar("led", 1);
  p.end();
}

void save_cfg() {
  Preferences p;
  p.begin("vicdisp", false);
  p.putString("ssid", s_cfg.ssid);
  p.putString("pass", s_cfg.pass);
  p.putString("name", s_cfg.name);
  p.putBool("con", s_cfg.cerbo_on);
  p.putBool("cauto", s_cfg.cerbo_auto);
  p.putString("chost", s_cfg.chost);
  p.putUChar("led", s_cfg.led);
  p.end();
}

// ---------------------------------------------------------------- RGB-LED (Rueckseite des CYD)
// Ampel fuer Batterie (Ladezustand) oder Gas (Truma LevelControl/Mopeka, niedrigste Flasche):
// bis 10 % rot, bis 50 % orange, darueber gruen; ohne Wert aus. Gemeinsame Anode -> Low = an.
// Nur LEDC-PWM (3 Kanaele), keine eigene Task: Auswertung alle 2 s aus loop().
#ifndef VD_LED_R
#define VD_LED_R 4
#define VD_LED_G 16
#define VD_LED_B 17
#endif
static const int LED_LOW = 10, LED_MID = 50;   // Grenzen in Prozent
bool s_led_init = false;
int s_led_pct = -1;          // angezeigter Wert, -1 = keiner
char s_led_src = 0;          // 'b' Batterie, 'g' Gas
uint32_t s_led_ms = 0;
uint32_t s_led_rgb = 0xFFFFFFFF;

void led_write(uint32_t rgb) {
  if (rgb == s_led_rgb) return;
  if (!s_led_init) {
    ledcAttach(VD_LED_R, 5000, 8);
    ledcAttach(VD_LED_G, 5000, 8);
    ledcAttach(VD_LED_B, 5000, 8);
    s_led_init = true;
  }
  s_led_rgb = rgb;
  // bei Dunkelheit (Lichtsensor, Helligkeit-Automatik an) auf rund ein Achtel gedimmt
  auto ch = [](uint32_t v) -> uint32_t { return sat_display_night() ? (v + 7) / 8 : v; };
  ledcWrite(VD_LED_R, 255 - ch((rgb >> 16) & 0xFF));
  ledcWrite(VD_LED_G, 255 - ch((rgb >> 8) & 0xFF));
  ledcWrite(VD_LED_B, 255 - ch(rgb & 0xFF));
}

// niedrigster Ladezustand aller Batteriemonitore (Bluetooth und Cerbo GX), -1 = keiner
int led_battery() {
  float best = -1;
  auto take = [&](const VictronInfo &v) {
    if (v.kind != VIC_SHUNT || v.record == 0x0D || !v.has || !isfinite(v.soc) || v.soc < 0 || v.soc > 100) return;
    if (!v.gx && (v.age_s < 0 || v.age_s > 600)) return;   // Bluetooth: Wert aelter als 10 min zaehlt nicht
    if (best < 0 || v.soc < best) best = v.soc;
  };
  for (auto &v : victron_list()) take(v);
  if (s_cfg.cerbo_on)
    for (auto &v : cerbo_list()) take(v);
  for (auto &b : bms_list())   // Akkus mit eigenem BMS (Super B …) und Shunts: nur frische Werte
    if ((b.dev == BMS_DEV_BATTERY || b.dev == BMS_DEV_SHUNT) && b.has && b.soc >= 0 && b.soc <= 100 && b.age_s >= 0 && b.age_s <= 600 && (best < 0 || b.soc < best)) best = b.soc;
  return best < 0 ? -1 : (int)lroundf(best);
}

// niedrigster Fuellstand aller Gasflaschen mit Messwert (Truma LevelControl, Mopeka mit Propan), -1 = keiner
int led_gas() {
  int best = -1;
  for (auto &t : truma_list())
    if (t.has_reading && t.percent >= 0 && (best < 0 || t.percent < best)) best = t.percent;
  for (auto &m : mopeka_list())   // Mopeka: nur Gas und nur frische Werte (sendet etwa jede Sekunde)
    if (m.medium == MOP_PROPANE && m.has && m.age_s >= 0 && m.age_s <= 600 && (best < 0 || m.percent < best)) best = m.percent;
  return best;
}

void led_loop() {
  if (s_led_ms && millis() - s_led_ms < 2000) return;
  s_led_ms = millis();
  int pct = -1;
  char src = 0;
  if (s_cfg.led == 1 || s_cfg.led == 2) {
    pct = led_battery();
    src = 'b';
  }
  if ((s_cfg.led == 1 && pct < 0) || s_cfg.led == 3) {
    pct = led_gas();
    src = 'g';
  }
  if (pct < 0) src = 0;
  s_led_pct = pct;
  s_led_src = src;
  if (pct < 0) {
    if (s_led_init) led_write(0);
    return;
  }
  uint32_t rgb = pct <= LED_LOW ? 0xFF0000 : pct <= LED_MID ? 0xFF3C00 : 0x00A000;   // rot / orange / gruen
  static bool night = false;   // Wechsel hell/dunkel: neu schreiben, auch wenn die Farbe gleich bleibt
  if (night != sat_display_night()) {
    night = !night;
    s_led_rgb = 0xFFFFFFFF;
  }
  led_write(rgb);
}

// ---------------------------------------------------------------- WLAN
// Einrichtungs-WLAN: 4.3.2.1 mit Captive Portal und eigenem Passwort (devsetup)
void ap_on() {
  if (WiFi.getMode() & WIFI_AP) return;
  WiFi.mode(s_cfg.ssid.isEmpty() ? WIFI_AP : WIFI_AP_STA);
  devsetup::ap_start(s_id);
}

void ap_off() {
  if (!(WiFi.getMode() & WIFI_AP)) return;
  devsetup::ap_stop();
  WiFi.mode(WIFI_STA);
}

void wifi_begin() {
  WiFi.persistent(false);
  WiFi.setHostname(s_id.c_str());
  if (s_cfg.ssid.isEmpty()) {
    ap_on();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(s_cfg.ssid.c_str(), s_cfg.pass.c_str());
  s_lost_ms = millis();
  if (devsetup::force_ap()) ap_on();   // Mehrfach-Reset: Einrichtungs-WLAN sofort
}

void wifi_loop() {
  if (s_cfg.ssid.isEmpty()) return;
  uint32_t now = millis();
  if (WiFi.isConnected()) {
    if (!s_conn_ms) {
      s_conn_ms = now;
      if (!s_mdns) s_mdns = MDNS.begin(s_id.c_str());
      if (s_mdns) MDNS.addService("http", "tcp", 80);
    }
    s_lost_ms = 0;
    if (!devsetup::force_ap() && (WiFi.getMode() & WIFI_AP) && now - s_conn_ms > AP_OFF_MS && WiFi.softAPgetStationNum() == 0) ap_off();
  } else {
    s_conn_ms = 0;
    if (!s_lost_ms) s_lost_ms = now;
    if (now - s_lost_ms > AP_AFTER_MS) ap_on();
  }
}

// ---------------------------------------------------------------- Cerbo GX
// Aus: keine Adresse im Cerbo-Modul -> kein Client, kein Task, keine Kacheln.
void cerbo_apply() {
  CerboConfig c = cerbo_config();
  std::string want = s_cfg.cerbo_on ? std::string(s_cfg.chost.c_str()) : std::string();
  if (c.host == want) return;
  c.host = want;
  c.portal.clear();
  cerbo_set_config(c);
}

// automatisch finden: gemeinsames Modul gx_find (172.24.24.1, Gateway, venus/einstein.local, dann das eigene Netz;
// jeder Treffer wird per MQTT als GX bestaetigt). Fund kommt aus der Such-Task -> hier nur merken, in loop() uebernehmen.
char s_found_host[48] = "";
volatile bool s_found_new = false;

void cerbo_find() {
  if (s_found_new) {
    s_found_new = false;
    if (s_cfg.chost != s_found_host) {
      s_cfg.chost = s_found_host;
      save_cfg();
    }
    cerbo_apply();
  }
  if (!s_cfg.cerbo_on || !s_cfg.cerbo_auto || !WiFi.isConnected()) return;
  if (s_cfg.chost.isEmpty()) {   // noch kein GX eingetragen: suchen, hoechstens einmal pro Minute
    uint32_t now = millis();
    if (!gx_find_running() && (!s_probe_ms || now - s_probe_ms > 60000)) {
      s_probe_ms = now | 1;
      gx_find_start();
    }
    return;
  }
  gx_find_watch();   // eingetragener GX 3 Minuten nicht erreichbar (neue IP?) -> neu suchen
}

// ---------------------------------------------------------------- Weboberflaeche
void send_json(JsonDocument &d) {
  String s;
  serializeJson(d, s);
  s_srv.send(200, "application/json", s);
}
void send_ok() { s_srv.send(200, "application/json", "{\"ok\":true}"); }
void send_err(const char *e) {
  JsonDocument d;
  d["error"] = tr(e);
  String s;
  serializeJson(d, s);
  s_srv.send(400, "application/json", s);
}
bool body(JsonDocument &d) {
  if (deserializeJson(d, s_srv.arg("plain"))) {
    send_err("Ungueltiges JSON");
    return false;
  }
  return true;
}

void h_state() {
  JsonDocument d;
  d["id"] = s_id;
  d["name"] = s_cfg.name;
  d["fw"] = VD_FW_VERSION;
  d["upd_slot"] = (uint32_t)updater_slot_size();   // 0 = alte Speicheraufteilung ohne Updater
  d["upd_ok"] = updater_available();
  d["uptime"] = millis() / 1000;
  d["heap"] = ESP.getFreeHeap();
  sat_display_light(d["light"].to<JsonObject>());
  JsonObject w = d["wifi"].to<JsonObject>();
  w["ssid"] = s_cfg.ssid;
  w["connected"] = WiFi.isConnected();
  w["ip"] = WiFi.isConnected() ? WiFi.localIP().toString() : "";
  w["rssi"] = WiFi.isConnected() ? WiFi.RSSI() : 0;
  w["ap"] = (WiFi.getMode() & WIFI_AP) != 0;
  w["force_ap"] = devsetup::force_ap();
  w["mdns"] = s_mdns ? String("http://") + s_id + ".local/" : String("");
  JsonObject c = d["cerbo"].to<JsonObject>();
  CerboConfig cc = cerbo_config();
  c["on"] = s_cfg.cerbo_on;
  c["auto"] = s_cfg.cerbo_auto;
  c["host"] = s_cfg.chost;
  c["user"] = cc.user;
  c["has_pass"] = !cc.pass.empty();
  c["portal"] = cerbo_portal_id();
  c["connected"] = cerbo_connected();
  c["status"] = tr_parts(cerbo_status());
  c["find"] = tr_parts(gx_find_status());
  JsonObject l = d["led"].to<JsonObject>();
  l["mode"] = s_cfg.led;
  l["src"] = tr(s_led_src == 'b' ? "Batterie" : s_led_src == 'g' ? "Gas" : "");
  l["pct"] = s_led_pct;
  // Einstellbares am GX (MultiPlus: an Netz/Ladegeraet/Wechselrichter gleich -> nur einmal je Geraeteart)
  JsonArray ctl = c["ctl"].to<JsonArray>();
  bool have[2] = {false, false};
  for (auto &v : cerbo_list()) {
    if (!(v.can_limit || v.can_mode) || v.mode_set > 1 || have[v.mode_set]) continue;
    have[v.mode_set] = true;
    JsonObject o = ctl.add<JsonObject>();
    o["key"] = v.key;
    o["name"] = v.mode_set == 0 ? "MultiPlus" : v.name;
    o["set"] = v.mode_set;
    if (v.can_limit && !isnan(v.ac_limit)) o["limit"] = v.ac_limit;
    if (v.can_mode) o["mode"] = v.mode;
  }

  JsonObject tm = d["truma"].to<JsonObject>();
  tm["interval"] = truma_interval();
  tm["scanning"] = truma_scan_running();
  tm["max"] = TRUMA_MAX_SENSORS;
  JsonArray ts = tm["sensors"].to<JsonArray>();
  for (auto &t : truma_list()) {
    JsonObject o = ts.add<JsonObject>();
    o["mac"] = t.mac;
    o["name"] = t.name;
    o["has"] = t.has_reading;
    o["percent"] = t.percent;
    o["kg"] = t.kg;
    o["battery"] = t.battery;
    o["age"] = t.age_s;
    o["busy"] = t.busy;
    o["status"] = tr_parts(t.status);
  }
  JsonArray tf = tm["found"].to<JsonArray>();
  for (auto &f : truma_scan_results()) {
    JsonObject o = tf.add<JsonObject>();
    o["mac"] = f.mac;
    o["addr_type"] = f.addr_type;
    o["rssi"] = f.rssi;
    o["configured"] = f.configured;
  }

  // Mopeka (Pro Check, Pro Plus, Pro Check Universal/H2O, Standard …): Werbepakete, keine Verbindung
  JsonObject mp = d["mopeka"].to<JsonObject>();
  mp["scanning"] = mopeka_discovery_running();
  mp["max"] = MOPEKA_MAX_SENSORS;
  JsonArray mtk = mp["tanks"].to<JsonArray>();
  for (size_t i = 0; i < mopeka_tank_count(); i++) mtk.add(tr(mopeka_tank(i).name));
  JsonArray ms = mp["sensors"].to<JsonArray>();
  for (auto &m : mopeka_list()) {
    JsonObject o = ms.add<JsonObject>();
    o["mac"] = m.mac;
    o["name"] = m.name;
    o["model"] = m.model_text;
    o["medium"] = (int)m.medium;
    o["tank"] = m.tank;
    o["empty"] = m.empty_mm;
    o["full"] = m.full_mm;
    o["cap"] = m.capacity;
    o["unit"] = m.unit;
    o["has"] = m.has;
    o["percent"] = m.percent;
    if (m.level_mm != MOPEKA_NO_VALUE) o["mm"] = m.level_mm;
    o["amount"] = m.amount;
    o["battery"] = m.battery;
    if (m.temperature != MOPEKA_NO_VALUE) o["temp"] = m.temperature;
    if (m.quality != MOPEKA_NO_VALUE) o["quality"] = m.quality;
    o["age"] = m.age_s;
    o["status"] = tr_parts(m.status);
  }
  JsonArray mf = mp["found"].to<JsonArray>();
  for (auto &f : mopeka_found()) {
    JsonObject o = mf.add<JsonObject>();
    o["mac"] = f.mac;
    o["model"] = f.model_text;
    o["rssi"] = f.rssi;
    o["button"] = f.button;
    o["configured"] = f.configured;
  }


#if WITH_BMS
  JsonObject bm = d["bms"].to<JsonObject>();
  bm["scanning"] = bms_discovery_running();
  bm["max"] = BMS_MAX;
  bm["supersense"] = (bool)BMS_SUPERSENSE_ON;   // SuperSense-Tankmodule (Beta) eingebaut
  // EcoFlow: Benutzer-ID des Kontos (Anmeldung ueber das Internet, Passwort wird nicht gespeichert)
  bm["ecoflow"] = (bool)BMS_ECOFLOW_ON;
  bm["ef_uid_set"] = bms_ef_uid_set();
  bm["ef_uid_hint"] = bms_ef_uid_hint();
  bm["ef_login_state"] = bms_ef_login_state();
  bm["ef_login_text"] = tr_parts(bms_ef_login_text());
  JsonArray bd = bm["devices"].to<JsonArray>();
  for (auto &b : bms_list()) {
    JsonObject o = bd.add<JsonObject>();
    o["mac"] = b.mac;
    o["name"] = b.name;
    o["model"] = b.model_text;
    o["connected"] = b.connected;
    o["has"] = b.has;
    o["age"] = b.age_s;
    o["pin"] = b.pin_set;
    o["pin_need"] = b.pin_need;
    if (b.has) {
      o["soc"] = b.soc;
      o["soh"] = b.soh;
      if (isfinite(b.volt)) o["v"] = b.volt;
      if (isfinite(b.amp)) o["a"] = b.amp;
      o["ttg"] = b.ttg_s;
      o["ok"] = b.ok;
      o["chg"] = b.charging;
      o["dis"] = b.discharging;
      o["low"] = b.low;
      o["chg_ok"] = b.charge_allowed;
      o["dis_ok"] = b.discharge_allowed;
      if (isfinite(b.temp)) o["t"] = b.temp;
      if (b.cycles >= 0) o["cyc"] = b.cycles;
      if (b.alarms) o["alarms"] = b.alarms;
      if (!b.cells_mv.empty()) {
        JsonArray c = o["cells"].to<JsonArray>();
        for (uint16_t mv : b.cells_mv) c.add(mv);
      }
    }
    o["serial"] = b.serial;
    o["fw"] = b.firmware;
    o["status"] = tr_parts(b.status);
    o["dev"] = b.dev;
    o["sum"] = tr_parts(bms_value_text(b));
    // Power Station: Ausgaenge (-1 = unbekannt)
    if (b.can_ac) o["ac"] = b.ac_on;
    if (b.can_dc) o["dc"] = b.dc_on;
    // Dieselheizung / Kuehlbox: Zustand und Sollwerte fuer die Knoepfe
    if (b.dev == BMS_DEV_HEATER) {
      o["hon"] = b.ht_on;
      o["hmode"] = b.ht_mode;
      if (b.ht_level > 0) o["hlv"] = b.ht_level;
      if (b.ht_set > 0) o["hset"] = b.ht_set;
    }
    if (b.dev == BMS_DEV_FRIDGE && b.fr_zones) {
      o["fon"] = b.fr_on;
      o["fset"] = b.fr_set[0];
      if (b.fr_zones > 1) o["fset2"] = b.fr_set[1];
    }
  }
  JsonArray bf = bm["found"].to<JsonArray>();
  for (auto &f : bms_found()) {
    JsonObject o = bf.add<JsonObject>();
    o["mac"] = f.mac;
    o["name"] = f.name;
    o["addr_type"] = f.addr_type;
    o["rssi"] = f.rssi;
    o["model"] = (int)f.model;
    o["service"] = f.service;
    o["product"] = f.product;
    o["cap"] = f.capacity_ah;
    o["cells"] = f.cells;
    o["configured"] = f.configured;
  }
  JsonArray bl = bm["log"].to<JsonArray>();
  for (auto &line : bms_log()) bl.add(line);
#endif

  JsonObject vi = d["victron"].to<JsonObject>();
  vi["scanning"] = victron_discovery_running();
  vi["max"] = VICTRON_MAX_DEVICES;
  JsonArray vs = vi["devices"].to<JsonArray>();
  for (auto &v : victron_list()) {
    JsonObject o = vs.add<JsonObject>();
    o["mac"] = v.mac;
    o["name"] = v.name;
    o["kind_text"] = tr(victron_kind_text(v.kind));
    o["has"] = v.has;
    o["age"] = v.age_s;
    o["status"] = tr_parts(v.status);
    if (v.has) {
      if (!isnan(v.bat_v)) o["v"] = v.bat_v;
      if (!isnan(v.bat_i)) o["i"] = v.bat_i;
      if (v.kind == VIC_SOLAR && !isnan(v.pv_w)) o["pv"] = v.pv_w;
      if (v.kind == VIC_SHUNT && !isnan(v.soc)) o["soc"] = v.soc;
      if (v.state >= 0) o["state_text"] = tr(victron_state_text(v.state));
    }
  }
  JsonArray vf = vi["found"].to<JsonArray>();
  for (auto &f : victron_found()) {
    JsonObject o = vf.add<JsonObject>();
    o["mac"] = f.mac;
    o["name"] = f.name;
    o["rssi"] = f.rssi;
    o["configured"] = f.configured;
  }
  send_json(d);
}

// GX einstellen: {"key":"cerbo:grid","limit":16} | {"key":"cerbo:grid","mode":3}
void h_cerbo() {
  JsonDocument d;
  if (!body(d)) return;
  std::string key = d["key"] | "";
  bool ok = true;
  if (d["limit"].is<float>()) ok = cerbo_set_limit(key, d["limit"].as<float>());
  if (ok && d["mode"].is<int>()) ok = cerbo_set_mode(key, d["mode"].as<int>());
  if (!ok) return send_err("Geht gerade nicht - Cerbo verbunden und Wert einstellbar?");
  send_ok();
}

// Truma LevelControl: {"scan":true} | {"add":{mac,addr_type,name}} | {"remove":mac} | {"rename":{mac,name}} |
// {"measure":""|mac} | {"interval":min}
void h_truma() {
  JsonDocument d;
  if (!body(d)) return;
  if (d["scan"] | false) truma_scan_start();
  if (!d["add"].isNull()) {
    std::string err = truma_add(d["add"]["mac"] | "", d["add"]["addr_type"] | 0, d["add"]["name"] | "");
    if (!err.empty()) return send_err(err.c_str());
  }
  if (d["remove"].is<const char *>()) truma_remove(d["remove"].as<const char *>());
  if (!d["rename"].isNull()) truma_rename(d["rename"]["mac"] | "", d["rename"]["name"] | "");
  if (d["measure"].is<const char *>()) truma_measure(d["measure"].as<const char *>());
  if (d["interval"].is<int>()) truma_set_interval(constrain((int)d["interval"], 1, 1440));
  send_ok();
}

// Mopeka: {"scan":true} | {"add":{mac,name,tank}} | {"remove":mac} | {"rename":{mac,name}} |
// {"tank":{mac,tank,empty,full,cap}} (tank 255 = eigene Masse)

void h_mopeka() {
  JsonDocument d;
  if (!body(d)) return;
  if (d["scan"] | false) mopeka_discovery_start();
  if (!d["add"].isNull()) {
    std::string err = mopeka_add(d["add"]["mac"] | "", d["add"]["name"] | "", d["add"]["tank"] | 0);
    if (!err.empty()) return send_err(err.c_str());
  }
  if (d["remove"].is<const char *>()) mopeka_remove(d["remove"].as<const char *>());
  if (!d["rename"].isNull()) mopeka_rename(d["rename"]["mac"] | "", d["rename"]["name"] | "");
  if (!d["tank"].isNull()) {
    std::string err = mopeka_set_tank(d["tank"]["mac"] | "", d["tank"]["tank"] | 0, d["tank"]["empty"] | 38,
                                      d["tank"]["full"] | 366, d["tank"]["cap"] | 11.0f);
    if (!err.empty()) return send_err(err.c_str());
  }
  send_ok();
}

#if WITH_BMS
// Akkus (BMS): {"scan":true} | {"add":{mac,addr_type,name,model}} | {"remove":mac} | {"rename":{mac,name}}
void h_bms() {
  JsonDocument d;
  if (!body(d)) return;
  if (d["scan"] | false) bms_discovery_start();
  if (!d["add"].isNull()) {
    std::string err = bms_add(d["add"]["mac"] | "", d["add"]["addr_type"] | 0, d["add"]["name"] | "",
                              (BmsModel)(int)(d["add"]["model"] | (int)BMS_SUPERB), d["add"]["cap"] | 0,
                              d["add"]["cells"] | 0);
    if (!err.empty()) return send_err(err.c_str());
    std::string pin = d["add"]["pin"] | "";
    if (!pin.empty()) bms_set_pin(d["add"]["mac"] | "", pin);   // Kopplungs-PIN gleich mit speichern
  }
  if (d["remove"].is<const char *>()) bms_remove(d["remove"].as<const char *>());
  if (!d["rename"].isNull()) bms_rename(d["rename"]["mac"] | "", d["rename"]["name"] | "");
  if (!d["pin"].isNull()) bms_set_pin(d["pin"]["mac"] | "", d["pin"]["pin"] | "");
  // Dieselheizung {"heat":{mac,what,value}} bzw. Kuehlbox {"fridge":{mac,what,value}} (siehe bms.h)
  if (!d["heat"].isNull() && !bms_heater_cmd(d["heat"]["mac"] | "", d["heat"]["what"] | -1, d["heat"]["value"] | 0))
    return send_err("Das ging nicht");
  if (!d["fridge"].isNull() && !bms_fridge_cmd(d["fridge"]["mac"] | "", d["fridge"]["what"] | -1, d["fridge"]["value"] | 0))
    return send_err("Das ging nicht");
  // Power Station: {"output":{mac,ac:true|false,on:true|false}}
  if (!d["output"].isNull() && !bms_set_output(d["output"]["mac"] | "", d["output"]["ac"] | true, d["output"]["on"] | false))
    return send_err("Dieses Gerät lässt sich nicht schalten");
  // EcoFlow: {"ef_uid":"…"} von Hand oder {"ef_login":{email,password}} (holt die Benutzer-ID, Passwort wird nicht gespeichert)
  if (d["ef_uid"].is<const char *>()) bms_ef_set_uid(d["ef_uid"].as<const char *>());
  if (!d["ef_login"].isNull()) {
    std::string err = bms_ef_login_start(d["ef_login"]["email"] | "", d["ef_login"]["password"] | "");
    if (!err.empty()) return send_err(err.c_str());
  }
  send_ok();
}
#endif

// Victron per Bluetooth: {"scan":true} | {"add":{mac,key,name}} | {"key":{mac,key}} | {"remove":mac} | {"rename":{mac,name}}
void h_victron() {
  JsonDocument d;
  if (!body(d)) return;
  if (d["scan"] | false) victron_discovery_start();
  if (!d["add"].isNull()) {
    std::string err = victron_add(d["add"]["mac"] | "", d["add"]["key"] | "", d["add"]["name"] | "");
    if (!err.empty()) return send_err(err.c_str());
  }
  if (!d["key"].isNull()) {
    std::string err = victron_set_key(d["key"]["mac"] | "", d["key"]["key"] | "");
    if (!err.empty()) return send_err(err.c_str());
  }
  if (d["remove"].is<const char *>()) victron_remove(d["remove"].as<const char *>());
  if (!d["rename"].isNull()) victron_rename(d["rename"]["mac"] | "", d["rename"]["name"] | "");
  send_ok();
}

void h_config() {
  JsonDocument d;
  if (!body(d)) return;
  bool restart = false;
  if (d["name"].is<const char *>()) s_cfg.name = (const char *)d["name"];
  if (d["wifi"]["ssid"].is<const char *>()) {
    String ss = (const char *)d["wifi"]["ssid"];
    if (ss != s_cfg.ssid) restart = true;
    s_cfg.ssid = ss;
    if (d["wifi"]["pass"].is<const char *>() && strlen((const char *)d["wifi"]["pass"])) {
      s_cfg.pass = (const char *)d["wifi"]["pass"];
      restart = true;
    }
  }
  if (d["cerbo"].is<JsonObject>()) {
    JsonObject c = d["cerbo"];
    if (c["on"].is<bool>()) s_cfg.cerbo_on = c["on"];
    if (c["auto"].is<bool>()) s_cfg.cerbo_auto = c["auto"];
    if (c["host"].is<const char *>()) s_cfg.chost = (const char *)c["host"];
    CerboConfig cc = cerbo_config();
    if (c["user"].is<const char *>()) cc.user = (const char *)c["user"];
    if (c["pass"].is<const char *>() && strlen((const char *)c["pass"])) cc.pass = (const char *)c["pass"];
    cc.host = s_cfg.cerbo_on ? std::string(s_cfg.chost.c_str()) : std::string();
    cc.portal.clear();
    cerbo_set_config(cc);
    s_probe_ms = 0;
  }
  if (d["led"].is<int>()) {
    int m = d["led"];
    if (m >= 0 && m <= 3) s_cfg.led = (uint8_t)m;
    s_led_ms = 0;   // gleich neu auswerten
  }
  save_cfg();
  JsonDocument r;
  r["ok"] = true;
  r["restart"] = restart;
  send_json(r);
  if (restart) {
    delay(300);
    ESP.restart();
  }
}

// Netze suchen: jeder Name nur einmal (staerkstes Signal), ohne versteckte Netze, staerkste zuerst
void h_wifiscan() {
  if (!(WiFi.getMode() & WIFI_STA)) WiFi.mode(WIFI_AP_STA);   // Suche braucht den Client-Teil
  int n = WiFi.scanNetworks(false, false);
  if (n < 0) {
    delay(500);
    n = WiFi.scanNetworks(false, false);
  }
  if (n < 0) return send_err("Suche fehlgeschlagen - bitte nochmal.");
  std::vector<int> idx;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i).isEmpty()) continue;
    bool dup = false;
    for (int &j : idx)
      if (WiFi.SSID(j) == WiFi.SSID(i)) {
        if (WiFi.RSSI(i) > WiFi.RSSI(j)) j = i;
        dup = true;
        break;
      }
    if (!dup) idx.push_back(i);
  }
  std::sort(idx.begin(), idx.end(), [](int a, int b) { return WiFi.RSSI(a) > WiFi.RSSI(b); });
  JsonDocument d;
  JsonArray a = d["networks"].to<JsonArray>();
  for (int i : idx) {
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = WiFi.SSID(i);
    o["rssi"] = WiFi.RSSI(i);
    o["open"] = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  send_json(d);
}

void h_display_get() {
  JsonDocument d;
  sat_display_json(d.to<JsonObject>());
  send_json(d);
}

void h_display_set() {
  JsonDocument d;
  if (!body(d)) return;
  sat_display_apply(d.as<JsonObjectConst>());
  send_ok();
}

void h_update_done() {
  if (!s_upl_ok) return devsetup::deny();
  bool ok = !Update.hasError();
  if (!ok) return send_err("Update fehlgeschlagen");
  send_ok();
  if (ok) {
    delay(500);
    ESP.restart();
  }
}

// In den kleinen Updater starten (WLAN-Daten werden uebergeben)
void h_updater() {
  String err = updater_start(s_cfg.ssid, s_cfg.pass, s_id);
  send_err(err.c_str());   // kommt nur bei Fehler zurueck
}

// Upload in den Bereich neben der laufenden Firmware: mit neuer Aufteilung nur der Updater (max. 1 MB, Groesse vorher
// pruefen, sonst waere der Updater halb ueberschrieben); mit alter Aufteilung (min_spiffs) wie bisher die Firmware.
void h_update_data() {
  HTTPUpload &u = s_srv.upload();
  if (u.status == UPLOAD_FILE_START) {
    s_upl_ok = devsetup::upload_allowed();
    size_t want = s_srv.hasArg("size") ? (size_t)s_srv.arg("size").toInt() : 0, slot = updater_slot_size();
    if (s_upl_ok && slot && (!want || want > slot)) s_upl_ok = false;
    if (s_upl_ok) Update.begin(want ? want : UPDATE_SIZE_UNKNOWN);
  } else if (!s_upl_ok) {
    return;
  } else if (u.status == UPLOAD_FILE_WRITE) {
    Update.write(u.buf, u.currentSize);
  } else if (u.status == UPLOAD_FILE_END) {
    Update.end(true);
  }
}

void web_begin() {
  devsetup::begin(s_srv);   // Anmeldung, Passwort, Captive Portal
  s_srv.on("/", HTTP_GET, [] { s_srv.send_P(200, "text/html; charset=utf-8", VD_PAGE); });
  s_srv.on("/api/state", HTTP_GET, devsetup::guard(h_state));
  s_srv.on("/api/config", HTTP_POST, devsetup::guard(h_config));
  s_srv.on("/api/wifiscan", HTTP_GET, devsetup::guard(h_wifiscan));
  s_srv.on("/api/victron", HTTP_POST, devsetup::guard(h_victron));
  s_srv.on("/api/truma", HTTP_POST, devsetup::guard(h_truma));
  s_srv.on("/api/mopeka", HTTP_POST, devsetup::guard(h_mopeka));
#if WITH_BMS
  s_srv.on("/api/bms", HTTP_POST, devsetup::guard(h_bms));
#endif
  s_srv.on("/api/cerbo", HTTP_POST, devsetup::guard(h_cerbo));
  s_srv.on("/api/display", HTTP_GET, devsetup::guard(h_display_get));
  s_srv.on("/api/display", HTTP_POST, devsetup::guard(h_display_set));
  s_srv.on("/api/reboot", HTTP_POST, devsetup::guard([] {
             send_ok();
             delay(300);
             ESP.restart();
           }));
  s_srv.on("/api/update", HTTP_POST, h_update_done, h_update_data);
  s_srv.on("/api/updater", HTTP_POST, devsetup::guard(h_updater));
  s_srv.begin();
}
}  // namespace

// Fuer cerbo.cpp (beim Panel in net.cpp)
bool net_connected() { return WiFi.isConnected(); }
bool net_ap_active() { return (WiFi.getMode() & WIFI_AP) != 0; }

std::string vd_title() { return std::string(s_cfg.name.length() ? s_cfg.name.c_str() : s_id.c_str()); }

void setup() {
  Serial.begin(115200);
  delay(100);
  uint64_t mac = ESP.getEfuseMac();
  char id[32];
  snprintf(id, sizeof(id), "SOC-Victron-%02X%02X%02X", (unsigned)(mac >> 24) & 0xFF, (unsigned)(mac >> 32) & 0xFF,
           (unsigned)(mac >> 40) & 0xFF);
  s_id = id;
  load_cfg();
  devsetup::Boot boot = devsetup::boot_check();   // zuerst: Mehrfach-Reset erkennen
  if (boot.factory) {   // 5x: WLAN-Daten loeschen (Passwort setzt devsetup zurueck)
    s_cfg.ssid = "";
    s_cfg.pass = "";
    save_cfg();
  }
  // Cerbo/Victron vor dem Display anlegen: das Display fragt beim Start schon deren Werte ab (Sperren muessen existieren)
  cerbo_begin();
  cerbo_apply();     // Cerbo aus: Adresse im Cerbo-Modul leeren (kein Client, keine Kacheln)
  gx_find_set_handler([](const std::string &host, const std::string &) {
    snprintf(s_found_host, sizeof(s_found_host), "%s", host.c_str());
    s_found_new = true;
  });
  victron_begin();   // gespeicherte Bluetooth-Geraete
  mopeka_begin();    // Mopeka-Tanksensoren (Werbepakete, laeuft ueber den Bluetooth-Verteiler truma.cpp)
  bms_begin();       // Akkus mit BMS (Super B …): gespeicherte Geraete, Verbindung macht der Bluetooth-Task
  truma_begin();     // Bluetooth-Task: Truma LevelControl + Victron + Akkus, startet den Funk erst bei Bedarf
  sat_display_begin();
  if (sat_display_reset_requested()) {   // beim Einschalten 10 s gedrueckt: Passwort zuruecksetzen
    devsetup::password_reset();
    log_w("Passwort auf Standard zurueckgesetzt");
  }
  wifi_begin();
  web_begin();
}

void loop() {
  devsetup::loop();
  wifi_loop();
  cerbo_find();
  s_srv.handleClient();
  sat_display_loop();
  led_loop();
  delay(2);
}
