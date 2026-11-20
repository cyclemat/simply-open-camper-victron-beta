# Changelog – Victron-Display

Neueste Version oben. *Beta* = eingebaut, aber noch nicht an echter Hardware getestet – Rückmeldungen willkommen.

## 0.2.5 – 2026-11-20

**Neu**
- 🏕️ **Hobby** *(Beta, nur Waveshare 7")* – Wohnwagen/Reisemobile von Hobby: über die HobbyConnect-Box per Bluetooth
  oder per Kabel am LSG (CI-BUS, RS232-Wandler, UART und Pins wählbar), beide ab Werk aus. Lichter schaltbar,
  Bordbatterie als Batteriewächter-Kachel, Wasser, Temperaturen, Heizung, Kühlschrank, Klima.

**Verbessert**
- **Sensoren und Heizung/Kühlbox nicht mehr unter „Akkus“:** eigene Karten „Sensoren per Bluetooth“ und „Dieselheizung
  und Kühlbox per Bluetooth“, jeweils mit eigener Suche (Waveshare 7": Menüpunkt „Sensoren“, Heizung/Kühlbox unter
  „Gas“). Waveshare 7": bis zu 12 Bluetooth-Geräte statt 6.
- Gefundene Ruuvi-, SwitchBot-, Simarine-, BTHome-Sensoren, Heizungen und Kühlboxen tragen in der Suchliste jetzt den
  richtigen Namen (vorher teils „Power Quarry“).
- Waveshare 7": linke Leiste der Einstellungen scrollt (feste Zeilenhöhe).
- Waveshare 7": Browser-Editor mit **Seitenleiste** wie am Display (Netzwerk, Shelly, Anzeige, Gas, Victron, Akkus,
  Power Stations, Sensoren, Fahrzeug, Sicherung, Info); Hobby auch im Browser einstellbar.
- Waveshare 7": Browser-Editor **am Handy** bedienbar – unten Geräte · Panel · Einstellungen, Kacheln als Liste.
- README: Verdrahtung des RS232-TTL-Wandlers für den Hobby-CI-BUS ergänzt.

## 0.2.4 – 2026-11-19

**Neu**
- 🔋 **Zellspannungen** jetzt auch bei JK-BMS, ANT, Eco-Worthy, Renogy-Akku und Vatrer.
- 🔋 Akku-Übersicht (Kachel 2 s drücken): zusätzlich Kapazität, Gesundheit, Zellen min–max mit Δ und jede Zelle
  einzeln; wischen zum Blättern.
- 🔥 **Dieselheizung** *(Beta)* – Vevor/Hcalory (AA55/AA66, auch verschlüsselt), HeaterCC/AirHeater (ABBA):
  Zustand, Stufe/Soll, Innen-/Brennkammertemperatur, Spannung, Fehler; ein/aus, Stufe ±, Temperatur ±.
- 🧊 **Kühlbox** *(Beta)* – Alpicool, IceCube/Plug-in Festivals, Bodega, Brass Monkey: Ist/Soll je Zone, Spannung;
  ein/aus, Soll ±.
- 📡 **BTHome** *(Beta)* – Shelly BLU, Xiaomi mit pvvx-Firmware u. v. a.: Temperatur/Feuchte bzw. Zustand
  (Tür, Bewegung, Nässe, Taster), auch verschlüsselt.
- ⚖️ **Senso4s** *(Beta)* – Gasflaschen-Waage Basic/Plus per Bluetooth, eingerichtet wie Mopeka: Füllstand in % und kg.

## 0.2.3 – 2026-11-19

**Neu**
- 💧 **Simarine STRx-Tanksensoren** *(Beta)* – STR1/STR2/STR3 per Bluetooth-Broadcast. Offen lesbar oder verschlüsselt
  mit dem Schlüssel des Sensors aus der Simarine-App (kein App-Key nötig). Füllstand, Inhalt in Litern, Tankart.

**Verbessert**
- Feld „PIN / Passwort“ nimmt bis zu 32 Zeichen an (Simarine-Schlüssel).

## 0.2.1 – 2026-11-19

**Neu**
- 🌡️ **Ruuvi** *(Beta)* – Ruuvi Tag und Ruuvi Air: Temperatur, Luftfeuchte, Luftdruck, CO₂, Feinstaub, Batterie.
- 🌡️ **SwitchBot** *(Beta)* – Meter, Meter Plus, Outdoor Meter: Temperatur, Luftfeuchte, Batterie. Kein Hub nötig.
- 🔋 **CS-BATTERIES** *(Beta)* – Anmeldung der CS-App automatisch; eigenes Passwort im Feld „PIN / Passwort“.
- 🔋 **JBD-Akkus**: Zellspannungen, weitere Bluetooth-Variante (FFE0).

**Verbessert**
- Feld „PIN / Passwort“ nimmt auch Buchstaben an.

## 0.2.0 – 2026-11-19

**Neu**
- 🛢️ **Mopeka** – Gas- und Tanksensoren per Bluetooth.
- 💧 **SuperSense-Tankmodul** *(Beta)*.
- 🖥️ **Waveshare 7"** – dieselbe Firmware mit Einstellungen am Bildschirm, Shelly und eingebautem MQTT-Broker.
- 🔋 Akkus mit Bluetooth-BMS und ⚡ Power Stations, Kopplungs-PIN.
- 🌍 Englische Fassung, Web-Flasher mit Sprach- und Geräteauswahl.
