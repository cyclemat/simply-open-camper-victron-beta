// Uebersetzung der Panel-Oberflaeche
//
// Die Texte im Code sind deutsch. tr("Deutscher Text") liefert die Uebersetzung der beim Bauen gewaehlten
// Sprache (platformio.ini: [features] lang = de | en | es | fr | ...) oder – falls es keine gibt – den deutschen
// Text unveraendert. Die Uebersetzungen stehen je Sprache in src/i18n/<kuerzel>.h als Paare
// {"Deutsch", "Uebersetzung"}.
//
// Ganze Texte uebersetzen die Oberflaechenbausteine (ui_label, ui_button, ui_row, Hinweise, Dialoge,
// lv_label_set_text) automatisch. Bruchstuecke ("Ziel " + Temperatur) und Formate ("vor %ld Min.") werden an
// der Stelle mit tr() umschlossen.
//
// Neue Sprache: python tools/i18n_update.py <kuerzel>, dann src/i18n/<kuerzel>.h uebersetzen und
// "lang = <kuerzel>" setzen. Fehlende Eintraege bleiben einfach deutsch.
#pragma once
#include <string>

#ifndef PANEL_LANG
#define PANEL_LANG de
#endif
// Dezimaltrenner in Zahlen auf dem Display: 1 = Komma (deutsch, Standard), 0 = Punkt (englisches Victron-Display)
#ifndef UI_DECIMAL_COMMA
#define UI_DECIMAL_COMMA 1
#endif

const char *tr(const char *de);
static inline const char *tr(const std::string &de) { return tr(de.c_str()); }
const char *i18n_lang();                 // Sprachkuerzel, z. B. "en"
// Mehrzeilige Auswahlliste ("Nie\n30 Sekunden\n...") zeilenweise uebersetzen
std::string tr_lines(const char *de);
// Zusammengesetzte Werte ("Absorption  ·  14,2 V", "86 %  ·  laedt") stueckweise uebersetzen: jedes Stueck zwischen
// den Trennern " · " / ", " fuer sich (Zahlen bleiben, wie sie sind). Ohne Uebersetzungstabelle (Deutsch) unveraendert.
std::string tr_parts(const std::string &de);
