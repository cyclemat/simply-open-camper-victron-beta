// Satelliten-Protokoll (MQTT) – gemeinsam fuer die Satelliten-Firmware und das Panel
//
// Ein Satellit ist ein eigener ESP32, der Truma LevelControl, Mopeka und Victron per Bluetooth liest und die Werte
// an den MQTT-Broker schickt. Topics (alle "retained", <base> = "panelsat"):
//   <base>/<id>/online              "online" / "offline" (Last Will)
//   <base>/<id>/info                {"name","fw","ip","rssi","uptime","heap"}
//   <base>/<id>/truma/<mac>         Werte eines LevelControl  (Felder wie TrumaInfo)
//   <base>/<id>/mopeka/<mac>        Werte eines Mopeka-Sensors (Felder wie MopekaInfo)
//   <base>/<id>/victron/<mac>       Werte eines Victron-Geraets (Felder wie VictronInfo)
//   <base>/<id>/cmd                 Befehle an den Satelliten, z. B. {"measure":"<mac>"} (leer = alle)
// Eigene Sensoren und Schalter direkt am Satelliten (siehe SatPoint unten):
//   <base>/<id>/sensor/<punkt>      {"name","kind","value","unit","class","ok",...}  z. B. punkt = "tank1", "temp2"
//   <base>/<id>/switch/<punkt>      {"name","kind","on",...}   schalten mit .../switch/<punkt>/set "ON"/"OFF"/"TOGGLE"
// Ein leerer Inhalt auf einem Geraete-Topic heisst: Geraet wurde am Satelliten entfernt.
// "age" ist das Alter der Werte beim Senden in Sekunden; der Empfaenger zaehlt selbst weiter.
//
// Drei Wege zum Panel (am Satelliten einzeln waehlbar):
//   MQTT       die Topics oben
//   UDP        je Geraet ein Paket {"sat":<id>,"name":<Satellit>,"type":"truma|mopeka|victron","mac":..,"d":{..}}
//              bzw. {"sat":..,"type":"info",...} an Port SAT_UDP_PORT (Broadcast oder feste Panel-IP)
//   Bluetooth  Werbepakete mit Herstellerdaten (Kennung 0xFFFF, "P"), je Paket ein Geraet in Kurzform, verschluesselt,
//              der Satellit wechselt jede Sekunde zum naechsten Geraet. Ohne Namen und Statustexte.
#pragma once
#include <ArduinoJson.h>
#include <math.h>
#include <string>
#include "truma.h"
#include "mopeka.h"
#include "victron.h"

#define SAT_BASE "panelsat"
#define SAT_UDP_PORT 47811
#define SAT_ADV_COMPANY 0xFFFF   // Bluetooth-SIG-Kennung fuer Tests/eigene Zwecke
enum SatType : uint8_t {
  SAT_T_TRUMA = 1, SAT_T_MOPEKA = 2, SAT_T_VICTRON = 3,
  SAT_T_SENSOR = 4,   // eigener Sensor am Satelliten (Tank, Temperatur, Analog, Neigung, Eingang)
  SAT_T_SWITCH = 5,   // Schalter/Relais am Satelliten
};

// ---------------------------------------------------------------- Bluetooth
// Satelliten heissen im Bluetooth-Modus "Satellite-<MAC>" (MAC ohne Doppelpunkte, gross). Die Werte sind mit einem
// gemeinsamen Passwort verschluesselt (AES-128-CTR, Schluessel = SHA-256("PanelSat:" + Passwort), erste 16 Byte;
// Nonce = Bluetooth-MAC des Satelliten + Zaehler). Das Panel nimmt nur Pakete mit passender Pruefsumme an und
// fuegt solche Satelliten automatisch hinzu.
// Paket (Herstellerdaten, max. 26 Byte):
//   [0..1] 0xFF 0xFF  [2] 'P'  [3] Typ<<4 | Version  [4..5] Zaehler
//   verschluesselt: [6..11] MAC des Sensors  [12] Bits (0 Werte da, 1 misst, 2 Liter, 3 Fehler)
//                   [13..] Werte je Typ (max. 11 Byte)  letzte 2 Byte: CRC-16 ueber MAC, Bits und Werte
#define SAT_DEFAULT_PASSWORD "panelsat"

struct SatPoint;   // eigene Anschluesse, siehe unten

void sat_key_from_password(const std::string &pw, uint8_t key[16]);
std::string sat_ble_name(const std::string &ble_mac);   // "aa:bb:.." -> "Satellite-AABB.."

// Klartext-Paket bauen (0 = passt nicht), dann mit sat_adv_seal verschluesseln
size_t sat_adv_encode(uint8_t *out, size_t cap, const TrumaInfo &t);
size_t sat_adv_encode(uint8_t *out, size_t cap, const MopekaInfo &m);
size_t sat_adv_encode(uint8_t *out, size_t cap, const VictronInfo &v);
// Pruefsumme anhaengen und verschluesseln; liefert die neue Laenge (0 = kein Platz)
size_t sat_adv_seal(uint8_t *md, size_t len, size_t cap, const uint8_t key[16], const std::string &sender, uint16_t counter);
// Entschluesseln und pruefen (md inkl. Hersteller-ID, sender = Bluetooth-MAC des Satelliten).
// Liefert Typ und MAC des Sensors und fuellt das passende Ziel.
bool sat_adv_decode(const uint8_t *md, size_t len, const uint8_t key[16], const std::string &sender, SatType &type,
                    std::string &mac, TrumaInfo &t, MopekaInfo &m, VictronInfo &v, SatPoint &sp);

// Vom Bluetooth-Scan (truma.cpp) fuer Herstellerdaten mit Kennung 0xFFFF aufgerufen; true = war ein Satellit.
// Im Panel umgesetzt (panelsat.cpp), im Satelliten leer.
bool panelsat_on_adv(const std::string &sender, const uint8_t *md, size_t len, int rssi);

// ---------------------------------------------------------------- eigene Anschluesse am Satelliten ("Punkte")
// Sensoren und Schalter direkt an den GPIOs des Satelliten. Kennung je Punkt: Art + Nummer, z. B. "tank1", "temp2".
// MQTT:  <base>/<id>/sensor/<punkt>  bzw.  <base>/<id>/switch/<punkt>   (Inhalt: sat_to_json(SatPoint))
//        schalten: <base>/<id>/switch/<punkt>/set  "ON" / "OFF" / "TOGGLE"
// UDP:   {"sat","name","type":"sensor|switch","mac":<punkt>,"d":{...}}; Befehle als verschluesseltes Paket (sat_cmd_*)
// Bluetooth: Typ SAT_T_SENSOR/SAT_T_SWITCH, statt der Sensor-MAC [Art, Nummer, Klasse, 0, 0, 0],
//        Werte: [0..3] float  [4] Sonden mit Kontakt (Bits)  [5] Sondenzahl | 0x80 unplausibel  [6..9] Befehls-Nonce
//        [10] Alter in s. Bits: 0 Wert da, 1 an (Schalter/Eingang), 3 Fehler
enum SatPointKind : uint8_t { SP_NONE = 0, SP_TANK = 1, SP_TEMP = 2, SP_ANALOG = 3, SP_TILT = 4, SP_INPUT = 5, SP_SWITCH = 6 };
enum SatClass : uint8_t {
  SC_NONE = 0, SC_TEMP = 1, SC_HUMIDITY = 2, SC_LEVEL = 3, SC_VOLTAGE = 4, SC_PRESSURE = 5, SC_CURRENT = 6, SC_TILT = 7,
  SC_BINARY = 8,
  SC_ONOFF = 9,     // Zustand an/aus (z. B. Landstrom), angezeigt als "An"/"Aus" statt "zu"/"offen"
  SC_COUNT = 10
};

struct SatPoint {
  std::string id;            // "tank1", "temp2", "analog1", "tilt1", "input1", "switch1"
  SatPointKind kind = SP_NONE;
  uint8_t idx = 0;           // 1..
  std::string name;
  uint8_t cls = SC_NONE;
  std::string unit;
  bool ok = false;           // Wert vorhanden
  float value = NAN;
  bool on = false;           // Schalter/Eingang
  uint8_t probes = 0;        // Tank: Sonden mit Wasserkontakt (Bit 0 = Sonde 1)
  uint8_t probes_n = 0;      // Tank: Zahl der Sonden
  bool odd = false;          // Tank: Sonden widersprechen sich (oben nass, unten trocken)
  uint32_t nonce = 0;        // Schalter: aktuelle Befehls-Nonce (UDP/Bluetooth)
  int32_t age_s = -1;
  std::string status;
  // nur im Panel
  std::string key, sat;
};

std::string sat_point_id(SatPointKind k, uint8_t idx);
bool sat_point_parse(const std::string &id, SatPointKind &k, uint8_t &idx);
const char *sat_kind_name(SatPointKind k);         // "tank", "temp", ...
const char *sat_class_name(uint8_t cls);           // "temperature", "humidity", ...
uint8_t sat_class_of(const char *name);
const char *sat_class_unit(uint8_t cls);           // Standard-Einheit der Klasse
bool sat_point_is_switch(SatPointKind k);
size_t sat_adv_encode(uint8_t *out, size_t cap, const SatPoint &p);
void sat_to_json(JsonObject o, const SatPoint &p);
void sat_from_json(JsonObjectConst o, SatPoint &p);

// Schaltbefehl Panel -> Satellit (Bluetooth-Werbepaket oder UDP), verschluesselt mit dem gemeinsamen Passwort.
// [0..1] 0xFF 0xFF [2] 'C' [3] Version [4..5] Zaehler | verschluesselt: [6] Befehl (1 = schalten) [7] Art [8] Nummer
// [9] Wert (0 aus, 1 an, 2 umschalten) [10..13] Nonce des Satelliten [14..15] CRC-16
// Nonce der Verschluesselung = Bluetooth-MAC des Ziel-Satelliten + Zaehler, nur der richtige Satellit kann es lesen.
// Die Nonce des Satelliten wechselt nach jedem angenommenen Befehl – ein mitgeschnittenes Paket wirkt nicht noch einmal.
size_t sat_cmd_seal(uint8_t *out, size_t cap, const uint8_t key[16], const std::string &target_ble_mac, uint16_t counter,
                    SatPointKind kind, uint8_t idx, uint8_t val, uint32_t nonce);
bool sat_cmd_open(const uint8_t *in, size_t len, const uint8_t key[16], const std::string &own_ble_mac, SatPointKind &kind,
                  uint8_t &idx, uint8_t &val, uint32_t &nonce);

void sat_to_json(JsonObject o, const TrumaInfo &t);
void sat_to_json(JsonObject o, const MopekaInfo &m);
void sat_to_json(JsonObject o, const VictronInfo &v);

// mac und key setzt der Aufrufer
void sat_from_json(JsonObjectConst o, TrumaInfo &t);
void sat_from_json(JsonObjectConst o, MopekaInfo &m);
void sat_from_json(JsonObjectConst o, VictronInfo &v);
