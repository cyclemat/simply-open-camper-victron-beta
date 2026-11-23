# Fertige Firmware – Simple Open Camper Victron Edition (CYD 2,8" und Waveshare 7"), Version 0.2.7

Am einfachsten über den [⚡ Web-Flasher](https://cyclemat.github.io/simply-open-camper-victron-beta/). Die vorige Fassung
(0.2.5) liegt in [`vorher/`](vorher/).

| Datei | Wofuer |
|---|---|
| [victron-display-cyd.bin](victron-display-cyd.bin) | **CYD 2,8", erstes Aufspielen per USB** (Deutsch): komplett (Bootloader, Partitionen, Firmware, Updater) – `esptool.py write_flash 0x0 victron-display-cyd.bin` – **loescht die Einstellungen** |
| [victron-display-cyd-en.bin](victron-display-cyd-en.bin) | dasselbe auf **Englisch** / **first install via USB** (English, erases settings) |
| [ota/victron-display-cyd.bin](ota/victron-display-cyd.bin) | CYD: **Update ueber die Weboberflaeche** des Displays (Deutsch) – Einstellungen bleiben |
| [ota/victron-display-cyd-en.bin](ota/victron-display-cyd-en.bin) | CYD: **update via the web interface** (English) – settings stay |
| [ota/updater-esp32.bin](ota/updater-esp32.bin) | nur der kleine Updater (Adresse 0x2F0000), normalerweise nicht noetig |
| [victron-display-ws7.bin](victron-display-ws7.bin) | **Waveshare 7"** (ESP32-S3), erstes Aufspielen per USB: komplett – `esptool.py --chip esp32s3 write_flash 0x0 victron-display-ws7.bin` (nur Deutsch) – **loescht die Einstellungen** |
| [ota/victron-display-ws7.bin](ota/victron-display-ws7.bin) | **Waveshare 7"**: Update im Browser-Editor des Displays (Einstellungen → Info → Firmware aktualisieren) |

Neu in 0.2.7 (Auszug): neuer Name, Orion XS/Smart bedienen (Ein/Aus, Ladestrom – Beta), Waveshare 7" mit Live-Ansicht.
Alles: [CHANGELOG.md](../CHANGELOG.md).
