# Fertige Firmware – Victron-Display (CYD 2,8" und Waveshare 7"), Version 0.2.5

Die vorige Fassung liegt in [`vorher/`](vorher/).

| Datei | Wofuer |
|---|---|
| [victron-display-cyd.bin](victron-display-cyd.bin) | **Erstes Aufspielen per USB** (Deutsch): komplett (Bootloader, Partitionen, Firmware, Updater) – `esptool.py write_flash 0x0 victron-display-cyd.bin` |
| [victron-display-cyd-en.bin](victron-display-cyd-en.bin) | dasselbe auf **Englisch** / **first install via USB** (English) |
| [ota/victron-display-cyd.bin](ota/victron-display-cyd.bin) | **Update ueber die Weboberflaeche** des Displays (Deutsch) |
| [ota/victron-display-cyd-en.bin](ota/victron-display-cyd-en.bin) | **Update via the web interface** (English) |
| [ota/updater-esp32.bin](ota/updater-esp32.bin) | nur der kleine Updater (Adresse 0x2F0000), normalerweise nicht noetig |
| [victron-display-ws7.bin](victron-display-ws7.bin) | **Waveshare 7"** (ESP32-S3), erstes Aufspielen per USB: komplett – `esptool.py --chip esp32s3 write_flash 0x0 victron-display-ws7.bin` (Beta, nur Deutsch) |
| [ota/victron-display-ws7.bin](ota/victron-display-ws7.bin) | **Waveshare 7"**: Update im Browser-Editor des Displays (System → Firmware aktualisieren) |
