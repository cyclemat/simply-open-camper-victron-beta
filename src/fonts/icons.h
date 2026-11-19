// Automatisch erzeugt von tools/gen_fonts.py – Phosphor Icons (MIT)
#pragma once
#include <string.h>

#define ICON_BULB "\xEE\x80\x80"
#define ICON_FILAMENT "\xEE\x80\x81"
#define ICON_PENDANT "\xEE\x80\x82"
#define ICON_LAMP "\xEE\x80\x83"
#define ICON_STRIP "\xEE\x80\x84"
#define ICON_SPARKLE "\xEE\x80\x85"
#define ICON_PLUG "\xEE\x80\x86"
#define ICON_TOGGLE "\xEE\x80\x87"
#define ICON_FAN "\xEE\x80\x88"
#define ICON_TV "\xEE\x80\x89"
#define ICON_SPEAKER "\xEE\x80\x8A"
#define ICON_HEATER "\xEE\x80\x8B"
#define ICON_COFFEE "\xEE\x80\x8C"
#define ICON_WASHER "\xEE\x80\x8D"
#define ICON_PLANT "\xEE\x80\x8E"
#define ICON_GARDEN "\xEE\x80\x8F"
#define ICON_GARAGE "\xEE\x80\x90"
#define ICON_DOOR "\xEE\x80\x91"
#define ICON_BED "\xEE\x80\x92"
#define ICON_COUCH "\xEE\x80\x93"
#define ICON_DESK "\xEE\x80\x94"
#define ICON_KITCHEN "\xEE\x80\x95"
#define ICON_BATH "\xEE\x80\x96"
#define ICON_STAR "\xEE\x80\x97"
#define ICON_MOON "\xEE\x80\x98"
#define ICON_LIGHTNING "\xEE\x80\x99"
#define ICON_SUN "\xEE\x80\x9A"
#define ICON_SUN_DIM "\xEE\x80\x9B"
#define ICON_THERMO "\xEE\x80\x9C"
#define ICON_PALETTE "\xEE\x80\x9D"
#define ICON_POWER "\xEE\x80\x9E"
#define ICON_GEAR "\xEE\x80\x9F"
#define ICON_WIFI "\xEE\x80\xA0"
#define ICON_WIFI_OFF "\xEE\x80\xA1"
#define ICON_LINK "\xEE\x80\xA2"
#define ICON_LINK_OFF "\xEE\x80\xA3"
#define ICON_PENCIL "\xEE\x80\xA4"
#define ICON_CLOSE "\xEE\x80\xA5"
#define ICON_CHECK "\xEE\x80\xA6"
#define ICON_LEFT "\xEE\x80\xA7"
#define ICON_RIGHT "\xEE\x80\xA8"
#define ICON_INFO "\xEE\x80\xA9"
#define ICON_GRID "\xEE\x80\xAA"
#define ICON_EYE "\xEE\x80\xAB"
#define ICON_EYE_OFF "\xEE\x80\xAC"
#define ICON_LOCK "\xEE\x80\xAD"
#define ICON_WARNING "\xEE\x80\xAE"
#define ICON_REFRESH "\xEE\x80\xAF"
#define ICON_MONITOR "\xEE\x80\xB0"
#define ICON_LIST "\xEE\x80\xB1"
#define ICON_HOUSE "\xEE\x80\xB2"
#define ICON_WAND "\xEE\x80\xB3"
#define ICON_BATTERY "\xEE\x80\xB4"
#define ICON_PLUS "\xEE\x80\xB5"
#define ICON_GAS "\xEE\x80\xB6"
#define ICON_BLUETOOTH "\xEE\x80\xB7"
#define ICON_CLOCK "\xEE\x80\xB8"
#define ICON_FLAME "\xEE\x80\xB9"
#define ICON_SEARCH "\xEE\x80\xBA"

struct IconEntry { const char *id; const char *utf8; };
static const IconEntry ICON_TABLE[] = {
    {"bulb", ICON_BULB},
    {"filament", ICON_FILAMENT},
    {"pendant", ICON_PENDANT},
    {"lamp", ICON_LAMP},
    {"strip", ICON_STRIP},
    {"sparkle", ICON_SPARKLE},
    {"plug", ICON_PLUG},
    {"toggle", ICON_TOGGLE},
    {"fan", ICON_FAN},
    {"tv", ICON_TV},
    {"speaker", ICON_SPEAKER},
    {"heater", ICON_HEATER},
    {"coffee", ICON_COFFEE},
    {"washer", ICON_WASHER},
    {"plant", ICON_PLANT},
    {"garden", ICON_GARDEN},
    {"garage", ICON_GARAGE},
    {"door", ICON_DOOR},
    {"bed", ICON_BED},
    {"couch", ICON_COUCH},
    {"desk", ICON_DESK},
    {"kitchen", ICON_KITCHEN},
    {"bath", ICON_BATH},
    {"star", ICON_STAR},
    {"moon", ICON_MOON},
    {"lightning", ICON_LIGHTNING},
    {"sun", ICON_SUN},
    {"sun_dim", ICON_SUN_DIM},
    {"thermo", ICON_THERMO},
    {"palette", ICON_PALETTE},
    {"power", ICON_POWER},
    {"gear", ICON_GEAR},
    {"wifi", ICON_WIFI},
    {"wifi_off", ICON_WIFI_OFF},
    {"link", ICON_LINK},
    {"link_off", ICON_LINK_OFF},
    {"pencil", ICON_PENCIL},
    {"close", ICON_CLOSE},
    {"check", ICON_CHECK},
    {"left", ICON_LEFT},
    {"right", ICON_RIGHT},
    {"info", ICON_INFO},
    {"grid", ICON_GRID},
    {"eye", ICON_EYE},
    {"eye_off", ICON_EYE_OFF},
    {"lock", ICON_LOCK},
    {"warning", ICON_WARNING},
    {"refresh", ICON_REFRESH},
    {"monitor", ICON_MONITOR},
    {"list", ICON_LIST},
    {"house", ICON_HOUSE},
    {"wand", ICON_WAND},
    {"battery", ICON_BATTERY},
    {"plus", ICON_PLUS},
    {"gas", ICON_GAS},
    {"bluetooth", ICON_BLUETOOTH},
    {"clock", ICON_CLOCK},
    {"flame", ICON_FLAME},
    {"search", ICON_SEARCH}
};
static const int ICON_TABLE_LEN = 59;
static const int TILE_ICON_COUNT = 26;

// Liefert das UTF-8-Zeichen eines Icons anhand seiner ID (z. B. "bulb"); Fallback: Gluehbirne
static inline const char *icon_utf8(const char *id) {
    if (id) for (int i = 0; i < ICON_TABLE_LEN; i++) if (strcmp(ICON_TABLE[i].id, id) == 0) return ICON_TABLE[i].utf8;
    return ICON_BULB;
}
