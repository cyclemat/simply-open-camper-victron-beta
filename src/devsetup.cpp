#include "devsetup.h"

#include <ArduinoJson.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include "i18n/i18n.h"
#include "dbglog.h"

namespace devsetup {
namespace {
void h_appw_get();
void h_appw_set();
const char *NS = "devsetup";
const uint32_t BOOT_OK_MS = 10000;   // so lange gelaufen = normaler Start, Zaehler wieder 0
const IPAddress AP_IP(4, 3, 2, 1);

WebServer *s_srv = nullptr;
DNSServer *s_dns = nullptr;
String s_pw = DEVSETUP_DEFAULT_PW, s_ssid;        // Zugangspasswort (Weboberflaeche)
String s_appw = DEVSETUP_DEFAULT_PW;               // WLAN-Passwort des Einrichtungs-WLANs
bool s_apon = true;                                // false = Einrichtungs-WLAN offen
bool s_pw_set = false, s_force_ap = false, s_bootc_clear = false, s_loaded = false;
uint32_t s_ap_restart_ms = 0, s_lock_until = 0;
uint8_t s_fails = 0, s_tok_next = 0;
String s_tokens[4];   // angemeldete Browser, nur im RAM

void load() {
  if (s_loaded) return;
  s_loaded = true;
  Preferences p;
  if (!p.begin(NS, true)) return;   // noch nichts gespeichert
  s_pw = p.getString("apw", DEVSETUP_DEFAULT_PW);
  s_pw_set = p.getBool("pwset", false);
  s_appw = p.getString("appw", DEVSETUP_DEFAULT_PW);
  s_apon = p.getBool("apon", true);
  p.end();
  if (s_pw.length() < 8) {
    s_pw = DEVSETUP_DEFAULT_PW;
    s_pw_set = false;
  }
  if (s_appw.length() < 8) s_appw = DEVSETUP_DEFAULT_PW;
}

// Einrichtungs-WLAN mit den aktuellen Einstellungen (neu) starten; offen = ohne Passwort
void softap() { WiFi.softAP(s_ssid.c_str(), s_apon ? s_appw.c_str() : nullptr); }

void save() {
  Preferences p;
  p.begin(NS, false);
  p.putString("apw", s_pw);
  p.putBool("pwset", s_pw_set);
  p.putString("appw", s_appw);
  p.putBool("apon", s_apon);
  p.end();
}

void send_json(int code, JsonDocument &d) {
  String s;
  serializeJson(d, s);
  s_srv->send(code, "application/json", s);
}

void send_err(const char *e) {
  JsonDocument d;
  d["error"] = tr(e);   // Victron-Display auf Englisch: Meldung uebersetzen (Deutsch: unveraendert)
  send_json(400, d);
}

bool body(JsonDocument &d) {
  if (deserializeJson(d, s_srv->arg("plain"))) {
    send_err("Ungueltiges JSON");
    return false;
  }
  return true;
}

void new_session() {
  char t[33];
  for (int i = 0; i < 4; i++) snprintf(t + i * 8, 9, "%08lx", (unsigned long)esp_random());
  s_tokens[s_tok_next] = t;
  s_tok_next = (s_tok_next + 1) % 4;
  s_srv->sendHeader("Set-Cookie", String("socauth=") + t + "; Path=/; HttpOnly; SameSite=Strict");
}

bool locked() {
  if (s_lock_until && (int32_t)(millis() - s_lock_until) < 0) {
    send_err("Zu viele Versuche - bitte 30 Sekunden warten.");
    return true;
  }
  s_lock_until = 0;
  return false;
}

void wrong_pw() {
  if (++s_fails >= 5) {
    s_fails = 0;
    s_lock_until = (millis() + 30000) | 1;
  }
  send_err("Passwort falsch.");
}

void h_auth() {
  JsonDocument d;
  d["setup"] = !s_pw_set;
  d["authed"] = authed();
  send_json(200, d);
}

void h_login() {
  JsonDocument d;
  if (!body(d) || locked()) return;
  if (!s_pw_set) return deny();
  if (s_pw != (const char *)(d["pw"] | "")) return wrong_pw();
  s_fails = 0;
  new_session();
  s_srv->send(200, "application/json", "{\"ok\":true}");
}

// {"old":"...","new":"..."} - erstes Mal mit dem Standardpasswort, danach mit dem aktuellen
void h_setpw() {
  JsonDocument d;
  if (!body(d) || locked()) return;
  String oldp = d["old"] | "", newp = d["new"] | "";
  if (oldp != s_pw) return wrong_pw();
  if (newp.length() < 8 || newp.length() > 63) return send_err("Das neue Passwort braucht 8 bis 63 Zeichen.");
  if (newp == DEVSETUP_DEFAULT_PW) return send_err("Bitte ein anderes als das Standardpasswort nehmen.");
  s_fails = 0;
  s_pw = newp;
  s_pw_set = true;
  save();
  for (auto &k : s_tokens) k = "";   // alle anderen abmelden
  new_session();
  s_srv->send(200, "application/json", "{\"ok\":true}");
}

// WLAN-Passwort des Einrichtungs-WLANs (nur angemeldet): GET {"on":true,"custom":false}
void h_appw_get() {
  JsonDocument d;
  d["on"] = s_apon;
  d["custom"] = s_appw != DEVSETUP_DEFAULT_PW;
  d["ssid"] = s_ssid;
  send_json(200, d);
}

// POST {"on":true,"pw":"..."} (pw leer = unveraendert)
void h_appw_set() {
  JsonDocument d;
  if (!body(d)) return;
  bool on = d["on"] | s_apon;
  String pw = d["pw"] | "";
  if (pw.length() && (pw.length() < 8 || pw.length() > 63)) return send_err("Das WLAN-Passwort braucht 8 bis 63 Zeichen.");
  s_apon = on;
  if (pw.length()) s_appw = pw;
  save();
  s_srv->send(200, "application/json", "{\"ok\":true}");
  if (ap_active()) s_ap_restart_ms = (millis() + 2000) | 1;   // Einrichtungs-WLAN mit der neuen Einstellung
}

void to_portal() {
  s_srv->sendHeader("Location", String("http://") + AP_IP.toString() + "/", true);
  s_srv->send(302, "text/plain", "");
}

#if WITH_DBGLOG
// Debug-Log (alle Geraete mit dieser Weboberflaeche): GET = Stand + Dateien | ?ring=1 = Arbeitsspeicher als Text |
// ?file=log-NNNNN.txt = Datei herunterladen. POST {"on":bool} | {"delete":"log-NNNNN.txt"|"*"}
void h_dbglog_get() {
  if (s_srv->arg("ring") == "1") {
    std::string r = dbglog_ring();
    s_srv->sendHeader("Cache-Control", "no-store");
    s_srv->send(200, "text/plain; charset=utf-8", String(r.c_str()));
    return;
  }
  String f = s_srv->arg("file");
  if (f.length()) {
    std::string dir = dbglog_dir();
    if (!dbglog_valid_name(f.c_str()) || dir.empty()) return send_err("Datei unbekannt");
    FILE *fp = fopen((dir + "/" + f.c_str()).c_str(), "r");
    if (!fp) return send_err("Datei nicht gefunden");
    s_srv->setContentLength(CONTENT_LENGTH_UNKNOWN);
    s_srv->sendHeader("Content-Disposition", "attachment; filename=\"" + f + "\"");
    s_srv->send(200, "text/plain; charset=utf-8", "");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) s_srv->sendContent(buf, n);
    fclose(fp);
    s_srv->sendContent("");
    return;
  }
  JsonDocument d;
  d["avail"] = true;
  d["on"] = dbglog_on();
  d["files_ok"] = !dbglog_dir().empty();
  d["sd"] = (bool)DBGLOG_CYD_SD;
  d["current"] = dbglog_current();
  d["lost"] = dbglog_lost();
  d["ring_kb"] = (int)(dbglog_ring_size() / 1024);
  JsonArray a = d["files"].to<JsonArray>();
  for (auto &x : dbglog_files()) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = x.name;
    o["size"] = x.size;
  }
  send_json(200, d);
}

void h_dbglog_set() {
  JsonDocument d;
  if (!body(d)) return;
  if (d["on"].is<bool>()) dbglog_set(d["on"].as<bool>());
  if (d["delete"].is<const char *>() && !dbglog_delete(d["delete"].as<const char *>()))
    return send_err("Datei lässt sich nicht löschen (die aktuelle bleibt, solange der Debug-Log läuft).");
  JsonDocument r;
  r["ok"] = true;
  send_json(200, r);
}
#endif

void h_not_found() {
  if (ap_active() && s_srv->hostHeader() != AP_IP.toString()) return to_portal();
  s_srv->send(404, "text/plain", "Nicht gefunden");
}
}  // namespace

// ---------------------------------------------------------------- Mehrfach-Reset
Boot boot_check() {
  load();
  Boot b;
  Preferences p;
  p.begin(NS, false);
  b.count = p.getUChar("bootc", 0) + 1;
  p.putUChar("bootc", b.count);
  p.end();
  if (b.count >= 5) {
    b.factory = b.force_ap = true;
    password_reset();
    log_w("%ux schnell neu gestartet: WLAN und Passwort auf Werkseinstellung", (unsigned)b.count);
  } else if (b.count >= 3) {
    b.force_ap = true;
    log_w("%ux schnell neu gestartet: Einrichtungs-WLAN bleibt an", (unsigned)b.count);
  }
  s_force_ap = b.force_ap;
  return b;
}

bool force_ap() { return s_force_ap; }

// ---------------------------------------------------------------- Einrichtungs-WLAN
void ap_start(const String &ssid) {
  load();
  s_ssid = ssid;
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0), IPAddress(4, 3, 2, 2), AP_IP);
  softap();
  WiFi.AP.enableDhcpCaptivePortal();
  if (!s_dns) {
    s_dns = new DNSServer();
    s_dns->setErrorReplyCode(DNSReplyCode::NoError);
    s_dns->start(53, "*", AP_IP);
  }
  log_w("Einrichtungs-WLAN %s an, http://%s/", ssid.c_str(), AP_IP.toString().c_str());
}

void ap_stop() {
  if (s_dns) {
    s_dns->stop();
    delete s_dns;
    s_dns = nullptr;
  }
  WiFi.softAPdisconnect(true);
}

bool ap_active() { return (WiFi.getMode() & WIFI_AP) != 0; }

// ---------------------------------------------------------------- Weboberflaeche
void begin(WebServer &srv) {
  load();
  s_srv = &srv;
  static const char *HDR[] = {"Cookie"};
  srv.collectHeaders(HDR, 1);
  // Erkennungsadressen der Betriebssysteme (Android, Apple, Windows, Firefox) -> Einstellungsseite
  for (const char *u : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/library/test/success.html",
                        "/connecttest.txt", "/ncsi.txt", "/redirect", "/fwlink", "/canonical.html", "/success.txt"})
    srv.on(u, HTTP_GET, to_portal);
  srv.on("/api/auth", HTTP_GET, h_auth);
  srv.on("/api/login", HTTP_POST, h_login);
  srv.on("/api/setpw", HTTP_POST, h_setpw);
  srv.on("/api/appw", HTTP_GET, guard(h_appw_get));
  srv.on("/api/appw", HTTP_POST, guard(h_appw_set));
#if WITH_DBGLOG
  srv.on("/api/dbglog", HTTP_GET, guard(h_dbglog_get));
  srv.on("/api/dbglog", HTTP_POST, guard(h_dbglog_set));
#endif
  srv.onNotFound(h_not_found);
}

void loop() {
  if (s_dns) s_dns->processNextRequest();
  if (!s_bootc_clear && millis() > BOOT_OK_MS) {
    s_bootc_clear = true;
    Preferences p;
    p.begin(NS, false);
    p.putUChar("bootc", 0);
    p.end();
  }
  if (s_ap_restart_ms && (int32_t)(millis() - s_ap_restart_ms) >= 0) {
    s_ap_restart_ms = 0;
    softap();   // gleiches Netz, neue Einstellung
  }
}

bool authed() {
  if (!s_pw_set || !s_srv) return false;
  String c = s_srv->header("Cookie");
  int i = c.indexOf("socauth=");
  if (i < 0) return false;
  String t = c.substring(i + 8, i + 8 + 32);
  if (t.length() != 32) return false;
  for (auto &k : s_tokens)
    if (k.length() && k == t) return true;
  return false;
}

void deny() { s_srv->send(401, "application/json", s_pw_set ? "{\"auth\":\"login\"}" : "{\"auth\":\"setup\"}"); }

std::function<void()> guard(std::function<void()> f) {
  return [f] {
    if (!authed()) return deny();
    f();
  };
}

bool upload_allowed() { return authed(); }

bool password_set() {
  load();
  return s_pw_set;
}

void password_reset() {
  s_pw = DEVSETUP_DEFAULT_PW;
  s_pw_set = false;
  s_appw = DEVSETUP_DEFAULT_PW;
  s_apon = true;
  for (auto &k : s_tokens) k = "";
  save();
}

}  // namespace devsetup
