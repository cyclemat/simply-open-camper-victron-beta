// Mopeka Tanksensoren per Bluetooth LE – alle Modelle in einem Modul:
//   Pro-Familie (Hersteller-ID 0x0059, 10 Byte): Pro Check, Pro Plus, Pro Check Universal, Pro Check H2O,
//   Lippert BottleCheck, Pro-200, TD40/TD200 (auch mit Mobilfunk)
//   Standard-Familie (Hersteller-ID 0x000D, 23 Byte): Mopeka Check Standard, XL, eTrailer
// Die Sensoren senden ihre Messwerte unverschluesselt in den Werbepaketen. Keine Verbindung, kein Schluessel.
//
// Das Modul haengt nur an NimBLE-freien Typen: Der Bluetooth-Scan (truma.cpp im Panel, spaeter der Satellit)
// reicht jedes Paket mit Herstellerdaten an mopeka_on_adv() weiter. Gespeichert wird per Preferences ("mopeka").
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>

#define MOPEKA_MAX_SENSORS 4
#define MOPEKA_KEY_PREFIX "mopeka:"
#define MOPEKA_TANK_CUSTOM 255

enum MopekaMedium : uint8_t { MOP_PROPANE = 0, MOP_AIR = 1, MOP_WATER = 2 };

// Flaschen-/Tankvorgabe: Fuellhoehe in mm bei leer und voll, Inhalt bei voll (kg bzw. Liter)
struct MopekaTank {
  const char *name;
  uint16_t empty_mm, full_mm;
  float capacity;
};

struct MopekaInfo {
  std::string key;             // "mopeka:<mac>" – so liegt die Kachel im Layout
  std::string mac, name;
  uint16_t model = 0;          // Pro: Hardware-Byte, Standard: 0x100 | Hardware-ID
  std::string model_text;      // "Pro Check", "Pro Plus", ...
  MopekaMedium medium = MOP_PROPANE;
  // Flasche
  uint8_t tank = 0;            // Index in mopeka_tanks() oder MOPEKA_TANK_CUSTOM
  uint16_t empty_mm = 38, full_mm = 366;
  float capacity = 11;
  const char *unit = "kg";     // "kg" bei Gas, sonst "l"
  // letzte Werte
  bool has = false;            // mindestens ein brauchbarer Messwert
  int percent = 0;
  int level_mm = 0;            // geglaettete Fuellhoehe
  float amount = 0;            // Inhalt in kg bzw. l (0, wenn kein Inhalt angegeben)
  int battery = 0;             // %
  float battery_v = 0;
  int temperature = 0;         // °C
  int quality = 0;             // 0..3
  int rssi = 0;
  int32_t age_s = -1;          // Sekunden seit dem letzten Paket
  std::string status;          // "OK", "Kein Echo – Sensor richtig angebracht?", ...
};

struct MopekaFound {
  std::string mac, model_text;
  uint16_t model = 0;
  int rssi = 0;
  bool button = false;         // Sync-Taste wurde waehrend der Suche gedrueckt
  bool configured = false;
};

void mopeka_begin();
uint32_t mopeka_version();                         // aendert sich bei neuen Werten/Konfiguration
bool mopeka_has_devices();
static inline bool mopeka_is_key(const std::string &k) { return k.rfind(MOPEKA_KEY_PREFIX, 0) == 0; }

// Vom Bluetooth-Scan fuer jedes Paket mit Herstellerdaten aufgerufen.
// md = komplette Herstellerdaten inkl. der zwei Bytes Hersteller-ID. true = war ein Mopeka-Paket.
bool mopeka_on_adv(const std::string &mac, const uint8_t *md, size_t len, int rssi);

std::vector<MopekaInfo> mopeka_list();
bool mopeka_get(const std::string &key, MopekaInfo &out);

void mopeka_discovery_start();                     // 3 Minuten nach Sensoren suchen
bool mopeka_discovery_running();
std::vector<MopekaFound> mopeka_found();

std::string mopeka_add(const std::string &mac, const std::string &name, int tank);  // "" = ok
void mopeka_remove(const std::string &mac);
void mopeka_rename(const std::string &mac, const std::string &name);
// tank = Vorgabe; bei MOPEKA_TANK_CUSTOM gelten empty_mm/full_mm/capacity
std::string mopeka_set_tank(const std::string &mac, int tank, int empty_mm, int full_mm, float capacity);

size_t mopeka_tank_count();
const MopekaTank &mopeka_tank(size_t i);
const char *mopeka_model_text(uint16_t model);
