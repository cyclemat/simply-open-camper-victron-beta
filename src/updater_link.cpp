// Kleiner Updater – siehe updater_link.h
#include "updater_link.h"
#include <Preferences.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const esp_partition_t *slot() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
}

size_t updater_slot_size() {
  const esp_partition_t *p = slot();
  // Nur die neue Aufteilung (Updater 1 MB) gilt – bei min_spiffs.csv waere ota_1 die zweite volle Firmware
  return p && p->size <= 0x100000 ? p->size : 0;
}

bool updater_available() {
  const esp_partition_t *p = slot();
  if (!p || p->size > 0x100000) return false;
  esp_app_desc_t d;
  return esp_ota_get_partition_description(p, &d) == ESP_OK;
}

String updater_start(const String &ssid, const String &pass, const String &name) {
  if (!updater_slot_size()) return "Dieses Gerät hat noch die alte Speicheraufteilung – einmal per USB neu aufspielen.";
  if (!updater_available()) return "Kein Updater vorhanden – einmal per USB aufspielen (Umgebung …-updater).";
  Preferences p;
  p.begin("upd", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.putString("name", name);
  p.end();
  if (esp_ota_set_boot_partition(slot()) != ESP_OK) return "Der Updater lässt sich nicht starten.";
  delay(600);   // Antwort an den Browser noch rausschicken lassen
  ESP.restart();
  return "";
}
