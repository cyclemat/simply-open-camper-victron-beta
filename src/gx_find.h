// Victron GX (Cerbo GX, Ekrano, Venus GX …) im Netz automatisch finden – fuer alle Firmwares mit Cerbo-Anbindung.
// Reihenfolge: 172.24.24.1 (eigenes WLAN des GX), Gateway, venus.local / einstein.local, dann das eigene Netz (/24)
// nach offenem MQTT-Port 1883. Jeder Treffer wird geprueft: kurz per MQTT anmelden und auf "N/<Portal-ID>/system/0/Serial"
// warten – so wird ein anderer MQTT-Broker (z. B. der von Zigbee2MQTT) nicht faelschlich als GX genommen.
// Laeuft als kurzlebige Task (Stack 6 KB), die sich am Ende selbst beendet; kein Dauer-RAM.
// Ohne Rueckruf traegt sie den gefundenen GX direkt in der Cerbo-Anbindung ein (cerbo_set_config).
#pragma once
#include <functional>
#include <string>

using GxFound = std::function<void(const std::string &host, const std::string &portal)>;

bool gx_find_start(GxFound on_found = nullptr);   // false = laeuft schon oder kein Netz
// Empfaenger fuer Funde ohne eigenen Rueckruf (auch fuer gx_find_watch). Ohne: direkt in cerbo_set_config eintragen.
// Achtung: wird in der Such-Task aufgerufen, nicht in loop().
void gx_find_set_handler(GxFound h);
bool gx_find_running();
std::string gx_find_status();                     // "Suche …", "Gefunden: …", "Kein GX gefunden" ("" = noch nie gesucht)
// Einmal pro Sekunde aufrufen: ist der eingetragene GX laenger nicht erreichbar (neue IP vom Router?), wird neu gesucht
// (hoechstens alle 10 Minuten).
void gx_find_watch();
