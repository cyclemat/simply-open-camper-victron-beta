// GX im Netz finden – siehe gx_find.h
#include "gx_find.h"
#include "cerbo.h"

#if WITH_CERBO
#include <Arduino.h>
#include <WiFi.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i18n/i18n.h"

namespace {

const uint16_t PORT = 1883;
const int BATCH = 6;                 // gleichzeitige Verbindungsversuche (Sockets sind knapp: Webserver, MQTT …)
const int CONNECT_MS = 350;
const int VERIFY_MS = 3500;          // so lange auf die Portal-ID warten

SemaphoreHandle_t s_mtx = nullptr;
std::string s_status;
volatile bool s_running = false;
GxFound s_cb;
GxFound s_handler;   // Standard-Empfaenger (gx_find_set_handler)
uint32_t s_last_ms = 0, s_down_ms = 0;

void set_status(const std::string &s) {
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  s_status = s;
  xSemaphoreGive(s_mtx);
  log_i("GX-Suche: %s", s.c_str());
}

// ---------------------------------------------------------------- Port 1883 offen?
int open_nb(uint32_t ip_be) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = ip_be;
  connect(fd, (sockaddr *)&a, sizeof(a));
  return fd;
}

// mehrere Adressen gleichzeitig: liefert die mit offenem Port
std::vector<uint32_t> probe(const std::vector<uint32_t> &ips) {
  std::vector<uint32_t> open;
  for (size_t i = 0; i < ips.size(); i += BATCH) {
    int fds[BATCH];
    size_t n = 0;
    for (; n < BATCH && i + n < ips.size(); n++) fds[n] = open_nb(ips[i + n]);
    uint32_t t0 = millis();
    bool done[BATCH] = {false};
    while (millis() - t0 < (uint32_t)CONNECT_MS) {
      fd_set w;
      FD_ZERO(&w);
      int mx = -1;
      for (size_t k = 0; k < n; k++)
        if (fds[k] >= 0 && !done[k]) {
          FD_SET(fds[k], &w);
          mx = fds[k] > mx ? fds[k] : mx;
        }
      if (mx < 0) break;
      timeval tv = {0, 50 * 1000};
      if (select(mx + 1, nullptr, &w, nullptr, &tv) <= 0) continue;
      for (size_t k = 0; k < n; k++)
        if (fds[k] >= 0 && !done[k] && FD_ISSET(fds[k], &w)) {
          done[k] = true;
          int err = 0;
          socklen_t l = sizeof(err);
          getsockopt(fds[k], SOL_SOCKET, SO_ERROR, &err, &l);
          if (!err) open.push_back(ips[i + k]);
        }
    }
    for (size_t k = 0; k < n; k++)
      if (fds[k] >= 0) close(fds[k]);
  }
  return open;
}

// ---------------------------------------------------------------- MQTT: ist das ein GX? -> Portal-ID
bool send_all(int fd, const uint8_t *p, size_t n) {
  while (n) {
    int k = send(fd, p, n, 0);
    if (k <= 0) return false;
    p += k;
    n -= k;
  }
  return true;
}

// liest ein MQTT-Paket (Kopf + Rest) mit Zeitlimit; false bei Fehler/Ende
bool read_packet(int fd, uint32_t until, uint8_t &type, std::vector<uint8_t> &body) {
  auto rd = [&](uint8_t *b, size_t n) {
    while (n) {
      int32_t left = (int32_t)(until - millis());
      if (left <= 0) return false;
      fd_set r;
      FD_ZERO(&r);
      FD_SET(fd, &r);
      timeval tv = {0, (long)std::min<int32_t>(left, 200) * 1000};
      if (select(fd + 1, &r, nullptr, nullptr, &tv) <= 0) continue;
      int k = recv(fd, b, n, 0);
      if (k <= 0) return false;
      b += k;
      n -= k;
    }
    return true;
  };
  uint8_t h;
  if (!rd(&h, 1)) return false;
  type = h >> 4;
  uint32_t len = 0, mul = 1;
  for (int i = 0; i < 4; i++) {
    uint8_t c;
    if (!rd(&c, 1)) return false;
    len += (c & 0x7F) * mul;
    mul <<= 7;
    if (!(c & 0x80)) break;
  }
  if (len > 2048) return false;
  body.resize(len);
  return len == 0 || rd(body.data(), len);
}

std::string verify(uint32_t ip_be) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return "";
  timeval tv = {1, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  a.sin_addr.s_addr = ip_be;
  std::string portal;
  if (connect(fd, (sockaddr *)&a, sizeof(a)) == 0) {
    char id[24];
    snprintf(id, sizeof(id), "soc-find-%04lx", (unsigned long)(esp_random() & 0xFFFF));
    uint8_t c[64];
    size_t n = 0, il = strlen(id);
    c[n++] = 0x10;
    c[n++] = 12 + il;
    const uint8_t vh[] = {0, 4, 'M', 'Q', 'T', 'T', 4, 0x02, 0, 30};
    memcpy(c + n, vh, sizeof(vh));
    n += sizeof(vh);
    c[n++] = 0;
    c[n++] = il;
    memcpy(c + n, id, il);
    n += il;
    static const char TOPIC[] = "N/+/system/0/Serial";
    const size_t tl = sizeof(TOPIC) - 1;
    uint8_t s[48];
    size_t m = 0;
    s[m++] = 0x82;
    s[m++] = 2 + 2 + tl + 1;
    s[m++] = 0;
    s[m++] = 1;
    s[m++] = 0;
    s[m++] = tl;
    memcpy(s + m, TOPIC, tl);
    m += tl;
    s[m++] = 0;
    uint32_t until = millis() + VERIFY_MS;
    uint8_t type;
    std::vector<uint8_t> body;
    if (send_all(fd, c, n) && read_packet(fd, until, type, body) && type == 2 && body.size() >= 2 && body[1] == 0 &&
        send_all(fd, s, m)) {
      while (portal.empty() && read_packet(fd, until, type, body)) {
        if (type != 3 || body.size() < 2) continue;
        size_t l = body[0] << 8 | body[1];
        if (l + 2 > body.size()) continue;
        std::string t((const char *)body.data() + 2, l);   // N/<portal>/system/0/Serial
        size_t e = t.find('/', 2);
        if (t.rfind("N/", 0) == 0 && e != std::string::npos) portal = t.substr(2, e - 2);
      }
      const uint8_t disc[2] = {0xE0, 0};
      send_all(fd, disc, 2);
    }
  }
  close(fd);
  return portal;
}

uint32_t resolve(const char *name) {
  addrinfo hints = {}, *res = nullptr;
  hints.ai_family = AF_INET;
  if (getaddrinfo(name, nullptr, &hints, &res) != 0 || !res) return 0;
  uint32_t ip = ((sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(res);
  return ip;
}

std::string ip_str(uint32_t ip_be) {
  char b[16];
  const uint8_t *p = (const uint8_t *)&ip_be;
  snprintf(b, sizeof(b), "%u.%u.%u.%u", p[0], p[1], p[2], p[3]);
  return b;
}

void apply_default(const std::string &host, const std::string &) {
  CerboConfig c = cerbo_config();
  if (c.host == host) return;
  c.host = host;
  c.portal.clear();   // Portal-ID erkennt die Cerbo-Anbindung selbst
  cerbo_set_config(c);
}

bool try_host(uint32_t ip, bool named, const char *what) {
  if (!ip) return false;
  set_status(std::string(tr("Prüfe ")) + tr(what) + " (" + ip_str(ip) + ") …");
  if (probe({ip}).empty()) return false;
  std::string portal = verify(ip);
  // eigener Name/Adresse des GX: auch ohne Antwort annehmen (aeltere Venus-Versionen senden die Kennung erst nach
  // einer Anforderung); im Netz gefundene Broker nur mit Kennung
  if (portal.empty() && !named) return false;
  set_status(tr("Gefunden: ") + ip_str(ip) + (portal.empty() ? "" : " (Portal-ID " + portal + ")"));
  (s_cb ? s_cb : s_handler ? s_handler : GxFound(apply_default))(ip_str(ip), portal);
  return true;
}

void find_task(void *) {
  std::vector<uint32_t> tried;
  auto seen = [&](uint32_t ip) {
    for (uint32_t t : tried)
      if (t == ip) return true;
    tried.push_back(ip);
    return false;
  };
  bool ok = false;
  uint32_t gx_ap = inet_addr("172.24.24.1");
  uint32_t gw = (uint32_t)WiFi.gatewayIP();
  if (!seen(gx_ap)) ok = try_host(gx_ap, true, "WLAN des GX");
  if (!ok && gw && !seen(gw)) ok = try_host(gw, true, "Router/GX");
  for (const char *n : {"venus.local", "einstein.local"}) {
    if (ok) break;
    set_status(std::string(tr("Suche ")) + n + " …");
    uint32_t ip = resolve(n);
    if (ip && !seen(ip)) ok = try_host(ip, true, n);
  }
  if (!ok) {   // eigenes Netz (/24) nach offenem Port 1883 absuchen, Treffer per MQTT pruefen
    uint32_t me = (uint32_t)WiFi.localIP(), mask = (uint32_t)WiFi.subnetMask();
    if (me && mask) {
      uint32_t net = me & ntohl(0xFFFFFF00UL);   // hoechstens 254 Adressen, auch bei groesseren Netzen
      std::vector<uint32_t> ips;
      for (uint32_t h = 1; h < 255; h++) {
        uint32_t ip = net | htonl(h);
        if (ip != me && !seen(ip)) ips.push_back(ip);
      }
      set_status(tr("Durchsuche das Netz ") + ip_str(net) + "/24 …");
      for (uint32_t ip : probe(ips)) {
        if (try_host(ip, false, "Fund")) {
          ok = true;
          break;
        }
      }
    }
  }
  if (!ok) set_status("Kein GX gefunden. Am GX: Einstellungen > Dienste > MQTT einschalten.");
  s_cb = nullptr;
  s_last_ms = millis();
  s_running = false;
  vTaskDelete(nullptr);
}

}  // namespace

bool gx_find_start(GxFound on_found) {
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
  if (s_running) return false;
  if (!WiFi.isConnected()) {
    set_status("Kein WLAN – zuerst mit dem Netz des GX verbinden.");
    return false;
  }
  s_cb = on_found;
  s_running = true;
  set_status("Suche …");
  if (xTaskCreate(find_task, "gxfind", 6144, nullptr, 2, nullptr) != pdPASS) {
    s_running = false;
    set_status("Suche konnte nicht starten (Speicher).");
    return false;
  }
  return true;
}

bool gx_find_running() { return s_running; }

void gx_find_set_handler(GxFound h) { s_handler = h; }

std::string gx_find_status() {
  if (!s_mtx) return "";
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  std::string s = s_status;
  xSemaphoreGive(s_mtx);
  return s;
}

void gx_find_watch() {
  if (!cerbo_configured() || cerbo_connected() || !WiFi.isConnected()) {
    s_down_ms = 0;
    return;
  }
  uint32_t now = millis();
  if (!s_down_ms) s_down_ms = now | 1;
  if (now - s_down_ms < 180000) return;                    // 3 min nicht erreichbar
  if (s_last_ms && now - s_last_ms < 600000) return;       // hoechstens alle 10 min
  s_down_ms = 0;
  gx_find_start();
}

#else
bool gx_find_start(GxFound) { return false; }
void gx_find_set_handler(GxFound) {}
bool gx_find_running() { return false; }
std::string gx_find_status() { return ""; }
void gx_find_watch() {}
#endif
