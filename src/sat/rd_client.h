// Zusatzdisplay (Satellit im Modus SAT_REMOTE): zeigt ausgewaehlte Kacheln des Hauptpanels und schaltet darueber.
// Gegenstueck im Panel: src/ui/ui_remote.cpp (Ablauf und Paketformat dort beschrieben).
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#ifndef SAT_REMOTE
#define SAT_REMOTE 0
#endif

struct RdTile {
  std::string key, name, icon, value, sub;
  uint32_t accent = 0x9AA5B1, lit_color = 0;
  int fill = -1;
  bool lit = false, dim = false;
  uint8_t act = 0;
  uint32_t ms = 0;   // zuletzt empfangen
  // Licht: Helligkeit 0..100 (-1 = nicht dimmbar), Farbe, Farbtemperatur (Mired, -1 = keine)
  int bri = -1, hue = -1, sat = -1, ct = -1, ct_min = 0, ct_max = 0;
  bool color = false;
  // MultiPlus/Ladegeraet ueber den GX des Hauptpanels: vset 0 = nicht schaltbar, 1 MultiPlus, 2 Ladegeraet;
  // vmode = Schalterstellung (1 nur Laden, 2 nur Wechselrichter, 3 ein, 4 aus); vlim = Begrenzung in 0,1 A (-1 = keine)
  uint8_t vset = 0;
  int vmode = -1, vlim = -1;
};

#if SAT_REMOTE
void rd_begin();
void rd_loop();
std::vector<RdTile> rd_tiles();          // in der Reihenfolge des Hauptpanels (nur schon empfangene)
// Ohne Kopie (wenig RAM beim CYD): f fuer jede Kachel in Reihenfolge, unter Sperre – f darf rd_* nicht aufrufen
size_t rd_count();
void rd_each(void (*f)(const RdTile &, void *), void *ctx);
bool rd_action(const std::string &key);  // Antippen: Befehl ans Hauptpanel
// Dimm-Fenster: 'b' Helligkeit, 'h' hue*1000+sat, 'c' Mired; MultiPlus: 'm' Schalter, 'l' Begrenzung in 0,1 A
bool rd_set(const std::string &key, char what, int value);
bool rd_tile(const std::string &key, RdTile &out);
uint32_t rd_version();                   // aendert sich bei jedem neuen Wert
uint32_t rd_keys_version();              // aendert sich nur, wenn die Kachelliste selbst neu ist
bool rd_panel_seen();                    // Hauptpanel meldet sich (in den letzten 60 s)
std::string rd_panel_ip();               // IP des Hauptpanels ("" = unbekannt)
std::string rd_sign(const std::string &msg);   // Pruefwert (Satelliten-Passwort) fuer HTTP-Anfragen ans Hauptpanel
std::string rd_status();                 // fuer die Weboberflaeche
void rd_mqtt_in(const std::string &sub, const char *data, size_t len);   // aus sat_send.cpp (MQTT-Task)
void rd_mqtt_reconnected();              // MQTT neu verbunden: Kachel-Abos neu anlegen
#endif
