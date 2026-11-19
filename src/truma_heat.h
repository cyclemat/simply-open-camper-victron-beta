// Gemeinsames Datenmodell fuer die Truma-Heizung (Combi/VarioHeat) – unabhaengig vom Bedienteil.
// Zwei Integrationen liefern es, jede mit eigenem Schalter beim Kompilieren:
//   truma_cpplus.cpp  (WITH_TRUMA_CPPLUS) – CP Plus ueber den LIN-Bus, das Panel spielt die iNet Box
//   truma_inetx.cpp   (WITH_TRUMA_INETX)  – iNet-X-Bedienteil per Bluetooth LE
// Beide haengen nicht an der Oberflaeche und sind fuer den Satelliten wiederverwendbar.
#pragma once
#include <stdint.h>
#include <math.h>
#include <string>

#ifndef WITH_TRUMA_CPPLUS
#define WITH_TRUMA_CPPLUS 0
#endif
#ifndef WITH_TRUMA_INETX
#define WITH_TRUMA_INETX 0
#endif

#ifndef WITH_ALDE
#define WITH_ALDE 0
#endif

#define TRUMA_HEAT_PREFIX "trumaheat:"
#define TRUMA_HEAT_KEY_CPPLUS "trumaheat:cpplus"
#define TRUMA_HEAT_KEY_INETX "trumaheat:inetx"
#define TRUMA_HEAT_KEY_ALDE "trumaheat:alde"     // Alde Compact 3020/3030 (gleiche Kachel, eigenes Modul alde.cpp)

// Wie das Bedienteil die Energiequelle waehlt (fuer die Bedienelemente)
enum HeatEnergyStyle : uint8_t {
  HES_TRUMA = 0,   // Gas / Mix 900/1800 W / Elektro 900/1800 W
  HES_INETX = 1,   // nur Elektrostufe 0/900/1800 W
  HES_ALDE = 2,    // Gas an/aus und Elektro aus/1/2/3 kW unabhaengig
};

enum TrumaWater : uint8_t { TW_OFF = 0, TW_ECO = 1, TW_HOT = 2, TW_BOOST = 3 };
enum TrumaEnergy : uint8_t { TE_NONE = 0, TE_GAS = 1, TE_ELECTRIC = 2, TE_MIX = 3 };

struct TrumaHeatInfo {
  std::string key;               // TRUMA_HEAT_KEY_* – so liegt die Kachel im Layout
  std::string source;            // "Truma CP Plus", "Truma iNet X"
  bool connected = false;        // Verbindung zum Bedienteil steht
  bool has = false;              // mindestens ein Status empfangen
  // Heizung
  bool heat_on = false;
  float room_target = NAN;       // °C
  float room_actual = NAN;       // °C
  uint8_t heat_mode = 0;         // Index in mode_names
  const char *mode_names[4] = {nullptr, nullptr, nullptr, nullptr};
  uint8_t mode_count = 0;
  bool running = false;          // Brenner/Heizstab arbeitet gerade
  // Boiler
  TrumaWater water = TW_OFF;
  float water_actual = NAN;      // °C
  bool water_boost = true;       // Bedienteil kennt "Boost"
  bool water_hot = true;         // Bedienteil kennt "Heiss" (Alde: nur aus/an/Boost)
  // Energie
  TrumaEnergy energy = TE_GAS;
  uint16_t el_power = 0;         // W: Truma 0/900/1800, Alde 0/1000/2000/3000
  bool energy_writable = true;   // iNet X: Gas/Elektro folgt der Heizung, nur die Elektrostufe ist waehlbar
  HeatEnergyStyle energy_style = HES_TRUMA;
  // Sonstiges
  float outdoor = NAN;           // Aussentemperatur (Alde)
  float fluid = NAN;             // Glykol-/Vorlauftemperatur (Alde 3030, roter Bus)
  float battery_v = NAN;
  int error = 0;                 // 0 = kein Fehler
  bool busy = false;             // Befehl ist unterwegs
  int32_t age_s = -1;            // Sekunden seit dem letzten Status
  std::string status;            // "OK", "Warte auf CP Plus …", ...
};

static inline bool truma_heat_is_key(const std::string &k) { return k.rfind(TRUMA_HEAT_PREFIX, 0) == 0; }
