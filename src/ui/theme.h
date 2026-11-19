// Farben, Schriften und kleine Bausteine der Panel-Oberflaeche
#pragma once
#include <lvgl.h>
#include "../fonts/fonts.h"
#include "../fonts/icons.h"
#include "../i18n/i18n.h"

// Uebersetzung: jeder Label-Text laeuft durch tr() (Texte ohne Uebersetzung bleiben unveraendert).
// Fuer Namen, die nie uebersetzt werden duerfen (Geraetenamen), (lv_label_set_text)(obj, txt) schreiben –
// die Klammer um den Namen umgeht das Makro.
#define lv_label_set_text(obj, txt) lv_label_set_text((obj), tr(txt))

#define C_BG        lv_color_hex(0x0F1215)
#define C_BAR       lv_color_hex(0x0B0D10)
#define C_LINE      lv_color_hex(0x232A31)
#define C_TILE      lv_color_hex(0x181D22)
#define C_TILE_LINE lv_color_hex(0x272E36)
#define C_CARD      lv_color_hex(0x161A1F)
#define C_FIELD     lv_color_hex(0x20262D)
#define C_TEXT      lv_color_hex(0xE9EDF1)
#define C_TEXT2     lv_color_hex(0x8D97A2)
#define C_TEXT3     lv_color_hex(0x5E6873)
#define C_AMBER     lv_color_hex(0xF3A43B)
#define C_AMBER_D   lv_color_hex(0xDD7B1F)
#define C_BLUE      lv_color_hex(0x3C8DFF)
#define C_BLUE_D    lv_color_hex(0x2266D6)
#define C_RED       lv_color_hex(0xFF6A5C)
#define C_GREEN     lv_color_hex(0x5BD38A)
#define C_INK       lv_color_hex(0x1E1206)   // dunkle Schrift auf hellen Kacheln

#define F_SMALL  (&font_ui_16)
#define F_BODY   (&font_ui_20)
#define F_TITLE  (&font_ui_26)
#define F_BIG    (&font_ui_40)
#define F_ICON_S (&font_icons_24)
#define F_ICON_M (&font_icons_32)
#define F_ICON_L (&font_icons_44)

void theme_init();

// Bausteine
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);
lv_obj_t *ui_icon(lv_obj_t *parent, const char *icon, const lv_font_t *font, lv_color_t color);
// Schaltflaeche mit optionalem Icon; primary = Akzentfarbe
lv_obj_t *ui_button(lv_obj_t *parent, const char *icon, const char *text, bool primary, lv_event_cb_t cb, void *ud);
lv_obj_t *ui_icon_button(lv_obj_t *parent, const char *icon, lv_event_cb_t cb, void *ud, int size = 52);
// Einstellungszeile: Titel links, Wert rechts, optional Pfeil. Liefert das Wert-Label.
lv_obj_t *ui_row(lv_obj_t *parent, const char *title, const char *value, lv_event_cb_t cb, void *ud,
                 bool chevron = true, lv_obj_t **row_out = nullptr);
void ui_style_slider(lv_obj_t *s, lv_color_t color);
void ui_toast(const char *text);
void ui_plain(lv_obj_t *o);   // Standardstile entfernen, nicht scrollbar
