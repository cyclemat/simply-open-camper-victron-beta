// Zigbee2MQTT ueber MQTT: Geraeteliste, Zustaende, Befehle, Umbenennen
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

enum CtlKind : uint8_t { CTL_LIGHT = 1, CTL_SWITCH = 2 };
enum ColorCap : uint8_t { COLOR_NONE = 0, COLOR_XY = 1, COLOR_HS = 2 };

// Kopie eines steuerbaren Elements (eine Lampe bzw. ein Schaltkanal) fuer UI und Web
struct ControlView {
  std::string key;          // "<ieee>" oder "<ieee>#<endpoint>"
  std::string ieee, ep;
  std::string name;         // Zigbee2MQTT friendly_name
  std::string model, vendor, description;
  CtlKind kind = CTL_LIGHT;
  bool has_bri = false, has_ct = false;
  uint8_t color = COLOR_NONE;
  int bri_max = 254, ct_min = 153, ct_max = 500;
  // Zustand
  bool known = false, on = false, online = true, pending = false;
  int bri = -1;             // Rohwert 0..bri_max
  int ct = -1;              // Mired
  float hue = -1, sat = -1; // 0..360 / 0..100
  float cx = -1, cy = -1;   // CIE xy
  std::string cmode;        // "xy", "hs", "color_temp"
  std::string info;         // Messwerte fuer die Kachel, z. B. "45 W · 6,70 kWh" (Zigbee direkt: Steckdosen)
  int bri_pct() const { return bri < 0 ? -1 : (bri * 100 + bri_max / 2) / (bri_max ? bri_max : 254); }
};

void z2m_begin();
void z2m_restart();                     // nach Aenderung der MQTT-Einstellungen
bool z2m_connected();
std::string z2m_broker_host();   // Broker, den das Panel nutzt (eingetragen oder im Panel-WLAN gefunden), "" = keiner
bool z2m_bridge_online();
const char *z2m_status_text();
const char *z2m_diag_text();            // Zustand der Geraeteliste (Diagnose)
uint32_t z2m_devices_version();         // aendert sich bei neuer Geraeteliste
uint32_t z2m_state_version();           // aendert sich bei jeder Zustandsmeldung

bool z2m_get(const std::string &key, ControlView &out);
std::vector<ControlView> z2m_all();     // alphabetisch nach Name
size_t z2m_count();

void z2m_toggle(const std::string &key);
void z2m_publish_retained(const std::string &topic, const std::string &payload);   // gespeichert beim Broker (leer = loeschen)
void z2m_set_on(const std::string &key, bool on);
void z2m_set_brightness_pct(const std::string &key, int pct);
void z2m_set_ct(const std::string &key, int mired);
void z2m_set_color_hs(const std::string &key, float hue, float sat);
// Benennt das Geraet in Zigbee2MQTT um; liefert Fehlermeldung oder "" bei Erfolg (asynchron bestaetigt)
std::string z2m_rename(const std::string &ieee, const std::string &new_name);
void z2m_refresh_states();
// Geraeteliste von Zigbee2MQTT neu holen (sonst gilt der Zwischenspeicher in LittleFS) – Editor/Knopf
void z2m_reload_devices();
// Fuer weitere Geraetearten am selben Broker (Shelly)
void z2m_publish(const std::string &topic, const std::string &payload);
std::string z2m_client_id();
