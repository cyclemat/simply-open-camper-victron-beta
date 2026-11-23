// Votronic (Batteriecomputer/Smart Shunt, Solarregler SR/MPP, Ladegeraete VBCS/VAC, Ladebooster VCC) am Panel.
// Zwei Wege, beide einzeln abschaltbar und ab Werk AUS:
//  - Bluetooth zum "Bluetooth Connector S-BC" (Kopplung mit PIN, Dauerverbindung, Meldungen von Batteriecomputer
//    und Solarregler)
//  - Kabel an der Display-Buchse eines Geraets ("Display Link", RJ11, 1000 Baud, Rahmen zu 16 Byte) – nur Empfang
// Die Werte erscheinen als Victron-artige Kacheln: "vot:bat" (Batteriewaechter), "vot:solar", "vot:charger",
// "vot:booster". Eigene Umsetzung nach Protokollwissen aus syssi/esphome-votronic (Apache-2.0).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "victron.h"

#ifndef WITH_VOTRONIC
#if PANEL_SATELLITE || SOC_VICTRON_EDITION
#define WITH_VOTRONIC 0
#else
#define WITH_VOTRONIC 1
#endif
#endif

#define VOTRONIC_PIN_DEFAULT "173928"   // feste Kopplungs-PIN des S-BC (laut Vorlage)

class NimBLEAdvertisedDevice;

struct VotronicCand {
  std::string mac, name;
  uint8_t type = 0;
  int rssi = 0;
};

struct VotronicCfg {
  bool ble_on = false;       // Bluetooth zum S-BC
  std::string mac;           // gemerkter S-BC ("" = noch keiner gewaehlt)
  uint8_t mac_type = 0;
  std::string pin = VOTRONIC_PIN_DEFAULT;
  bool uart_on = false;      // Kabel an der Display-Buchse
  int8_t uart = 1;           // Hardware-UART (1 …)
  int8_t rx = -1;            // GPIO (nur Empfang)
};

// Eine Zeile der Werteliste auf der Einstellungsseite (Geraet, Bezeichnung, Wert als Text)
struct VotronicRow {
  std::string dev, name, value;
};

#if WITH_VOTRONIC
void votronic_begin();                       // Einstellungen laden, Kabel-Task nur wenn eingeschaltet
VotronicCfg votronic_cfg();
void votronic_set_cfg(const VotronicCfg &c); // speichern und anwenden
bool votronic_active();                      // mindestens ein Weg an
std::string votronic_status();               // Bluetooth
std::string votronic_uart_status();          // Kabel
uint32_t votronic_version();
bool votronic_get(const std::string &key, VictronInfo &v);   // "vot:…"
std::vector<VictronInfo> votronic_list();    // alle Geraete mit Werten
std::vector<VotronicRow> votronic_rows();    // alle Werte (Einstellungsseite, Editor)
void votronic_search(uint32_t ms);           // S-BC suchen (Einstellungsseite offen)
std::vector<VotronicCand> votronic_candidates();
int votronic_uart_count();
void votronic_print_debug();                 // serieller Befehl "votronic"
// Bluetooth-Verteiler (truma.cpp)
bool votronic_ble_needed();
bool votronic_on_adv(const NimBLEAdvertisedDevice *dev);
void votronic_scan_wanted(bool &passive, bool &active);
bool votronic_want_connect();
void votronic_connect_now();
void votronic_ble_loop();
#else
inline void votronic_begin() {}
inline VotronicCfg votronic_cfg() { return VotronicCfg(); }
inline void votronic_set_cfg(const VotronicCfg &) {}
inline bool votronic_active() { return false; }
inline std::string votronic_status() { return ""; }
inline std::string votronic_uart_status() { return ""; }
inline uint32_t votronic_version() { return 0; }
inline bool votronic_get(const std::string &, VictronInfo &) { return false; }
inline std::vector<VictronInfo> votronic_list() { return {}; }
inline std::vector<VotronicRow> votronic_rows() { return {}; }
inline void votronic_search(uint32_t) {}
inline std::vector<VotronicCand> votronic_candidates() { return {}; }
inline int votronic_uart_count() { return 0; }
inline void votronic_print_debug() {}
inline bool votronic_ble_needed() { return false; }
inline bool votronic_on_adv(const NimBLEAdvertisedDevice *) { return false; }
inline void votronic_scan_wanted(bool &, bool &) {}
inline bool votronic_want_connect() { return false; }
inline void votronic_connect_now() {}
inline void votronic_ble_loop() {}
#endif
