// Victron Cerbo GX (Venus OS) per MQTT: Solar, Netz/Landstrom, Ladegeraet, Batterie und DC-System.
// Am GX: Einstellungen -> Dienste -> MQTT (unverschluesselt, Port 1883) einschalten.
// Das Panel verbindet sich mit einem eigenen MQTT-Client direkt mit dem GX (unabhaengig vom Zigbee2MQTT-Broker),
// abonniert nur die benoetigten Werte und haelt den GX per Keepalive (R/<Portal-ID>/keepalive) am Senden.
// Die fuenf Werte erscheinen als Victron-Kacheln mit den Schluesseln "cerbo:solar", "cerbo:grid",
// "cerbo:charger", "cerbo:battery" und "cerbo:dc" (Anzeige ueber dieselben Funktionen wie Victron Instant Readout).
// Schalter: [features] cerbo in platformio.ini (-DWITH_CERBO=0 baut es aus)
#pragma once
#include <stdint.h>
#include <string>
#include <vector>
#include "victron.h"

#ifndef WITH_CERBO
#define WITH_CERBO 1
#endif

#define CERBO_KEY_PREFIX "cerbo:"

inline bool cerbo_enabled() { return WITH_CERBO; }
inline bool cerbo_is_key(const std::string &k) { return k.rfind(CERBO_KEY_PREFIX, 0) == 0; }

struct CerboConfig {
  std::string host;         // IP oder Name des GX, leer = aus
  uint16_t port = 1883;
  std::string portal;       // Portal-ID, leer = automatisch erkennen
  std::string user, pass;   // nur noetig, wenn am GX ein Passwort fuer das lokale Netz gesetzt ist
};

void cerbo_begin();
uint32_t cerbo_version();                          // aendert sich bei neuen Werten/Konfiguration
bool cerbo_configured();
bool cerbo_connected();
std::string cerbo_debug_text();   // Trennungen seit dem Start und letzte Ursache (serieller Befehl "debug")
std::string cerbo_status();                        // "Verbunden", "GX nicht erreichbar", ...
std::string cerbo_portal_id();                     // erkannte bzw. eingestellte Portal-ID

CerboConfig cerbo_config();
void cerbo_set_config(const CerboConfig &c);       // speichert und verbindet neu

bool cerbo_get(const std::string &key, VictronInfo &out);
std::vector<VictronInfo> cerbo_list();             // leer, solange kein GX eingetragen ist

// Einstellen ueber den GX (nur Kacheln mit can_limit/can_mode): Eingangsstrom-Begrenzung in A bzw. Schalter
// (MultiPlus: 1 nur Laden, 2 nur Wechselrichter, 3 ein, 4 aus; Ladegeraet: 1 ein, 4 aus). false = nicht moeglich.
bool cerbo_set_limit(const std::string &key, float amps);
bool cerbo_set_mode(const std::string &key, int mode);

const char *cerbo_mode_text(int mode);             // Schalter des Wechselrichters/Laders: "Ein", "Nur Laden", ...
const char *cerbo_source_text(int src);            // aktiver AC-Eingang: "Netz", "Landstrom", "Generator", ...
// Ein-/Ausgaenge des GX (Relais, Tanks, Temperatursensoren, Digitaleingaenge) – nur was am GX eingerichtet ist.
// Erscheinen im Panel wie die Anschluesse eines Satelliten "GX" (panelsat.cpp, Schluessel "satio:GX/<punkt>").
struct SatPoint;
std::vector<SatPoint> cerbo_points();
bool cerbo_set_relay(int idx, bool on);   // idx = Punktnummer (Relais 1 = 1); nur Relais mit Funktion "manuell"
