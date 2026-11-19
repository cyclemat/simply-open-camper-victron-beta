// Victron-Display: eigene, schlanke Firmware fuer das Mini-Display (ESP32-2432S028R / CYD).
// Verbindet sich per WLAN mit dem Cerbo GX (MQTT im LAN) und zeigt alle Werte automatisch als Kacheln.
// Dazu Victron-Geraete direkt per Bluetooth ("Instant Readout") und Truma LevelControl (Gasflaschen),
// keine Satelliten-Funktionen. Einrichtung: WLAN "SOC-Victron-XXXX" (Passwort panelsat),
// die Einstellungsseite oeffnet sich von selbst (sonst http://4.3.2.1/) – WLAN eintragen (Heimnetz oder WLAN des Cerbo), Cerbo wird automatisch gesucht.
// Umgebung esp32-cyd-victron (satellite/platformio.ini), im Panel-Menue "satellite-cyd-victron".
#pragma once
#include <string>

std::string vd_title();   // Anzeigename (Titelzeile des Displays)
