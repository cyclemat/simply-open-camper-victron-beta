// Hymer Connect per Bluetooth LE: direkt zur SCU (Smart Control Unit) im Fahrzeug, ohne Cloud.
// Gilt fuer Fahrzeuge der Erwin Hymer Group mit HYMER Connect (Hymer, Buerstner, Dethleffs, Eriba, LMC, Carado ...).
//
// Ablauf wie in der Hymer-App: Nordic-UART-Dienst, Kopplung (Just Works – an der SCU vorher "CONNECTION" druecken),
// darueber TLS 1.0/1.1 (hymer_tls.cpp) und darin protobuf-Nachrichten ("PIA"): Abos der Sensorgruppen, danach schickt
// die SCU laufend Werte (Bus-ID, Sensor-ID, Wert). Schalten (Licht, Hauptschalter, Pumpe ...) mit derselben Nachricht.
// Protokoll nach Facetti/hymer-connect-ha-ble (MIT) bzw. BetaHydri/hymer-connect-ha, eigene Umsetzung.
//
// Die Werte erscheinen als Satelliten-Punkte (SatPoint): auf dem Panel unter "satio:Hymer/<punkt>", am Satelliten als
// eigene Punkte mit Nummern ab HYMER_IDX_BASE – damit laufen sie ohne Extra-Kacheln ueber MQTT, UDP und Bluetooth.
// Schalter: -DWITH_HYMER=1 ([features] hymer in platformio.ini); ein-/ausschalten zur Laufzeit ueber die
// Bluetooth-Protokolle (BLE_P_HYMER).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "sat_proto.h"

#ifndef WITH_HYMER
#define WITH_HYMER 0
#endif

#define HYMER_IDX_BASE 100          // Punkt-Nummern der Hymer-Werte (am Satelliten: "analog103", "switch112" ...)
#define HYMER_SAT_ID "Hymer"        // Panel: Schluessel "satio:Hymer/<punkt>"

class NimBLEAdvertisedDevice;

static inline bool hymer_enabled() { return WITH_HYMER; }
void hymer_begin();
uint32_t hymer_version();

// Werte als Punkte (nur solche, die die SCU schon gemeldet hat)
std::vector<SatPoint> hymer_points();
bool hymer_has_switches();
bool hymer_switch(uint8_t idx, uint8_t val);   // val: 0 aus, 1 an, 2 umschalten
// Name eines Hymer-Punkts (auch ohne WITH_HYMER – das Panel braucht ihn fuer Satelliten-Pakete per Bluetooth)
const char *hymer_point_name(uint8_t idx);     // nullptr = kein Hymer-Punkt
static inline bool hymer_is_idx(uint8_t idx) { return idx >= HYMER_IDX_BASE; }

// Kopplung: erst hier starten, dann an der SCU (Bedienteil) "CONNECTION" druecken
void hymer_pair_start();
// Geraeteliste zum Auswaehlen: hymer_browse() solange die Liste offen ist (aktiv suchen, ~15 s Nachlauf)
struct HymerCand {
  std::string mac, name;
  int rssi;
  uint8_t score;   // 2 = sicher Hymer (Power-Dienst/Name), 1 = nur Nordic UART
};
void hymer_browse();
std::vector<HymerCand> hymer_candidates();
void hymer_pair_with(const std::string &mac);  // Koppeln nur mit diesem Geraet
bool hymer_pairing();
bool hymer_paired();
bool hymer_mobile_registered();                 // Panel bei der SCU als Mobilgeraet angemeldet (QR-Code)
bool hymer_connected();                        // TLS steht, Werte laufen
void hymer_forget();
void hymer_disconnect();                       // Protokoll ausgeschaltet: trennen, Kopplung bleibt
std::string hymer_status();
void hymer_print_debug();                      // serieller Befehl "hymer"

// Fuer den Bluetooth-Scan in truma.cpp
void hymer_ble_setup();
bool hymer_on_adv(const NimBLEAdvertisedDevice *dev);   // true = war eine SCU
void hymer_scan_wanted(bool &passive, bool &active);
bool hymer_want_connect();
void hymer_connect_now();                               // im Scan-Task, Scan ist gestoppt
