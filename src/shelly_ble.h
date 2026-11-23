// Shelly Gen2/Gen3/Gen4 per Bluetooth (BLE-RPC, Beta): Panel schaltet und liest Shellys ohne WLAN/Broker.
// Geraete erscheinen wie die MQTT-Shellys als ControlView mit Schluessel "shb:<mac>#switch:0" bzw. "#light:0" und laufen
// ueber die z2m_*-Funktionen (Kacheln, Regeln, Live-Ansicht, Zusatzdisplays).
//
// Ab Werk AUS (NVS "shellyble": on, n, m<i>/t<i>/n<i>/p<i>). Kurze Verbindungen ueber den Bluetooth-Verteiler (truma.cpp):
// Stand alle 30 s, Befehle sofort. Protokoll: Shelly-API "BLE RPC" (Dienst _mOS_RPC_SVC_ID_, Laenge senden/empfangen,
// JSON-RPC wie im WLAN), Passwort per Digest SHA-256. Gen1 hat kein Bluetooth.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "z2m.h"

#ifndef WITH_SHELLY_BLE
#define WITH_SHELLY_BLE 0
#endif
#define SHB_MAX 8

class NimBLEAdvertisedDevice;

inline bool shb_is_key(const std::string &key) { return key.compare(0, 4, "shb:") == 0; }

struct ShbChannel {
  std::string comp;          // "switch:0", "light:1"
  bool on = false, known = false;
  int bri = -1;              // Licht: 0..100
  float power = NAN, energy = NAN;   // W, kWh
};
struct ShbInfo {
  std::string mac, name, model, status;
  bool has_pw = false, auth = false, online = false;
  int32_t age_s = -1;        // seit letzter Antwort
  int rssi = 0;
  std::vector<ShbChannel> ch;
};
struct ShbFound {
  std::string mac, name;
  uint8_t type = 0;
  int rssi = 0;
  bool configured = false;
};

#if WITH_SHELLY_BLE
void shb_begin();
bool shb_enabled();
void shb_set_enabled(bool on);
std::vector<ShbInfo> shb_list();
std::vector<ShbFound> shb_found();
void shb_search(uint32_t ms);                 // Suche starten (0 = stoppen)
bool shb_searching();
std::string shb_add(const std::string &mac, uint8_t type, const std::string &name, const std::string &pw);   // "" = ok
void shb_remove(const std::string &mac);
void shb_rename(const std::string &mac, const std::string &name);
void shb_set_password(const std::string &mac, const std::string &pw);
uint32_t shb_version();                       // aendert sich bei neuen Werten/Geraeten
// Kacheln (ueber z2m_*)
bool shb_get(const std::string &key, ControlView &out);
void shb_all(std::vector<ControlView> &out);
size_t shb_count();
void shb_toggle(const std::string &key);
void shb_set_on(const std::string &key, bool on);
void shb_set_brightness_pct(const std::string &key, int pct);
// Bluetooth-Verteiler (truma.cpp, Bluetooth-Task)
bool shb_ble_needed();
bool shb_on_adv(const NimBLEAdvertisedDevice *dev);
void shb_scan_wanted(bool &passive, bool &active);
bool shb_want_connect();
void shb_connect_now();
#else
inline void shb_begin() {}
inline bool shb_enabled() { return false; }
inline void shb_set_enabled(bool) {}
inline std::vector<ShbInfo> shb_list() { return {}; }
inline std::vector<ShbFound> shb_found() { return {}; }
inline void shb_search(uint32_t) {}
inline bool shb_searching() { return false; }
inline std::string shb_add(const std::string &, uint8_t, const std::string &, const std::string &) { return "aus"; }
inline void shb_remove(const std::string &) {}
inline void shb_rename(const std::string &, const std::string &) {}
inline void shb_set_password(const std::string &, const std::string &) {}
inline uint32_t shb_version() { return 0; }
inline bool shb_get(const std::string &, ControlView &) { return false; }
inline void shb_all(std::vector<ControlView> &) {}
inline size_t shb_count() { return 0; }
inline void shb_toggle(const std::string &) {}
inline void shb_set_on(const std::string &, bool) {}
inline void shb_set_brightness_pct(const std::string &, int) {}
inline bool shb_ble_needed() { return false; }
inline bool shb_on_adv(const NimBLEAdvertisedDevice *) { return false; }
inline void shb_scan_wanted(bool &, bool &) {}
inline bool shb_want_connect() { return false; }
inline void shb_connect_now() {}
#endif
