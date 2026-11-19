// Akkus mit eigenem BMS per Bluetooth (Verbindung, nicht nur Werbepakete):
//   Super B Epsilon (App "Be in Charge" 2.1.1): meldet seine Werte selbst (Notify), Entsperren mit "SuperB".
//   Power Quarry VBxxx / CBT-BMS (App "Power Quarry" 3.5.2): Spannung/Strom alle 20 s, Ladezustand und Rest jede Minute, binaer (AA 55 …) oder
//   ASCII (~…\r, Pylontech-artig, Herstellerdaten "CBT^" Version 2).
//   Bulltron (App "BullTron" 1.1.36.2): Modbus-RTU (Adresse 0xD2, Funktion 0x03, CRC-16), Dienst 0xFFF0
//   (0xFFF1 Antworten, 0xFFF2 Anfragen), alle 20 s 62 Laufdaten-Register.
//   JBD / Xiaoxiang (Dienst 0xFF00, DD A5 03 … 77), JK-BMS (JK02, 0xFFE1, 300-Byte-Rahmen 55 AA EB 90),
//   LiTime / Redodo / PowerQueen (0xFFE1/0xFFE2) – Protokolle nach der Beschreibung in mr-manuel/
//   venus-os_dbus-serialbattery (dort AGPL-3.0; hier eigene Umsetzung, kein uebernommener Code).
// Alle ausser Super B werden nur kurz verbunden: verbinden, abfragen, trennen (alle 20 s).
// Haengt am Bluetooth-Ablauf von truma.cpp (Scan-Task): bms_on_adv / bms_scan_wanted / bms_want_connect /
// bms_connect_now – wie Hymer und iNet X. Ein eingerichteter Akku bleibt dauerhaft verbunden und meldet seine Werte
// selbst (Notify); bei Abbruch wird nach kurzer Pause neu verbunden.
// SICHERHEIT: Es wird nur entsperrt und gelesen. Die Akku-Konsole kennt auch Befehle, die die Leistungsschalter
// schalten oder Einstellungen aendern – solche Befehle werden nie gesendet.
// Schalter: -DWITH_BMS=1 (ab Werk aus). Zur Laufzeit zusaetzlich BLE_P_BMS (ble_proto_on).
// NVS-Namensraum "bms": n, m<i> MAC, t<i> Adresstyp, k<i> Modell, a<i> Name, c<i> Kapazitaet Ah, z<i> Zellen.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

#ifndef WITH_BMS
#define WITH_BMS 0
#endif

// Einzelne Akku-Typen ein-/ausbauen (spart Flash, z. B. beim Mini-Display). Ab Werk alle an.
// Im Mini-Display: satellite/platformio.ini, Abschnitt [bms].
#ifndef BMS_SUPERB_ON
#define BMS_SUPERB_ON 1
#endif
#ifndef BMS_PQ_ON
#define BMS_PQ_ON 1          // Power Quarry (binaer und ASCII)
#endif
#ifndef BMS_DALY_ON
#define BMS_DALY_ON 1        // Daly / Bulltron
#endif
#ifndef BMS_JBD_ON
#define BMS_JBD_ON 1
#endif
#ifndef BMS_JK_ON
#define BMS_JK_ON 1
#endif
#ifndef BMS_LITIME_ON
#define BMS_LITIME_ON 1      // LiTime / Redodo / PowerQueen
#endif
#ifndef BMS_RENOGY_ON
#define BMS_RENOGY_ON 1
#endif
#ifndef BMS_ECOWORTHY_ON
#define BMS_ECOWORTHY_ON 1
#endif
#ifndef BMS_ANT_ON
#define BMS_ANT_ON 1
#endif
#ifndef BMS_VATRER_ON
#define BMS_VATRER_ON 1
#endif
// Renogy-Geraete ausser Akkus: Laderegler (Rover/Wanderer/Adventurer ueber BT-1/BT-2), DC-DC-Lader (DCC/RBC),
// Wechselrichter (RNGRIU…/BTRIC…), Smart Shunt 300 – nach cyrils/renogy-bt (GPL-3.0) und IAmTheMitchell/renogy-ble
// (Apache-2.0). Nur lesen.
#ifndef BMS_RENOGY_DEV_ON
#define BMS_RENOGY_DEV_ON 1
#endif
// Power Stations (bms_ps.inc) – UNGETESTET: Bluetti (Modbus, neuere verschluesselt), Allpowers
#ifndef BMS_BLUETTI_ON
#define BMS_BLUETTI_ON 1
#endif
#ifndef BMS_ALLPOWERS_ON
#define BMS_ALLPOWERS_ON 1
#endif
#ifndef BMS_ANKER_ON
#define BMS_ANKER_ON 1         // Anker SOLIX C300/C800/C1000/C1000 Gen 2/F2600/F3800
#endif
#ifndef BMS_ECOFLOW_ON
#define BMS_ECOFLOW_ON 1       // EcoFlow Delta 2/River 2/River 3/Delta 3 (braucht die EcoFlow-Benutzer-ID, ~80 KB Flash)
#endif

// SuperSense (comworks) Tankmodul "SSBT…" (bis 4 Tanksensoren, auch Sensoren mit eingebautem Bluetooth) – BETA,
// UNGETESTET: Protokoll aus der Android-App rekonstruiert (docs/protokolle/supersense_ble.md). Ab Werk aus, nur die
// Victron-Beta schaltet es ein (satellite/platformio.ini [bms] supersense).
#ifndef BMS_SUPERSENSE_ON
#define BMS_SUPERSENSE_ON 0
#endif
// Ruuvi Tag / Ruuvi Air (Hersteller-ID 0x0499): Klima-Sensor, sendet seine Werte offen in Werbepaketen – keine
// Verbindung, kein Passwort. BETA, ungetestet. Formate 3, 5 (Tag) und 6 (Air) nach der offiziellen Ruuvi-Doku.
#ifndef BMS_RUUVI_ON
#define BMS_RUUVI_ON 1
#endif
// SwitchBot Thermo-Hygrometer (Meter, Meter Plus, Outdoor Meter): Werte offen in Werbepaketen (Dienst 0xFD3D bzw.
// Hersteller-ID 0x0969), keine Verbindung. BETA, ungetestet. Nach der SwitchBot-BLE-Doku (OpenWonderLabs).
#ifndef BMS_SWITCHBOT_ON
#define BMS_SWITCHBOT_ON 1
#endif
// Simarine STRx (STR1 analog, STR2 NMEA 2000, STR3 RV-C): Tankfuellstand in Werbepaketen ("Broadcast Mode" in der
// Simarine-App einschalten). Offen lesbar oder AES-CCM mit dem Schluessel aus der App (32 Hex-Zeichen im Feld PIN /
// Passwort). BETA, ungetestet. Nach der oeffentlichen Spezifikation Simarine/strx-ble-broadcast-protocol.
#ifndef BMS_SIMARINE_ON
#define BMS_SIMARINE_ON 1
#endif
#define BMS_TANKS 4   // Kanaele je SuperSense-Tankmodul

#define BMS_MAX 6
#define BMS_KEY_PREFIX "bms:"

enum BmsModel : uint8_t {
  BMS_UNKNOWN = 0, BMS_SUPERB = 1, BMS_PQ_HEX = 2, BMS_PQ_ASCII = 3,
  BMS_BULLTRON = 4,   // Daly (Bulltron verbaut Daly): Modbus D2, zweite Variante 0x81/0x51
  BMS_JBD = 5, BMS_JK = 6, BMS_LITIME = 7, BMS_RENOGY = 8, BMS_ECOWORTHY = 9, BMS_ANT = 10, BMS_VATRER = 11,
  // Renogy: BT-TH-… (BT-1/BT-2-Modul) sagt nicht, was dahinter haengt – die erste Verbindung klaert es
  // (Laderegler, DC-DC-Lader oder Akku) und speichert das Ergebnis
  BMS_RENOGY_BT = 12, BMS_RENOGY_SOLAR = 13, BMS_RENOGY_DCC = 14, BMS_RENOGY_INV = 15, BMS_RENOGY_SHUNT = 16,
  BMS_RENOGY_PRO = 17,  // Renogy-Akku "Pro" (RNGRBP…/RNGC…, Hersteller-ID 0xE14C): Adresse 0xFF, Strom in 0,1 A
  BMS_BLUETTI = 18, BMS_ALLPOWERS = 19,  // Power Stations (bms_ps.inc)
  BMS_ANKER = 20 /*C1000, C800, F2600*/, BMS_ANKER_C300 = 21, BMS_ANKER_G2 = 22 /*C1000 Gen 2*/, BMS_ANKER_F3800 = 23,
  BMS_ECOFLOW = 24 /*Delta 2, River 2*/, BMS_ECOFLOW_PB = 25 /*River 3, Delta 3*/,
  BMS_SS_TANK = 26,  // SuperSense Tankmodul (SSBT)
  BMS_RUUVI = 27,    // Ruuvi Tag / Air (nur Werbepakete)
  BMS_SWITCHBOT = 28,// SwitchBot Meter / Meter Plus / Outdoor Meter (nur Werbepakete)
  BMS_SIMARINE = 29  // Simarine STRx Tanksensor (nur Werbepakete)
};

// Geraeteart (bestimmt die Kachel: Akku/Shunt = Batteriewaechter, sonst Solar, Ladebooster, Wechselrichter)
enum BmsDev : uint8_t {
  BMS_DEV_BATTERY = 0, BMS_DEV_SOLAR = 1, BMS_DEV_DCDC = 2, BMS_DEV_INVERTER = 3, BMS_DEV_SHUNT = 4, BMS_DEV_PSTATION = 5,
  BMS_DEV_TANK = 6,  // Tankmodul: tank_pct/tank_temp je Kanal
  BMS_DEV_CLIMATE = 7   // Klima-Sensor (Ruuvi): temp, hum, press, co2, pm25
};
BmsDev bms_dev_of(BmsModel m);
// Abgleich/Erweiterung nach patman15/aiobmsble (Apache-2.0): Daly-Varianten und -Erkennung, JK-Aufbau nach
// Firmware-Version, LiTime/Redodo-Hersteller-ID 0x585A, Renogy (BT-TH-…), Eco-Worthy (ECO-WORTHY…/DCHOUSE…).

struct BmsInfo {
  std::string key, mac, name;
  BmsModel model = BMS_UNKNOWN;
  std::string model_text;             // "Super B", ergaenzt um das Modell aus der Geraete-Info
  std::string serial, firmware;
  bool connected = false;
  bool has = false;                   // mindestens ein Statuspaket empfangen
  bool pin_set = false;               // Kopplungs-PIN gespeichert (Super B)
  bool pin_need = false;              // Geraet hat beim Koppeln eine PIN verlangt
  int32_t age_s = -1;                 // Sekunden seit dem letzten Statuspaket
  int soc = -1, soh = -1;             // %
  float volt = NAN, amp = NAN;        // Strom: + laden, - entladen
  int32_t ttg_s = -1;                 // Restzeit bis leer, -1 = unbekannt
  bool ok = true, charging = false, discharging = false, low = false;
  bool charge_allowed = true, discharge_allowed = true;
  float temp = NAN;                   // Akku-Temperatur °C (Power Quarry)
  int cycles = -1;                    // Ladezyklen (Power Quarry)
  int capacity_ah = 0;                // Nennkapazitaet (Power Quarry, fuer die Restzeit)
  std::vector<uint16_t> cells_mv;     // Zellspannungen (Power Quarry)
  uint32_t alarms = 0;                // Alarmbits (Power Quarry binaer), 0 = keine
  std::string status;                 // "Verbunden", "Verbinde …", "Nicht erreichbar" …
  // Renogy-Geraete (dev != Akku): volt/amp = Batterie bzw. Ladestrom; NAN/-1 = unbekannt
  uint8_t dev = BMS_DEV_BATTERY;      // BmsDev
  float pv_v = NAN, pv_w = NAN;       // Solar-Eingang
  float in_v = NAN, in_a = NAN, in_w = NAN;   // Lichtmaschine (DC-DC-Lader)
  float load_a = NAN, load_w = NAN;   // Lastausgang (Laderegler)
  float ac_v = NAN, ac_a = NAN, ac_w = NAN, ac_hz = NAN, grid_v = NAN;   // Wechselrichter: Ausgang, Netz-Eingang
  float starter_v = NAN;              // Smart Shunt: Starterbatterie
  int yield_wh = -1;                  // Ertrag heute
  int chg_state = -1;                 // Ladephase bzw. Zustand in Victron-Nummerierung (victron_state_text)
  // Power Stations: Leistungen in W, Schalter -1 unbekannt / 0 aus / 1 an
  float ps_in_w = NAN, ps_out_w = NAN, ps_ac_in_w = NAN, ps_dc_in_w = NAN, ps_ac_out_w = NAN, ps_dc_out_w = NAN;
  int8_t ac_on = -1, dc_on = -1;
  bool can_ac = false, can_dc = false;   // Ausgang schaltbar (bms_set_output)
  int remain_min = -1;                // Restzeit laut Geraet
  // Tankmodul (BMS_DEV_TANK): Fuellstand % und Temperatur je Kanal, -1/NAN = kein Sensor am Kanal
  int tank_pct[BMS_TANKS] = {-1, -1, -1, -1};
  int tank_raw[BMS_TANKS] = {-1, -1, -1, -1};
  float tank_temp[BMS_TANKS] = {NAN, NAN, NAN, NAN};
  // Klima-Sensor (BMS_DEV_CLIMATE): temp = Temperatur, volt = Batterie; NAN/-1 = nicht gemeldet
  float hum = NAN, press = NAN, pm25 = NAN;   // % rF, hPa, µg/m³
  int co2 = -1;                               // ppm (Ruuvi Air)
  // Simarine STRx: Inhalt in l (NAN = Kapazitaet unbekannt), Tankart 0 sonstige, 1 Frisch-, 2 Grau-, 3 Schwarzwasser,
  // 4 Kraftstoff (-1 unbekannt), Modus 1..3 verschluesselt / 4 offen (0 unbekannt)
  float tank_l = NAN, tank_cap_l = NAN;
  int tank_kind = -1, sm_mode = 0;
};

struct BmsFound {
  std::string mac, name;
  uint8_t addr_type = 0;
  int rssi = 0;
  BmsModel model = BMS_UNKNOWN;
  bool service = false;               // wirbt mit dem Dienst des Herstellers (sicherer Treffer)
  std::string product;                // z. B. "VB101" (Power Quarry)
  int capacity_ah = 0, cells = 0;     // aus der Produkttabelle (Power Quarry)
  bool configured = false;
  std::string serial;                 // EcoFlow: Seriennummer aus den Werbedaten
  uint8_t enc = 7;                    // EcoFlow: Verschluesselungsart
};

// Waehrend der Suche gesehenes Bluetooth-Geraet, das keinem Akku-Typ zugeordnet werden konnte – zum Zuordnen von Hand
struct BmsSeen {
  std::string mac, name, svc;   // svc: beworbene Dienste (kurz, z. B. "ffe0 fff0")
  uint8_t addr_type = 0;
  int rssi = 0;
  uint16_t mid = 0;             // Hersteller-ID (0 = keine)
};

const char *bms_model_text(BmsModel m);

#if WITH_BMS
class NimBLEAdvertisedDevice;
void bms_begin();
uint32_t bms_version();
bool bms_has_devices();
// Bluetooth-Ablauf (nur aus dem Scan-Task von truma.cpp)
bool bms_on_adv(const NimBLEAdvertisedDevice *dev);   // true = Akku-Kandidat (waehrend der Suche)
void bms_scan_wanted(bool &passive, bool &active);
bool bms_want_connect();
void bms_connect_now();                               // Scan ist gestoppt, blockiert einige Sekunden
// Abfragen und Einrichten
std::vector<BmsInfo> bms_list();
bool bms_get(const std::string &key, BmsInfo &out);
void bms_discovery_start();                           // 60 s aktiv suchen
bool bms_discovery_running();
std::vector<BmsFound> bms_found();
std::vector<BmsSeen> bms_seen();                      // nicht zugeordnete Geraete der letzten Suche
std::string bms_add(const std::string &mac, uint8_t addr_type, const std::string &name, BmsModel model,
                    int capacity_ah = 0, int cells = 0);   // "" = ok
bool bms_want_poll();                                 // Power Quarry: Abfrage faellig (Scan-Task)
void bms_poll_now();                                  // Abfrage ausfuehren, blockiert 1–3 s (Scan laeuft weiter)
void bms_remove(const std::string &mac);
void bms_rename(const std::string &mac, const std::string &name);
void bms_set_pin(const std::string &mac, const std::string &pin);   // Kopplungs-PIN (nur Ziffern, "" = keine)
std::vector<std::string> bms_log();                   // letzte Meldungen (Fehlersuche ueber die Weboberflaeche)
bool bms_set_output(const std::string &mac, bool ac, bool on);   // Power Station: AC- bzw. DC-Ausgang schalten
// EcoFlow: Benutzer-ID des Kontos (fuer die Anmeldung an der Power Station). Anmeldung holt sie per HTTPS.
bool bms_ef_uid_set();
std::string bms_ef_uid_hint();                        // "…1234" (nur die letzten Stellen)
void bms_ef_set_uid(const std::string &uid);
std::string bms_ef_login_start(const std::string &email, const std::string &password);   // "" = gestartet
int bms_ef_login_state();                             // 0 nichts, 1 laeuft, 2 ok, 3 Fehler
std::string bms_ef_login_text();
std::string bms_value_text(const BmsInfo &b);         // Kurzfassung ("78 %  ·  13,25 V  ·  40 W", Solar: W · V · Phase)
std::vector<std::pair<std::string, std::string>> bms_rows(const BmsInfo &b);   // alle Werte (Bezeichnung, Wert)
std::string bms_cells_text(const BmsInfo &b);   // "3,312–3,327 V  ·  Δ 15 mV", "" ohne Zellwerte
std::vector<std::pair<std::string, std::string>> bms_cell_rows(const BmsInfo &b);   // je Zelle eine Zeile
#else
inline void bms_begin() {}
inline uint32_t bms_version() { return 0; }
inline bool bms_has_devices() { return false; }
inline std::vector<BmsInfo> bms_list() { return {}; }
inline bool bms_get(const std::string &, BmsInfo &) { return false; }
inline bool bms_set_output(const std::string &, bool, bool) { return false; }
#endif
