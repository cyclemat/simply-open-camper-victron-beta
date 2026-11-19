// WLAN (Netzwerk-, Router- und Kombi-Modus), DHCP/DNS im Panel-WLAN, mDNS, Uhrzeit
#pragma once
#include <Arduino.h>
#include <vector>

struct WifiNet {
  String ssid;
  int rssi;
  bool secure;
};

void net_begin();
void net_loop();
void net_reconnect();                // nach Aenderung von g_settings.wifi_*
uint8_t net_mode();                  // NetMode beim Start (Wechsel erfordert Neustart)

// Camp-WLAN (Station)
bool net_connected();
String net_ip();
String net_ssid();
int net_rssi();
const char *net_status_text();

// Panel-WLAN (Access Point)
bool net_ap_active();
String net_ap_ip();
int net_ap_clients();
// Sucht unter den Teilnehmern im Panel-WLAN einen MQTT-Broker (Port 1883). Blockiert kurz.
bool net_find_broker(String &host);

// Adresse, unter der der Editor erreichbar ist (Heimnetz bevorzugt)
String net_editor_ip();

void net_scan_start();
bool net_scan_running();
bool net_scan_take(std::vector<WifiNet> &out);  // true, sobald die Suche fertig (oder gescheitert) ist
bool net_scan_failed();              // letzte Suche gescheitert (nach net_scan_take abfragen)
// WLAN-Suche fuer die Weboberflaeche (aus dem Web-Task): anstossen und Ergebnis abholen. Die Suche selbst laeuft in
// net_loop(); das Ergebnis bleibt liegen, bis die naechste Suche startet. false = Suche laeuft noch.
void net_web_scan_request();
bool net_web_scan_result(std::vector<WifiNet> &out, bool &failed);
uint32_t net_offline_ms();           // wie lange das Camp-WLAN schon getrennt ist (0 = verbunden / Router-Modus)
bool net_time_valid();
void net_set_time(uint32_t epoch);   // Uhrzeit vom Browser uebernehmen (ohne Internet)
void net_set_time_manual(time_t epoch);   // Uhrzeit von Hand (Einstellungen am Panel)
const char *net_time_source();       // "Internet", "Browser", "Uhrenchip", "von Hand" oder "" (unbekannt)
