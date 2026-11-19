// Satellit mit eigener Anzeige – z. B. ESP32-2432S028R ("Cheap Yellow Display": 2,8" 320x240 ILI9341, resistiver
// Touch XPT2046). Zeigt die Werte des Satelliten als Kacheln im Stil des Panels (eigene Sensoren/Schalter, Truma,
// Mopeka, Victron); Schalter per Antippen, Blaettern per Wischen. Welche Kacheln in welcher Reihenfolge, wie viele
// pro Seite, Helligkeit und Dimmen stellt man in der Weboberflaeche des Satelliten ein (Karte "Display", NVS "satdisp").
// Kein Bildspeicher (Board ohne PSRAM). Nur mit -DSAT_DISPLAY=1 (Umgebung esp32-cyd), sonst leer.
#pragma once
#include <ArduinoJson.h>

void sat_display_begin();
void sat_display_loop();
bool sat_display_present();                    // true = Firmware mit Display
void sat_display_json(JsonObject o);           // Weboberflaeche: alle Kacheln (Schluessel, Name, Wert) und Einstellungen
bool sat_display_apply(JsonObjectConst c);     // Weboberflaeche: Einstellungen uebernehmen und speichern
bool sat_display_reset_requested();            // beim Einschalten 10 s gedrueckt gehalten (Passwort zuruecksetzen)
void sat_display_light(JsonObject o);          // Lichtsensor: {ldr: Messwert (groesser = dunkler), night, auto}
bool sat_display_night();
void sat_display_web_touch();   // Zugriff auf die Weboberflaeche (Zusatzdisplay: Kacheln solange entladen)                      // Helligkeit-Automatik an und es ist dunkel (z. B. LED dimmen)
