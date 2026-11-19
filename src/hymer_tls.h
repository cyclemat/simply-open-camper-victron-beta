// Kleiner TLS-1.0/1.1-Client fuer die Hymer-SCU (Bluetooth, Nordic UART).
// Die SCU spricht nur TLS 1.0/1.1 mit RSA-Schluesseltausch und AES-CBC-SHA. mbedTLS 3 (ESP-IDF 5) kann diese
// alten Versionen nicht mehr, deshalb hier selbst aus den mbedTLS-Bausteinen (RSA, AES, HMAC, MD5/SHA1) gebaut.
// Kein Pruefen des Zertifikats (wie die Hymer-App und die HA-Integration) – die Verbindung ist ohnehin gekoppelt.
// Nur innerhalb von hymer.cpp benutzt; kein Socket, die Bytes laufen ueber feed()/out.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

class HymerTls {
 public:
  ~HymerTls() { reset(); }
  void reset();                                        // Puffer freigeben, Zustand zuruecksetzen
  // ClientHello erzeugen (Anfang des Handshakes)
  bool start(std::vector<uint8_t> &out);
  // Empfangene Bytes verarbeiten. Was zu senden ist, landet in out; Klartext geht an on_plain.
  // false = fataler Fehler (err() sagt warum)
  bool feed(const uint8_t *d, size_t n, std::vector<uint8_t> &out,
            const std::function<void(const uint8_t *, size_t)> &on_plain);
  bool ready() const { return st_ == ST_DONE; }
  // Anwendungsdaten verschluesseln (darf beliebig lang sein, wird in Records zerteilt)
  bool send(const uint8_t *d, size_t n, std::vector<uint8_t> &out);
  const std::string &err() const { return err_; }

 private:
  enum St { ST_IDLE, ST_HELLO, ST_WAIT_FIN, ST_DONE, ST_FAIL };
  bool fail(const char *why);
  bool record(uint8_t type, const uint8_t *frag, size_t n, std::vector<uint8_t> &out,
              const std::function<void(const uint8_t *, size_t)> &on_plain);
  bool handshake_msg(uint8_t type, const uint8_t *body, size_t n, const uint8_t *raw, size_t raw_n,
                     std::vector<uint8_t> &out);
  bool client_flight(std::vector<uint8_t> &out);
  void put_record(uint8_t type, const uint8_t *d, size_t n, std::vector<uint8_t> &out);   // ggf. verschluesselt
  bool open_record(uint8_t type, uint8_t *frag, size_t &n);                              // entschluesseln + pruefen
  void finished_data(const char *label, uint8_t out[12]);
  void mac(const uint8_t *key, uint64_t seq, uint8_t type, const uint8_t *d, size_t n, uint8_t out[20]);

  St st_ = ST_IDLE;
  std::string err_;
  uint16_t ver_ = 0x0302;                // ausgehandelte Version (0x0301 TLS 1.0, 0x0302 TLS 1.1)
  uint16_t suite_ = 0;
  uint8_t crand_[32], srand_[32], master_[48];
  std::vector<uint8_t> cert_;            // DER des Server-Zertifikats
  bool cert_req_ = false;
  std::vector<uint8_t> transcript_;      // alle Handshake-Nachrichten fuer Finished
  std::vector<uint8_t> hs_;              // Handshake-Nachricht, die ueber mehrere Records verteilt ist
  uint8_t *rec_ = nullptr;               // Record-Puffer (Empfang)
  size_t rec_len_ = 0;
  // Schluessel
  bool wr_enc_ = false, rd_enc_ = false;
  uint8_t wmac_[20], rmac_[20], wkey_[32], rkey_[32], wiv_[16], riv_[16];
  uint8_t keylen_ = 16;
  uint64_t wseq_ = 0, rseq_ = 0;
};
