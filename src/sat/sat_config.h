// Satellit: Einstellungen (Preferences "sat")
#pragma once
#include <Arduino.h>
#include "../settings.h"   // BleProto-Bits und ble_proto_on() – hier im Satelliten umgesetzt

#include "../soc_version.h"
#define SAT_FW_VERSION SOC_VERSION   // gemeinsame Versionsnummer (soc_version.h)
#define SAT_DEFAULT_BASE "panelsat"
#define SAT_AP_PASS "panelsat"      // Passwort des Einrichtungs-WLANs

// Wege zum Panel – beliebig kombinierbar
enum SatMode : uint8_t {
  SAT_M_MQTT = 0x01,   // an einen MQTT-Broker (auch Home Assistant)
  SAT_M_UDP = 0x02,    // UDP-Pakete ins WLAN (Broadcast oder feste Panel-IP), ohne Broker
  SAT_M_BLE = 0x04,    // Bluetooth-Werbepakete direkt ans Panel, ohne WLAN
};

struct SatConfig {
  String id;                    // Teil der Topics: <base>/<id>/..., Standard "sat-<MAC-Ende>"
  String name;                  // Anzeigename, z. B. "Satellit Heck"
  String wifi_ssid, wifi_pass;
  String mqtt_host, mqtt_user, mqtt_pass;
  uint16_t mqtt_port = 1883;
  String base = SAT_DEFAULT_BASE;
  bool ha = true;               // Home-Assistant-Discovery
  String ha_prefix = "homeassistant";
  uint8_t ble = 0;              // ausgewertete Bluetooth-Protokolle (ab Werk keine)
  uint8_t modes = 0;            // SatMode-Bits (ab Werk keine; Einrichtung per Knopf schaltet UDP ein)
  String udp_target;            // leer = Broadcast, sonst IP des Panels
  String ble_pw;                // Satelliten-Passwort (wie im Panel, verschluesselt die Pakete), leer = "panelsat"
};

extern SatConfig g_sat;

void sat_config_load();
void sat_config_save();
String sat_default_id();      // "Satellite-<Bluetooth-MAC>" – so heisst der Satellit auch per Bluetooth
String sat_ble_mac();         // "aa:bb:cc:dd:ee:ff"
String sat_topic(const char *sub);   // "<base>/<id>/<sub>"
void sat_ble_listen(bool on);        // Bluetooth-Scan fuer Schaltbefehle des Panels an/aus
