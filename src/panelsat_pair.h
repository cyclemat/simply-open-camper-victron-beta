// Panel: Satelliten per Knopf einrichten – siehe sat_pair_proto.h
// Der Bluetooth-Verteiler (truma.cpp) meldet Satelliten im Einrichtungsmodus und baut die Verbindung in seiner
// Scan-Pause auf. In Satelliten und im Victron-Display gibt es das nicht (dort greifen leere "weak"-Fassungen).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

class NimBLEAdvertisedDevice;

// Aufrufe aus truma.cpp (Bluetooth-Task). Gesucht wird nur nach "Neuen Satelliten hinzufuegen" (3 Minuten).
bool satpair_searching();
bool satpair_on_adv(const NimBLEAdvertisedDevice *dev);   // true = war ein Satellit im Einrichtungsmodus
bool satpair_want_connect();
void satpair_connect_now();

// Bedienung (Panel-Einstellungen, Weboberflaeche)
struct SatPairCand {
  std::string mac, name;
  int rssi = 0;
};
void satpair_search_start();                     // 3 Minuten nach Satelliten im Einrichtungsmodus suchen
void satpair_search_stop();
uint32_t satpair_search_left();                  // Sekunden bis zum Ende der Suche, 0 = sucht nicht
std::vector<SatPairCand> satpair_candidates();   // Satelliten im Einrichtungsmodus in der Naehe
std::string satpair_start(const std::string &mac); // "" = gestartet, sonst Grund (z. B. "WLAN ist aus")
int satpair_state();                               // 0 nichts, 1 laeuft, 2 fertig, 3 Fehler
std::string satpair_status();                      // Text zum Zustand
std::string satpair_ip();                          // gemeldete IP nach Erfolg
uint32_t satpair_version();                        // aendert sich bei neuen Kandidaten/Zustand
