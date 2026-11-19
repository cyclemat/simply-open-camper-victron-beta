// Kleiner TLS-1.0/1.1-Client fuer die Hymer-SCU – siehe hymer_tls.h
#include "hymer_tls.h"
#include "hymer.h"

#if WITH_HYMER
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

static const size_t REC_MAX = 5 + 16384 + 2048;   // groesster erlaubter TLS-Record (verschluesselt)
static const uint16_t TLS_RSA_AES128_SHA = 0x002F, TLS_RSA_AES256_SHA = 0x0035;
enum : uint8_t { CT_CCS = 20, CT_ALERT = 21, CT_HS = 22, CT_APP = 23 };
enum : uint8_t { HS_HELLO = 1, HS_SHELLO = 2, HS_CERT = 11, HS_SKX = 12, HS_CREQ = 13, HS_SDONE = 14, HS_CKX = 16, HS_FIN = 20 };

static int rng(void *, unsigned char *b, size_t n) {
  esp_fill_random(b, n);
  return 0;
}

static void hmac(mbedtls_md_type_t t, const uint8_t *key, size_t klen, const uint8_t *a, size_t alen, const uint8_t *b,
                 size_t blen, uint8_t *out) {
  mbedtls_md_context_t c;
  mbedtls_md_init(&c);
  mbedtls_md_setup(&c, mbedtls_md_info_from_type(t), 1);
  mbedtls_md_hmac_starts(&c, key, klen);
  if (alen) mbedtls_md_hmac_update(&c, a, alen);
  if (blen) mbedtls_md_hmac_update(&c, b, blen);
  mbedtls_md_hmac_finish(&c, out);
  mbedtls_md_free(&c);
}

// P_hash aus RFC 2246/4346, Ergebnis wird auf out geXORt
static void p_hash_xor(mbedtls_md_type_t t, size_t hlen, const uint8_t *sec, size_t slen, const std::vector<uint8_t> &seed,
                       uint8_t *out, size_t olen) {
  uint8_t a[20], o[20];
  hmac(t, sec, slen, seed.data(), seed.size(), nullptr, 0, a);   // A(1)
  for (size_t pos = 0; pos < olen; pos += hlen) {
    hmac(t, sec, slen, a, hlen, seed.data(), seed.size(), o);
    for (size_t i = 0; i < hlen && pos + i < olen; i++) out[pos + i] ^= o[i];
    hmac(t, sec, slen, a, hlen, nullptr, 0, a);                  // A(i+1)
  }
}

// PRF von TLS 1.0/1.1: P_MD5(erste Haelfte) XOR P_SHA1(zweite Haelfte)
static void prf(const uint8_t *sec, size_t slen, const char *label, const uint8_t *s1, size_t s1n, const uint8_t *s2,
                size_t s2n, uint8_t *out, size_t olen) {
  std::vector<uint8_t> seed(label, label + strlen(label));
  seed.insert(seed.end(), s1, s1 + s1n);
  if (s2n) seed.insert(seed.end(), s2, s2 + s2n);
  size_t half = (slen + 1) / 2;
  memset(out, 0, olen);
  p_hash_xor(MBEDTLS_MD_MD5, 16, sec, half, seed, out, olen);
  p_hash_xor(MBEDTLS_MD_SHA1, 20, sec + slen - half, half, seed, out, olen);
}

static void hs_msg(uint8_t type, const uint8_t *body, size_t n, std::vector<uint8_t> &out) {
  out.push_back(type);
  out.push_back((n >> 16) & 0xFF);
  out.push_back((n >> 8) & 0xFF);
  out.push_back(n & 0xFF);
  out.insert(out.end(), body, body + n);
}

void HymerTls::reset() {
  if (rec_) heap_caps_free(rec_);
  rec_ = nullptr;
  rec_len_ = 0;
  st_ = ST_IDLE;
  err_.clear();
  cert_.clear();
  cert_.shrink_to_fit();
  transcript_.clear();
  transcript_.shrink_to_fit();
  hs_.clear();
  hs_.shrink_to_fit();
  cert_req_ = false;
  wr_enc_ = rd_enc_ = false;
  wseq_ = rseq_ = 0;
  memset(master_, 0, sizeof(master_));
  memset(wkey_, 0, sizeof(wkey_));
  memset(rkey_, 0, sizeof(rkey_));
}

bool HymerTls::fail(const char *why) {
  err_ = why;
  st_ = ST_FAIL;
  return false;
}

bool HymerTls::start(std::vector<uint8_t> &out) {
  reset();
  // Im PSRAM, wenn vorhanden (Panel); sonst intern (Satellit) – nur waehrend der Verbindung belegt
  rec_ = (uint8_t *)heap_caps_malloc(REC_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!rec_) rec_ = (uint8_t *)heap_caps_malloc(REC_MAX, MALLOC_CAP_8BIT);
  if (!rec_) return fail("Kein Speicher für TLS");
  esp_fill_random(crand_, sizeof(crand_));
  uint32_t t = (uint32_t)time(nullptr);
  crand_[0] = t >> 24; crand_[1] = t >> 16; crand_[2] = t >> 8; crand_[3] = t;
  std::vector<uint8_t> b = {0x03, 0x02};                         // hoechste Version: TLS 1.1
  b.insert(b.end(), crand_, crand_ + 32);
  b.push_back(0);                                                // keine Sitzungs-ID
  b.insert(b.end(), {0x00, 0x06, 0x00, 0x2F, 0x00, 0x35, 0x00, 0xFF});   // AES128-SHA, AES256-SHA, Reneg-SCSV
  b.insert(b.end(), {0x01, 0x00});                               // keine Kompression
  std::vector<uint8_t> m;
  hs_msg(HS_HELLO, b.data(), b.size(), m);
  transcript_ = m;
  ver_ = 0x0301;                                                 // Record-Schicht des Hello wie ueblich mit 1.0
  put_record(CT_HS, m.data(), m.size(), out);
  ver_ = 0x0302;
  st_ = ST_HELLO;
  return true;
}

void HymerTls::mac(const uint8_t *key, uint64_t seq, uint8_t type, const uint8_t *d, size_t n, uint8_t out[20]) {
  uint8_t h[13];
  for (int i = 0; i < 8; i++) h[i] = (uint8_t)(seq >> (56 - 8 * i));
  h[8] = type;
  h[9] = ver_ >> 8;
  h[10] = ver_ & 0xFF;
  h[11] = n >> 8;
  h[12] = n & 0xFF;
  hmac(MBEDTLS_MD_SHA1, key, 20, h, 13, d, n, out);
}

void HymerTls::put_record(uint8_t type, const uint8_t *d, size_t n, std::vector<uint8_t> &out) {
  do {
    size_t k = n > 16384 ? 16384 : n;
    if (!wr_enc_) {
      out.insert(out.end(), {type, (uint8_t)(ver_ >> 8), (uint8_t)ver_, (uint8_t)(k >> 8), (uint8_t)k});
      out.insert(out.end(), d, d + k);
    } else {
      size_t pad = (16 - (k + 21) % 16) % 16;
      size_t plen = k + 20 + pad + 1;
      bool explicit_iv = ver_ >= 0x0302;
      size_t flen = plen + (explicit_iv ? 16 : 0);
      size_t at = out.size();
      out.resize(at + 5 + flen);
      uint8_t *r = out.data() + at;
      r[0] = type; r[1] = ver_ >> 8; r[2] = ver_; r[3] = flen >> 8; r[4] = flen;
      uint8_t *p = r + 5 + (explicit_iv ? 16 : 0);
      memcpy(p, d, k);
      mac(wmac_, wseq_++, type, d, k, p + k);
      memset(p + k + 20, (int)pad, pad + 1);
      uint8_t iv[16];
      uint8_t *ivp = wiv_;                     // TLS 1.0: IV = letzter Block des vorigen Records
      if (explicit_iv) {
        esp_fill_random(iv, 16);
        memcpy(r + 5, iv, 16);
        ivp = iv;
      }
      mbedtls_aes_context a;
      mbedtls_aes_init(&a);
      mbedtls_aes_setkey_enc(&a, wkey_, keylen_ * 8);
      mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_ENCRYPT, plen, ivp, p, p);
      mbedtls_aes_free(&a);
    }
    d += k;
    n -= k;
  } while (n);
}

bool HymerTls::open_record(uint8_t type, uint8_t *frag, size_t &n) {
  bool explicit_iv = ver_ >= 0x0302;
  size_t off = explicit_iv ? 16 : 0;
  if (n < off + 32 || (n - off) % 16) return fail("TLS: Record kaputt");
  uint8_t iv[16];
  uint8_t *ivp = riv_;
  if (explicit_iv) {
    memcpy(iv, frag, 16);
    ivp = iv;
  }
  uint8_t *p = frag + off;
  size_t dlen = n - off;
  mbedtls_aes_context a;
  mbedtls_aes_init(&a);
  mbedtls_aes_setkey_dec(&a, rkey_, keylen_ * 8);
  mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_DECRYPT, dlen, ivp, p, p);
  mbedtls_aes_free(&a);
  size_t pad = p[dlen - 1];
  if (pad + 1 + 20 > dlen) return fail("TLS: Auffüllung falsch");
  for (size_t i = 0; i <= pad; i++)
    if (p[dlen - 1 - i] != pad) return fail("TLS: Auffüllung falsch");
  size_t plen = dlen - pad - 1 - 20;
  uint8_t m[20];
  mac(rmac_, rseq_++, type, p, plen, m);
  if (memcmp(m, p + plen, 20) != 0) return fail("TLS: Prüfsumme falsch");
  memmove(frag, p, plen);
  n = plen;
  return true;
}

void HymerTls::finished_data(const char *label, uint8_t out[12]) {
  uint8_t h[36];
  mbedtls_md5(transcript_.data(), transcript_.size(), h);
  mbedtls_sha1(transcript_.data(), transcript_.size(), h + 16);
  prf(master_, 48, label, h, 36, nullptr, 0, out, 12);
}

bool HymerTls::client_flight(std::vector<uint8_t> &out) {
  if (cert_.empty()) return fail("TLS: kein Zertifikat");
  std::vector<uint8_t> m;
  if (cert_req_) {
    // Die SCU fragt nach einem Client-Zertifikat: keins schicken (wie die HA-Integration)
    static const uint8_t empty[3] = {0, 0, 0};
    hs_msg(HS_CERT, empty, 3, m);
  }
  // Pre-Master-Secret mit dem oeffentlichen RSA-Schluessel der SCU verschluesseln
  uint8_t pm[48];
  pm[0] = 0x03;
  pm[1] = 0x02;
  esp_fill_random(pm + 2, 46);
  mbedtls_x509_crt crt;
  mbedtls_x509_crt_init(&crt);
  int rc = mbedtls_x509_crt_parse_der(&crt, cert_.data(), cert_.size());
  if (rc != 0 || !mbedtls_pk_can_do(&crt.pk, MBEDTLS_PK_RSA)) {
    mbedtls_x509_crt_free(&crt);
    return fail("TLS: Zertifikat unlesbar");
  }
  std::vector<uint8_t> enc(2 + 512);
  size_t olen = 0;
  rc = mbedtls_pk_encrypt(&crt.pk, pm, sizeof(pm), enc.data() + 2, &olen, 512, rng, nullptr);
  mbedtls_x509_crt_free(&crt);
  cert_.clear();
  cert_.shrink_to_fit();
  if (rc != 0) return fail("TLS: RSA fehlgeschlagen");
  enc[0] = olen >> 8;
  enc[1] = olen & 0xFF;
  enc.resize(2 + olen);
  hs_msg(HS_CKX, enc.data(), enc.size(), m);
  transcript_.insert(transcript_.end(), m.begin(), m.end());
  put_record(CT_HS, m.data(), m.size(), out);

  // Schluessel ableiten
  prf(pm, 48, "master secret", crand_, 32, srand_, 32, master_, 48);
  memset(pm, 0, sizeof(pm));
  bool tls10 = ver_ < 0x0302;
  size_t kb_len = 40 + 2 * keylen_ + (tls10 ? 32 : 0);
  uint8_t kb[40 + 64 + 32];
  prf(master_, 48, "key expansion", srand_, 32, crand_, 32, kb, kb_len);
  const uint8_t *p = kb;
  memcpy(wmac_, p, 20); p += 20;
  memcpy(rmac_, p, 20); p += 20;
  memcpy(wkey_, p, keylen_); p += keylen_;
  memcpy(rkey_, p, keylen_); p += keylen_;
  if (tls10) {
    memcpy(wiv_, p, 16); p += 16;
    memcpy(riv_, p, 16);
  }
  memset(kb, 0, sizeof(kb));

  static const uint8_t ccs = 1;
  put_record(CT_CCS, &ccs, 1, out);
  wr_enc_ = true;
  wseq_ = 0;
  uint8_t vd[12];
  finished_data("client finished", vd);
  m.clear();
  hs_msg(HS_FIN, vd, 12, m);
  transcript_.insert(transcript_.end(), m.begin(), m.end());
  put_record(CT_HS, m.data(), m.size(), out);
  st_ = ST_WAIT_FIN;
  return true;
}

bool HymerTls::handshake_msg(uint8_t type, const uint8_t *b, size_t n, const uint8_t *raw, size_t raw_n,
                             std::vector<uint8_t> &out) {
  if (type == HS_FIN) {
    if (st_ != ST_WAIT_FIN || n != 12) return fail("TLS: Finished unerwartet");
    uint8_t vd[12];
    finished_data("server finished", vd);
    if (memcmp(vd, b, 12) != 0) return fail("TLS: Finished passt nicht");
    transcript_.clear();
    transcript_.shrink_to_fit();
    st_ = ST_DONE;
    return true;
  }
  transcript_.insert(transcript_.end(), raw, raw + raw_n);
  switch (type) {
    case HS_SHELLO: {
      if (n < 38) return fail("TLS: ServerHello kaputt");
      ver_ = (b[0] << 8) | b[1];
      if (ver_ != 0x0301 && ver_ != 0x0302) return fail("TLS: Version nicht unterstützt");
      memcpy(srand_, b + 2, 32);
      size_t sid = b[34];
      if (n < 35 + sid + 3) return fail("TLS: ServerHello kaputt");
      suite_ = (b[35 + sid] << 8) | b[36 + sid];
      if (suite_ == TLS_RSA_AES128_SHA) keylen_ = 16;
      else if (suite_ == TLS_RSA_AES256_SHA) keylen_ = 32;
      else return fail("TLS: Verschlüsselung nicht unterstützt");
      return true;
    }
    case HS_CERT: {
      if (n < 6) return fail("TLS: Zertifikat fehlt");
      size_t len = (b[3] << 16) | (b[4] << 8) | b[5];
      if (6 + len > n) return fail("TLS: Zertifikat kaputt");
      cert_.assign(b + 6, b + 6 + len);
      return true;
    }
    case HS_SKX: return fail("TLS: Schlüsseltausch nicht unterstützt");
    case HS_CREQ: cert_req_ = true; return true;
    case HS_SDONE: return client_flight(out);
    default: return true;
  }
}

bool HymerTls::record(uint8_t type, const uint8_t *frag_in, size_t n, std::vector<uint8_t> &out,
                      const std::function<void(const uint8_t *, size_t)> &on_plain) {
  uint8_t *frag = (uint8_t *)frag_in;
  if (rd_enc_ && !open_record(type, frag, n)) return false;
  switch (type) {
    case CT_ALERT: {
      char b[48];
      snprintf(b, sizeof(b), "TLS: SCU meldet Fehler %d", n >= 2 ? frag[1] : -1);
      return fail(b);
    }
    case CT_CCS:
      rd_enc_ = true;
      rseq_ = 0;
      return true;
    case CT_HS:
      hs_.insert(hs_.end(), frag, frag + n);
      while (hs_.size() >= 4) {
        size_t len = (hs_[1] << 16) | (hs_[2] << 8) | hs_[3];
        if (len > 32768) return fail("TLS: Nachricht zu groß");
        if (hs_.size() < 4 + len) break;
        std::vector<uint8_t> msg(hs_.begin(), hs_.begin() + 4 + len);
        hs_.erase(hs_.begin(), hs_.begin() + 4 + len);
        if (!handshake_msg(msg[0], msg.data() + 4, len, msg.data(), msg.size(), out)) return false;
      }
      return true;
    case CT_APP:
      if (st_ == ST_DONE && n) on_plain(frag, n);
      return true;
    default:
      return true;
  }
}

bool HymerTls::feed(const uint8_t *d, size_t n, std::vector<uint8_t> &out,
                    const std::function<void(const uint8_t *, size_t)> &on_plain) {
  if (st_ == ST_FAIL || st_ == ST_IDLE || !rec_) return false;
  while (n) {
    size_t k = REC_MAX - rec_len_ < n ? REC_MAX - rec_len_ : n;
    memcpy(rec_ + rec_len_, d, k);
    rec_len_ += k;
    d += k;
    n -= k;
    while (rec_len_ >= 5) {
      size_t len = (rec_[3] << 8) | rec_[4];
      if (len + 5 > REC_MAX || rec_[1] != 0x03) return fail("TLS: Datenstrom kaputt");
      if (rec_len_ < len + 5) break;
      if (!record(rec_[0], rec_ + 5, len, out, on_plain)) return false;
      memmove(rec_, rec_ + 5 + len, rec_len_ - 5 - len);
      rec_len_ -= 5 + len;
    }
    if (rec_len_ == REC_MAX) return fail("TLS: Datenstrom kaputt");
  }
  return true;
}

bool HymerTls::send(const uint8_t *d, size_t n, std::vector<uint8_t> &out) {
  if (st_ != ST_DONE) return false;
  put_record(CT_APP, d, n, out);
  return true;
}

#else
void HymerTls::reset() {}
bool HymerTls::start(std::vector<uint8_t> &) { return false; }
bool HymerTls::feed(const uint8_t *, size_t, std::vector<uint8_t> &, const std::function<void(const uint8_t *, size_t)> &) {
  return false;
}
bool HymerTls::send(const uint8_t *, size_t, std::vector<uint8_t> &) { return false; }
#endif
