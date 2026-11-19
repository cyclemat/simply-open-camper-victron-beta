#include "lv_psram_alloc.h"
#include "esp_heap_caps.h"

void *lv_psram_malloc(size_t size) {
  void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *lv_psram_realloc(void *p, size_t size) {
  if (!p) return lv_psram_malloc(size);
  void *n = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return n ? n : heap_caps_realloc(p, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void lv_psram_free(void *p) { heap_caps_free(p); }
