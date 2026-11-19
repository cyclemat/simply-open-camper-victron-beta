// Hymer Connect – einmalige Anmeldung beim Hymer-Server (EHG) fuer die Kopplung mit der SCU.
//
// Ablauf wie in der HA-Integration (Facetti/hymer-connect-ha-ble, MIT):
//   1. Anmeldung mit E-Mail/Passwort des Hymer-Kontos (OAuth2 "password") -> Zugangs-Token
//   2. QR-Code-Text (aus den Fahrzeugunterlagen) pruefen: GET /api/ehg/v1/vehicles/byToken
//   3. Bestaetigungs-Token holen: POST /api/ehg/v1/accounts/confirmationToken
// QR-Text und Bestaetigungs-Token gehen danach per Bluetooth (in TLS) an die SCU ("PairMobileRequest", hymer.cpp).
// Das Passwort wird nicht gespeichert. Die App-Kennung ("Authorization: Basic …") der Hymer-App ist als Standard
// eingebaut (Wert aus der HA-Integration); ein im Webinterface eingetragener Wert hat Vorrang.
// Laeuft einmalig in einem eigenen Task, der sich danach beendet; TLS-Puffer liegen dabei im PSRAM.
#pragma once
#include <string>

struct HymerCloudReq {
  std::string user, pass, brand, qr, basic;
  bool own_basic = false;   // false = Standard-App-Kennung, true = eigene (basic leer = gespeicherte behalten)
};

enum HymerCloudState { HC_IDLE = 0, HC_RUNNING = 1, HC_OK = 2, HC_ERROR = 3 };

// Startet die Anmeldung im Hintergrund; false + err, wenn etwas fehlt oder schon eine laeuft
bool hymer_cloud_start(const HymerCloudReq &r, std::string &err);
int hymer_cloud_state();
std::string hymer_cloud_text();

// Gespeicherte Daten (NVS "hymer")
std::string hymer_cloud_qr();        // QR-Code-Text
std::string hymer_cloud_conf();      // Bestaetigungs-Token
std::string hymer_cloud_brand();
bool hymer_cloud_has_basic();     // eigener Wert gespeichert (sonst gilt der Standard)
std::string hymer_cloud_user();      // zuletzt benutzte E-Mail (nur zur Anzeige)
void hymer_cloud_forget();           // alles loeschen

// mbedtls-Speicher voruebergehend ins PSRAM (zaehlt mit, darf verschachtelt werden)
void hymer_mbedtls_psram(bool on);
