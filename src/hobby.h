// Hobby-Wohnwagen/-Reisemobile: Licht, Batterie, Wasser, Heizung, Kuehlschrank, Klima direkt am Panel.
// Zwei Wege, beide einzeln abschaltbar und ab Werk AUS:
//  - Bluetooth zur "HobbyConnect"-Box (BLE-Name "HobbyConnect Data", Textnachrichten "KEY:WERT")
//  - Kabel zum LSG-Modul (CI-BUS, seriell 115200 Baud ueber RS232-Wandler, Zeilen "$<id>:<wert>")
// Beide liefern dieselben Variablennamen (LIGHT_KUECHE, IBS0_UBAT …). Die Werte erscheinen wie die Anschluesse eines
// Satelliten "Hobby" (Schluessel "satio:Hobby/<punkt>"), die Bordbatterie zusaetzlich als Batteriewaechter-Kachel
// "hobby:bat". Protokolle: docs/protokolle/hobby_ble_cibus.md.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "sat_proto.h"
#include "victron.h"

#ifndef WITH_HOBBY
#if PANEL_SATELLITE
#define WITH_HOBBY 0
#else
#define WITH_HOBBY 1
#endif
#endif

#define HOBBY_SAT_ID "Hobby"
#define HOBBY_BAT_KEY "hobby:bat"

class NimBLEAdvertisedDevice;

struct HobbyCand {
  std::string mac, name;
  uint8_t type = 0;
  int rssi = 0;
};

struct HobbyCfg {
  bool ble_on = false;       // Bluetooth zur HobbyConnect-Box
  std::string mac;           // gemerkte Box ("" = noch keine gewaehlt)
  uint8_t mac_type = 0;
  bool uart_on = false;      // Kabel zum LSG (CI-BUS)
  int8_t uart = 1;           // Hardware-UART (1 …)
  int8_t rx = -1, tx = -1;   // GPIOs
};

#if WITH_HOBBY
void hobby_begin();                     // Einstellungen laden, Kabel-Task nur wenn eingeschaltet
HobbyCfg hobby_cfg();
void hobby_set_cfg(const HobbyCfg &c);  // speichern und anwenden
bool hobby_active();                    // mindestens ein Weg an
std::string hobby_status();             // Bluetooth
std::string hobby_uart_status();        // Kabel
uint32_t hobby_version();
std::vector<SatPoint> hobby_points();
bool hobby_switch(uint8_t idx, bool on);
bool hobby_battery(VictronInfo &v);     // Bordbatterie (IBS) als Batteriewaechter
void hobby_search(uint32_t ms);         // Box suchen (Einstellungsseite offen)
std::vector<HobbyCand> hobby_candidates();
int hobby_uart_count();                 // Zahl der Hardware-UARTs des Chips
void hobby_print_debug();               // serieller Befehl "hobby"
// Bluetooth-Verteiler (truma.cpp)
bool hobby_ble_needed();
bool hobby_on_adv(const NimBLEAdvertisedDevice *dev);
void hobby_scan_wanted(bool &passive, bool &active);
bool hobby_want_connect();
void hobby_connect_now();
void hobby_ble_loop();                  // Befehle senden, Sitzung erneuern (Bluetooth-Task)
#else
inline void hobby_begin() {}
inline HobbyCfg hobby_cfg() { return HobbyCfg(); }
inline void hobby_set_cfg(const HobbyCfg &) {}
inline bool hobby_active() { return false; }
inline std::string hobby_status() { return ""; }
inline std::string hobby_uart_status() { return ""; }
inline uint32_t hobby_version() { return 0; }
inline std::vector<SatPoint> hobby_points() { return {}; }
inline bool hobby_switch(uint8_t, bool) { return false; }
inline bool hobby_battery(VictronInfo &) { return false; }
inline void hobby_search(uint32_t) {}
inline std::vector<HobbyCand> hobby_candidates() { return {}; }
inline int hobby_uart_count() { return 0; }
inline void hobby_print_debug() {}
inline bool hobby_ble_needed() { return false; }
inline bool hobby_on_adv(const NimBLEAdvertisedDevice *) { return false; }
inline void hobby_scan_wanted(bool &, bool &) {}
inline bool hobby_want_connect() { return false; }
inline void hobby_connect_now() {}
inline void hobby_ble_loop() {}
#endif
