// Satellit: eigene Anschluesse an den GPIOs – Sensoren und Schalter, in der Weboberflaeche frei belegbar
//   Tanks       bis zu 4 Tanks mit je bis zu 5 Sonden (Stangen verschiedener Laenge). Jede Sonde hat eine Prozentstufe;
//               angezeigt wird die hoechste Stufe mit Wasserkontakt. Entweder je Sonde ein GPIO (gemeinsame Masse-Stange)
//               oder alle Sonden ueber eine Widerstandsleiter an einem Analog-Pin. Abfrage im eigenen Intervall,
//               die Sonden werden nur zum Messen kurz bestromt (gegen Korrosion).
//   DS18B20     1-Wire-Bus an einem GPIO, bis zu 8 Fuehler
//   Analog      bis zu 4 Analogeingaenge (nur ADC1, z. B. ESP32: GPIO 32–39, C3: 0–4), zwei Punkte zum Umrechnen
//   Die Pinliste haengt vom Chip ab (ESP32, ESP32-C3, -S3, -C6), siehe PINS in sat_io.cpp.
//   MPU-6050    GY-521 an I2C: Neigung laengs und quer, Nullpunkt per Knopf
//   Schalter    bis zu 8 Ausgaenge (Relais), schaltbar per MQTT, UDP, Bluetooth und Weboberflaeche
//   Eingaenge   bis zu 8 Kontakte/Taster
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include "../sat_proto.h"

#define SAT_IO_TANKS 4
#define SAT_IO_PROBES 5
#define SAT_IO_DS 8
#define SAT_IO_ANALOG 4
#ifndef SAT_IO_SWITCHES
#define SAT_IO_SWITCHES 8      // Relaisplatinen mit Schieberegister setzen mehr (z. B. 16)
#endif
#define SAT_IO_INPUTS 8

void sat_io_begin();
void sat_io_loop();
uint32_t sat_io_version();                 // steigt bei neuen Werten
std::vector<SatPoint> sat_io_points();     // alle eingerichteten Sensoren und Schalter
bool sat_io_has_switches();

// Schalter Nummer idx (1..8): val 0 aus, 1 an, 2 umschalten
bool sat_io_switch(uint8_t idx, uint8_t val);
// dasselbe mit Nonce (UDP/Bluetooth): nur wenn die Nonce passt, danach gilt eine neue
bool sat_io_switch_nonce(uint8_t idx, uint8_t val, uint32_t nonce);
uint32_t sat_io_last_switch_ms();          // millis() der letzten Schaltung (fuer schnelle Rueckmeldung)

void sat_io_config_json(JsonObject o);     // Einstellungen fuer die Weboberflaeche
String sat_io_set_config(JsonObjectConst o);   // pruefen, speichern, anwenden; "" = ok, sonst Fehlertext
void sat_io_state_json(JsonObject o);      // Live-Werte und Rohwerte fuer die Weboberflaeche
String sat_io_action(JsonObjectConst o);   // Knoepfe: Nullpunkt, Fuehler suchen, Tanks messen, schalten
void sat_io_pins_json(JsonArray a);        // GPIOs des Chips mit Faehigkeiten und Hinweisen
const char *sat_io_chip();                 // "ESP32", "ESP32-C3", "ESP32-S3", "ESP32-C6"
bool sat_io_tilt(float &x, float &y);      // Neigung laengs/quer (alle 100 ms, geglaettet, mit Nullpunkt); false = kein MPU
bool sat_i2c_begin(int sda, int scl);      // gemeinsamer I2C-Bus (MPU-6050, OLED)
