#!/usr/bin/env python3
"""Erzeugt die englische Weboberflaeche des Victron-Displays aus der deutschen (src/vicdisp/vd_page.h).

Das Woerterbuch tools/i18n/vicdisp_en.json ist eine Liste von Paaren [deutsch, englisch]. Jedes Paar ist eine
exakte Textersetzung (laengste zuerst). Kurze Woerter immer mit Zusammenhang eintragen (z. B. "'Aus']" statt "Aus"),
damit kein Code getroffen wird. Ein Eintrag, der in der Seite nicht (mehr) vorkommt, bricht ab – so faellt auf,
wenn sich die deutsche Seite geaendert hat.

Ergebnis: src/generated/vd_page_en.h (gleicher Variablenname VD_PAGE). Laeuft als PlatformIO-Vorab-Skript der
Victron-Umgebungen (custom_lang = en nutzt die Datei); von Hand: python tools/i18n_page.py
"""
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) if '__file__' in globals() else None


def generate(root):
    src = os.path.join(root, 'src', 'vicdisp', 'vd_page.h')
    dic = os.path.join(root, 'tools', 'i18n', 'vicdisp_en.json')
    out = os.path.join(root, 'src', 'generated', 'vd_page_en.h')
    text = open(src, encoding='utf-8').read().replace('\r\n', '\n')   # Windows-Checkout mit CRLF: Eintraege mit \n passen sonst nicht
    pairs = json.load(open(dic, encoding='utf-8'))
    missing = []
    # laengste zuerst, damit ganze Saetze vor einzelnen Woertern ersetzt werden
    for de, en in sorted(pairs, key=lambda p: -len(p[0])):
        if de not in text:
            missing.append(de)
            continue
        text = text.replace(de, en)
    if missing:
        sys.stderr.write('i18n_page: %d Eintraege passen nicht mehr zu vd_page.h:\n' % len(missing))
        for m in missing:
            sys.stderr.write('  %r\n' % m)
        sys.exit(1)
    text = text.replace('// Victron-Display: Weboberflaeche', '// ERZEUGT von tools/i18n_page.py – nicht von Hand aendern. Victron-Display: Weboberflaeche (Englisch)', 1)
    # Reste pruefen: Umlaute ausserhalb von Code deuten auf vergessene Texte
    rest = sorted(set(re.findall(r'[^\s<>"\'`]*[äöüÄÖÜß][^\s<>"\'`]*', text)))
    if rest:
        sys.stderr.write('i18n_page: noch deutsch? %s\n' % ', '.join(rest[:40]))
    os.makedirs(os.path.dirname(out), exist_ok=True)
    old = open(out, encoding='utf-8').read() if os.path.exists(out) else None
    if old != text:   # nur bei Aenderung schreiben (sonst baut PlatformIO unnoetig neu)
        with open(out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    return out


try:
    Import('env')  # noqa: F821  (PlatformIO/SCons)
    # Immer erzeugen (schreibt nur bei Aenderung): eine NEUE Kopfdatei in src/ laesst PlatformIO beim naechsten Lauf
    # den ganzen Build-Ordner leeren – deshalb soll sie schon beim deutschen Build da sein, nicht erst beim englischen.
    root = os.path.dirname(env.subst('$PROJECT_SRC_DIR'))  # noqa: F821
    out = generate(root)
    if env.GetProjectOption('custom_lang', 'de') == 'en':  # noqa: F821
        print('i18n_page: englische Weboberflaeche ->', out)
except NameError:
    if __name__ == '__main__':
        print(generate(ROOT))
