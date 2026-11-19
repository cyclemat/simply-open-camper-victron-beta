// Truma LevelControl (Gasflaschen-Fuellstand) direkt per Bluetooth LE.
// Protokoll aus github.com/cyclemat/HAOS-Truma-Levelcontrol-Integration:
// Passwort (5 Byte) lesen, RC4-Schluessel = Passwort XOR "TRUMA", Messung mit 0x03/0x02 ausloesen,
// nach 3 s Messwert lesen und entschluesseln.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include <time.h>

#define TRUMA_MAX_SENSORS 4
#define TRUMA_KEY_PREFIX "truma:"

struct TrumaInfo {
  std::string key;          // "truma:<mac>" – so liegt die Kachel im Layout
  std::string mac;
  std::string name;
  bool has_reading = false;
  int percent = 0;
  float kg = 0;
  int battery = 0;
  int temperature = 0;
  int quality = 0;          // 0..3
  int32_t age_s = -1;       // Sekunden seit der letzten Messung, -1 = unbekannt
  bool busy = false;        // misst gerade
  std::string status;       // fuer Anzeige, z. B. "Warte auf Sensor …"
};

struct TrumaFound {
  std::string mac;
  uint8_t addr_type = 0;
  int rssi = 0;
  bool configured = false;
};

void truma_begin();
void truma_ble_init();                            // Bluetooth sofort starten (Satellit: fuer seine Werbepakete)
uint32_t truma_version();                         // aendert sich bei neuen Werten/Konfiguration
std::vector<TrumaInfo> truma_list();
bool truma_get(const std::string &key, TrumaInfo &out);
static inline bool truma_is_key(const std::string &k) { return k.rfind(TRUMA_KEY_PREFIX, 0) == 0; }

void truma_measure(const std::string &mac);       // leer = alle
void truma_ble_census(uint16_t seconds);           // serieller Befehl "ble": Geraete in Reichweite zaehlen
void truma_scan_start();                           // bis zu 3 Minuten nach LevelControls suchen
bool truma_scan_running();
std::vector<TrumaFound> truma_scan_results();
std::string truma_add(const std::string &mac, uint8_t addr_type, const std::string &name);  // "" = ok
void truma_remove(const std::string &mac);
void truma_rename(const std::string &mac, const std::string &name);
void truma_set_interval(uint16_t minutes);
uint16_t truma_interval();
