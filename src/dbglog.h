// Debug-Log (Fehlersuche) fuer alle Firmwares: schreibt alle Log-Ausgaben (Arduino log_x/ets_printf und ESP-IDF
// ESP_LOGx) mit Uhrzeit in einen Ringpuffer (im Browser ansehbar) und – wenn eine SD-Karte da ist – in Dateien.
// Alle 60 s eine Zustandszeile (Speicher, WLAN, dazu was die Firmware per dbglog_set_status meldet).
//
// Ab Werk AUS (NVS "dbglog" on). Aus = keine Puffer, kein Task, nur ein NVS-Lesezugriff beim Start.
// An: Zwischenpuffer intern (aus dem ROM-Ausgabekanal 2, ISR-fest), Task "dbglog" verschiebt jede 250 ms in den
// Ringpuffer (PSRAM, sonst intern) und schreibt alle 2 s in die Datei <Ordner>/log-NNNNN.txt (je max. 1 MB,
// hoechstens 10 Dateien, die aeltesten werden geloescht).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#ifndef WITH_DBGLOG
#define WITH_DBGLOG 1
#endif
#ifndef DBGLOG_CYD_SD
#define DBGLOG_CYD_SD 0   // nur CYD-Builds mit genug Flash (Victron-Display, Zusatzdisplay)
#endif

struct DbgFile {
  std::string name;
  long size = 0;
};

#if WITH_DBGLOG
// ring: Groesse des Ringpuffers (z. B. 64 KB mit PSRAM, 8 KB ohne). storage: liefert den Ordner fuer Dateien
// ("" = keine Dateien, z. B. ohne SD-Karte); wird im Task aufgerufen (darf die Karte einbinden), bis er einen
// Ordner liefert hoechstens alle 30 s.
void dbglog_begin(size_t ring, const char *(*storage)() = nullptr);
bool dbglog_on();
void dbglog_set(bool on);                         // speichern und sofort an- bzw. ausschalten
void dbglog_set_status(std::string (*fn)());      // Zusatz fuer die Zustandszeile (z. B. "MQTT verbunden · …")
void dbglog_note(const char *text);               // eigene Zeile ins Log (z. B. "Debug-Log eingeschaltet")
std::string dbglog_ring();                        // Inhalt des Ringpuffers (aelteste Zeile zuerst)
std::string dbglog_dir();                         // Ordner der Dateien ("" = keine)
std::string dbglog_current();                     // Name der aktuellen Datei
std::vector<DbgFile> dbglog_files();
bool dbglog_valid_name(const std::string &name);  // nur log-NNNNN.txt (Schutz vor Pfadangaben)
bool dbglog_delete(const std::string &name);      // "*" = alle ausser der aktuellen
uint32_t dbglog_lost();                           // verlorene Zeichen (Zwischenpuffer voll)
size_t dbglog_ring_size();                        // Groesse des Ringpuffers in Byte
#if DBGLOG_CYD_SD
const char *dbglog_cyd_sd();                      // CYD: microSD am VSPI (18/19/23, CS 5) einbinden -> "/sd/logs"
#endif
#else
inline void dbglog_begin(size_t, const char *(*)() = nullptr) {}
inline bool dbglog_on() { return false; }
inline void dbglog_set(bool) {}
inline void dbglog_set_status(std::string (*)()) {}
inline void dbglog_note(const char *) {}
inline std::string dbglog_ring() { return std::string(); }
inline std::string dbglog_dir() { return std::string(); }
inline std::string dbglog_current() { return std::string(); }
inline std::vector<DbgFile> dbglog_files() { return {}; }
inline bool dbglog_valid_name(const std::string &) { return false; }
inline bool dbglog_delete(const std::string &) { return false; }
inline uint32_t dbglog_lost() { return 0; }
inline size_t dbglog_ring_size() { return 0; }
#endif
