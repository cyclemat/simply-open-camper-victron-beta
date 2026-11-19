// Persistente Einstellungen (NVS)
#pragma once
#include <Arduino.h>

enum NetMode : uint8_t {
  NET_STA = 0,    // Netzwerk-Modus: Panel im Camp-WLAN
  NET_AP = 1,     // Router-Modus: Panel spannt eigenes WLAN mit DHCP auf
  NET_APSTA = 2,  // Kombi: eigenes WLAN + Camp-WLAN mit Weiterleitung ins Internet
  NET_OFF = 3,    // WLAN ganz aus: spart internen RAM (~40 KB) und stoert Bluetooth nicht
};

enum SaverMode : uint8_t {
  SAVER_DIM = 0,     // Dashboard abgedunkelt
  SAVER_BLACK = 1,   // schwarzes Bild, Beleuchtung aus (schont das LCD am meisten)
  SAVER_CLOCK = 2,   // dunkle Uhr, wechselt jede Minute die Position
  SAVER_BOUNCE = 3,  // huepfendes Logo mit Farbwechsel
  SAVER_DUEL = 4,    // Ball-Duell: zwei Schlaeger spielen automatisch
};

// Bluetooth-Protokolle, die das Panel auswertet (in den Einstellungen waehlbar)
enum BleProto : uint8_t {
  BLE_P_TRUMA = 0x01,    // Truma LevelControl (Gasflaschen)
  BLE_P_MOPEKA = 0x02,   // Mopeka-Tanksensoren
  BLE_P_VICTRON = 0x04,  // Victron Instant Readout
  BLE_P_INETX = 0x08,    // Truma iNet X (Heizung)
  BLE_P_SAT = 0x10,      // Satelliten, die ihre Werte per Bluetooth senden
  BLE_P_HYMER = 0x20,    // Hymer Connect (SCU im Fahrzeug, gekoppelt)
  BLE_P_BMS = 0x40,      // Akkus mit BMS per Bluetooth (Super B …), nur mit WITH_BMS
};
#define BLE_P_DEFAULT (BLE_P_TRUMA | BLE_P_MOPEKA | BLE_P_VICTRON)

struct Settings {
  uint8_t ble_protos = BLE_P_DEFAULT;
  uint8_t net_mode = NET_STA;
  uint8_t web_mode = 0;        // Webinterface: 0 immer an, 1 nur bei Bedarf (Knopf am Panel, aus nach 15 min ohne Zugriff)
  String ap_ssid, ap_pass;     // Panel-WLAN (Router/Kombi)
  uint8_t ap_channel = 13;
  bool mqtt_on = true;         // MQTT ganz aus: kein Client, keine Verbindungsversuche (Zigbee2MQTT faellt weg)
  bool mqtt_auto = true;       // Broker im Panel-WLAN automatisch suchen (Router/Kombi)
  String wifi_ssid, wifi_pass;
  String hostname;
  String mqtt_host, mqtt_user, mqtt_pass, mqtt_base;
  uint16_t mqtt_port = 1883;
  // Eingebauter MQTT-Broker (nur mit WITH_BROKER, z. B. P4): ab Werk aus; an = Panel nutzt ihn selbst
  bool broker_on = false;
  String broker_user, broker_pass;   // leeres Passwort = ohne Anmeldung
  uint8_t brightness = 80;     // %
  uint16_t dim_after = 120;    // Sekunden, 0 = nie
  uint8_t dim_level = 10;      // %, 0 = Display aus
  uint8_t saver = 0;           // SaverMode: Ruhezustand
  bool saver_recolor = true;   // Huepfendes Logo: Farbe beim Abprallen wechseln
  String tz;
  // LED-Ring (nur Boards mit UI_LEDRING, z. B. CrowPanel 1.46")
  bool ring_knob = true;       // beim Drehen zeigt ein Lichtpunkt die Stelle in der Geraeteliste
  bool ring_warn = true;       // rot pulsieren, wenn eine Gasflasche/Batterie auf dem Panel knapp wird
  uint8_t ring_warn_pct = 10;  // Grenze in %
  uint8_t ring_bri = 30;       // Helligkeit in %
};

extern Settings g_settings;

void settings_load();
void settings_save();
bool settings_mqtt_configured();            // Host gesetzt oder automatische Suche aktiv
bool settings_mqtt_auto_active();           // automatische Broker-Suche im Panel-WLAN
Settings settings_copy();                    // thread-sicher (fuer den Web-Task)
void settings_apply(const Settings &s);      // uebernimmt + speichert (nur aus dem Haupt-Task)
bool ble_proto_on(uint8_t p);                // thread-sicher, z. B. aus dem Bluetooth-Scan
void ble_proto_set(uint8_t p, bool on);      // speichert sofort
