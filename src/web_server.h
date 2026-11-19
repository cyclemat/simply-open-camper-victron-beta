// HTTP-Server: liefert den Editor aus und stellt die REST-API bereit
#pragma once
#include <stdint.h>
void web_begin();          // startet den Server, wenn "immer an" (sonst erst mit web_start)
void web_start();          // jetzt starten (bei Bedarf: laeuft bis 15 min ohne Zugriff)
void web_stop();
void web_loop();           // in loop(): bei Bedarf nach 15 min ohne Zugriff beenden
bool web_running();
bool web_busy();           // Webinterface wird gerade benutzt (Anfrage in den letzten 10 s)
uint32_t web_idle_left_s();  // bei Bedarf: Sekunden bis zum Abschalten (0 = immer an oder aus)
// Zugangspasswort fuer Editor/Weboberflaeche (wie Satelliten, devsetup.h): ab Werk "panelsat", beim ersten Oeffnen
// muss ein eigenes vergeben werden. NVS "devsetup": apw, pwset.
bool web_password_set();     // false = noch das Standardpasswort
void web_password_reset();   // zurueck auf "panelsat" (Panel-Menue, serieller Befehl webpw)
