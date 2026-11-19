// Truma iNet X per Bluetooth LE: Das Geraet koppelt sich wie die Truma-App ans Bedienteil (Just Works, gebondet)
// und spricht dessen Bus-Protokoll (Rahmen mit Adressen, Inhalt CBOR: Thema/Parameter/Wert).
// Wire-Format nach rpodgorny/hass-truma-inetx (Beschreibung), eigene Umsetzung.
// Schalter: -DWITH_TRUMA_INETX=1. Der Bluetooth-Scan (truma.cpp) reicht die Werbepakete weiter und baut die
// Verbindung auf; Protokoll und Zustand liegen hier.
#pragma once
#include "truma_heat.h"
#include <string>

class NimBLEAdvertisedDevice;

static inline bool inetx_enabled() { return WITH_TRUMA_INETX; }
void inetx_begin();
uint32_t inetx_version();
bool inetx_get(TrumaHeatInfo &out);

void inetx_set_heat(bool on, float target_c, uint8_t mode);   // mode: 0 Komfort, 1 Schnell
void inetx_set_water(TrumaWater w);
void inetx_set_energy(TrumaEnergy e, uint16_t el_power);       // nur die Elektrostufe ist schreibbar

// Kopplung: erst hier starten, dann am iNet X "Gerät hinzufügen" waehlen (3 Minuten Zeit)
void inetx_pair_start();
bool inetx_pairing();
bool inetx_paired();
void inetx_forget();
void inetx_disconnect();                                     // Protokoll ausgeschaltet: Verbindung trennen, Kopplung bleibt

// Fuer den Bluetooth-Scan in truma.cpp
void inetx_ble_setup();                                      // Sicherheit/MTU – bei der BLE-Initialisierung
bool inetx_on_adv(const NimBLEAdvertisedDevice *dev);        // true = war ein iNet X
void inetx_scan_wanted(bool &passive, bool &active);
bool inetx_want_connect();
void inetx_connect_now();                                    // im Scan-Task, Scan ist gestoppt
