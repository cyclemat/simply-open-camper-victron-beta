// Ersteinrichtung fuer alle Geraete mit eingebauter Weboberflaeche (Arduino-WebServer: Satelliten, Victron-Display).
// Das Panel (ESP-IDF-httpd) setzt dieselben Regeln eigenstaendig um.
//
//  - Einrichtungs-WLAN auf 4.3.2.1 (wie WLED): Android zeigt die Anmeldeseite nicht, wenn seine Pruefadresse auf
//    eine private Adresse (192.168.x.x) zeigt. Der DHCP-Server nennt das Geraet als DNS-Server und die Portal-
//    Adresse (Option 114); ein kleiner DNS-Server leitet jede Adresse um - die Seite oeffnet sich von selbst.
//    DNS-Server nur, solange das Einrichtungs-WLAN an ist.
//  - Zwei getrennte Passwoerter:
//    Zugangspasswort (Weboberflaeche): ab Werk "panelsat", beim ersten Oeffnen muss ein eigenes vergeben werden
//    (Cookie-Anmeldung, 5 Fehlversuche -> 30 s Pause).
//    WLAN-Passwort des Einrichtungs-WLANs: eigene Einstellung, ab Werk an mit "panelsat", abschaltbar (dann offen –
//    geschuetzt bleibt die Weboberflaeche durch das Zugangspasswort).
//  - Mehrfach-Reset: schnell hintereinander aus/ein (bzw. Reset-Taste), jeweils unter 10 s gelaufen.
//    3x = Einrichtungs-WLAN sofort an und bis zum Neustart an, 5x = beide Passwoerter auf Werkseinstellung, und
//    das Geraet loescht seine WLAN-Daten (boot_check() meldet factory).
// NVS-Namensraum "devsetup": apw (Zugangspasswort), pwset, appw (WLAN-Passwort), apon (WLAN-Passwort an),
// bootc (Einschaltzaehler).
#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <functional>

#define DEVSETUP_DEFAULT_PW "panelsat"

namespace devsetup {

struct Boot {
  uint8_t count = 0;
  bool force_ap = false;   // Einrichtungs-WLAN sofort an und anlassen
  bool factory = false;    // WLAN-Daten loeschen (Passwort ist schon zurueckgesetzt)
};

Boot boot_check();                         // ganz am Anfang von setup()
bool force_ap();                           // Ergebnis von boot_check (Mehrfach-Reset >= 3)

void ap_start(const String &ssid);         // Einrichtungs-WLAN an (Modus vorher setzen), DNS-Server an
void ap_stop();                            // Einrichtungs-WLAN und DNS-Server aus
bool ap_active();

void begin(WebServer &srv);                // laden, /api/auth|login|setpw|appw und Portal-Adressen anmelden
void loop();                               // DNS, Zaehler zuruecksetzen, Einrichtungs-WLAN nach Passwortwechsel neu

bool authed();                             // angemeldeter Browser?
void deny();                               // 401 mit {"auth":"login"|"setup"}
std::function<void()> guard(std::function<void()> f);   // Handler nur fuer Angemeldete
bool upload_allowed();                     // fuer Upload-Rueckrufe (Firmware): beim Start des Uploads pruefen

bool password_set();                       // false = Zugangspasswort ist noch das Standardpasswort
void password_reset();                     // beide Passwoerter zurueck auf "panelsat" (Touch-Halten, 5x-Reset)

}  // namespace devsetup
