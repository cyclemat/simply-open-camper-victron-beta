// Kleiner Updater (wie bei Tasmota) fuer Geraete mit 4 MB Flash (Satelliten, CYD-Satellit, Victron-Display):
// Partitionstabelle satellite/partitions_upd.csv – app0 (ota_0) volle Firmware, app1 (ota_1) Updater (src/updater/).
//
// Ablauf eines Updates ueber die Weboberflaeche:
//   1. Die volle Firmware legt WLAN-Zugang und Geraetenamen im NVS-Namensraum "upd" ab (updater_start) und startet
//      in den Updater.
//   2. Der Updater verbindet sich mit diesem WLAN (sonst Einrichtungs-WLAN 4.3.2.1), zeigt die Upload-Seite
//      (gleiches Zugangspasswort, devsetup) und schreibt die neue Firmware nach app0. Danach startet er sie.
//   3. Ohne Upload geht er nach 15 Minuten von selbst zur Firmware zurueck.
// Die volle Firmware kann ueber ihren eigenen Upload nur den Updater erneuern (app1 ist 1 MB gross).
#pragma once
#include <Arduino.h>
#include <stddef.h>

bool updater_available();          // gueltiger Updater in app1?
size_t updater_slot_size();        // Groesse von app1 (0 = Partitionstabelle ohne Updater, z. B. noch min_spiffs)
// WLAN-Daten uebergeben und in den Updater starten. Rueckgabe: Fehlertext, sonst kehrt die Funktion nicht zurueck.
String updater_start(const String &ssid, const String &pass, const String &name);
