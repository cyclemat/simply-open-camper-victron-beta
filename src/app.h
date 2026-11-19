// Thread-sichere Ereignisse zwischen Netzwerk-/Web-Tasks und der Oberflaeche (Arduino-loop)
#pragma once
#include <stdint.h>
#include <stddef.h>

enum AppEventType : uint8_t {
  EV_LAYOUT_CHANGED = 1,   // Layout neu laden und Oberflaeche neu aufbauen
  EV_BG_CHANGED,           // s = Seiten-ID
  EV_SETTINGS_CHANGED,     // ptr = Settings* (neue Werte), wird vom Empfaenger geloescht
  EV_WIFI_CHANGED,         // ptr = Settings* (nur ssid/pass relevant)
  EV_REBOOT,
  EV_SAVER_PREVIEW,        // Bildschirmschoner sofort zeigen
  EV_SAVER_LOGO,           // eigenes Logo wurde ersetzt/geloescht
};

struct AppEvent {
  AppEventType type;
  void *ptr;
  char s[24];
};

void app_events_init();
bool app_post(AppEventType type, void *ptr = nullptr, const char *s = nullptr);
bool app_poll(AppEvent &ev);

// Kurze Meldungen fuer die Toast-Anzeige, aus jedem Task aufrufbar
void app_toast(const char *fmt, ...);
bool app_pop_toast(char *buf, size_t len);
