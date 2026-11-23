// Anzeige fuer Satelliten mit Display – siehe sat_display.h
// Optik wie das Hauptpanel: LVGL, gleiche Farben (ui/theme.h), Schriften Inter 16/20 und Symbolschrift.
#include "sat_display.h"

#if SAT_DISPLAY
#include <Arduino.h>
#include <WiFi.h>
#include <math.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <vector>
#include <new>
#include <deque>
#include <LovyanGFX.hpp>
#include <lvgl.h>
#include "esp_heap_caps.h"
#include <Preferences.h>
#if SAT_VICTRON_ONLY   // eigene Firmware "Victron-Display" (src/vicdisp/): Cerbo GX, Victron-Bluetooth, Truma LevelControl, Mopeka
#include "../vicdisp/vd_main.h"
#else
#include "sat_config.h"
#include "sat_io.h"
#endif
#include "../mopeka.h"
#include "../truma.h"
#include "../victron_ctl.h"
#include "../victron.h"
#include "../bms.h"
#if WITH_CERBO
#include "../cerbo.h"
#endif
#define POP_ON (WITH_CERBO || SAT_REMOTE)   // Bedienfenster MultiPlus/Ladegeraet
#include "rd_client.h"
#include "../fonts/fonts.h"
#include "../fonts/icons.h"
#include "../ui/theme.h"
#if SAT_SPLASH   // Startbild 3 s (tools/embed_splash_cyd.py, Bild assets/splash_cyd.jpg 320x240)
#include "../generated/splash_cyd_jpg.h"
#endif

// ---------------------------------------------------------------- Hardware: ESP32-2432S028R
// Display ILI9341 an HSPI (SCK 14, MOSI 13, MISO 12, CS 15, DC 2), Licht GPIO 21.
// Touch XPT2046 an eigenem SPI (CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36). RGB-LED 4/16/17 (Low = an).
#ifndef SAT_TOUCH_XMIN
#define SAT_TOUCH_XMIN 300    // Rohwerte des Touch-Controllers an den Raendern – bei Bedarf nachstellen
#define SAT_TOUCH_XMAX 3900
// Y vertauscht (2026-11-13 am echten Board): quer liegt die Waagerechte auf der Y-Achse des Touch-Controllers,
// links/rechts waren gespiegelt
#define SAT_TOUCH_YMIN 3700
#define SAT_TOUCH_YMAX 200
#endif
#ifndef SAT_DISPLAY_ROTATION
#define SAT_DISPLAY_ROTATION 1   // quer
#endif

namespace {
class CydDisplay : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_XPT2046 touch_;

 public:
  CydDisplay() {
    {
      auto c = bus_.config();
      c.spi_host = SPI2_HOST;   // HSPI
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.spi_3wire = false;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 14;
      c.pin_mosi = 13;
      c.pin_miso = 12;
      c.pin_dc = 2;
      bus_.config(c);
      panel_.setBus(&bus_);
    }
    {
      auto c = panel_.config();
      c.pin_cs = 15;
      c.pin_rst = -1;
      c.pin_busy = -1;
      c.panel_width = 240;
      c.panel_height = 320;
      c.offset_rotation = 0;
      c.readable = true;
      c.invert = false;
      c.rgb_order = false;
      c.dlen_16bit = false;
      c.bus_shared = false;
      panel_.config(c);
    }
    {
      auto c = light_.config();
      c.pin_bl = 21;
      c.invert = false;
      c.freq = 12000;
      c.pwm_channel = 7;
      light_.config(c);
      panel_.setLight(&light_);
    }
    {
      auto c = touch_.config();
      c.x_min = SAT_TOUCH_XMIN;
      c.x_max = SAT_TOUCH_XMAX;
      c.y_min = SAT_TOUCH_YMIN;
      c.y_max = SAT_TOUCH_YMAX;
      c.pin_int = 36;
      c.bus_shared = false;
      c.offset_rotation = 0;
      c.spi_host = SPI3_HOST;   // VSPI
      c.freq = 1000000;
      c.pin_sclk = 25;
      c.pin_mosi = 32;
      c.pin_miso = 39;
      c.pin_cs = 33;
      touch_.config(c);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};

static CydDisplay s_lcd;
static const int W = 320, H = 240;
static const int BUF_LINES = 20;   // Zeichenpuffer 320 x 20 Pixel (12,5 KB intern, DMA-faehig)
static lv_disp_draw_buf_t s_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;
static uint32_t s_last_touch = 0;
static bool s_dim = false;
static const uint8_t DIMMED = 12;
// Einstellungen (Weboberflaeche, NVS "satdisp")
static int s_per = 4;            // Kacheln pro Seite fuer Seiten ohne eigene Angabe: 4 (2x2), 2 (gross) oder 1 (ganz)
static std::vector<uint8_t> s_pl; // Kacheln je Seite (Seite 1, 2, …) aus dem Editor; danach gilt s_per
static int s_lay = 0;            // Aufteilung, fuer die die Kacheln gerade angeordnet sind
static uint8_t s_bright = 200;   // Helligkeit 20..255
static uint16_t s_dim_min = 2;   // nach so vielen Minuten ohne Beruehrung dimmen, 0 = nie
#if SAT_REMOTE
#define SAT_CLOCK 1   // Uhr als Bildschirmschoner (Zeit kommt vom Hauptpanel)
#else
#define SAT_CLOCK 0
#endif
static uint8_t s_saver = SAT_CLOCK;   // nach der Dimm-Zeit: 0 nur abdunkeln, 1 Uhr zeigen
#if SAT_REMOTE   // LED-Ampel (siehe led_loop)
static uint8_t s_led = 1;
static std::string s_led_key;
static bool s_led_init = false;
static uint32_t s_led_rgb = 0xFFFFFFFF, s_led_ms = 0;
static int s_led_pct = -1;
#endif
static lv_obj_t *s_clk = nullptr, *s_clk_time = nullptr, *s_clk_date = nullptr;
static void clock_close() {
  if (!s_clk) return;
  lv_obj_del_async(s_clk);
  s_clk = s_clk_time = s_clk_date = nullptr;
}

// Lichtsensor vorne neben dem Display (LDR an GPIO 34, nur Analogeingang): Helligkeit hell/dunkel umschalten.
// Groesserer Messwert = dunkler. Geglaettet, mit Hysterese (zurueck auf hell unter 75 % der Grenze) und 3 s
// Bestaetigung, damit ein Schatten nicht flackert. Kein eigener Task, gelesen alle 0,5 s aus sat_display_loop().
#ifndef SAT_LDR_PIN
#define SAT_LDR_PIN 34
#endif
static bool s_auto = true;          // Helligkeit automatisch (ab Werk an)
static uint8_t s_night = 40;        // Helligkeit bei Dunkelheit
static uint16_t s_ldr_thr = 300;    // ab diesem Messwert ist es dunkel
static float s_ldr = -1;            // geglaetteter Messwert, -1 = noch keiner
static bool s_night_on = false;     // Sensor meldet dunkel
static uint32_t s_ldr_ms = 0, s_ldr_chg_ms = 0;

static uint8_t cur_bright() { return s_auto && s_night_on ? s_night : s_bright; }
struct TileCfg {
  std::string k, n;   // Schluessel ("p:tank1", "t:<mac>", "m:<mac>", "v:<mac>"), eigener Name
  bool on = true;
  uint8_t pg = 255;   // feste Seite (0 = Seite 1) aus dem Editor, 255 = automatisch (hinten anhaengen)
};
static std::vector<TileCfg> s_cfg;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  s_lcd.startWrite();
  s_lcd.setAddrWindow(a->x1, a->y1, w, h);
  s_lcd.writePixels((lgfx::rgb565_t *)px, w * h);
  s_lcd.endWrite();
  lv_disp_flush_ready(drv);
}

static void touch_cb(lv_indev_drv_t *, lv_indev_data_t *data) {
  lgfx::touch_point_t tp;
  bool down = s_lcd.getTouch(&tp) > 0;
  if (down) {
    s_last_touch = millis();
    if (s_dim) {   // erste Beruehrung weckt nur (nicht an LVGL weitergeben)
      s_dim = false;
      clock_close();
      s_lcd.setBrightness(cur_bright());
      data->state = LV_INDEV_STATE_RELEASED;
      return;
    }
  }
  data->state = down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  if (down) {
    data->point.x = tp.x;
    data->point.y = tp.y;
  }
}

// ---------------------------------------------------------------- Inhalt
struct Item {
  std::string key, name, value, icon;
  lv_color_t accent;
  int sw = -1;     // Schalter: Punktnummer (idx) fuer sat_io_switch, sonst -1
  bool on = false;
  std::string ctl;   // Cerbo-Schluessel, wenn ueber den GX einstellbar (Bedienfenster beim Antippen)
  bool ract = false;      // Zusatzdisplay: Antippen schaltet ueber das Hauptpanel
  uint32_t lit_rgb = 0;   // Zusatzdisplay: Lichtfarbe einer eingeschalteten Kachel (0 = Bernstein)
};

#if SAT_VICTRON_ONLY
static const int MAX_TILES = 4, TOP = 8, GAP = 8, DOTS_H = 16;   // ohne Kopfzeile: nur Kacheln
#else
static const int MAX_TILES = 4, TOP = 30, GAP = 8, DOTS_H = 16;
#endif

struct Tile {
  lv_obj_t *box, *icon, *name, *value, *sub;   // sub = zweiter Wert (Zeile darunter, kleiner)
};
static Tile s_tiles[MAX_TILES];
static lv_obj_t *s_title, *s_net, *s_dots;
static bool s_built = false;   // Hauptschirm aufgebaut (erst nach dem Startbild; im Web-Modus entladen)
// Kachelliste als deque: liegt in kleinen Stuecken im RAM. Ein vector brauchte bei 36 Kacheln einen
// zusammenhaengenden Block von ~7 KB – den fand das CYD (ohne PSRAM, zerstueckelter Speicher) oft nicht mehr.
typedef std::deque<Item> ItemList;
static ItemList s_items;
static int s_page = 0;
static uint32_t s_next_ms = 0;

static std::string num(float v, int dec, const char *unit) {
  if (isnan(v)) return "–";
  char b[32];
  snprintf(b, sizeof(b), "%.*f %s", dec, v, unit);
#if UI_DECIMAL_COMMA
  for (char *p = b; *p; p++)
    if (*p == '.') *p = ',';
#endif
  return b;
}

#if !SAT_VICTRON_ONLY
static const char *point_icon(const SatPoint &p) {
  switch (p.kind) {
    case SP_TANK: return ICON_BATH;
    case SP_TEMP: return ICON_THERMO;
    case SP_INPUT: return ICON_DOOR;
    case SP_SWITCH: return ICON_POWER;
    case SP_TILT: return ICON_HOUSE;
    default: break;
  }
  switch (p.cls) {
    case SC_TEMP: return ICON_THERMO;
    case SC_VOLTAGE: return ICON_BATTERY;
    case SC_CURRENT: return ICON_LIGHTNING;
    case SC_LEVEL: return ICON_BATH;
    case SC_ONOFF: return ICON_PLUG;
    default: return ICON_INFO;
  }
}
#endif

// Victron-Werte als Kachel – Bluetooth-Geraete ("v:<mac>") und Cerbo GX ("c:cerbo:<id>")
static Item vic_item(const VictronInfo &v, const std::string &key) {
  Item it;
  it.key = key;
  it.name = v.name.empty() ? std::string(tr(victron_kind_text(v.kind))) : v.name;
  it.accent = v.has ? C_BLUE : C_TEXT3;
  if (v.can_limit || v.can_mode) it.ctl = v.key;   // GX (MultiPlus/Ladegeraet/Orion) oder Orion per Bluetooth
  switch (v.kind) {
    case VIC_SOLAR: it.icon = ICON_SUN; break;
    case VIC_SHUNT: it.icon = ICON_BATTERY; break;
    case VIC_CHARGER: it.icon = ICON_LIGHTNING; break;
    case VIC_GRID: it.icon = ICON_PLUG; break;
    case VIC_INVERTER: it.icon = ICON_POWER; break;
    case VIC_PSTATION: it.icon = ICON_BATTERY; break;
    case VIC_LITHIUM: it.icon = ICON_BATTERY; break;
    case VIC_PROTECT: it.icon = ICON_POWER; break;
    case VIC_DCSYS: it.icon = ICON_BULB; break;
    default: it.icon = ICON_LIGHTNING; break;
  }
  if (!v.has) {
    it.value = v.status.empty() ? "–" : v.status;
    return it;
  }
  switch (v.kind) {
    case VIC_SOLAR:
      it.value = num(v.pv_w, 0, "W") + (isnan(v.yield_kwh) ? std::string("") : "  ·  " + num(v.yield_kwh, 2, "kWh"));
      it.accent = (!isnan(v.pv_w) && v.pv_w > 5) ? C_AMBER : C_TEXT2;
      break;
    case VIC_SHUNT: {
      if (victron_is_sense(v)) {   // Smart Battery Sense: Spannung und Temperatur
        it.value = num(v.bat_v, 2, "V") + (v.aux_mode == 2 && !isnan(v.aux) ? "  ·  " + num(v.aux, 0, "°C") : std::string());
        it.accent = C_BLUE;
        break;
      }
      float w = victron_dc_watt(v);
      it.value = num(v.soc, 0, "%") + "  ·  " + num(v.bat_v, 1, "V") + (isnan(w) ? std::string() : "  ·  " + num(w, 0, "W"));
      it.accent = (!isnan(v.soc) && v.soc < 20) ? C_RED : C_GREEN;
      break;
    }
    case VIC_CHARGER:
      it.value = num(v.power_w, 0, "W") + "  ·  " + victron_state_text(v.state);
      break;
    case VIC_GRID:
      it.value = v.ac_connected == 0 ? std::string("nicht verbunden") : num(v.ac_w, 0, "W") + "  ·  " + num(v.ac_v, 0, "V");
      it.accent = v.ac_connected == 0 ? C_TEXT3 : C_BLUE;
      break;
    case VIC_INVERTER:
      it.value = num(v.ac_w, 0, "W") + "  ·  " + victron_state_text(v.state);
      it.accent = (!isnan(v.ac_w) && v.ac_w > 5) ? C_AMBER : C_TEXT2;
      break;
    case VIC_PSTATION:
      it.value = num(v.soc, 0, "%") + "  ·  " + tr("raus ") + num(v.ps_out_w, 0, "W");
      it.accent = (!isnan(v.soc) && v.soc < 20) ? C_RED : C_GREEN;
      break;
    case VIC_DCDC: {   // Booster: Leistung gross, darunter Strom und Spannung (ohne Strom: Spannung und Zustand)
      float w = victron_dc_watt(v);
      if (!isnan(w))
        it.value = num(w, 0, "W") + "  ·  " + num(v.bat_i, 1, "A") + "  ·  " + num(v.bat_v, 1, "V");
      else
        it.value = isnan(v.bat_i) ? num(v.bat_v, 2, "V") + "  ·  " + victron_state_text(v.state)
                                  : num(v.bat_i, 1, "A") + "  ·  " + num(v.bat_v, 1, "V");
      it.accent = (!isnan(v.bat_i) && v.bat_i > 0.2f) ? C_GREEN : C_TEXT2;
      break;
    }
    case VIC_LITHIUM: {   // Spannung gross, darunter Temperatur und Zellen (niedrigste–hoechste)
      float lo = NAN, hi = NAN;
      for (float c : v.cell_v)
        if (!isnan(c)) {
          lo = isnan(lo) || c < lo ? c : lo;
          hi = isnan(hi) || c > hi ? c : hi;
        }
      it.value = num(v.bat_v, 2, "V");
      if (v.aux_mode == 2 && !isnan(v.aux)) it.value += "  ·  " + num(v.aux, 0, "°C");
      if (!isnan(lo)) {
        std::string l = num(lo, 2, "");
        l.pop_back();   // Leerzeichen vor der (leeren) Einheit
        it.value += std::string("  ·  ") + tr("Zellen ") + l + "–" + num(hi, 2, "V");
      }
      it.accent = v.alarm ? C_RED : C_GREEN;
      break;
    }
    case VIC_PROTECT:
      it.value = std::string(v.output == 1 ? "Ausgang an" : v.output == 0 ? "Ausgang aus" : "–") + "  ·  " + num(v.in_v, 2, "V");
      it.accent = v.alarm || v.error > 0 ? C_RED : v.output == 1 ? C_GREEN : C_AMBER;
      break;
    case VIC_DCSYS:
      it.value = num(v.power_w, 0, "W") + "  ·  " + num(v.bat_i, 1, "A");
      it.accent = (!isnan(v.power_w) && fabsf(v.power_w) > 2) ? C_AMBER : C_TEXT2;
      break;
    default:
      it.value = num(v.bat_v, 2, "V");
      break;
  }
  return it;
}

// Liefert das Geraet wirklich etwas? (Der GX meldet manche Werte als null, Bluetooth-Geraete ohne Paket/Schluessel)
static bool vic_has_value(const VictronInfo &v) {
  if (!v.has) return false;
  if (!v.gx) return true;
  switch (v.kind) {
    case VIC_SOLAR: return !isnan(v.pv_w);
    case VIC_SHUNT: return !isnan(v.soc) || !isnan(v.bat_v);
    case VIC_CHARGER: return !isnan(v.power_w) || !isnan(v.bat_i) || v.state >= 0;
    case VIC_GRID: return !isnan(v.ac_w) || v.ac_connected >= 0;
    case VIC_INVERTER: return !isnan(v.ac_w) || v.state >= 0;
    case VIC_DCSYS: return !isnan(v.power_w);
    default: return !isnan(v.bat_v);
  }
}

static void collect_raw(ItemList &s_items) {
  s_items.clear();
#if SAT_REMOTE
  // Zusatzdisplay: nur die Kacheln vom Hauptpanel (fertig aufbereitet)
  rd_each(
      [](const RdTile &r, void *ctx) {
        auto &v = *(ItemList *)ctx;
        v.emplace_back();
        Item &it = v.back();
        it.key = "r:" + r.key;
        it.name = r.name;
        it.icon = r.icon;
        it.value = r.sub.empty() ? r.value : r.value + "  ·  " + r.sub;
        it.accent = r.dim ? C_TEXT3 : lv_color_hex(r.accent);
        it.ract = r.act != 0;
        it.on = r.lit;
        it.lit_rgb = r.lit_color;
        if (r.vset || r.vlim >= 0) it.ctl = r.key;   // MultiPlus/Ladegeraet: Bedienfenster (lange druecken)
      },
      &s_items);
  return;
#endif
#if WITH_CERBO
  for (auto &v : cerbo_list()) {
#if SAT_VICTRON_ONLY
    if (!vic_has_value(v)) continue;   // keine leeren Kacheln
#endif
    s_items.push_back(vic_item(v, "c:" + v.key));
  }
#endif
#if !SAT_VICTRON_ONLY
  // Eigene Anschluesse (Tanks, Temperatur, Analog, Eingaenge, Schalter) und Hymer-Punkte
  for (auto &p : sat_io_points()) {
    Item it;
    it.key = "p:" + p.id;
    it.name = p.name;
    it.icon = point_icon(p);
    it.accent = p.ok ? C_BLUE : C_TEXT3;
    if (p.kind == SP_SWITCH) {
      it.sw = p.idx;
      it.on = p.on;
      it.value = p.on ? "An" : "Aus";
      it.accent = C_AMBER;
    } else if (p.kind == SP_INPUT) {
      it.value = p.cls == SC_BINARY ? (p.on ? "offen" : "zu") : (p.on ? "An" : "Aus");
      it.accent = p.on ? C_AMBER : C_TEXT3;
    } else if (!p.ok) {
      it.value = p.status.empty() ? "–" : p.status;
    } else {
      int dec = p.kind == SP_TANK || p.cls == SC_LEVEL ? 0 : 1;
      it.value = num(p.value, dec, p.unit.c_str());
      if (p.kind == SP_TANK) it.accent = p.value < 15 ? C_RED : C_BLUE;
    }
    s_items.push_back(it);
  }
#endif
#if WITH_BMS
  // Akkus mit eigenem BMS (Super B …): erst mit Werten eine Kachel
  for (auto &b : bms_list()) {
    if (!b.has) continue;
    if (b.dev == BMS_DEV_TANK) {   // SuperSense Tankmodul: eine Kachel je angeschlossenem Tanksensor
      int used = 0;
      for (int k = 0; k < BMS_TANKS; k++) used += b.tank_pct[k] >= 0;
      for (int k = 0; k < BMS_TANKS; k++) {
        if (b.tank_pct[k] < 0) continue;
        Item it;
        it.key = "b:" + b.mac + "#" + std::to_string(k + 1);
        std::string base = b.name.empty() ? tr("Tank") : b.name;
        it.name = used > 1 ? base + " " + std::to_string(k + 1) : base;
        it.icon = ICON_BATH;
        it.value = std::to_string(b.tank_pct[k]) + " %";
        if (isfinite(b.tank_temp[k])) it.value += "  ·  " + num(b.tank_temp[k], 1, "°C");
        bool stale = b.age_s < 0 || b.age_s > 180;
        it.accent = stale ? C_TEXT3 : C_BLUE;
        s_items.push_back(it);
      }
      continue;
    }
    if (b.dev == BMS_DEV_HEATER || b.dev == BMS_DEV_FRIDGE) {   // Dieselheizung / Kuehlbox
      Item it;
      it.key = "b:" + b.mac;
      bool heat = b.dev == BMS_DEV_HEATER;
      it.name = b.name.empty() ? std::string(heat ? tr("Heizung") : tr("Kühlbox")) : b.name;
      it.icon = heat ? ICON_FLAME : ICON_KITCHEN;
      it.value = bms_value_text(b);
      int on = heat ? b.ht_on : b.fr_on;
      it.accent = !b.connected && b.age_s > 120 ? C_TEXT3 : heat && b.ht_err ? C_RED : on == 1 ? (heat ? C_AMBER : C_BLUE) : C_TEXT3;
      s_items.push_back(it);
      continue;
    }
    if (b.dev == BMS_DEV_CLIMATE) {   // Ruuvi: Temperatur  ·  Feuchte (CO2)
      Item it;
      it.key = "b:" + b.mac;
      it.name = b.name.empty() ? std::string("Ruuvi") : b.name;
      it.icon = ICON_THERMO;
      it.value = isfinite(b.temp) || b.note.empty() ? num(b.temp, 1, "°C") : b.note;
      if (isfinite(b.hum)) it.value += "  ·  " + num(b.hum, 0, "%");
      if (isfinite(b.temp) && !b.note.empty()) it.value += "  ·  " + b.note;
      if (b.co2 >= 0) it.value += "  ·  " + std::to_string(b.co2) + " ppm";
      it.accent = b.connected ? C_BLUE : C_TEXT3;
      s_items.push_back(it);
      continue;
    }
    if (b.dev == BMS_DEV_SOLAR || b.dev == BMS_DEV_DCDC || b.dev == BMS_DEV_INVERTER || b.dev == BMS_DEV_PSTATION) {
      VictronInfo v;
      victron_from_bms(b, v);
      s_items.push_back(vic_item(v, "b:" + b.mac));
      continue;
    }
    Item it;
    it.key = "b:" + b.mac;
    it.name = b.name.empty() ? tr("Akku") : b.name;
    it.icon = ICON_BATTERY;
    std::string second = num(b.volt, 1, "V");
    if (isfinite(b.volt) && isfinite(b.amp)) second += "  ·  " + num(b.volt * b.amp, 0, "W");
    it.value = std::to_string(b.soc) + " %  ·  " + second;
    bool stale = !b.connected || b.age_s > 120;
    it.accent = stale ? C_TEXT3 : (!b.ok || b.soc <= 10) ? C_RED : b.soc <= 50 ? C_AMBER : C_GREEN;
    s_items.push_back(it);
  }
#endif
  for (auto &t : truma_list()) {
#if SAT_VICTRON_ONLY
    if (!t.has_reading) continue;   // keine leeren Kacheln
#endif
    Item it;
    it.key = "t:" + t.mac;
    it.name = t.name.empty() ? tr("Gasflasche") : t.name;
    it.icon = ICON_GAS;
    if (t.has_reading) {
      it.value = std::to_string(t.percent) + " %  ·  " + num(t.kg, 1, "kg");
      it.accent = t.percent < 15 ? C_RED : C_GREEN;
    } else {
      it.value = t.status.empty() ? "–" : t.status;
      it.accent = C_TEXT3;
    }
    s_items.push_back(it);
  }
  for (auto &m : mopeka_list()) {
#if SAT_VICTRON_ONLY
    if (!m.has) continue;   // keine leeren Kacheln
#endif
    Item it;
    it.key = "m:" + m.mac;
    it.name = m.name.empty() ? "Mopeka" : m.name;
    it.icon = m.medium == MOP_PROPANE ? ICON_GAS : ICON_BATH;
    if (m.has) {
      it.value = std::to_string(m.percent) + " %";
      if (m.amount > 0) it.value += "  ·  " + num(m.amount, m.medium == MOP_PROPANE ? 1 : 0, m.unit);
      it.accent = m.percent < 15 ? C_RED : C_GREEN;
    } else {
      it.value = m.status.empty() ? "–" : m.status;
      it.accent = C_TEXT3;
    }
    s_items.push_back(it);
  }
  for (auto &v : victron_list()) {
#if SAT_VICTRON_ONLY
    if (!vic_has_value(v)) continue;   // eingerichtet, aber (noch) nichts empfangen: keine Kachel
#endif
    s_items.push_back(vic_item(v, "v:" + v.mac));
  }
}

static int page_cnt(int p);
// Anzeige-Liste: Seiten, Reihenfolge, Auswahl und Namen aus den Einstellungen. Kacheln mit fester Seite (Editor)
// stehen auf ihrer Seite, freie Plaetze bleiben leer (Item ohne Schluessel). Neue Geraete und Kacheln ohne feste
// Seite kommen dahinter (sichtbar).
static void collect() {
  ItemList raw;
  s_items.clear();
  s_items.shrink_to_fit();   // alte Liste zuerst freigeben (wenig RAM beim CYD)
  collect_raw(raw);
  std::vector<bool> used(raw.size(), false);
  std::vector<ItemList> fixed;
  ItemList autos;
  for (auto &c : s_cfg)
    for (size_t i = 0; i < raw.size(); i++)
      if (!used[i] && raw[i].key == c.k) {
        used[i] = true;
        if (c.on) {
          Item it = std::move(raw[i]);
          if (!c.n.empty()) it.name = c.n;
          if (c.pg < 16) {
            if (fixed.size() <= c.pg) fixed.resize(c.pg + 1);
            fixed[c.pg].push_back(std::move(it));
          } else {
            autos.push_back(std::move(it));
          }
        }
        break;
      }
  for (size_t i = 0; i < raw.size(); i++)
    if (!used[i]) autos.push_back(std::move(raw[i]));
  raw.clear();
  raw.shrink_to_fit();
  size_t total = autos.size();
  for (size_t p = 0; p < fixed.size(); p++) total += std::max<size_t>(page_cnt(p), fixed[p].size());
  (void)total;
  for (size_t p = 0; p < fixed.size(); p++) {
    size_t n = page_cnt(p);
    for (size_t j = 0; j < n; j++) {
      if (j < fixed[p].size()) s_items.push_back(std::move(fixed[p][j]));
      else s_items.emplace_back();   // Luecke
    }
    for (size_t j = n; j < fixed[p].size(); j++) autos.push_back(std::move(fixed[p][j]));   // passt nicht mehr auf die Seite
  }
  for (auto &a : autos) s_items.push_back(std::move(a));
  while (!s_items.empty() && s_items.back().key.empty()) s_items.pop_back();
}

static int norm_per(int v) { return v == 1 ? 1 : v == 2 ? 2 : 4; }
static int page_cnt(int p) { return p < (int)s_pl.size() ? s_pl[p] : s_per; }
static size_t page_start(int p) {
  size_t st = 0;
  for (int i = 0; i < p; i++) st += page_cnt(i);
  return st;
}
static int pages() {
  if (s_items.empty()) return 1;
  int p = 0;
  for (size_t st = 0; st < s_items.size(); p++) st += page_cnt(p);
  return p;
}

#if POP_ON
// ---------------------------------------------------------------- Bedienfenster MultiPlus/Ladegeraet (ueber den GX)
// Lange druecken auf eine Kachel mit Einstellmoeglichkeit (Netz, Ladegeraet, Wechselrichter) oeffnet es:
// Eingangsstrom-Begrenzung (- / +, ganze Ampere) und Schalter. Nur, was der GX als einstellbar meldet.
// Victron-Display: direkt am eigenen GX; Zusatzdisplay: ueber das Hauptpanel (dessen GX).
// Die LVGL-Objekte entstehen erst beim Oeffnen und werden beim Schliessen wieder geloescht.
struct PopV {
  std::string name;
  bool dcdc = false;      // Orion: "Ladestrom (max.)" 1–50 A statt Netzstrom-Begrenzung
  bool can_limit = false, can_mode = false;
  uint8_t mode_set = 0;   // 0 MultiPlus, 1 Ladegeraet
  int mode = -1;
  float limit = NAN;
};
static bool pop_get(const std::string &key, PopV &p) {
#if SAT_REMOTE
  RdTile t;
  if (!rd_tile(key, t) || (!t.vset && t.vlim < 0)) return false;
  p.name = t.name;
  p.can_mode = t.vset != 0;
  p.mode_set = t.vset == 2 ? 1 : 0;
  p.mode = t.vmode;
  p.can_limit = t.vlim >= 0;
  p.limit = t.vlim >= 0 ? t.vlim / 10.f : NAN;
  return true;
#else
  VictronInfo v;
  if (!victron_get(key, v) || (!v.can_limit && !v.can_mode)) return false;
  p.name = v.mode_set == 0 ? std::string("MultiPlus") : v.name;
  p.dcdc = v.kind == VIC_DCDC;
  p.can_limit = v.can_limit;
  p.can_mode = v.can_mode;
  p.mode_set = v.mode_set;
  p.mode = v.mode;
  p.limit = v.ac_limit;
  return true;
#endif
}
static bool pop_set_limit(const std::string &key, float a) {
#if SAT_REMOTE
  return rd_set(key, 'l', (int)lroundf(a * 10));
#else
  if (key.rfind(VICTRON_KEY_PREFIX, 0) == 0) return vctl_set_limit(key.substr(strlen(VICTRON_KEY_PREFIX)), a);
  return cerbo_set_limit(key, a);
#endif
}
static bool pop_set_mode(const std::string &key, int m) {
#if SAT_REMOTE
  return rd_set(key, 'm', m);
#else
  if (key.rfind(VICTRON_KEY_PREFIX, 0) == 0) return vctl_set_mode(key.substr(strlen(VICTRON_KEY_PREFIX)), m);
  return cerbo_set_mode(key, m);
#endif
}

static lv_obj_t *s_pop = nullptr, *s_pop_lim = nullptr;
static lv_obj_t *s_pop_mbtn[4];
static int s_pop_mval[4], s_pop_nm = 0;
static std::string s_pop_key;
static bool s_pop_dcdc = false;
static float s_pop_val = NAN;
static uint32_t s_pop_send_ms = 0, s_pop_edit_ms = 0;

static void pop_flush() {
  if (!s_pop_send_ms) return;
  s_pop_send_ms = 0;
  pop_set_limit(s_pop_key, s_pop_val);
}

static void pop_show_limit() {
  if (!s_pop_lim) return;
  if (isnan(s_pop_val)) {
    lv_label_set_text(s_pop_lim, "–");
    return;
  }
  lv_label_set_text(s_pop_lim, num(s_pop_val, 1, "A").c_str());
}

static void pop_show_mode(int mode) {
  for (int i = 0; i < s_pop_nm; i++) lv_obj_set_style_bg_color(s_pop_mbtn[i], s_pop_mval[i] == mode ? C_BLUE : C_FIELD, 0);
}

static void pop_close_cb(lv_event_t *) {
  pop_flush();
  if (s_pop) lv_obj_del_async(s_pop);   // aus dem eigenen Ereignis heraus nur verzoegert loeschen
  s_pop = s_pop_lim = nullptr;
  s_pop_nm = 0;
  s_next_ms = 0;
}

static void pop_step_cb(lv_event_t *e) {
  int d = (int)(intptr_t)lv_event_get_user_data(e);
  if (isnan(s_pop_val)) return;
  float v = roundf(s_pop_val) + d;
  float lo = s_pop_dcdc ? 1 : 0, hi = s_pop_dcdc ? 50 : 100;
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  s_pop_val = v;
  pop_show_limit();
  s_pop_edit_ms = millis();
  s_pop_send_ms = (millis() + 800) | 1;   // erst nach kurzer Pause senden (mehrfach tippen = ein Befehl)
}

static void pop_mode_cb(lv_event_t *e) {
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i < 0 || i >= s_pop_nm) return;
  if (pop_set_mode(s_pop_key, s_pop_mval[i])) pop_show_mode(s_pop_mval[i]);
  s_pop_edit_ms = millis();
}

static lv_obj_t *pop_label(lv_obj_t *parent, const char *txt, const lv_font_t *font, lv_color_t col) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, col, 0);
  lv_label_set_text(l, txt);
  return l;
}

static lv_obj_t *pop_btn(const char *txt, int x, int y, int w, int h, lv_event_cb_t cb, intptr_t ud) {
  lv_obj_t *b = lv_obj_create(s_pop);
  lv_obj_remove_style_all(b);
  lv_obj_set_style_radius(b, 10, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, C_FIELD, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, C_TILE_LINE, 0);
  lv_obj_set_style_border_color(b, C_BLUE, LV_STATE_PRESSED);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
  lv_obj_center(pop_label(b, txt, &font_ui_16, C_TEXT));
  return b;
}

static void pop_open(const std::string &key) {
  PopV v;
  if (s_pop || !pop_get(key, v)) return;
  s_pop_key = key;
  s_pop_dcdc = v.dcdc;
  s_pop_val = v.limit;
#if !SAT_REMOTE
  if (v.dcdc && key.rfind(VICTRON_KEY_PREFIX, 0) == 0) vctl_refresh(key.substr(strlen(VICTRON_KEY_PREFIX)));   // Stand lesen
#endif
  s_pop_send_ms = s_pop_edit_ms = 0;
  s_pop = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(s_pop);
  lv_obj_set_size(s_pop, W, H);
  lv_obj_set_style_bg_opa(s_pop, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_pop, C_BG, 0);
  lv_obj_clear_flag(s_pop, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_pop, LV_OBJ_FLAG_CLICKABLE);   // Kacheln darunter nicht antippbar
  lv_obj_t *ttl = pop_label(s_pop, v.name.c_str(), &font_ui_20, C_TEXT);
  lv_label_set_long_mode(ttl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(ttl, W - 112);
  lv_obj_set_pos(ttl, 12, 12);
  pop_btn("Fertig", W - 92, 6, 84, 34, pop_close_cb, 0);
  int y = 50;
  if (v.can_limit) {
    lv_obj_set_pos(pop_label(s_pop, v.dcdc ? "Ladestrom (max.)" : "Netzstrom-Begrenzung", &font_ui_16, C_TEXT2), 12, y);
    y += 22;
    pop_btn("–", 12, y, 72, 44, pop_step_cb, -1);
    pop_btn("+", W - 84, y, 72, 44, pop_step_cb, 1);
    s_pop_lim = pop_label(s_pop, "", &font_ui_20, C_TEXT);
    lv_obj_align(s_pop_lim, LV_ALIGN_TOP_MID, 0, y + 10);
    pop_show_limit();
    y += 54;
  }
  if (v.can_mode) {
    lv_obj_set_pos(pop_label(s_pop, "Schalter", &font_ui_16, C_TEXT2), 12, y);
    y += 22;
    static const char *MP_TXT[4] = {"Ein", "Aus", "Nur Laden", "Nur Wechselrichter"};
    static const int MP_VAL[4] = {3, 4, 1, 2};
    static const char *CH_TXT[2] = {"Ein", "Aus"};
    static const int CH_VAL[2] = {1, 4};
    s_pop_nm = v.mode_set == 0 ? 4 : 2;
    int bw = (W - 36) / 2, bh = 40;
    for (int i = 0; i < s_pop_nm; i++) {
      s_pop_mval[i] = v.mode_set == 0 ? MP_VAL[i] : CH_VAL[i];
      s_pop_mbtn[i] = pop_btn(v.mode_set == 0 ? MP_TXT[i] : CH_TXT[i], 12 + (i % 2) * (bw + 12), y + (i / 2) * (bh + 6),
                              bw, bh, pop_mode_cb, i);
    }
    pop_show_mode(v.mode);
  }
}

// einmal pro Sekunde: Werte vom GX uebernehmen (nicht direkt nach eigener Eingabe)
static void pop_sync() {
  if (!s_pop || (s_pop_edit_ms && millis() - s_pop_edit_ms < 3000)) return;
  PopV v;
  if (!pop_get(s_pop_key, v)) return;
  if (!s_pop_send_ms) {
    s_pop_val = v.limit;
    pop_show_limit();
  }
  pop_show_mode(v.mode);
}
#endif

// Info-Fenster: Name, WLAN und Adresse der Weboberflaeche gross und lesbar.
// Oeffnen: Platzhalter-Kachel antippen oder von oben nach unten wischen; schliessen: irgendwo antippen.
static lv_obj_t *s_info = nullptr;

static void info_close_cb(lv_event_t *) {
  if (!s_info) return;
  lv_obj_del_async(s_info);   // nichts im Speicher behalten
  s_info = nullptr;
}

static void info_line(int &y, const char *label, const std::string &val, const lv_font_t *font) {
  lv_obj_t *l = lv_label_create(s_info);
  lv_obj_set_style_text_font(l, &font_ui_16, 0);
  lv_obj_set_style_text_color(l, C_TEXT2, 0);
  lv_label_set_text(l, label);
  lv_obj_set_pos(l, 14, y);
  y += lv_font_get_line_height(&font_ui_16) + 2;
  l = lv_label_create(s_info);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, C_TEXT, 0);
  lv_obj_set_width(l, W - 28);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  (lv_label_set_text)(l, val.c_str());   // Klammer: keine Uebersetzung (Namen/Adressen)
  lv_obj_set_pos(l, 14, y);
  y += lv_font_get_line_height(font) + 10;
}

static void info_open() {
  if (s_info) return;
  s_info = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(s_info);
  lv_obj_set_size(s_info, W, H);
  lv_obj_set_style_bg_opa(s_info, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_info, C_BG, 0);
  lv_obj_clear_flag(s_info, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_info, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_info, info_close_cb, LV_EVENT_CLICKED, nullptr);
  int y = 10;
#if SAT_VICTRON_ONLY
  info_line(y, "Gerät", vd_title(), &font_ui_20);
#else
  info_line(y, "Gerät", std::string(g_sat.name.length() ? g_sat.name.c_str() : g_sat.id.c_str()), &font_ui_20);
#endif
  if (WiFi.isConnected()) {
    info_line(y, "WLAN", std::string(WiFi.SSID().c_str()), &font_ui_20);
    info_line(y, "Weboberfläche im Browser", std::string("http://") + WiFi.localIP().toString().c_str(), &font_ui_20);
  } else if (WiFi.getMode() & WIFI_AP) {
    info_line(y, "Einrichtungs-WLAN verbinden", std::string(WiFi.softAPSSID().c_str()), &font_ui_20);
    info_line(y, "Seite öffnet sich, sonst im Browser", std::string("http://") + WiFi.softAPIP().toString().c_str(), &font_ui_20);
  } else {
    info_line(y, "WLAN", tr("nicht verbunden"), &font_ui_20);
  }
  lv_obj_t *l = lv_label_create(s_info);
  lv_obj_set_style_text_font(l, &font_ui_16, 0);
  lv_obj_set_style_text_color(l, C_TEXT3, 0);
  lv_label_set_text(l, "Antippen zum Schließen");
  lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -8);
}

#if WITH_BMS
// Akku-Uebersicht: alle Werte eines Akkus, jede Sekunde aufgefrischt (refresh), antippen schliesst.
// Nur waehrend es offen ist im Speicher.
static lv_obj_t *s_bat = nullptr;
static std::string s_bat_key;
static const int BAT_N = 48;   // Zeilen aus bms_rows() plus je Zelle eine (scrollbar)
static lv_obj_t *s_bat_title = nullptr, *s_bat_lbl[BAT_N] = {nullptr}, *s_bat_val[BAT_N] = {nullptr};

static void bat_close_cb(lv_event_t *) {
  if (!s_bat) return;
  lv_obj_del_async(s_bat);
  s_bat = nullptr;
}

static void bat_sync() {
  if (!s_bat) return;
  BmsInfo b;
  if (!bms_get(s_bat_key, b)) {
    bat_close_cb(nullptr);
    return;
  }
  (lv_label_set_text)(s_bat_title, b.name.c_str());
  auto rows = bms_rows(b);
  for (auto &c : bms_cell_rows(b)) rows.push_back(c);
  for (int i = 0; i < BAT_N; i++) {
    if (i < (int)rows.size()) {
      (lv_label_set_text)(s_bat_lbl[i], tr(rows[i].first));
      (lv_label_set_text)(s_bat_val[i], tr_parts(rows[i].second).c_str());
      lv_obj_clear_flag(s_bat_lbl[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_bat_val[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(s_bat_lbl[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_bat_val[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
}

static void bat_open(const std::string &key) {
  if (s_bat) return;
  s_bat_key = key;
  s_bat = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(s_bat);
  lv_obj_set_size(s_bat, W, H);
  lv_obj_set_style_bg_opa(s_bat, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(s_bat, C_BG, 0);
  lv_obj_add_flag(s_bat, LV_OBJ_FLAG_SCROLLABLE);   // viele Zeilen (Zellen): wischen zum Blaettern
  lv_obj_set_scroll_dir(s_bat, LV_DIR_VER);
  lv_obj_set_style_pad_bottom(s_bat, 8, 0);
  lv_obj_add_flag(s_bat, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_bat, bat_close_cb, LV_EVENT_CLICKED, nullptr);
  s_bat_title = lv_label_create(s_bat);
  lv_obj_set_style_text_font(s_bat_title, &font_ui_20, 0);
  lv_obj_set_style_text_color(s_bat_title, C_TEXT, 0);
  lv_obj_set_width(s_bat_title, W - 20);
  lv_label_set_long_mode(s_bat_title, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(s_bat_title, 10, 4);
  int lh = lv_font_get_line_height(&font_ui_16) + 4;
  for (int i = 0; i < BAT_N; i++) {
    int y = 32 + i * lh;
    s_bat_lbl[i] = lv_label_create(s_bat);
    lv_obj_set_style_text_font(s_bat_lbl[i], &font_ui_16, 0);
    lv_obj_set_style_text_color(s_bat_lbl[i], C_TEXT2, 0);
    lv_obj_set_pos(s_bat_lbl[i], 10, y);
    s_bat_val[i] = lv_label_create(s_bat);
    lv_obj_set_style_text_font(s_bat_val[i], &font_ui_16, 0);
    lv_obj_set_style_text_color(s_bat_val[i], C_TEXT, 0);
    lv_obj_set_width(s_bat_val[i], W - 136);
    lv_label_set_long_mode(s_bat_val[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_bat_val[i], 126, y);   // rechts von der laengsten Bezeichnung ("Ladezustand")
  }
  bat_sync();
}
#endif

#if SAT_REMOTE
// ---------------------------------------------------------------- Dimm-Fenster (Zusatzdisplay, Licht vom Hauptpanel)
// Kachel lange druecken: Helligkeit; zur Seite wischen: Farbrad (wenn die Lampe Farbe kann) und Farbtemperatur.
// Lebt nur, solange es offen ist; schliesst sich nach 30 s ohne Beruehrung.
static lv_obj_t *s_dp = nullptr, *s_dp_page[3], *s_dp_pct = nullptr, *s_dp_dots = nullptr, *s_dp_ctl = nullptr;
static int s_dp_n = 0, s_dp_cur = 0;
static std::string s_dp_key;
static uint32_t s_dp_sent = 0;
static bool s_long_fired = false;   // Langdruck hat das Fenster geoeffnet: das folgende "Klick" nicht schalten

static void dp_close() {
  if (!s_dp) return;
  lv_obj_del_async(s_dp);
  s_dp = s_dp_pct = s_dp_dots = s_dp_ctl = nullptr;
}
static void dp_close_cb(lv_event_t *) { dp_close(); }

static void dp_show(int p) {
  s_dp_cur = (p + s_dp_n) % s_dp_n;
  for (int i = 0; i < s_dp_n; i++)
    if (i == s_dp_cur) lv_obj_clear_flag(s_dp_page[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_dp_page[i], LV_OBJ_FLAG_HIDDEN);
  std::string d;
  if (s_dp_n > 1)
    for (int i = 0; i < s_dp_n; i++) d += i == s_dp_cur ? " • " : " · ";
  lv_label_set_text(s_dp_dots, d.c_str());
}

static void dp_gesture_cb(lv_event_t *) {
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (d == LV_DIR_LEFT) dp_show(s_dp_cur + 1);
  else if (d == LV_DIR_RIGHT) dp_show(s_dp_cur - 1);
}

static void dp_bri_cb(lv_event_t *e) {
  lv_obj_t *sl = lv_event_get_target(e);
  int v = lv_slider_get_value(sl);
  char b[16];
  snprintf(b, sizeof(b), v ? "%d %%" : "Aus", v);
  lv_label_set_text(s_dp_pct, b);
  uint32_t now = millis();
  bool fin = lv_event_get_code(e) == LV_EVENT_RELEASED;
  if (fin || now - s_dp_sent > 400) {   // beim Ziehen hoechstens alle 0,4 s, beim Loslassen den Endwert
    s_dp_sent = now;
    rd_set(s_dp_key, 'b', v);
  }
}

static void dp_wheel_cb(lv_event_t *e) {
  lv_color_hsv_t hsv = lv_colorwheel_get_hsv(lv_event_get_target(e));
  rd_set(s_dp_key, 'h', (int)hsv.h * 1000 + std::min<int>(100, hsv.s));
}

static void dp_ct_cb(lv_event_t *e) {
  lv_obj_t *sl = lv_event_get_target(e);
  int mired = lv_slider_get_value(sl);
  char b[24];
  snprintf(b, sizeof(b), "%d K", mired > 0 ? 1000000 / mired : 0);
  lv_label_set_text(s_dp_ctl, b);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) rd_set(s_dp_key, 'c', mired);
}

static lv_obj_t *dp_label(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c) {
  lv_obj_t *l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, t);
  return l;
}

static lv_obj_t *dp_slider(lv_obj_t *p, int lo, int hi, int val, lv_event_cb_t cb) {
  lv_obj_t *sl = lv_slider_create(p);
  lv_obj_set_size(sl, W - 60, 26);
  lv_slider_set_range(sl, lo, hi);
  lv_slider_set_value(sl, val, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(sl, C_TILE_LINE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(sl, C_AMBER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(sl, lv_color_white(), LV_PART_KNOB);
  lv_obj_set_style_pad_all(sl, 6, LV_PART_KNOB);
  lv_obj_clear_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);   // Ziehen am Regler blaettert nicht
  lv_obj_add_event_cb(sl, cb, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(sl, cb, LV_EVENT_RELEASED, nullptr);
  return sl;
}

static void dp_open(const std::string &key) {
  RdTile t;
  if (s_dp || !rd_tile(key, t)) return;
  s_dp_key = key;
  s_dp = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_dp);
  lv_obj_set_size(s_dp, W, H);
  lv_obj_set_style_bg_color(s_dp, C_BG, 0);
  lv_obj_set_style_bg_opa(s_dp, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_dp, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_dp, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_dp, dp_gesture_cb, LV_EVENT_GESTURE, nullptr);
  lv_obj_t *nm = dp_label(s_dp, t.name.c_str(), &font_ui_20, C_TEXT);
  lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
  lv_obj_set_width(nm, W - 70);
  lv_obj_set_pos(nm, 12, 10);
  lv_obj_t *x = lv_btn_create(s_dp);
  lv_obj_set_size(x, 44, 36);
  lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -8, 4);
  lv_obj_set_style_bg_color(x, C_TILE, 0);
  lv_obj_add_event_cb(x, dp_close_cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_center(dp_label(x, "X", &font_ui_20, C_TEXT2));
  s_dp_n = 0;
  auto page = [&]() -> lv_obj_t * {
    lv_obj_t *p = lv_obj_create(s_dp);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, W, H - 66);
    lv_obj_set_pos(p, 0, 46);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    s_dp_page[s_dp_n++] = p;
    return p;
  };
  // Seite 1: Helligkeit
  lv_obj_t *p = page();
  int bri = t.bri >= 0 ? t.bri : (t.lit ? 100 : 0);
  char b[16];
  snprintf(b, sizeof(b), bri ? "%d %%" : "Aus", bri);
  s_dp_pct = dp_label(p, b, &font_ui_40, C_TEXT);
  lv_obj_align(s_dp_pct, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_align(dp_slider(p, 0, 100, bri, dp_bri_cb), LV_ALIGN_CENTER, 0, 30);
  if (t.color || t.ct >= 0) lv_obj_align(dp_label(p, "zur Seite wischen: Farbe", &font_ui_16, C_TEXT3), LV_ALIGN_BOTTOM_MID, 0, -2);
  // Seite 2: Farbrad
  if (t.color) {
    p = page();
    lv_obj_t *cw = lv_colorwheel_create(p, true);
    lv_obj_set_size(cw, 150, 150);
    lv_obj_center(cw);
    lv_obj_set_style_arc_width(cw, 22, 0);
    if (t.hue >= 0) lv_colorwheel_set_hsv(cw, lv_color_hsv_t{(uint16_t)t.hue, (uint8_t)(t.sat >= 0 ? t.sat : 100), 100});
    lv_obj_clear_flag(cw, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(cw, dp_wheel_cb, LV_EVENT_RELEASED, nullptr);
  }
  // Seite 3: Farbtemperatur (Weiss warm/kalt)
  if (t.ct >= 0 && t.ct_max > t.ct_min) {
    p = page();
    snprintf(b, sizeof(b), "%d K", t.ct > 0 ? 1000000 / t.ct : 0);
    s_dp_ctl = dp_label(p, b, &font_ui_40, C_TEXT);
    lv_obj_align(s_dp_ctl, LV_ALIGN_TOP_MID, 0, 14);
    // links kalt (wenig Mired), rechts warm
    lv_obj_t *sl = dp_slider(p, t.ct_min, t.ct_max, t.ct, dp_ct_cb);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xFFB15A), LV_PART_INDICATOR);
    lv_obj_align(sl, LV_ALIGN_CENTER, 0, 30);
    lv_obj_t *l1 = dp_label(p, "kalt", &font_ui_16, C_TEXT3), *l2 = dp_label(p, "warm", &font_ui_16, C_TEXT3);
    lv_obj_align(l1, LV_ALIGN_CENTER, -(W - 60) / 2 + 16, 62);
    lv_obj_align(l2, LV_ALIGN_CENTER, (W - 60) / 2 - 18, 62);
  }
  s_dp_dots = dp_label(s_dp, "", &font_ui_16, C_TEXT3);
  lv_obj_align(s_dp_dots, LV_ALIGN_BOTTOM_MID, 0, -1);
  dp_show(0);
}

static void dp_loop(uint32_t now) {
  if (s_dp && now - s_last_touch > 30000) dp_close();
}
#endif

// Kacheln mit Fenster (Akku-Uebersicht, MultiPlus/Ladegeraet bedienen) oeffnen erst nach ~2 s Druecken
// (LONG_PRESS_MS), damit Wischen und kurzes Antippen beim Blaettern nichts ausloesen.
#if SAT_REMOTE
static const uint16_t LONG_PRESS_MS = 1500;   // Dimm-Fenster (kurzes Halten 0,6 s schaltet)
#else
static const uint16_t LONG_PRESS_MS = 2000;
#endif
static void tile_long_cb(lv_event_t *e) {
  size_t k = page_start(s_page) + (int)(intptr_t)lv_event_get_user_data(e);
  if (k >= s_items.size()) return;
#if SAT_REMOTE
  if (s_items[k].ract) {   // Licht vom Hauptpanel: dimmbar oder farbig -> Dimm-Fenster
    RdTile t;
    std::string key = s_items[k].key.substr(2);
    if (rd_tile(key, t) && (t.bri >= 0 || t.color || t.ct >= 0)) {
      s_long_fired = true;
      dp_open(key);
    }
    return;
  }
#endif
#if WITH_BMS
  if (s_items[k].key.rfind("b:", 0) == 0) {
    bat_open(BMS_KEY_PREFIX + s_items[k].key.substr(2));
    return;
  }
#endif
#if POP_ON
  if (!s_items[k].ctl.empty()) {
#if SAT_REMOTE
    s_long_fired = true;
#endif
    pop_open(s_items[k].ctl);
  }
#endif
}

static void render();
#if SAT_REMOTE
static void refresh_visible();
#endif
// Schalter erst nach etwas laengerem Druck und ohne Wischen: beim Blaettern nichts versehentlich schalten
static const uint16_t SWITCH_HOLD_MS = 400, SWITCH_MOVE_PX = 35;   // resistiver Touch springt beim Loslassen
static uint32_t s_press_ms = 0, s_gesture_ms = 0;
static lv_point_t s_press_pt;
static void tile_press_cb(lv_event_t *) {
#if SAT_REMOTE
  s_long_fired = false;
#endif
  s_press_ms = millis();
  lv_indev_get_point(lv_indev_get_act(), &s_press_pt);
}
static bool switch_hold_ok() {
  lv_point_t p;
  lv_indev_get_point(lv_indev_get_act(), &p);
  uint32_t now = millis();
  bool ok = s_press_ms && now - s_press_ms >= SWITCH_HOLD_MS && now - s_gesture_ms > 800 &&
            abs(p.x - s_press_pt.x) <= SWITCH_MOVE_PX && abs(p.y - s_press_pt.y) <= SWITCH_MOVE_PX;
  return ok;
}

static void tile_cb(lv_event_t *e) {
  int slot = (int)(intptr_t)lv_event_get_user_data(e);
  size_t k = page_start(s_page) + slot;
#if SAT_REMOTE
  if (s_long_fired) {   // Loslassen nach dem Oeffnen des Dimm-Fensters: nicht schalten
    s_long_fired = false;
    return;
  }
#endif
  if (s_items.empty()) {   // Platzhalter-Kachel: Adresse gross zeigen
    info_open();
    return;
  }
#if SAT_REMOTE
  if (k < s_items.size() && s_items[k].ract) {   // Zusatzdisplay: ueber das Hauptpanel schalten
    if (switch_hold_ok() && rd_action(s_items[k].key.substr(2))) refresh_visible();   // gleich umspringen
    return;
  }
#endif
  // Akku- und MultiPlus-Kacheln: nur lange druecken (tile_long_cb)
  if (k < s_items.size() && s_items[k].sw >= 0 && switch_hold_ok()) {
#if !SAT_VICTRON_ONLY
    sat_io_switch((uint8_t)s_items[k].sw, 2);   // umschalten
#endif
    s_next_ms = 0;                               // gleich auffrischen
  }
}

static void gesture_cb(lv_event_t *) {
#if POP_ON
  if (s_pop) return;   // Bedienfenster offen: nicht blaettern
#endif
  if (s_info) return;
#if WITH_BMS
  if (s_bat) return;
#endif
#if SAT_REMOTE
  if (s_dp) return;
#endif
  s_gesture_ms = millis();
  lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (d == LV_DIR_BOTTOM) {   // von oben nach unten wischen: Geraet/WLAN/Adresse zeigen
    info_open();
    return;
  }
  if (d == LV_DIR_LEFT) s_page = (s_page + 1) % pages();
  else if (d == LV_DIR_RIGHT) s_page = (s_page + pages() - 1) % pages();
  else return;
  s_next_ms = 0;
}

static void build() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  static bool gest = false;   // build() laeuft beim Zusatzdisplay mehrfach (Web-Modus): Ereignis nur einmal anmelden
  if (!gest) {
    gest = true;
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, nullptr);
  }

  s_title = lv_label_create(scr);
  lv_obj_set_style_text_font(s_title, &font_ui_16, 0);
  lv_obj_set_style_text_color(s_title, C_TEXT, 0);
  lv_obj_set_pos(s_title, GAP + 2, 7);
  s_net = lv_label_create(scr);
  lv_obj_set_style_text_font(s_net, &font_ui_16, 0);
  lv_obj_set_style_text_color(s_net, C_TEXT2, 0);
  lv_obj_align(s_net, LV_ALIGN_TOP_RIGHT, -GAP - 2, 7);
#if SAT_VICTRON_ONLY
  lv_obj_add_flag(s_title, LV_OBJ_FLAG_HIDDEN);   // keine Kopfzeile, Adresse steht notfalls in der Platzhalter-Kachel
  lv_obj_add_flag(s_net, LV_OBJ_FLAG_HIDDEN);
#endif

  for (int i = 0; i < MAX_TILES; i++) {
    Tile &t = s_tiles[i];
    t.box = lv_obj_create(scr);
    lv_obj_remove_style_all(t.box);
    lv_obj_set_style_radius(t.box, 14, 0);
    lv_obj_set_style_bg_opa(t.box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(t.box, C_TILE, 0);
    lv_obj_set_style_border_width(t.box, 1, 0);
    lv_obj_set_style_border_color(t.box, C_TILE_LINE, 0);
    lv_obj_set_style_bg_color(t.box, C_FIELD, LV_STATE_PRESSED);
    lv_obj_clear_flag(t.box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(t.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(t.box, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(t.box, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(t.box, tile_press_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(t.box, tile_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

    t.icon = lv_label_create(t.box);
    lv_obj_set_style_text_font(t.icon, &font_icons_24, 0);
    lv_obj_set_pos(t.icon, 10, 8);
    t.name = lv_label_create(t.box);
    lv_obj_set_style_text_font(t.name, &font_ui_16, 0);
    lv_label_set_long_mode(t.name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(t.name, 42, 11);
    t.value = lv_label_create(t.box);
    lv_obj_set_style_text_font(t.value, &font_ui_20, 0);
    lv_label_set_long_mode(t.value, LV_LABEL_LONG_DOT);
    t.sub = lv_label_create(t.box);
    lv_obj_set_style_text_font(t.sub, &font_ui_16, 0);
    lv_label_set_long_mode(t.sub, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(t.sub, LV_OBJ_FLAG_HIDDEN);
  }
  s_dots = lv_label_create(scr);
  lv_obj_set_style_text_font(s_dots, &font_ui_16, 0);
  lv_obj_set_style_text_color(s_dots, C_TEXT3, 0);
  lv_obj_align(s_dots, LV_ALIGN_BOTTOM_MID, 0, -1);
}

// Hauptwert unten; mit zweitem Wert eine Zeile hoeher (zweiter Wert darunter)
static void place_value(Tile &t) {
  bool two = !lv_obj_has_flag(t.sub, LV_OBJ_FLAG_HIDDEN);
  const lv_font_t *sf = lv_obj_get_style_text_font(t.sub, 0);
  lv_obj_align(t.value, LV_ALIGN_BOTTOM_LEFT, 10, two ? -8 - lv_font_get_line_height(sf) : -10);
}

// Groesse und Lage der Kacheln fuer die aktuelle Seite: 4 (2x2), 2 grosse (volle Breite) oder 1 (ganzer Bildschirm).
// Groessere Kacheln bekommen groessere Schrift, wenn die Firmware sie mitbringt (SAT_BIG_FONT, Flash).
static void relayout() {
  if (!s_built) return;
  int n = page_cnt(s_page);
  s_lay = n;
  int cols = n == 4 ? 2 : 1, rows = n == 1 ? 1 : 2;
  int tw = (W - GAP * (cols + 1)) / cols, th = (H - TOP - DOTS_H - GAP * rows) / rows;
#if SAT_BIG_FONT
  const lv_font_t *fv = n == 1 ? &font_ui_40 : n == 2 ? &font_ui_26 : &font_ui_20;
  const lv_font_t *fs = n == 1 ? &font_ui_20 : &font_ui_16;
#else
  const lv_font_t *fv = &font_ui_20, *fs = &font_ui_16;
#endif
  for (int i = 0; i < MAX_TILES; i++) {
    Tile &t = s_tiles[i];
    int col = i % cols, row = (i / cols) % rows;
    lv_obj_set_size(t.box, tw, th);
    lv_obj_set_pos(t.box, GAP + col * (tw + GAP), TOP + row * (th + GAP));
    lv_obj_set_style_text_font(t.value, fv, 0);
    lv_obj_set_style_text_font(t.sub, fs, 0);
    lv_obj_set_style_text_font(t.name, n == 1 ? fs : &font_ui_16, 0);
    // feste Hoehe = eine Zeile: zu lange Texte enden mit "…" statt umzubrechen
    lv_obj_set_size(t.name, tw - 50, lv_font_get_line_height(lv_obj_get_style_text_font(t.name, 0)));
    lv_obj_set_size(t.value, tw - 20, lv_font_get_line_height(fv));
    lv_obj_set_size(t.sub, tw - 20, lv_font_get_line_height(fs));
    lv_obj_align(t.sub, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    place_value(t);
  }
}

static void set_text(lv_obj_t *l, const std::string &s) {
  // nur bei Aenderung neu zeichnen; Klammer um den Namen umgeht die Uebersetzung (Geraetenamen)
  if (strcmp(lv_label_get_text(l), s.c_str())) (lv_label_set_text)(l, s.c_str());
}

// "Wert1  ·  Wert2  ·  …": erster Wert gross, der Rest in der Zeile darunter (nebeneinander war nur der erste lesbar)
static void set_value(Tile &t, const std::string &s0) {
  static const char SEP[] = "  ·  ";
  const std::string s = tr_parts(s0);   // feste Teile ("Absorption", "Ausgang an") uebersetzen, Zahlen bleiben
  size_t p = s.find(SEP);
  std::string a = p == std::string::npos ? s : s.substr(0, p);
  std::string b = p == std::string::npos ? std::string() : s.substr(p + sizeof(SEP) - 1);
  set_text(t.value, a);
  bool had = !lv_obj_has_flag(t.sub, LV_OBJ_FLAG_HIDDEN);
  if (b.empty()) lv_obj_add_flag(t.sub, LV_OBJ_FLAG_HIDDEN);
  else { set_text(t.sub, b); lv_obj_clear_flag(t.sub, LV_OBJ_FLAG_HIDDEN); }
  if (had != !b.empty()) place_value(t);
}

static bool s_oom = false;   // zu wenig Speicher fuer alle Kacheln (CYD ohne PSRAM)
static void refresh() {
  try {
    collect();
    s_oom = false;
  } catch (const std::bad_alloc &) {   // nicht abstuerzen: Hinweis statt Kacheln
    s_items.clear();
    s_items.shrink_to_fit();
    if (!s_oom)
      log_w("Zu wenig Speicher fuer alle Kacheln: frei %u, groesster Block %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    s_oom = true;
  }
  render();
}

#if SAT_REMOTE
// Neue Werte vom Hauptpanel: nur die Kacheln der sichtbaren Seite nachtragen (statt die ganze Liste neu zu bauen)
static void refresh_visible() {
  size_t st = page_start(s_page), n = page_cnt(s_page);
  for (size_t k = st; k < st + n && k < s_items.size(); k++) {
    Item &it = s_items[k];
    if (it.key.rfind("r:", 0) != 0) continue;
    RdTile r;
    if (!rd_tile(it.key.substr(2), r)) continue;
    it.value = r.sub.empty() ? r.value : r.value + "  ·  " + r.sub;
    it.accent = r.dim ? C_TEXT3 : lv_color_hex(r.accent);
    it.on = r.lit;
    it.lit_rgb = r.lit_color;
  }
  render();
}
#endif

static void render() {
  if (!s_built) return;
  if (s_page >= pages()) s_page = pages() - 1;
  if (s_lay != page_cnt(s_page)) relayout();   // jede Seite kann 1, 2 oder 4 Kacheln haben
#if SAT_VICTRON_ONLY
  set_text(s_title, vd_title());
#else
  set_text(s_title, std::string(g_sat.name.length() ? g_sat.name.c_str() : g_sat.id.c_str()));
#endif
  std::string net = WiFi.isConnected() ? std::string(WiFi.localIP().toString().c_str())
                    : (WiFi.getMode() & WIFI_AP) ? std::string(tr("Einrichtung ")) + WiFi.softAPIP().toString().c_str()
                                                 : std::string(tr("kein WLAN"));
  set_text(s_net, net);
  for (int i = 0; i < MAX_TILES; i++) {
    Tile &t = s_tiles[i];
    size_t k = page_start(s_page) + i;
    if (i >= page_cnt(s_page)) {   // Seite mit 1 oder 2 grossen Kacheln: die uebrigen verstecken
      lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    if (k >= s_items.size()) {
      if (s_items.empty() && i == 0) {
        lv_obj_clear_flag(t.box, LV_OBJ_FLAG_HIDDEN);
        set_text(t.icon, ICON_INFO);
#if SAT_REMOTE
        set_text(t.name, tr(s_oom ? "Zu viele Kacheln" : rd_panel_seen() ? "Keine Kacheln" : "Suche Hauptpanel"));
        set_value(t, s_oom ? "Speicher reicht nicht  ·  weniger auswählen" : rd_panel_seen() ? "im Editor des Hauptpanels wählen" : "Antippen  ·  für Adresse");
#elif SAT_VICTRON_ONLY
        set_text(t.name, tr("Einrichten"));
        set_value(t, "Antippen  ·  für Adresse");
#else
        set_text(t.name, tr("Noch nichts da"));
        set_value(t, "Antippen  ·  für Adresse");
#endif
        lv_obj_set_style_text_color(t.icon, C_TEXT3, 0);
        lv_obj_set_style_text_color(t.name, C_TEXT2, 0);
        lv_obj_set_style_text_color(t.value, C_TEXT2, 0);
      } else {
        lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN);
      }
      continue;
    }
    const Item &it = s_items[k];
    if (it.key.empty()) {   // freier Platz auf einer Seite aus dem Editor
      lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(t.box, LV_OBJ_FLAG_HIDDEN);
    set_text(t.icon, it.icon);
    set_text(t.name, it.name);
    set_value(t, it.value);
    // eingeschalteter Schalter: Kachel in Bernstein wie die Lichter am Panel (Zusatzdisplay: Lichtfarbe vom Panel)
    bool lit = (it.sw >= 0 || it.ract) && it.on;
    lv_color_t lc = it.lit_rgb ? lv_color_hex(it.lit_rgb) : C_AMBER;
    lv_color_t ink = lv_color_brightness(lc) > 150 ? C_INK : lv_color_white();
    lv_obj_set_style_bg_color(t.box, lit ? lc : C_TILE, 0);
    lv_obj_set_style_border_color(t.box, lit ? (it.lit_rgb ? lv_color_darken(lc, 40) : C_AMBER_D) : C_TILE_LINE, 0);
    lv_obj_set_style_text_color(t.icon, lit ? ink : it.accent, 0);
    lv_obj_set_style_text_color(t.name, lit ? ink : C_TEXT2, 0);
    lv_obj_set_style_text_color(t.value, lit ? ink : C_TEXT, 0);
    lv_obj_set_style_text_color(t.sub, lit ? ink : C_TEXT2, 0);
  }
  std::string dots;
  if (pages() > 1)
    for (int p = 0; p < pages(); p++) dots += p == s_page ? " • " : " · ";   // Zeichen aus der Panel-Schrift
  set_text(s_dots, dots);
#if POP_ON
  pop_sync();
#endif
#if WITH_BMS
  bat_sync();
#endif
}
static void load_cfg() {
  Preferences p;
  p.begin("satdisp", true);
  String js = p.getString("cfg", "");
  p.end();
  if (js.isEmpty()) return;
  JsonDocument d;
  if (deserializeJson(d, js)) return;
  s_per = norm_per(d["p"] | 4);
  s_pl.clear();
  for (int v : d["pl"].as<JsonArray>()) s_pl.push_back(norm_per(v));
  s_bright = constrain((int)(d["b"] | 200), 20, 255);
  s_dim_min = d["d"] | 2;
  s_saver = d["sv"] | SAT_CLOCK;
#if SAT_REMOTE
  s_led = d["ld"] | 1;
  s_led_key = d["lk"] | "";
#endif
  s_auto = d["a"] | true;
  s_night = constrain((int)(d["nb"] | 40), 5, 255);
  s_ldr_thr = constrain((int)(d["lt"] | 300), 1, 4095);
  s_cfg.clear();
  for (JsonObject t : d["t"].as<JsonArray>())
    s_cfg.push_back({t["k"] | "", t["n"] | "", (bool)(t["o"] | 1), (uint8_t)(t["g"] | 255)});
}

static void save_cfg() {
  JsonDocument d;
  d["p"] = s_per;
  if (!s_pl.empty()) {
    JsonArray pl = d["pl"].to<JsonArray>();
    for (uint8_t v : s_pl) pl.add(v);
  }
  d["b"] = s_bright;
  d["d"] = s_dim_min;
  d["sv"] = s_saver;
#if SAT_REMOTE
  d["ld"] = s_led;
  if (!s_led_key.empty()) d["lk"] = s_led_key;
#endif
  d["a"] = s_auto;
  d["nb"] = s_night;
  d["lt"] = s_ldr_thr;
  JsonArray a = d["t"].to<JsonArray>();
  for (auto &c : s_cfg) {
    JsonObject t = a.add<JsonObject>();
    t["k"] = c.k;
    if (!c.n.empty()) t["n"] = c.n;
    t["o"] = c.on ? 1 : 0;
    if (c.pg < 16) t["g"] = c.pg;
  }
  String js;
  serializeJson(d, js);
  Preferences p;
  p.begin("satdisp", false);
  p.putString("cfg", js);
  p.end();
}
}  // namespace

bool sat_display_present() { return true; }

void sat_display_json(JsonObject o) {
  o["per"] = s_per;
  JsonArray pl = o["pl"].to<JsonArray>();
  for (uint8_t v : s_pl) pl.add(v);
  o["bright"] = s_bright;
  o["dim"] = s_dim_min;
  o["clock"] = SAT_CLOCK;   // Firmware kann die Uhr als Bildschirmschoner
  o["saver"] = s_saver;
#if SAT_REMOTE
  o["led"] = s_led;   // 0 aus, 1 automatisch, 2 Kachel led_key
  o["led_key"] = s_led_key;
  o["led_pct"] = s_led_pct;
  JsonArray lt = o["led_tiles"].to<JsonArray>();   // Kacheln mit Pegel (Batterie, Gas, Tank) – ohne Kopie
  rd_each(
      [](const RdTile &r, void *ctx) {
        if (r.fill < 0) return;
        JsonObject x = (*(JsonArray *)ctx).add<JsonObject>();
        x["k"] = r.key;
        x["n"] = r.name;
      },
      &lt);
#endif
  o["auto"] = s_auto;
  o["night"] = s_night;
  o["ldr_thr"] = s_ldr_thr;
  // alle bekannten Kacheln: erst in der eingestellten Reihenfolge, dann neue
  ItemList raw;
  collect_raw(raw);
  std::vector<bool> used(raw.size(), false);
  JsonArray a = o["tiles"].to<JsonArray>();
  auto add = [&](const Item &it, const TileCfg *c) {
    JsonObject t = a.add<JsonObject>();
    t["k"] = it.key;
    t["name"] = it.name;
    t["value"] = it.value;
    t["n"] = c ? c->n : "";
    t["on"] = c ? c->on : true;
    if (c && c->pg < 16) t["pg"] = c->pg;
  };
  for (auto &c : s_cfg)
    for (size_t i = 0; i < raw.size(); i++)
      if (!used[i] && raw[i].key == c.k) {
        used[i] = true;
        add(raw[i], &c);
        break;
      }
  for (size_t i = 0; i < raw.size(); i++)
    if (!used[i]) add(raw[i], nullptr);
}

// ---------------------------------------------------------------- Touch kalibrieren
// LovyanGFX zeichnet nacheinander Pfeile in die vier Ecken, die man antippt. Ergebnis: 8 Werte (16 Bytes) in NVS
// "satdisp"/"tcal", beim Start per setTouchCalibrate gesetzt. Kein Dauer-RAM (laeuft blockierend im loop()).
// Ausloesen: Weboberflaeche (Display -> "Touch kalibrieren") oder beim Einschalten 2 s auf den Bildschirm druecken.
static volatile bool s_cal_req = false;
static bool s_reset_req = false;
bool sat_display_reset_requested() { return s_reset_req; }

static void touch_cal_load() {
  uint16_t cal[8];
  Preferences p;
  p.begin("satdisp", true);
  size_t n = p.getBytesLength("tcal") == sizeof(cal) ? p.getBytes("tcal", cal, sizeof(cal)) : 0;
  p.end();
  if (n == sizeof(cal)) s_lcd.setTouchCalibrate(cal);
}

static void touch_cal_run() {
  uint16_t cal[8];
  s_lcd.setBrightness(cur_bright());
  s_lcd.fillScreen(TFT_BLACK);
  s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  s_lcd.setTextDatum(textdatum_t::middle_center);
  s_lcd.setTextSize(2);
  s_lcd.drawString("Touch kalibrieren", W / 2, H / 2 - 14);
  s_lcd.setTextSize(1);
  s_lcd.drawString("Pfeilspitzen nacheinander antippen", W / 2, H / 2 + 12);
  s_lcd.calibrateTouch(cal, TFT_WHITE, TFT_BLACK, 16);
  Preferences p;
  p.begin("satdisp", false);
  p.putBytes("tcal", cal, sizeof(cal));
  p.end();
  s_lcd.setTouchCalibrate(cal);
  s_lcd.fillScreen(TFT_BLACK);
  s_last_touch = millis();
  s_dim = false;
  lv_obj_invalidate(lv_scr_act());   // Kacheln neu zeichnen
}

bool sat_display_apply(JsonObjectConst c) {
  if (c["calibrate"] | false) {   // nur Kalibrierung anstossen, sonst nichts aendern
    s_cal_req = true;
    return true;
  }
  if (c["per"].is<int>()) s_per = norm_per(c["per"].as<int>());
  if (c["pl"].is<JsonArrayConst>()) {   // Kacheln je Seite (Editor), hoechstens 16 Seiten
    s_pl.clear();
    for (int v : c["pl"].as<JsonArrayConst>())
      if (s_pl.size() < 16) s_pl.push_back(norm_per(v));
  }
  if (c["bright"].is<int>()) s_bright = constrain(c["bright"].as<int>(), 20, 255);
  if (c["dim"].is<int>()) s_dim_min = constrain(c["dim"].as<int>(), 0, 600);
  if (c["saver"].is<int>()) s_saver = c["saver"].as<int>() ? SAT_CLOCK : 0;
#if SAT_REMOTE
  if (c["led"].is<int>()) s_led = constrain(c["led"].as<int>(), 0, 2);
  if (c["led_key"].is<const char *>()) s_led_key = c["led_key"].as<const char *>();
  s_led_rgb = 0xFFFFFFFF;   // gleich neu auswerten
  s_led_ms = 0;
#endif
  if (c["auto"].is<bool>()) s_auto = c["auto"];
  if (c["night"].is<int>()) s_night = constrain(c["night"].as<int>(), 5, 255);
  if (c["ldr_thr"].is<int>()) s_ldr_thr = constrain(c["ldr_thr"].as<int>(), 1, 4095);
  if (c["tiles"].is<JsonArrayConst>()) {
    s_cfg.clear();
    for (JsonObjectConst t : c["tiles"].as<JsonArrayConst>()) {
      const char *k = t["k"] | "";
      if (!*k) continue;
      int pg = t["pg"] | 255;
      s_cfg.push_back({k, t["n"] | "", t["on"] | true, (uint8_t)(pg >= 0 && pg < 16 ? pg : 255)});
    }
  }
  save_cfg();
  s_page = 0;
  relayout();
  s_dim = false;
  s_lcd.setBrightness(cur_bright());
  s_last_touch = millis();
  s_next_ms = 0;   // gleich neu zeichnen
  return true;
}

static uint32_t s_splash_until = 0;   // != 0: Startbild steht noch, LVGL zeichnet noch nicht

static void ldr_loop(uint32_t now) {
  if (now - s_ldr_ms < 500) return;
  s_ldr_ms = now;
  int raw = analogRead(SAT_LDR_PIN);
  s_ldr = s_ldr < 0 ? raw : s_ldr * 0.8f + raw * 0.2f;
  bool want = s_night_on ? s_ldr > s_ldr_thr * 0.75f : s_ldr > s_ldr_thr;
  if (want == s_night_on) {
    s_ldr_chg_ms = 0;
    return;
  }
  if (!s_ldr_chg_ms) s_ldr_chg_ms = now | 1;
  if (now - s_ldr_chg_ms < 3000) return;
  s_ldr_chg_ms = 0;
  s_night_on = want;
  if (s_auto && !s_dim) s_lcd.setBrightness(cur_bright());
}

void sat_display_light(JsonObject o) {
  o["ldr"] = s_ldr < 0 ? -1 : (int)lroundf(s_ldr);
  o["night"] = s_night_on;
  o["auto"] = s_auto;
}

bool sat_display_night() { return s_auto && s_night_on; }

void sat_display_begin() {
  for (int p : {4, 16, 17}) {   // RGB-LED aus (Low = an)
    pinMode(p, OUTPUT);
    digitalWrite(p, HIGH);
  }
  s_lcd.init();
  s_lcd.setRotation(SAT_DISPLAY_ROTATION);
  touch_cal_load();
  load_cfg();
  analogRead(SAT_LDR_PIN);   // richtet den Pin als Analogeingang ein – erst danach laesst sich der Bereich setzen
  analogSetPinAttenuation(SAT_LDR_PIN, ADC_0db);   // empfindlichster Bereich (der LDR liefert nur kleine Spannungen)
  s_ldr = analogRead(SAT_LDR_PIN);
  s_night_on = s_ldr > s_ldr_thr;
  s_lcd.setBrightness(cur_bright());
#if SAT_SPLASH
  if (SPLASH_CYD_JPG_LEN) {   // direkt aufs Display, der Decoder braucht nur kurz ~4 KB
    s_lcd.drawJpg(SPLASH_CYD_JPG, SPLASH_CYD_JPG_LEN, 0, 0);
    s_splash_until = (millis() + 5000) | 1;   // 5 s, danach erst den Hauptschirm aufbauen
  }
#endif

  {   // beim Einschalten gedrueckt halten: 2 s = Touch kalibrieren, 10 s = Passwort zuruecksetzen
    lgfx::touch_point_t tp;
    uint32_t t0 = millis();
    bool shown = false;
    const uint32_t HOLD_MAX = 10000;
    while (s_lcd.getTouch(&tp) > 0 && millis() - t0 < HOLD_MAX) {
      if (!shown && millis() - t0 >= 2000 && HOLD_MAX > 2000) {
        shown = true;
        s_lcd.fillScreen(TFT_BLACK);
        s_lcd.setTextColor(TFT_WHITE, TFT_BLACK);
        s_lcd.setTextDatum(textdatum_t::middle_center);
        s_lcd.setTextSize(1);
        s_lcd.drawString("Jetzt loslassen: Touch kalibrieren", W / 2, H / 2 - 10);
        s_lcd.drawString("Weiter halten (10 s): Passwort zuruecksetzen", W / 2, H / 2 + 10);
      }
      delay(20);
    }
    uint32_t held = millis() - t0;
    if (held >= HOLD_MAX && HOLD_MAX > 2000) {
      s_reset_req = true;
      s_lcd.fillScreen(TFT_BLACK);
      s_lcd.drawString("Passwort zurueckgesetzt (panelsat)", W / 2, H / 2);
      delay(1500);
    } else if (held >= 2000) {
      s_cal_req = true;
    }
  }

  lv_init();
  lv_color_t *buf = (lv_color_t *)heap_caps_malloc(W * BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  lv_disp_draw_buf_init(&s_buf, buf, nullptr, W * BUF_LINES);
  lv_disp_drv_init(&s_disp_drv);
  s_disp_drv.hor_res = W;
  s_disp_drv.ver_res = H;
  s_disp_drv.flush_cb = flush_cb;
  s_disp_drv.draw_buf = &s_buf;
  lv_disp_drv_register(&s_disp_drv);
  lv_indev_drv_init(&s_indev_drv);
  s_indev_drv.type = LV_INDEV_TYPE_POINTER;
  s_indev_drv.read_cb = touch_cb;
  s_indev_drv.long_press_time = LONG_PRESS_MS;
  lv_indev_drv_register(&s_indev_drv);

  s_last_touch = millis();
  if (!s_splash_until) {   // ohne Startbild gleich; sonst nach dem Startbild (sat_display_loop)
    build();
    s_built = true;
    relayout();
    refresh();
  }
}

#if SAT_REMOTE
// ---------------------------------------------------------------- RGB-LED (Rueckseite des CYD) beim Zusatzdisplay
// Ampel nach dem Pegel einer Kachel vom Hauptpanel (Batterie, Gas, Tank): bis 10 % rot, bis 50 % orange, sonst gruen,
// ohne Wert aus. 1 = automatisch: niedrigster Pegel aller Kacheln mit Pegel; 2 = eine bestimmte Kachel.
// Gemeinsame Anode -> Low = an. Nachts (Lichtsensor) auf ein Achtel gedimmt. Auswertung alle 2 s, keine Task.
static void led_write(uint32_t rgb) {
  static bool night = false;
  if (night != sat_display_night()) {   // Wechsel hell/dunkel: neu schreiben, auch bei gleicher Farbe
    night = !night;
    s_led_rgb = 0xFFFFFFFF;
  }
  if (rgb == s_led_rgb) return;
  if (!s_led_init) {
    ledcAttach(4, 5000, 8);
    ledcAttach(16, 5000, 8);
    ledcAttach(17, 5000, 8);
    s_led_init = true;
  }
  s_led_rgb = rgb;
  auto ch = [](uint32_t v) -> uint32_t { return sat_display_night() ? (v + 7) / 8 : v; };
  ledcWrite(4, 255 - ch((rgb >> 16) & 0xFF));
  ledcWrite(16, 255 - ch((rgb >> 8) & 0xFF));
  ledcWrite(17, 255 - ch(rgb & 0xFF));
}
static void led_loop(uint32_t now) {
  if (s_led_ms && now - s_led_ms < 2000) return;
  s_led_ms = now;
  int pct = -1;
  if (s_led)
    rd_each(
        [](const RdTile &r, void *ctx) {
          int &p = *(int *)ctx;
          if (r.fill < 0 || r.dim) return;
          if (s_led == 2 && r.key != s_led_key) return;
          if (p < 0 || r.fill < p) p = r.fill;
        },
        &pct);
  s_led_pct = pct;
  if (pct < 0) {
    if (s_led_init) led_write(0);
    return;
  }
  led_write(pct <= 10 ? 0xFF0000 : pct <= 50 ? 0xFF3C00 : 0x00A000);
}
#endif

#if SAT_CLOCK
// ---------------------------------------------------------------- Uhr als Bildschirmschoner
// Schwarzer Bildschirm mit grosser Uhr und Datum; jede Minute an einer etwas anderen Stelle. Lebt nur, solange er
// sichtbar ist (beim Wecken wieder geloescht).
static void clock_text() {
  static const char *DAYS[] = {"So", "Mo", "Di", "Mi", "Do", "Fr", "Sa"};
  time_t t = time(nullptr);
  struct tm tm;
  localtime_r(&t, &tm);
  char b[24];
  snprintf(b, sizeof(b), "%02d:%02d", tm.tm_hour, tm.tm_min);
  lv_label_set_text(s_clk_time, b);
  snprintf(b, sizeof(b), "%s, %02d.%02d.%04d", DAYS[tm.tm_wday % 7], tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);
  lv_label_set_text(s_clk_date, b);
  // leicht versetzen (alle 5 Minuten eine andere Stelle)
  int step = (tm.tm_min / 5) % 4;
  static const int8_t DX[] = {0, 12, -10, 6}, DY[] = {-8, 6, 10, -4};
  lv_obj_align(s_clk_time, LV_ALIGN_CENTER, DX[step], DY[step] - 14);
  lv_obj_align_to(s_clk_date, s_clk_time, LV_ALIGN_OUT_BOTTOM_MID, 0, -6);
}

static void clock_open() {
  if (s_clk) return;
  s_clk = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(s_clk);
  lv_obj_set_size(s_clk, W, H);
  lv_obj_set_style_bg_color(s_clk, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_clk, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_clk, LV_OBJ_FLAG_SCROLLABLE);
  s_clk_time = lv_label_create(s_clk);
  lv_obj_set_style_text_font(s_clk_time, &font_clock_104, 0);
  lv_obj_set_style_text_color(s_clk_time, lv_color_hex(0x8D97A2), 0);
  s_clk_date = lv_label_create(s_clk);
  lv_obj_set_style_text_font(s_clk_date, &font_ui_20, 0);
  lv_obj_set_style_text_color(s_clk_date, lv_color_hex(0x5A6470), 0);
  clock_text();
}

static void clock_tick(uint32_t now) {
  static uint32_t last = 0;
  if (now - last < 1000) return;
  last = now;
  clock_text();
}
#endif

// ---------------------------------------------------------------- Web-Modus (Zusatzdisplay)
// Solange die Weboberflaeche benutzt wird (Zugriff in den letzten 15 s), Kacheln entladen und nur einen blinkenden
// Punkt zeigen: das CYD hat kein PSRAM, so bleibt fuer Anmelden/Einstellen genug Speicher frei.
static uint32_t s_web_ms = 0;
void sat_display_web_touch() { s_web_ms = millis() | 1; }
#if SAT_REMOTE
static bool s_webmode = false;
static lv_obj_t *s_wdot = nullptr;
static void web_enter() {
  dp_close();
#if SAT_CLOCK
  clock_close();
#endif
  lv_obj_clean(lv_scr_act());   // alle Kacheln und Beschriftungen loeschen
  s_info = nullptr;
  memset(s_tiles, 0, sizeof(s_tiles));
  s_title = s_net = s_dots = nullptr;
  s_items.clear();
  s_items.shrink_to_fit();
  s_built = false;
  s_wdot = lv_obj_create(lv_scr_act());
  lv_obj_remove_style_all(s_wdot);
  lv_obj_set_size(s_wdot, 14, 14);
  lv_obj_set_style_radius(s_wdot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_wdot, C_BLUE, 0);
  lv_obj_set_style_bg_opa(s_wdot, LV_OPA_COVER, 0);
  lv_obj_center(s_wdot);
  s_webmode = true;
  log_i("Web-Modus: Kacheln entladen, frei %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
}
static void web_exit() {
  lv_obj_clean(lv_scr_act());
  s_wdot = nullptr;
  s_webmode = false;
  build();
  s_built = true;
  s_lay = 0;
  relayout();
  refresh();
}
static void web_tick(uint32_t now) {
  if (!s_wdot) return;
  static uint32_t last = 0;
  if (now - last < 500) return;
  last = now;
  if (lv_obj_has_flag(s_wdot, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(s_wdot, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(s_wdot, LV_OBJ_FLAG_HIDDEN);
}
#endif

void sat_display_loop() {
  if (s_cal_req) {
    s_cal_req = false;
    touch_cal_run();
    s_splash_until = 0;
  }
  if (s_splash_until) {   // Startbild: 3 s oder bis zur ersten Beruehrung
    lgfx::touch_point_t tp;
    if ((int32_t)(millis() - s_splash_until) < 0 && s_lcd.getTouch(&tp) == 0) return;
    s_splash_until = 0;
    s_last_touch = millis();
    if (!s_built) {   // Hauptschirm erst jetzt aufbauen
      build();
      s_built = true;
      relayout();
      refresh();
    }
    lv_obj_invalidate(lv_scr_act());   // alles neu zeichnen
  }
#if POP_ON
  if (s_pop_send_ms && (int32_t)(millis() - s_pop_send_ms) >= 0) pop_flush();
#endif
  lv_timer_handler();
  uint32_t now = millis();
  ldr_loop(now);
  if (!s_dim && s_dim_min && now - s_last_touch > s_dim_min * 60000UL) {
    s_dim = true;
#if SAT_CLOCK
    if (s_saver == 1 && time(nullptr) > 1600000000) clock_open();
#endif
    s_lcd.setBrightness(s_clk ? std::max<uint8_t>(15, cur_bright() / 2) : std::min<uint8_t>(DIMMED, cur_bright()));
  }
#if SAT_CLOCK
  if (s_clk) clock_tick(now);
#endif
#if SAT_REMOTE
  led_loop(now);
  dp_loop(now);
#endif
#if SAT_REMOTE
  if (s_web_ms && now - s_web_ms < 15000) {   // Weboberflaeche in Benutzung: Kacheln entladen
    if (!s_webmode && s_built) web_enter();
    web_tick(now);
    return;
  }
  if (s_webmode) {
    web_exit();
    return;
  }
  // Zusatzdisplay: neue Kachelliste -> alles neu; nur neue Werte -> sichtbare Kacheln, gebuendelt hoechstens alle 0,2 s
  static uint32_t s_rd_keys = 0, s_rd_seen = 0, s_rd_ms = 0, s_rd_full = 0;
  if (rd_keys_version() != s_rd_keys && now - s_rd_full >= 400) {   // beim Laden hoechstens alle 0,4 s neu aufbauen
    s_rd_full = now;
    s_rd_keys = rd_keys_version();
    s_rd_seen = rd_version();
    s_next_ms = now + 5000;
    refresh();
    return;
  }
  if (rd_version() != s_rd_seen && now - s_rd_ms >= 200) {
    s_rd_seen = rd_version();
    s_rd_ms = now;
    refresh_visible();
  }
  if ((int32_t)(now - s_next_ms) < 0) return;
  s_next_ms = now + 5000;   // ganze Liste nur alle 5 s neu (Namen, Seiten); Werte kommen oben
  refresh();
#else
  if ((int32_t)(now - s_next_ms) < 0) return;
  s_next_ms = now + 1000;   // Werte einmal pro Sekunde auffrischen
  refresh();
#endif
}

#else
void sat_display_begin() {}
void sat_display_loop() {}
void sat_display_web_touch() {}
void sat_display_light(JsonObject) {}
bool sat_display_night() { return false; }
bool sat_display_present() { return false; }
void sat_display_json(JsonObject) {}
bool sat_display_apply(JsonObjectConst) { return false; }
bool sat_display_reset_requested() { return false; }
#endif
