// Speicher fuer LVGL-Objekte aus dem PSRAM (siehe lv_conf.h, LV_MEM_CUSTOM_*)
// Mit malloc landeten alle kleinen Objekte (< 1 KB) im knappen internen RAM – bei 70+ Kacheln
// fehlte er dann WLAN und MQTT.
#pragma once
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif
void *lv_psram_malloc(size_t size);
void *lv_psram_realloc(void *p, size_t size);
void lv_psram_free(void *p);
#ifdef __cplusplus
}
#endif
