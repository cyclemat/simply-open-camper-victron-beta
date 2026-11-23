// Victron per Bluetooth BEDIENEN (Beta): Orion Smart / Orion XS ein- und ausschalten, maximalen Ladestrom einstellen.
// Die Werte selbst kommen weiter aus "Instant Readout" (victron.cpp). Zum Schalten verbindet sich das Panel kurz mit dem
// Geraet (VictronConnect-Protokoll: Dienst 306b0001-…, Kopplung mit der PIN aus VictronConnect, ab Werk 000000),
// schreibt das Register und trennt wieder. Register wie im offiziellen VE.Direct-Dokument "Orion XS HEX protocol":
// 0x0200 Geraetemodus (1 an, 4 aus), 0xEDF0 maximaler Ladestrom (0,1 A, dauerhaft gespeichert).
// Eigene Umsetzung nach Protokollwissen aus vvvrrooomm/victron und Prinsessen/victron-ble-openhab (ohne Lizenz).
#pragma once
#include <stdint.h>
#include <string>
#include "victron.h"

#ifndef WITH_VICTRON_CTL
#if PANEL_SATELLITE
#define WITH_VICTRON_CTL 0
#else
#define WITH_VICTRON_CTL 1
#endif
#endif

#define VCTL_PIN_DEFAULT "000000"

class NimBLEAdvertisedDevice;

#if WITH_VICTRON_CTL
bool vctl_supported(uint8_t record);                      // Orion Smart (0x04), Orion XS (0x0F)
void vctl_on_adv(const std::string &mac, uint8_t addr_type);   // Adresstyp merken (Bluetooth-Scan)
void vctl_fill(VictronInfo &v);                          // Schalter/Begrenzung in die Kachel eintragen
bool vctl_set_mode(const std::string &mac, int mode);     // 1 an, 4 aus
bool vctl_set_limit(const std::string &mac, float amps);  // maximaler Ladestrom
void vctl_refresh(const std::string &mac);                // Stand lesen (Detailfenster offen), hoechstens alle 60 s
std::string vctl_pin(const std::string &mac);
void vctl_set_pin(const std::string &mac, const std::string &pin);
void vctl_print_debug();
// Bluetooth-Verteiler (truma.cpp)
bool vctl_ble_needed();
bool vctl_want_connect();
void vctl_connect_now();
#else
inline bool vctl_supported(uint8_t) { return false; }
inline void vctl_on_adv(const std::string &, uint8_t) {}
inline void vctl_fill(VictronInfo &) {}
inline bool vctl_set_mode(const std::string &, int) { return false; }
inline bool vctl_set_limit(const std::string &, float) { return false; }
inline void vctl_refresh(const std::string &) {}
inline std::string vctl_pin(const std::string &) { return ""; }
inline void vctl_set_pin(const std::string &, const std::string &) {}
inline void vctl_print_debug() {}
inline bool vctl_ble_needed() { return false; }
inline bool vctl_want_connect() { return false; }
inline void vctl_connect_now() {}
#endif
