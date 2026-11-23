// Debug-Log (Fehlersuche) – siehe dbglog.h
#include "dbglog.h"
#if WITH_DBGLOG
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "soc_version.h"

namespace {

const long FILE_MAX = 1024L * 1024L;   // je Datei
const int FILES_KEEP = 10;             // so viele Dateien bleiben liegen
const uint32_t TICK_MS = 250, FILE_MS = 2000, STATUS_MS = 60000, STORE_RETRY_MS = 30000;

// Zwischenpuffer: wird aus ets_printf (auch aus Interrupts, auch bei abgeschaltetem Cache) befuellt -> internes RAM,
// Zugriff nur unter Spinlock
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
char *volatile s_stage = nullptr;
char *s_take = nullptr;                // Kopie fuer den Task
size_t s_stage_sz = 0;
volatile size_t s_stage_n = 0;
volatile uint32_t s_lost = 0;

// Ringpuffer (PSRAM, sonst intern) + Dateiname, unter s_lock
SemaphoreHandle_t s_lock = nullptr;
char *s_ring = nullptr;
size_t s_ring_cfg = 8192, s_ring_sz = 0, s_ring_pos = 0;
bool s_ring_full = false;
std::string s_dir, s_file;

const char *(*s_storage)() = nullptr;
std::string (*s_status)() = nullptr;
bool s_on = false;
volatile bool s_run = false;
volatile bool s_task_alive = false;
vprintf_like_t s_prev = nullptr;

void IRAM_ATTR stage_putc(char c) {
  portENTER_CRITICAL_SAFE(&s_mux);
  char *st = s_stage;
  if (st) {
    if (s_stage_n < s_stage_sz) st[s_stage_n++] = c;
    else s_lost++;
  }
  portEXIT_CRITICAL_SAFE(&s_mux);
}

void stage_write(const char *s, size_t n) {
  portENTER_CRITICAL_SAFE(&s_mux);
  char *st = s_stage;
  if (st) {
    size_t k = std::min(n, s_stage_sz - s_stage_n);
    memcpy(st + s_stage_n, s, k);
    s_stage_n += k;
    s_lost += n - k;
  }
  portEXIT_CRITICAL_SAFE(&s_mux);
}

// ESP-IDF-Logs (ESP_LOGx) laufen nicht ueber ets_printf: zusaetzlich mitschreiben, dann normal ausgeben
int idf_vprintf(const char *fmt, va_list ap) {
  char b[128];
  va_list c;
  va_copy(c, ap);
  int n = vsnprintf(b, sizeof(b), fmt, c);
  va_end(c);
  if (n > 0) stage_write(b, std::min((size_t)n, sizeof(b) - 1));
  return s_prev ? s_prev(fmt, ap) : vprintf(fmt, ap);
}

void ring_put(const std::string &s) {   // unter s_lock
  if (!s_ring) return;
  const char *p = s.data();
  size_t n = s.size();
  if (n >= s_ring_sz) {   // groesser als der Ring: nur das Ende
    p += n - s_ring_sz + 1;
    n = s_ring_sz - 1;
  }
  while (n) {
    size_t k = std::min(n, s_ring_sz - s_ring_pos);
    memcpy(s_ring + s_ring_pos, p, k);
    s_ring_pos += k;
    p += k;
    n -= k;
    if (s_ring_pos == s_ring_sz) {
      s_ring_pos = 0;
      s_ring_full = true;
    }
  }
}

std::string stamp() {
  char b[24];
  time_t t = time(nullptr);
  if (t > 1700000000) {
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(b, sizeof(b), "%H:%M:%S ", &tm);
  } else {
    snprintf(b, sizeof(b), "+%lus ", (unsigned long)(millis() / 1000));
  }
  return b;
}

const char *reset_text() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "Einschalten";
    case ESP_RST_SW: return "Neustart per Software";
    case ESP_RST_PANIC: return "ABSTURZ (Panic)";
    case ESP_RST_INT_WDT: return "WATCHDOG (Interrupt)";
    case ESP_RST_TASK_WDT: return "WATCHDOG (Task)";
    case ESP_RST_WDT: return "WATCHDOG";
    case ESP_RST_BROWNOUT: return "SPANNUNGSEINBRUCH (Brownout)";
    case ESP_RST_DEEPSLEEP: return "Aufwachen";
    case ESP_RST_EXT: return "Reset-Taste";
    default: return "unbekannt";
  }
}

// full = auch die Angaben der Firmware (MQTT, Cerbo …) – erst nach dem Start, vorher gibt es deren Mutexe noch nicht
std::string status_line(bool full) {
  char b[220];
  uint32_t up = millis() / 1000;
  int n = snprintf(b, sizeof(b), "Zustand: Laufzeit %luh%02lum, intern frei %u KB (min %u, Block %u)",
                   (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
                   (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                   (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                   (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
  size_t ps = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (ps && n < (int)sizeof(b))
    n += snprintf(b + n, sizeof(b) - n, ", PSRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  if (full && n < (int)sizeof(b)) {
    if (WiFi.status() == WL_CONNECTED)
      n += snprintf(b + n, sizeof(b) - n, ", WLAN %d dBm %s", (int)WiFi.RSSI(), WiFi.localIP().toString().c_str());
    else
      n += snprintf(b + n, sizeof(b) - n, ", WLAN getrennt");
  }
  if (s_lost && n < (int)sizeof(b)) snprintf(b + n, sizeof(b) - n, ", %lu Zeichen verloren", (unsigned long)s_lost);
  std::string r = b;
  if (s_status && full) {
    std::string x = s_status();
    if (!x.empty()) r += " | " + x;
  }
  return r + "\n";
}

void mkdirs(const std::string &dir) {
  for (size_t i = 1; i <= dir.size(); i++)
    if (i == dir.size() || dir[i] == '/') mkdir(dir.substr(0, i).c_str(), 0775);
}

std::vector<DbgFile> list_files(const std::string &dir) {
  std::vector<DbgFile> r;
  if (dir.empty()) return r;
  DIR *d = opendir(dir.c_str());
  if (!d) return r;
  while (struct dirent *e = readdir(d)) {
    DbgFile f;
    f.name = e->d_name;
    if (!dbglog_valid_name(f.name)) continue;
    struct stat st;
    f.size = stat((dir + "/" + f.name).c_str(), &st) == 0 ? (long)st.st_size : -1;
    r.push_back(f);
  }
  closedir(d);
  std::sort(r.begin(), r.end(), [](const DbgFile &a, const DbgFile &b) { return a.name < b.name; });
  return r;
}

// Neue Datei log-NNNNN.txt (laufende Nummer im NVS), alte ueber FILES_KEEP loeschen
FILE *open_new(const std::string &dir) {
  Preferences p;
  uint32_t n = 1;
  if (p.begin("dbglog", false)) {
    n = p.getUInt("n", 0) + 1;
    p.putUInt("n", n);
    p.end();
  }
  char name[24];
  snprintf(name, sizeof(name), "log-%05lu.txt", (unsigned long)(n % 100000));
  FILE *fp = fopen((dir + "/" + name).c_str(), "a");
  if (!fp) return nullptr;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_file = name;
  xSemaphoreGive(s_lock);
  auto files = list_files(dir);
  for (int i = 0; i + FILES_KEEP < (int)files.size(); i++)
    if (files[i].name != name) remove((dir + "/" + files[i].name).c_str());
  return fp;
}

void task(void *) {
  s_task_alive = true;
  FILE *fp = nullptr;
  std::string dir, out, fbuf;
  bool line_start = true;
  uint32_t t_file = millis(), t_status = millis(), t_store = 0;
  bool first_store = true;
  {
    char h[200];
    time_t t = time(nullptr);
    char when[32] = "Uhrzeit unbekannt";
    if (t > 1700000000) {
      struct tm tm;
      localtime_r(&t, &tm);
      strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
    }
    snprintf(h, sizeof(h), "===== Debug-Log an – Simple Open Camper %s, %s, letzter Start: %s, Laufzeit %lu s =====\n",
             SOC_VERSION, when, reset_text(), (unsigned long)(millis() / 1000));
    out = h;
    out += stamp() + status_line(false);   // beim Einschalten waehrend des Starts: nur Speicher
  }
  while (s_run) {
    esp_rom_install_channel_putc(2, stage_putc);   // Arduino setzt Kanal 2 bei Serial.begin/setDebugOutput zurueck
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    size_t n;
    portENTER_CRITICAL(&s_mux);
    n = s_stage_n;
    memcpy(s_take, s_stage, n);
    s_stage_n = 0;
    portEXIT_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
      char c = s_take[i];
      if (c == '\r') continue;
      if (line_start) {
        out += stamp();
        line_start = false;
      }
      out += c;
      if (c == '\n') line_start = true;
    }
    uint32_t now = millis();
    if (now - t_status >= STATUS_MS) {
      t_status = now;
      if (!line_start) {
        out += '\n';
        line_start = true;
      }
      out += stamp() + status_line(now > 20000);
    }
    if (!out.empty()) {
      xSemaphoreTake(s_lock, portMAX_DELAY);
      ring_put(out);
      xSemaphoreGive(s_lock);
      fbuf += out;
      out.clear();
    }
    // Ablage (SD-Karte): erst suchen, wenn noetig, danach hoechstens alle 30 s erneut
    if (dir.empty() && s_storage && (first_store || now - t_store >= STORE_RETRY_MS)) {
      first_store = false;
      t_store = now;
      const char *d = s_storage();
      if (d && *d) {
        dir = d;
        mkdirs(dir);
        fp = open_new(dir);
        if (!fp) dir.clear();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dir = dir;
        if (!fp) s_file.clear();
        xSemaphoreGive(s_lock);
      }
    }
    if (!fp) {
      if (fbuf.size() > 16384) fbuf.erase(0, fbuf.size() - 16384);   // ohne Karte nicht endlos sammeln
      continue;
    }
    if (now - t_file < FILE_MS || fbuf.empty()) continue;
    t_file = now;
    bool ok = fwrite(fbuf.data(), 1, fbuf.size(), fp) == fbuf.size() && fflush(fp) == 0;
    if (ok) fsync(fileno(fp));
    fbuf.clear();
    if (!ok || ftell(fp) > FILE_MAX) {
      fclose(fp);
      fp = ok ? open_new(dir) : nullptr;
      if (!fp) {   // Karte weg: spaeter neu suchen
        dir.clear();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dir.clear();
        s_file.clear();
        xSemaphoreGive(s_lock);
      }
    }
  }
  if (fp) {
    if (!fbuf.empty()) fwrite(fbuf.data(), 1, fbuf.size(), fp);
    fclose(fp);
  }
  s_task_alive = false;
  vTaskDelete(nullptr);
}

void start() {
  if (s_run) return;
  if (!s_lock) s_lock = xSemaphoreCreateMutex();
  size_t stage = s_ring_cfg >= 32768 ? 2048 : 1024;
  char *st = (char *)heap_caps_malloc(stage, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_take = (char *)heap_caps_malloc(stage, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  s_ring = (char *)heap_caps_malloc(s_ring_cfg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_ring) s_ring = (char *)heap_caps_malloc(s_ring_cfg, MALLOC_CAP_8BIT);
  if (!st || !s_take || !s_ring) {
    heap_caps_free(st);
    heap_caps_free(s_take);
    heap_caps_free(s_ring);
    s_take = s_ring = nullptr;
    log_w("Debug-Log: kein Speicher");
    return;
  }
  s_ring_sz = s_ring_cfg;
  s_ring_pos = 0;
  s_ring_full = false;
  s_lost = 0;
  portENTER_CRITICAL(&s_mux);
  s_stage_sz = stage;
  s_stage_n = 0;
  s_stage = st;
  portEXIT_CRITICAL(&s_mux);
  s_prev = esp_log_set_vprintf(idf_vprintf);
  esp_rom_install_channel_putc(2, stage_putc);
  s_run = true;
  if (xTaskCreate(task, "dbglog", 4608, nullptr, 1, nullptr) != pdPASS) {
    s_run = false;
    log_w("Debug-Log: Task startet nicht");
  }
}

void stop() {
  if (!s_run) return;
  s_run = false;
  for (int i = 0; i < 40 && s_task_alive; i++) vTaskDelay(pdMS_TO_TICKS(50));   // Task schreibt den Rest weg
  esp_rom_install_channel_putc(2, nullptr);
  esp_log_set_vprintf(s_prev ? s_prev : vprintf);
  portENTER_CRITICAL(&s_mux);
  char *st = s_stage;
  s_stage = nullptr;
  s_stage_n = 0;
  portEXIT_CRITICAL(&s_mux);
  heap_caps_free(st);
  heap_caps_free(s_take);
  s_take = nullptr;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  heap_caps_free(s_ring);
  s_ring = nullptr;
  s_ring_sz = s_ring_pos = 0;
  s_dir.clear();
  s_file.clear();
  xSemaphoreGive(s_lock);
}

}  // namespace

void dbglog_begin(size_t ring, const char *(*storage)()) {
  s_ring_cfg = ring < 2048 ? 2048 : ring;
  s_storage = storage;
  Preferences p;
  if (p.begin("dbglog", true)) {
    s_on = p.getBool("on", false);
    p.end();
  }
  if (s_on) start();
}

bool dbglog_on() { return s_on; }

void dbglog_set(bool on) {
  Preferences p;
  if (p.begin("dbglog", false)) {
    p.putBool("on", on);
    p.end();
  }
  if (on == s_on) return;
  s_on = on;
  if (on) {
    start();
    dbglog_note("Debug-Log eingeschaltet");
  } else {
    dbglog_note("Debug-Log ausgeschaltet");
    stop();
  }
}

void dbglog_set_status(std::string (*fn)()) { s_status = fn; }

void dbglog_note(const char *text) {
  stage_write(text, strlen(text));
  stage_write("\n", 1);
}

std::string dbglog_ring() {
  std::string r;
  if (!s_lock) return r;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  if (s_ring) {
    if (s_ring_full) {
      r.assign(s_ring + s_ring_pos, s_ring_sz - s_ring_pos);
      r.append(s_ring, s_ring_pos);
      size_t nl = r.find('\n');   // angeschnittene erste Zeile weglassen
      if (nl != std::string::npos) r.erase(0, nl + 1);
    } else {
      r.assign(s_ring, s_ring_pos);
    }
  }
  xSemaphoreGive(s_lock);
  return r;
}

std::string dbglog_dir() {
  if (!s_lock) return std::string();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  std::string d = s_dir;
  xSemaphoreGive(s_lock);
  if (d.empty() && !s_run && s_storage) {   // ausgeschaltet: alte Dateien trotzdem zeigen, wenn eine Karte da ist
    const char *x = s_storage();
    if (x) d = x;
  }
  return d;
}

std::string dbglog_current() {
  if (!s_lock) return std::string();
  xSemaphoreTake(s_lock, portMAX_DELAY);
  std::string f = s_file;
  xSemaphoreGive(s_lock);
  return f;
}

std::vector<DbgFile> dbglog_files() { return list_files(dbglog_dir()); }

bool dbglog_valid_name(const std::string &n) {
  if (n.size() != 13 || n.compare(0, 4, "log-") != 0 || n.compare(9, 4, ".txt") != 0) return false;
  for (int i = 4; i < 9; i++)
    if (n[i] < '0' || n[i] > '9') return false;
  return true;
}

bool dbglog_delete(const std::string &name) {
  std::string dir = dbglog_dir(), cur = dbglog_current();
  if (dir.empty()) return false;
  if (name == "*") {
    for (auto &f : list_files(dir))
      if (f.name != cur) remove((dir + "/" + f.name).c_str());
    return true;
  }
  if (!dbglog_valid_name(name) || name == cur) return false;
  return remove((dir + "/" + name).c_str()) == 0;
}

uint32_t dbglog_lost() { return s_lost; }

size_t dbglog_ring_size() { return s_ring_cfg; }

#if DBGLOG_CYD_SD
#include <SD.h>
#include <SPI.h>
// CYD (ESP32-2432S028R): Kartenslot am VSPI. Erst einbinden, wenn der Debug-Log ihn braucht (spart RAM).
const char *dbglog_cyd_sd() {
  static bool ok = false;
  static SPIClass spi(VSPI);
  if (!ok) {
    spi.begin(18, 19, 23, 5);
    ok = SD.begin(5, spi, 16000000, "/sd", 2);
    if (!ok) spi.end();
  }
  return ok ? "/sd/logs" : "";
}
#endif

#endif   // WITH_DBGLOG
