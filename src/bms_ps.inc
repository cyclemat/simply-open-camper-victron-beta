// Power Stations per Bluetooth – Teil von bms.cpp (dort eingebunden, nutzt dessen Puffer und Hilfsfunktionen).
// ALLES UNGETESTET (noch keine Geraete zum Testen).
//
// Bluetti (nach Patrick762/bluetti-bt-lib und warhammerkid/bluetti_mqtt, beide MIT): Modbus-RTU, Adresse 0x01, ueber
// Dienst 0xFF00 (Notify 0xFF01, Schreiben 0xFF02). Zwei Register-Generationen ("iot 1": Typ ab 10, SoC 43,
// Leistungen 36..39; "iot 2": Typ ab 110 byteweise vertauscht, SoC 102, Leistungen 140..146). Neuere Geraete schicken
// nach dem Verbinden eine Aufforderung "**" und verschluesseln danach: Schluessel aus MD5 der Aufforderung, dann
// P-256-Schluesseltausch (eigener Schluessel signiert mit dem bekannten App-Schluessel), danach AES-256-CBC je
// Nachricht mit zufaelligem IV. Geschaltet wird nur der AC- bzw. DC-Ausgang (Register 3007/3008 bzw. 2011/2012).
//
// Allpowers (nach madninjaskillz/allpowers-ble, MIT): Dienst 0xFFF0, Werte kommen von selbst auf 0xFFF1
// ([7] Schalter-Bits, [8] SoC, [9..10] W rein, [11..12] W raus, [13..14] Restzeit min), Schalten ueber 0xFFF2
// (A5 65 00 B1 01 01 00 <Bits> <Pruefwert>).
//
// Anker SOLIX (nach flip-dots/SolixBLE, MIT) und EcoFlow (nach rabits/ha-ef-ble, Apache-2.0): siehe weiter unten.
#include <time.h>
#include "esp_random.h"
#include "mbedtls/aes.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"

// ---------------------------------------------------------------- Bluetti
static const char *const BLUETTI_TYPES[] = {"AC2A",  "AC2P",   "AC50B",  "AC60",   "AC60P", "AC70",  "AC70P", "AC180",
                                            "AC180T", "AC180P", "AC200L", "AC200M", "AC200PL", "AC300", "AC500", "AP300",
                                            "EB3A",  "EL10",   "EL100V2", "EL30V2", "EP500", "EP500P", "EP600", "EP760",
                                            "EP800", "EP2000", "HANDSFREE", "PR30V2", "PR100V2"};

// Werbename "<Typ><Seriennummer>", z. B. "AC1802235000123"
static bool bluetti_name(const std::string &un) {
  for (const char *t : BLUETTI_TYPES) {
    size_t n = strlen(t);
    if (un.compare(0, n, t) != 0) continue;
    size_t digits = 0;
    for (size_t i = n; i < un.size(); i++)
      if (isdigit((unsigned char)un[i])) digits++;
      else if (un[i] != ' ') return false;
    if (digits >= 6) return true;
  }
  return false;
}
static bool bluetti_type_ok(const std::string &t) {
  std::string u = upper(t);
  for (const char *x : BLUETTI_TYPES)
    if (u.rfind(x, 0) == 0) return true;
  return false;
}

static const uint8_t BL_LOCAL_KEY[16] = {0x45, 0x9F, 0xC5, 0x35, 0x80, 0x89, 0x41, 0xF1,
                                         0x70, 0x91, 0xE0, 0x99, 0x3E, 0xE3, 0xE9, 0x3D};
static const uint8_t BL_SIGN_KEY[32] = {0x4F, 0x19, 0xA1, 0x6E, 0x3E, 0x87, 0xBD, 0xD9, 0xBD, 0x24, 0xD3,
                                        0xE5, 0x49, 0x5B, 0x88, 0x04, 0x15, 0x11, 0x94, 0x3C, 0xBC, 0x8B,
                                        0x96, 0x9A, 0xDE, 0x96, 0x41, 0xD0, 0xF5, 0x6A, 0xF3, 0x37};

namespace {
struct BlSess {
  bool enc = false;              // Geraet verschluesselt
  uint8_t ukey[16], uiv[16];     // vorlaeufig (aus der Aufforderung)
  uint8_t skey[32];              // endgueltig (Schluesseltausch)
  bool ready = false;
};
}  // namespace

static int bl_rng(void *, unsigned char *b, size_t n) {
  esp_fill_random(b, n);
  return 0;
}
static uint16_t bl_sum(const uint8_t *p, size_t n) {
  uint32_t s = 0;
  for (size_t i = 0; i < n; i++) s += p[i];
  return (uint16_t)s;
}
static bool bl_cbc(bool enc, const uint8_t *key, unsigned bits, const uint8_t *iv_in, const uint8_t *in, uint8_t *out,
                   size_t n) {
  mbedtls_aes_context a;
  mbedtls_aes_init(&a);
  uint8_t iv[16];
  memcpy(iv, iv_in, 16);
  int r = enc ? mbedtls_aes_setkey_enc(&a, key, bits) : mbedtls_aes_setkey_dec(&a, key, bits);
  if (!r) r = mbedtls_aes_crypt_cbc(&a, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, n, iv, in, out);
  mbedtls_aes_free(&a);
  return r == 0;
}

// Verschluesseln: [Laenge 2][bei endgueltigem Schluessel: 4 Byte Zufall, IV = MD5 davon][Daten mit Nullen auf 16]
static std::string bl_encrypt(const BlSess &s, const uint8_t *p, size_t n, bool secure) {
  size_t pad = (n + 15) / 16 * 16;
  std::string out;
  out += (char)(n >> 8);
  out += (char)n;
  uint8_t iv[16];
  if (secure) {
    uint8_t seed[4];
    esp_fill_random(seed, 4);
    mbedtls_md5(seed, 4, iv);
    out.append((const char *)seed, 4);
  } else {
    memcpy(iv, s.uiv, 16);
  }
  std::vector<uint8_t> buf(pad, 0), enc(pad);
  memcpy(buf.data(), p, n);
  if (!bl_cbc(true, secure ? s.skey : s.ukey, secure ? 256 : 128, iv, buf.data(), enc.data(), pad)) return "";
  out.append((const char *)enc.data(), pad);
  return out;
}

static bool bl_raw_write(NimBLERemoteCharacteristic *wr, const std::string &d) {
  return wr->writeValue((const uint8_t *)d.data(), d.size(), wr->canWrite() && !wr->canWriteNoResponse());
}

// Naechste vollstaendige Nachricht aus dem Empfangspuffer: "**…" (Schluesseltausch im Klartext) oder verschluesselt
// (entschluesselt zurueckgegeben). false = nichts innerhalb von timeout_ms.
static bool bl_take(const std::string &mac, const BlSess &s, bool secure, std::string &msg, uint32_t timeout_ms) {
  for (uint32_t t0 = millis(); millis() - t0 < timeout_ms;) {
    delay(20);
    std::string chunk;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (!b) return false;
      std::string &rx = b->rx;
      if (rx.size() < 4) continue;
      if (rx[0] == '*' && rx[1] == '*') {
        size_t total = 2 + 2 + (uint8_t)rx[3] + 2;
        if (rx.size() < total) continue;
        msg = rx.substr(0, total);
        rx.erase(0, total);
        return true;
      }
      size_t len = (uint8_t)rx[0] << 8 | (uint8_t)rx[1];
      size_t hdr = secure ? 6 : 2, total = hdr + (len + 15) / 16 * 16;
      if (len == 0 || len > 400) {   // Unsinn: Puffer verwerfen
        rx.clear();
        continue;
      }
      if (rx.size() < total) continue;
      chunk = rx.substr(0, total);
      rx.erase(0, total);
    }
    const uint8_t *c = (const uint8_t *)chunk.data();
    size_t len = c[0] << 8 | c[1], hdr = secure ? 6 : 2, n = chunk.size() - hdr;
    uint8_t iv[16];
    if (secure) mbedtls_md5(c + 2, 4, iv);
    else memcpy(iv, s.uiv, 16);
    std::vector<uint8_t> out(n);
    if (!bl_cbc(false, secure ? s.skey : s.ukey, secure ? 256 : 128, iv, c + hdr, out.data(), n)) return false;
    msg.assign((const char *)out.data(), len < n ? len : n);
    return true;
  }
  return false;
}

// Schluesseltausch (nur wenn das Geraet nach dem Verbinden eine Aufforderung "**" schickt). enc_known: -1 unbekannt,
// 0 Klartext (nicht warten), 1 verschluesselt.
static bool bl_handshake(const std::string &mac, NimBLERemoteCharacteristic *wr, BlSess &s, int8_t enc_known) {
  if (enc_known == 0) return true;
  bool kex = false;
  for (uint32_t t0 = millis(); millis() - t0 < (enc_known == 1 ? 5000u : 2500u) && !kex;) {
    delay(30);
    Lock l;
    Bat *b = find_mac(mac);
    if (!b) return false;
    kex = b->rx.size() >= 2 && b->rx[0] == '*' && b->rx[1] == '*';
  }
  if (!kex) return enc_known != 1;   // keine Aufforderung: Klartext
  s.enc = true;
  std::string m;
  if (!bl_take(mac, s, false, m, 1500) || m.size() < 10 || m[2] != 1) {
    logf("%s: Bluetti-Aufforderung unvollstaendig", mac.c_str());
    return false;
  }
  const uint8_t *d4 = (const uint8_t *)m.data() + 4;
  uint8_t rev[4] = {d4[3], d4[2], d4[1], d4[0]};
  mbedtls_md5(rev, 4, s.uiv);
  for (int i = 0; i < 16; i++) s.ukey[i] = s.uiv[i] ^ BL_LOCAL_KEY[i];
  uint8_t rep[10] = {'*', '*', 0x02, 0x04, s.uiv[8], s.uiv[9], s.uiv[10], s.uiv[11], 0, 0};
  uint16_t cs = bl_sum(rep + 2, 6);
  rep[8] = cs >> 8;
  rep[9] = cs & 0xFF;
  if (!bl_raw_write(wr, std::string((const char *)rep, 10))) return false;
  // Bestaetigung (Klartext, Typ 3) und oeffentlicher Schluessel des Geraets (verschluesselt, Typ 4: 64 Byte + Signatur)
  uint8_t peer[65] = {0x04};
  bool got_peer = false;
  for (int i = 0; i < 4 && !got_peer; i++) {
    if (!bl_take(mac, s, false, m, 3000)) break;
    if (m.size() >= 2 + 2 + 128 && m[0] == '*' && m[1] == '*' && m[2] == 4) {
      memcpy(peer + 1, m.data() + 4, 64);
      got_peer = true;
    }
  }
  if (!got_peer) {
    logf("%s: Bluetti-Schluessel nicht erhalten", mac.c_str());
    return false;
  }
  // eigenes Schluesselpaar, signiert mit dem App-Schluessel ueber (eigener Schluessel + vorlaeufiger IV)
  bool ok = false;
  mbedtls_ecp_group grp;
  mbedtls_mpi d, z, r, sg, k1;
  mbedtls_ecp_point Q, P;
  mbedtls_ecp_group_init(&grp);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&sg);
  mbedtls_mpi_init(&k1);
  mbedtls_ecp_point_init(&Q);
  mbedtls_ecp_point_init(&P);
  do {
    if (mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1)) break;
    if (mbedtls_ecp_gen_keypair(&grp, &d, &Q, bl_rng, nullptr)) break;
    uint8_t mine[65];
    size_t ol = 0;
    if (mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &ol, mine, sizeof(mine)) || ol != 65) break;
    uint8_t tosign[64 + 16], hash[32];
    memcpy(tosign, mine + 1, 64);
    memcpy(tosign + 64, s.uiv, 16);
    mbedtls_sha256(tosign, sizeof(tosign), hash, 0);
    if (mbedtls_mpi_read_binary(&k1, BL_SIGN_KEY, 32)) break;
    if (mbedtls_ecdsa_sign(&grp, &r, &sg, &k1, hash, 32, bl_rng, nullptr)) break;
    uint8_t msgb[2 + 2 + 128 + 2];
    msgb[0] = '*';
    msgb[1] = '*';
    msgb[2] = 0x05;
    msgb[3] = 0x80;
    memcpy(msgb + 4, mine + 1, 64);
    if (mbedtls_mpi_write_binary(&r, msgb + 68, 32) || mbedtls_mpi_write_binary(&sg, msgb + 100, 32)) break;
    uint16_t c2 = bl_sum(msgb + 2, 130);
    msgb[132] = c2 >> 8;
    msgb[133] = c2 & 0xFF;
    std::string e = bl_encrypt(s, msgb, sizeof(msgb), false);
    if (e.empty() || !bl_raw_write(wr, e)) break;
    // Annahme abwarten (Typ 6, Daten 00)
    bool acc = false;
    for (int i = 0; i < 3 && !acc; i++) {
      if (!bl_take(mac, s, false, m, 3000)) break;
      acc = m.size() >= 5 && m[0] == '*' && m[1] == '*' && m[2] == 6 && m[4] == 0;
    }
    if (!acc) break;
    if (mbedtls_ecp_point_read_binary(&grp, &P, peer, 65)) break;
    if (mbedtls_ecdh_compute_shared(&grp, &z, &P, &d, bl_rng, nullptr)) break;
    if (mbedtls_mpi_write_binary(&z, s.skey, 32)) break;
    ok = true;
  } while (false);
  mbedtls_ecp_point_free(&Q);
  mbedtls_ecp_point_free(&P);
  mbedtls_mpi_free(&d);
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&sg);
  mbedtls_mpi_free(&k1);
  mbedtls_ecp_group_free(&grp);
  if (!ok) logf("%s: Bluetti-Schluesseltausch fehlgeschlagen", mac.c_str());
  s.ready = ok;
  return ok;
}

// Modbus-Anfrage (Funktion 3 lesen bzw. 6 schreiben) – im Klartext oder verschluesselt
static bool bl_ask(const std::string &mac, NimBLERemoteCharacteristic *wr, const BlSess &s, uint8_t fn, uint16_t reg,
                   uint16_t val, std::string &out) {
  uint8_t req[8] = {0x01, fn, (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(val >> 8), (uint8_t)val, 0, 0};
  uint16_t c = mb_crc(req, 6);
  req[6] = c & 0xFF;
  req[7] = c >> 8;
  s_mb_addr = 0x01;
  if (!s.enc) {
    if (fn == 0x03) return pq_ask(mac, wr, req, 8, 2, (uint8_t)(val * 2), out, 3000);
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (b) b->rx.clear();
    }
    bool w = pq_write(wr, req, 8, false);
    delay(600);
    return w;
  }
  {
    Lock l;
    Bat *b = find_mac(mac);
    if (!b) return false;
    b->rx.clear();
  }
  std::string e = bl_encrypt(s, req, 8, true);
  if (e.empty() || !bl_raw_write(wr, e)) return false;
  std::string m;
  if (!bl_take(mac, s, true, m, 3000)) return fn != 0x03;
  if (fn != 0x03) return true;
  return mb_frame(m, (uint8_t)(val * 2), out);
}

static std::string bl_text(const std::string &d, bool swap) {   // iot 2: Zeichen je 16-Bit-Wort vertauscht
  std::string s;
  for (size_t i = 0; i + 1 < d.size(); i += 2) {
    char a = swap ? d[i + 1] : d[i], b = swap ? d[i] : d[i + 1];
    if (a > 0x20 && a < 0x7F) s += a;
    if (b > 0x20 && b < 0x7F) s += b;
  }
  return s;
}

static bool bluetti_connect(Bat &b, NimBLEClient *c) {
  {
    Lock l;
    Bat *x = find_mac(b.mac);
    if (x) x->rx.clear();
  }
  b.rx.clear();
  if (generic_connect(b, c, 0xFF00, 0xFF01, 0xFF02, "Bluetti")) return true;
  logf("%s: Bluetti-Dienst FF00 mit FF01/FF02 fehlt", b.name.c_str());
  return false;
}

// ---------------------------------------------------------------- Allpowers
static void ap_rx(const std::string &mac, const uint8_t *d, size_t n) {   // jede Meldung ist ein ganzer Datensatz
  if (n < 15) return;
  Lock l;
  Bat *b = find_mac(mac);
  if (b) b->ap_frame.assign((const char *)d, n);
}

static bool allpowers_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteService *svc = c->getService(NimBLEUUID((uint16_t)0xFFF0));
  NimBLERemoteCharacteristic *nt = svc ? svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFF1)) : nullptr;
  NimBLERemoteCharacteristic *wr = svc ? svc->getCharacteristic(NimBLEUUID((uint16_t)0xFFF2)) : nullptr;
  if (!nt || !wr) {
    logf("%s: Allpowers-Dienst FFF0 mit FFF1/FFF2 fehlt", b.name.c_str());
    return false;
  }
  std::string mac = b.mac;
  if (!nt->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { ap_rx(mac, d, n); }, true))
    return false;
  b.wr = wr;
  b.wr_resp = false;   // wie die Vorlage: ohne Bestaetigung
  b.next_poll = millis() + 300;
  if (!b.has) logf("%s: verbunden (Allpowers)", b.name.c_str());
  return true;
}

// Schaltbefehl: A5 65 00 B1 01 01 00 <Bits: 0 DC, 1 AC, 5 Licht> <113 - Bits, +4 bei AC an>
static bool ap_switch(NimBLERemoteCharacteristic *wr, bool ac, bool dc, bool light) {
  uint8_t s[9] = {0xA5, 0x65, 0x00, 0xB1, 0x01, 0x01, 0x00, 0, 0};
  s[7] = (dc ? 0x01 : 0) | (ac ? 0x02 : 0) | (light ? 0x20 : 0);
  s[8] = (uint8_t)(113 - s[7] + (ac ? 4 : 0));
  return wr->writeValue(s, 9, false);
}

// ---------------------------------------------------------------- Anker SOLIX
// Nach flip-dots/SolixBLE (MIT). Dauerverbindung (Werte kommen als Meldungen). Pakete:
//   FF 09 <Laenge 2 LE = 10 + n> <Muster 3> <Befehl 2> <Nutzdaten n> <XOR-Pruefsumme>
// Befehl: Bit 15 Bruchstueck, Bit 14 verschluesselt, Bits 0..11 Nachrichtentyp (Antwort = Typ | 0x800).
// Nutzdaten: [Status <0xA1>] dann Felder <Schluessel a1..> <Laenge> <Wert>.
// Anmeldung (Muster 03 00 01): 001 -> 801, 003 -> 803, 029 -> 829, 005 -> 805, 021 (eigener P-256-Schluessel) -> 821
// (Schluessel des Geraets; gemeinsames Geheimnis = AES-Schluessel 16 + IV 16), 022 (Zeit, Zeitzone) -> 822, 027
// (Kennung registrieren) -> 827 (00 = frei, 09 = Taste am Geraet druecken). Zwei Huellen: "offen" (Klartext, danach
// AES-CBC) und "verschluesselt" (AES-GCM, vor dem Schluesseltausch mit festem Schluessel) – die Geraete nehmen je nach
// Firmware nur eine; wird die offene abgelehnt, nimmt das Panel beim naechsten Verbinden die andere.
static const char *AK_UUID = "b2dc0b17-b75d-4abf-ba6e-ec7c997c23e7";   // Kennung des Clients (wie SolixBLE)
static const uint8_t AK_GCM_KEY[16] = {0xb8, 0xff, 0x74, 0x22, 0x95, 0x5d, 0x4e, 0xb6,
                                       0xd5, 0x54, 0xa2, 0xc4, 0x70, 0x28, 0x05, 0x59};
static const uint8_t AK_GCM_NONCE[12] = {0x6b, 0xa3, 0xe3, 0xf2, 0xf3, 0xa6, 0x0f, 0x29, 0x71, 0xce, 0x5d, 0x1f};
static const uint8_t AK_GCM_AAD[16] = {0x33, 0x22, 0x11, 0x00, 0x77, 0x66, 0x55, 0x44,
                                       0xbb, 0xaa, 0x99, 0x88, 0xff, 0xee, 0xdd, 0xcc};

// Lage der Werte je Modellfamilie: Feldschluessel, erstes Byte (nach dem Typbyte = 1), Ende (0 = bis Feldende)
namespace {
struct AkField {
  uint8_t key, b, e;
};
struct AkLayout {
  AkField soc, in, out, ac_in, ac_out, solar, dc_out, ac_on, dc_on, temp, remain;
  uint16_t cmd_ac, cmd_dc, cmd_sub;   // Befehle (0 = keiner)
};
}  // namespace
static const AkLayout AK_LAYOUT[4] = {
    // C1000(X), C800(X), F2600
    {{0xc1, 1, 0}, {0xaf, 1, 0}, {0xb0, 1, 0}, {0xa5, 1, 0}, {0xa6, 1, 0}, {0xae, 1, 0}, {0xb2, 2, 0},
     {0xbb, 1, 0}, {0xb2, 1, 2}, {0xbd, 1, 0}, {0xa4, 1, 0}, 0x404a, 0x404b, 0x4040},
    // C300(X)
    {{0xbb, 1, 0}, {0xad, 1, 0}, {0xae, 1, 0}, {0xa5, 1, 0}, {0xa6, 1, 0}, {0xac, 1, 0}, {0xab, 1, 0},
     {0xb7, 1, 0}, {0xc1, 1, 0}, {0xb9, 1, 0}, {0xa4, 1, 0}, 0x404a, 0x404b, 0x4040},
    // C1000 Gen 2 / C1000 Plus
    {{0xa5, 3, 4}, {0, 0, 0}, {0xa6, 1, 3}, {0xa6, 3, 5}, {0xa7, 2, 4}, {0xa8, 2, 0}, {0xb2, 2, 0},
     {0xa7, 1, 2}, {0xb2, 1, 2}, {0xa5, 1, 2}, {0, 0, 0}, 0x4101, 0x4102, 0x4100},
    // F3800
    {{0xad, 1, 0}, {0, 0, 0}, {0xb2, 1, 0}, {0xa5, 1, 0}, {0xa6, 1, 0}, {0xae, 1, 0}, {0, 0, 0},
     {0xbc, 1, 0}, {0xac, 1, 0}, {0xbe, 1, 0}, {0xa4, 1, 0}, 0x404a, 0x404b, 0x4040},
};

// Erkennung: Name "…SOLIX C1000…" bzw. Herstellerdaten 0xFFFF mit Produkttyp
static BmsModel anker_detect(const std::string &un, const std::string &md) {
  uint16_t pt = 0;
  if (md.size() >= 12 && (uint8_t)md[0] == 0xFF && (uint8_t)md[1] == 0xFF && ((uint8_t)md[2] == 1 || (uint8_t)md[2] == 2))
    pt = (uint8_t)md[10] << 8 | (uint8_t)md[11];   // Version, MAC 6, Bindung 1, Produkttyp 2
  bool solix = un.find("SOLIX") != std::string::npos;
  if (pt == 0xb118 || pt == 0xb119 || (solix && (un.find("GEN 2") != std::string::npos || un.find("C1000 PLUS") != std::string::npos)))
    return BMS_ANKER_G2;
  if (pt == 0xb106 || pt == 0xb112 || (solix && un.find("F3800") != std::string::npos)) return BMS_ANKER_F3800;
  if (solix && un.find("C300") != std::string::npos) return BMS_ANKER_C300;
  if (pt == 0xb103 || (solix && (un.find("C1000") != std::string::npos || un.find("C800") != std::string::npos ||
                                 un.find("F2600") != std::string::npos)))
    return BMS_ANKER;
  return BMS_UNKNOWN;
}
static const AkLayout &ak_layout(BmsModel m) {
  return AK_LAYOUT[m == BMS_ANKER_C300 ? 1 : m == BMS_ANKER_G2 ? 2 : m == BMS_ANKER_F3800 ? 3 : 0];
}

// ---- Bausteine
static void ak_tlv(std::string &p, uint8_t key, const uint8_t *v, size_t n) {
  p += (char)key;
  p += (char)n;
  p.append((const char *)v, n);
}
static void ak_tlv_ts(std::string &p, uint8_t key) {
  uint32_t t = (uint32_t)time(nullptr);
  uint8_t v[4] = {(uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24)};
  ak_tlv(p, key, v, 4);
}
static std::string ak_packet(const uint8_t pattern[3], uint16_t cmd, const std::string &payload) {
  std::string f;
  size_t len = 10 + payload.size();
  f += (char)0xFF;
  f += (char)0x09;
  f += (char)(len & 0xFF);
  f += (char)(len >> 8);
  f.append((const char *)pattern, 3);
  f += (char)(cmd >> 8);
  f += (char)(cmd & 0xFF);
  f += payload;
  uint8_t x = 0;
  for (char c : f) x ^= (uint8_t)c;
  f += (char)x;
  return f;
}

static std::string ak_cbc(bool enc, const uint8_t *key, const uint8_t *iv, const std::string &in) {
  std::string buf = in;
  if (enc) {   // PKCS#7
    size_t pad = 16 - buf.size() % 16;
    buf.append(pad, (char)pad);
  } else if (buf.size() % 16) {
    return "";
  }
  std::string out(buf.size(), '\0');
  if (!bl_cbc(enc, key, 128, iv, (const uint8_t *)buf.data(), (uint8_t *)&out[0], buf.size())) return "";
  if (!enc && !out.empty()) {
    uint8_t pad = (uint8_t)out.back();
    if (pad >= 1 && pad <= 16 && pad <= out.size()) out.resize(out.size() - pad);
  }
  return out;
}
static std::string ak_gcm(bool enc, const uint8_t *key, const uint8_t *nonce, const std::string &in) {
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  std::string out;
  if (mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128) == 0) {
    if (enc) {
      out.assign(in.size() + 16, '\0');
      if (mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, in.size(), nonce, 12, AK_GCM_AAD, 16,
                                    (const uint8_t *)in.data(), (uint8_t *)&out[0], 16, (uint8_t *)&out[in.size()]))
        out.clear();
    } else if (in.size() >= 16) {
      size_t n = in.size() - 16;
      out.assign(n, '\0');
      if (mbedtls_gcm_auth_decrypt(&g, n, nonce, 12, AK_GCM_AAD, 16, (const uint8_t *)in.data() + n, 16,
                                   (const uint8_t *)in.data(), (uint8_t *)&out[0]))
        out.clear();
    }
  }
  mbedtls_gcm_free(&g);
  return out;
}

static std::string ak_seal(const AkSess &s, const std::string &p) {
  if (s.enc_outer) return ak_gcm(true, s.keyed ? s.key : AK_GCM_KEY, s.keyed ? s.iv : AK_GCM_NONCE, p);
  return s.keyed ? ak_cbc(true, s.key, s.iv, p) : p;
}
static std::string ak_open(const AkSess &s, const std::string &p) {
  if (s.enc_outer) return ak_gcm(false, s.keyed ? s.key : AK_GCM_KEY, s.keyed ? s.iv : AK_GCM_NONCE, p);
  return s.keyed ? ak_cbc(false, s.key, s.iv, p) : p;
}

// Felder einer (entschluesselten) Nutzlast: Status (falls < 0xA1) und Schluessel -> Wert
namespace {
struct AkParams {
  int status = -1;
  std::vector<std::pair<uint8_t, std::string>> f;
  const std::string *get(uint8_t k) const {
    for (auto &x : f)
      if (x.first == k) return &x.second;
    return nullptr;
  }
};
}  // namespace
static AkParams ak_parse(const std::string &p) {
  AkParams r;
  size_t i = 0;
  if (!p.empty() && (uint8_t)p[0] < 0xA1) r.status = (uint8_t)p[i++];
  while (i + 2 <= p.size()) {
    uint8_t k = p[i], n = p[i + 1];
    if (i + 2 + n > p.size()) break;
    r.f.emplace_back(k, p.substr(i + 2, n));
    i += 2 + n;
  }
  return r;
}
static float ak_int(const AkParams &pp, const AkField &f, bool sgn = false) {
  if (!f.key) return NAN;
  const std::string *v = pp.get(f.key);
  if (!v) return NAN;
  size_t b = f.b, e = f.e ? f.e : v->size();
  if (e > v->size() || b >= e || e - b > 4) return NAN;
  uint32_t x = 0;
  for (size_t i = e; i-- > b;) x = x << 8 | (uint8_t)(*v)[i];   // Little Endian
  if (sgn && e - b < 4 && (x >> (8 * (e - b) - 1)) & 1) return (float)((int32_t)x - (1 << (8 * (e - b))));
  return (float)x;
}

static void ak_rx(const std::string &mac, const uint8_t *d, size_t n) {   // jede Meldung ist ein Paket (oder Bruchstueck)
  Lock l;
  Bat *b = find_mac(mac);
  if (!b || n < 11) return;
  if (b->ak_q.size() >= 12) b->ak_q.pop_front();
  b->ak_q.emplace_back((const char *)d, n);
}

static bool anker_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteCharacteristic *nt = nullptr, *wr = nullptr;
  for (auto *s : c->getServices(true)) {
    if (!nt) nt = s->getCharacteristic(NimBLEUUID("8c850003-0302-41c5-b46e-cf057c562025"));
    if (!wr) wr = s->getCharacteristic(NimBLEUUID("8c850002-0302-41c5-b46e-cf057c562025"));
  }
  if (!nt || !wr) {
    logf("%s: Anker-Kanaele 8c850002/8c850003 fehlen (aeltere Geraete mit Dienst 1780 werden nicht unterstuetzt)",
         b.name.c_str());
    return false;
  }
  {
    Lock l;
    Bat *x = find_mac(b.mac);
    if (x) {
      x->ak_q.clear();
      x->ak = AkSess();
      x->ak.enc_outer = x->ak_outer == 1;
    }
  }
  std::string mac = b.mac;
  if (!nt->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { ak_rx(mac, d, n); }, true))
    return false;
  b.wr = wr;
  b.next_poll = millis() + 300;
  if (!b.has) logf("%s: verbunden (Anker SOLIX)", b.name.c_str());
  return true;
}

// Ein Paket aus der Warteschlange holen; Bruchstuecke zusammensetzen. false = nichts da.
static bool ak_next(const std::string &mac, uint8_t &channel, uint16_t &cmd, std::string &payload) {
  for (;;) {
    std::string f;
    {
      Lock l;
      Bat *b = find_mac(mac);
      if (!b || b->ak_q.empty()) return false;
      f = b->ak_q.front();
      b->ak_q.pop_front();
    }
    if (f.size() < 11 || (uint8_t)f[0] != 0xFF || (uint8_t)f[1] != 0x09) continue;
    size_t len = (uint8_t)f[2] | (uint8_t)f[3] << 8;
    if (len < 10 || len + 1 > f.size()) continue;
    channel = (uint8_t)f[6];
    cmd = (uint8_t)f[7] << 8 | (uint8_t)f[8];
    std::string p = f.substr(9, len - 9);
    if (cmd & 0x8000) {   // Bruchstueck: erstes Byte = Index << 4 | Anzahl
      if (p.empty()) continue;
      uint8_t idx = (uint8_t)p[0] >> 4, tot = (uint8_t)p[0] & 0x0F;
      Lock l;
      Bat *b = find_mac(mac);
      if (!b) return false;
      if (idx == 1) b->ak_frag.clear();
      b->ak_frag += p.substr(1);
      if (idx != tot) continue;
      p = b->ak_frag;
      b->ak_frag.clear();
    }
    payload = p;
    return true;
  }
}

static bool ak_send(const std::string &mac, NimBLERemoteCharacteristic *wr, const AkSess &s, const uint8_t pat[3],
                    uint16_t msgtype, const std::string &params, bool force_enc = false) {
  bool enc = force_enc || s.enc_outer || s.keyed;
  std::string pl = enc ? ak_seal(s, params) : params;
  if (enc && pl.empty()) return false;
  std::string f = ak_packet(pat, (uint16_t)((enc ? 0x4000 : 0) | (msgtype & 0x0FFF)), pl);
  (void)mac;
  return wr->writeValue((const uint8_t *)f.data(), f.size(), true) || wr->writeValue((const uint8_t *)f.data(), f.size(), false);
}

static void ak_client_id(std::string &p, const AkSess &s) {   // a2 = Kennung, nur bei der offenen Huelle
  if (!s.enc_outer) ak_tlv(p, 0xa2, (const uint8_t *)AK_UUID, strlen(AK_UUID));
}

static std::string ak_tz() {
  const char *tz = getenv("TZ");
  return tz && *tz ? tz : "UTC0";
}

// Anmeldung abarbeiten, bis frei (true) oder Zeit um (false). s wird im Akku gespeichert (Dauerverbindung).
static bool ak_negotiate(const std::string &mac, NimBLERemoteCharacteristic *wr, AkSess &s) {
  static const uint8_t NEG[3] = {0x03, 0x00, 0x01};
  bool got_any = false;
  uint8_t priv[32] = {0};
  bool have_priv = false;
  uint32_t t_open = 0;
  for (uint32_t t0 = millis(); millis() - t0 < 25000 && !s.authorized;) {
    if (!t_open || (!got_any && millis() - t_open > 8000)) {   // Eroeffnung (erneut) senden
      std::string p;
      ak_tlv_ts(p, 0xa1);
      ak_client_id(p, s);
      ak_send(mac, wr, s, NEG, 0x001, p);
      t_open = millis();
    }
    uint8_t ch = 0;
    uint16_t cmd = 0;
    std::string pl;
    if (!ak_next(mac, ch, cmd, pl)) {
      delay(40);
      continue;
    }
    if (ch == 0x0F || ch == 0x11) {   // Werte-Meldung des Geraets: entschluesselbar -> freigeschaltet
      if ((cmd & 0x4000) && s.keyed && !ak_open(s, pl).empty()) s.authorized = true;
      continue;
    }
    if (ch != 0x01) continue;
    got_any = true;
    if (cmd & 0x4000) pl = ak_open(s, pl);
    AkParams pp = ak_parse(pl);
    uint16_t mt = cmd & 0x0FFF;
    auto u8 = [](uint8_t v) { return std::string(1, (char)v); };
    std::string p;
    switch (mt) {
      case 0x801: {
        ak_tlv_ts(p, 0xa1);
        ak_client_id(p, s);
        p += u8(0xa3) + u8(1) + u8(0x20);
        p += u8(0xa4) + u8(2) + u8(0x00) + u8(0xF0);
        ak_send(mac, wr, s, NEG, 0x003, p);
        break;
      }
      case 0x803: {
        if (auto *v = pp.get(0xa2))
          if (v->size() >= 2) s.mtu = (uint8_t)(*v)[0] | (uint8_t)(*v)[1] << 8;
        if (auto *v = pp.get(0xa5))
          if (!v->empty()) s.auth = (uint8_t)(*v)[0];
        ak_tlv_ts(p, 0xa1);
        ak_client_id(p, s);
        ak_send(mac, wr, s, NEG, 0x029, p);
        break;
      }
      case 0x829: {
        ak_tlv_ts(p, 0xa1);
        ak_client_id(p, s);
        p += u8(0xa3) + u8(1) + u8(0x20);
        uint16_t mtu = s.enc_outer && s.mtu ? s.mtu : 0xF000;
        p += u8(0xa4) + u8(2) + u8(mtu & 0xFF) + u8(mtu >> 8);
        p += u8(0xa5) + u8(1) + u8(s.enc_outer ? 0x44 : 0x40);
        if (s.enc_outer && s.auth >= 0) p += u8(0xa6) + u8(1) + u8((uint8_t)s.auth);
        ak_send(mac, wr, s, NEG, 0x005, p);
        break;
      }
      case 0x805: {   // eigenen P-256-Schluessel schicken (64 Byte ohne 04)
        mbedtls_ecp_group grp;
        mbedtls_mpi d;
        mbedtls_ecp_point Q;
        mbedtls_ecp_group_init(&grp);
        mbedtls_mpi_init(&d);
        mbedtls_ecp_point_init(&Q);
        uint8_t pub[65];
        size_t ol = 0;
        bool ok = !mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) &&
                  !mbedtls_ecp_gen_keypair(&grp, &d, &Q, bl_rng, nullptr) &&
                  !mbedtls_ecp_point_write_binary(&grp, &Q, MBEDTLS_ECP_PF_UNCOMPRESSED, &ol, pub, sizeof(pub)) && ol == 65 &&
                  !mbedtls_mpi_write_binary(&d, priv, 32);
        mbedtls_ecp_point_free(&Q);
        mbedtls_mpi_free(&d);
        mbedtls_ecp_group_free(&grp);
        if (!ok) return false;
        have_priv = true;
        ak_tlv(p, 0xa1, pub + 1, 64);
        ak_send(mac, wr, s, NEG, 0x021, p);
        break;
      }
      case 0x821: {   // Schluessel des Geraets -> gemeinsames Geheimnis
        const std::string *v = pp.get(0xa1);
        if (!v || v->size() < 64 || !have_priv) return false;
        uint8_t peer[65] = {0x04}, z[32];
        memcpy(peer + 1, v->data() + v->size() - 64, 64);
        mbedtls_ecp_group grp;
        mbedtls_mpi d, Z;
        mbedtls_ecp_point P;
        mbedtls_ecp_group_init(&grp);
        mbedtls_mpi_init(&d);
        mbedtls_mpi_init(&Z);
        mbedtls_ecp_point_init(&P);
        bool ok = !mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) && !mbedtls_mpi_read_binary(&d, priv, 32) &&
                  !mbedtls_ecp_point_read_binary(&grp, &P, peer, 65) &&
                  !mbedtls_ecdh_compute_shared(&grp, &Z, &P, &d, bl_rng, nullptr) && !mbedtls_mpi_write_binary(&Z, z, 32);
        mbedtls_ecp_point_free(&P);
        mbedtls_mpi_free(&d);
        mbedtls_mpi_free(&Z);
        mbedtls_ecp_group_free(&grp);
        if (!ok) return false;
        memcpy(s.key, z, 16);
        memcpy(s.iv, z + 16, 16);
        s.keyed = true;
        if (!s.enc_outer) s.authorized = true;   // offene Huelle: mit dem Schluesseltausch freigeschaltet
        ak_tlv_ts(p, 0xa1);
        ak_client_id(p, s);
        int32_t west = 0;
        {
          time_t now = time(nullptr);
          struct tm lt, gt;
          localtime_r(&now, &lt);
          gmtime_r(&now, &gt);
          lt.tm_isdst = 0;
          west = (int32_t)difftime(mktime(&gt), mktime(&lt));
        }
        uint8_t w[4] = {(uint8_t)west, (uint8_t)(west >> 8), (uint8_t)(west >> 16), (uint8_t)(west >> 24)};
        ak_tlv(p, 0xa3, w, 4);
        std::string tz = ak_tz();
        ak_tlv(p, 0xa5, (const uint8_t *)tz.data(), tz.size());
        ak_send(mac, wr, s, NEG, 0x022, p);
        break;
      }
      case 0x822:
        if (!s.authorized) {
          ak_tlv_ts(p, 0xa1);
          ak_tlv(p, 0xa2, (const uint8_t *)AK_UUID, strlen(AK_UUID));
          ak_send(mac, wr, s, NEG, 0x027, p);
        }
        break;
      case 0x827:
        if (pp.status == 0) s.authorized = true;
        else if (pp.status == 9) logf("%s: Anker wartet auf Freigabe – am Geraet die Taste druecken", mac.c_str());
        else logf("%s: Anker lehnt die Anmeldung ab (Status %d)", mac.c_str(), pp.status);
        break;
      default: break;
    }
  }
  if (!got_any) logf("%s: Anker antwortet nicht auf die %s Anmeldung", mac.c_str(), s.enc_outer ? "verschluesselte" : "offene");
  s.replied = got_any;
  return s.authorized;
}

// Werte-Meldung (Kanal 0x0F/0x11) suchen und auswerten
static bool ak_telemetry(const std::string &mac, const AkSess &s, BmsModel m, float &soc, float &temp, Extra &ex,
                         uint32_t wait_ms) {
  const AkLayout &L = ak_layout(m);
  for (uint32_t t0 = millis(); millis() - t0 < wait_ms;) {
    uint8_t ch = 0;
    uint16_t cmd = 0;
    std::string pl;
    if (!ak_next(mac, ch, cmd, pl)) {
      delay(40);
      continue;
    }
    if (ch != 0x0F && ch != 0x11) continue;
    if (cmd & 0x4000) pl = ak_open(s, pl);
    AkParams pp = ak_parse(pl);
    if (pp.f.size() < 6) continue;   // keine Werte-Meldung (z. B. Bestaetigung eines Befehls)
    float v = ak_int(pp, L.soc);
    if (isnan(v) || v > 100) continue;
    soc = v;
    temp = ak_int(pp, L.temp, true);
    ex.ps_in = ak_int(pp, L.in);
    ex.ps_out = ak_int(pp, L.out);
    ex.ps_ac_in = ak_int(pp, L.ac_in);
    ex.ps_ac_out = ak_int(pp, L.ac_out);
    ex.ps_dc_in = ak_int(pp, L.solar);
    ex.ps_dc_out = ak_int(pp, L.dc_out);
    float ac = ak_int(pp, L.ac_on), dc = ak_int(pp, L.dc_on);
    ex.ac_on = isnan(ac) ? -1 : ac == 1 ? 1 : 0;   // 1 = Ausgang an
    ex.dc_on = isnan(dc) ? -1 : dc == 1 ? 1 : 0;
    float rm = ak_int(pp, L.remain);
    ex.remain = isnan(rm) ? -1 : (int)lroundf(rm / 10.0f * 60.0f);   // Zehntelstunden
    return true;
  }
  return false;
}

// Befehl im Sitzungskanal (Muster 03 00 0F): a1 = 21, a2 = Wert (Typ 1), fe = Zeitstempel (Typ 3)
static bool ak_command(const std::string &mac, NimBLERemoteCharacteristic *wr, const AkSess &s, uint16_t cmd, int value) {
  static const uint8_t SES[3] = {0x03, 0x00, 0x0F};
  std::string p;
  p += (char)0xa1;
  p += (char)1;
  p += (char)0x21;
  if (value >= 0) {
    p += (char)0xa2;
    p += (char)2;
    p += (char)1;
    p += (char)value;
  }
  uint32_t t = (uint32_t)time(nullptr);
  p += (char)0xfe;
  p += (char)5;
  p += (char)3;
  for (int i = 0; i < 4; i++) p += (char)(t >> (8 * i));
  return ak_send(mac, wr, s, SES, cmd, p, true);
}

// ---------------------------------------------------------------- EcoFlow
// Nach rabits/ha-ef-ble (Apache-2.0). Dauerverbindung, Werte kommen als Meldungen. Voraussetzung: die EcoFlow-
// Benutzer-ID (Konto, an das die Power Station in der App gebunden ist) – im Editor per Anmeldung abrufbar.
// Werbedaten (Hersteller-ID 0xB5B5): Version, Seriennummer (16 Zeichen), Status, Produkt, …, Faehigkeiten
// (Bits 3..5 = Verschluesselungsart). Verschluesselungsart 7: Rahmen 5A 5A <Typ<<4> 01 <Laenge LE> <Daten> <CRC16>;
// Schluesseltausch secp160r1 (micro-ecc), Sitzungsschluessel = MD5(Schluesseltabelle[Seed] + Zufall), AES-128-CBC.
// Innenpaket: AA <Version> <Laenge LE> <CRC8> 0D <Seq 4> 00 00 <Quelle> <Ziel> [<dQuelle> <dZiel>] <Satz> <Befehl>
// <Daten> <CRC16>. Anmeldung: Paket 35/89 (Status), dann 35/86 mit MD5(Benutzer-ID + Seriennummer) als Hex-Text.
// Werte: Delta 2 / River 2 feste Byte-Strukturen (PD 02/20/02, Wechselrichter 04/xx/02), River 3 / Delta 3 Protobuf
// (02/FE/15, DisplayPropertyUpload, nur geaenderte Felder). Schalten: Delta 2/River 2 Paket 20/42 (AC) bzw. 20/51
// (12 V), River 3/Delta 3 ConfigWrite (FE/11) Feld 76 (AC) bzw. 18 (12 V).
#if BMS_ECOFLOW_ON
#include "third_party/ecoflow/ef_keydata.h"
#include "third_party/uecc_ef.h"

static std::string s_ef_uid;   // EcoFlow-Benutzer-ID (NVS "bms" "efuid")

static uint8_t ef_crc8(const uint8_t *d, size_t n) {
  uint8_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x07) : (uint8_t)(c << 1);
  }
  return c;
}
static uint16_t ef_crc16(const uint8_t *d, size_t n) {   // CRC-16/ARC
  uint16_t c = 0;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xA001 : (c >> 1);
  }
  return c;
}

// Seriennummer -> Modellfamilie
static BmsModel ecoflow_model_of(const std::string &sn) {
  static const char *V2[] = {"R331", "R335", "R351", "R354", "R601", "R603", "R611", "R613", "R621", "R623"};
  static const char *PB[] = {"R651", "R653", "R654", "R655", "R631", "R634", "R635", "P231", "P321", "P322", "P351"};
  for (const char *p : V2)
    if (sn.rfind(p, 0) == 0) return BMS_ECOFLOW;
  for (const char *p : PB)
    if (sn.rfind(p, 0) == 0) return BMS_ECOFLOW_PB;
  return BMS_UNKNOWN;
}

// Werbedaten auswerten (md inkl. Hersteller-ID): Modell, Seriennummer, Verschluesselungsart
static BmsModel ecoflow_detect(const std::string &md, std::string &sn, uint8_t &enc) {
  if (md.size() < 2 + 17 || (uint8_t)md[0] != 0xB5 || (uint8_t)md[1] != 0xB5) return BMS_UNKNOWN;
  sn = md.substr(3, 16);
  uint8_t cap = md.size() > 2 + 22 ? (uint8_t)md[2 + 22] : 0x38;
  enc = (cap >> 3) & 7;
  return ecoflow_model_of(sn);
}

// Innenpaket bauen
static std::string ef_packet(uint8_t src, uint8_t dst, uint8_t cmd_set, uint8_t cmd_id, const std::string &payload,
                             uint8_t version) {
  std::string d;
  d += (char)0xAA;
  d += (char)version;
  d += (char)(payload.size() & 0xFF);
  d += (char)(payload.size() >> 8);
  d += (char)ef_crc8((const uint8_t *)d.data(), 4);
  d += (char)0x0D;
  d.append(4, '\0');   // Folgenummer
  d.append(2, '\0');
  d += (char)src;
  d += (char)dst;
  if ((version & 0x0F) >= 3) {
    d += (char)1;
    d += (char)1;
  }
  d += (char)cmd_set;
  d += (char)cmd_id;
  d += payload;
  uint16_t c = ef_crc16((const uint8_t *)d.data(), d.size());
  d += (char)(c & 0xFF);
  d += (char)(c >> 8);
  return d;
}

// Aeusserer Rahmen (Verschluesselungsart 7): Typ 0 = Befehl (Schluesseltausch, unverschluesselt), 1 = Protokoll
static std::string ef_frame(uint8_t frame_type, const std::string &payload) {
  std::string d;
  d += (char)0x5A;
  d += (char)0x5A;
  d += (char)(frame_type << 4);
  d += (char)0x01;
  d += (char)((payload.size() + 2) & 0xFF);
  d += (char)((payload.size() + 2) >> 8);
  d += payload;
  uint16_t c = ef_crc16((const uint8_t *)d.data(), d.size());
  d += (char)(c & 0xFF);
  d += (char)(c >> 8);
  return d;
}

static std::string ef_wrap(const EfSess &s, const std::string &pkt) {
  if (s.enc == 7) return ef_frame(1, ak_cbc(true, s.key, s.iv, pkt));
  if (s.enc == 1) {   // 5 Byte Kopf offen, Rest mit Nullen auf 16 und AES-CBC
    std::string inner = pkt.substr(5);
    inner.append((16 - inner.size() % 16) % 16, '\0');
    std::string out(inner.size(), '\0');
    if (!bl_cbc(true, s.key, 128, s.iv, (const uint8_t *)inner.data(), (uint8_t *)&out[0], inner.size())) return "";
    return pkt.substr(0, 5) + out;
  }
  return pkt;
}

static bool ef_write(NimBLERemoteCharacteristic *wr, const EfSess &s, const std::string &frame) {
  if (frame.empty()) return false;
  return wr->writeValue((const uint8_t *)frame.data(), frame.size(), s.enc == 7);
}

static void ef_rxcb(const std::string &mac, const uint8_t *d, size_t n) {
  Lock l;
  Bat *b = find_mac(mac);
  if (!b) return;
  if (b->ef_rx.size() + n > 6000) b->ef_rx.clear();
  b->ef_rx.append((const char *)d, n);
}

namespace {
struct EfPkt {
  uint8_t src = 0, dst = 0, cmd_set = 0, cmd_id = 0;
  std::string payload;
};
}  // namespace

// Innenpaket zerlegen (xor: River 3/Delta 3 verschleiern die Daten mit dem ersten Byte der Folgenummer)
static bool ef_parse_inner(const std::string &d, bool xor_payload, EfPkt &p) {
  const uint8_t *b = (const uint8_t *)d.data();
  if (d.size() < 18 || b[0] != 0xAA || b[1] == 4) return false;
  if (ef_crc8(b, 4) != b[4]) return false;
  uint8_t ver = b[1] & 0x0F;
  size_t plen = b[2] | b[3] << 8, start = ver == 2 ? 16 : 18;
  if (start + plen > d.size()) return false;
  p.src = b[12];
  p.dst = b[13];
  p.cmd_set = b[start - 2];
  p.cmd_id = b[start - 1];
  p.payload = d.substr(start, plen);
  if (xor_payload && b[6])
    for (auto &c : p.payload) c ^= (char)b[6];
  if ((b[1] & 0x10) && p.payload.size() >= 2 && (uint8_t)p.payload[p.payload.size() - 1] == 0xBB &&
      (uint8_t)p.payload[p.payload.size() - 2] == 0xBB)
    p.payload.resize(p.payload.size() - 2);
  return true;
}

// Alles Empfangene zerlegen: einfache Rahmen (Schluesseltausch) nach simple, Pakete nach pkts
static void ef_drain(const std::string &mac, const EfSess &s, bool xor_payload, std::vector<std::string> *simple,
                     std::vector<EfPkt> &pkts) {
  std::string buf;
  {
    Lock l;
    Bat *b = find_mac(mac);
    if (!b) return;
    buf.swap(b->ef_rx);
  }
  size_t i = 0;
  while (i < buf.size()) {
    const uint8_t *d = (const uint8_t *)buf.data();
    if (s.enc == 7) {
      size_t st = buf.find("\x5A\x5A", i);
      if (st == std::string::npos) {
        i = buf.size();
        break;
      }
      i = st;
      if (buf.size() - i < 8) break;
      size_t plen = d[i + 4] | d[i + 5] << 8, end = i + 6 + plen;
      if (plen < 2 || plen > 5000) {
        i += 2;
        continue;
      }
      if (end > buf.size()) break;
      uint16_t c = ef_crc16(d + i, 6 + plen - 2);
      if ((d[end - 2] | d[end - 1] << 8) != c) {
        i += 2;
        continue;
      }
      std::string data = buf.substr(i + 6, plen - 2);
      uint8_t ft = d[i + 2] >> 4;
      i = end;
      if (ft == 0) {
        if (simple) simple->push_back(data);
        continue;
      }
      if (!s.keyed) continue;
      std::string plain = ak_cbc(false, s.key, s.iv, data.substr(0, data.size() - data.size() % 16));
      EfPkt p;
      if (ef_parse_inner(plain, xor_payload, p)) pkts.push_back(p);
    } else {
      size_t st = buf.find('\xAA', i);
      if (st == std::string::npos) {
        i = buf.size();
        break;
      }
      i = st;
      if (buf.size() - i < 5) break;
      if (ef_crc8(d + i, 4) != d[i + 4]) {
        i++;
        continue;
      }
      size_t plen = d[i + 2] | d[i + 3] << 8;
      size_t inner = ((d[i + 1] & 0x0F) >= 3 ? 15 : 13) + plen;
      if (s.enc == 1) {
        size_t enc_len = (inner + 15) / 16 * 16;
        if (buf.size() - i < 5 + enc_len) break;
        std::string body(enc_len, '\0');
        if (bl_cbc(false, s.key, 128, s.iv, d + i + 5, (uint8_t *)&body[0], enc_len)) {
          EfPkt p;
          if (ef_parse_inner(buf.substr(i, 5) + body.substr(0, inner), xor_payload, p)) pkts.push_back(p);
        }
        i += 5 + enc_len;
      } else {
        if (buf.size() - i < 5 + inner) break;
        EfPkt p;
        if (ef_parse_inner(buf.substr(i, 5 + inner), xor_payload, p)) pkts.push_back(p);
        i += 5 + inner;
      }
    }
  }
  if (i < buf.size()) {   // Rest (unvollstaendig) zurueck in den Puffer, vor neu Empfangenes
    Lock l;
    Bat *b = find_mac(mac);
    if (b) b->ef_rx = buf.substr(i) + b->ef_rx;
  }
}

// ---- Protobuf (nur was gebraucht wird)
static bool pb_varint(const uint8_t *&p, const uint8_t *e, uint64_t &v) {
  v = 0;
  for (int sh = 0; p < e && sh < 64; sh += 7) {
    uint8_t c = *p++;
    v |= (uint64_t)(c & 0x7F) << sh;
    if (!(c & 0x80)) return true;
  }
  return false;
}

// Werte aus einem Paket in ex/soc/temp uebernehmen (nur vorhandene Felder aendern)
static bool ef_values(BmsModel m, const EfPkt &p, Extra &ex, float &soc, float &temp, float &volt) {
  const uint8_t *d = (const uint8_t *)p.payload.data();
  size_t n = p.payload.size();
  auto u16 = [&](size_t o) -> uint16_t { return d[o] | d[o + 1] << 8; };
  auto u32 = [&](size_t o) -> uint32_t { return d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
  if (m == BMS_ECOFLOW) {
    if (p.src == 0x02 && p.cmd_set == 0x20 && p.cmd_id == 0x02 && n >= 35) {   // PD-Herzschlag
      soc = d[14];
      ex.ps_out = u16(15);
      ex.ps_in = u16(17);
      int32_t rt = (int32_t)u32(19);
      ex.remain = rt > 0 ? rt : -1;
      ex.dc_on = d[33] == 1;   // 12-V-Ausgang (Auto)
      ex.ps_dc_out = d[34];
      return true;
    }
    if (p.src == 0x04 && p.cmd_id == 0x02 && n >= 13) {   // Wechselrichter
      ex.ps_ac_in = u16(9);
      ex.ps_ac_out = u16(11);
      if (n >= 46) ex.ac_on = d[45] == 1;
      return true;
    }
    if (p.src == 0x03 && p.cmd_set == 0x20 && p.cmd_id == 0x32 && n >= 57) {   // Akku
      volt = u32(12) / 1000.0f;
      temp = d[20];
      float f;
      memcpy(&f, d + 53, 4);
      if (isfinite(f) && f >= 0 && f <= 100) soc = f;
      return true;
    }
    return false;
  }
  if (!(p.src == 0x02 && p.cmd_set == 0xFE && p.cmd_id == 0x15)) return false;   // DisplayPropertyUpload
  const uint8_t *q = d, *e = d + n;
  bool any = false;
  while (q < e) {
    uint64_t key, v = 0;
    if (!pb_varint(q, e, key)) break;
    uint32_t f = (uint32_t)(key >> 3), wt = key & 7;
    float fv = NAN;
    if (wt == 0) {
      if (!pb_varint(q, e, v)) break;
    } else if (wt == 5) {
      if (e - q < 4) break;
      memcpy(&fv, q, 4);
      q += 4;
    } else if (wt == 1) {
      if (e - q < 8) break;
      q += 8;
      continue;
    } else if (wt == 2) {
      if (!pb_varint(q, e, v) || (uint64_t)(e - q) < v) break;
      q += v;
      continue;
    } else {
      break;
    }
    any = true;
    switch (f) {
      case 262: if (isfinite(fv)) soc = fv; break;                              // cms_batt_soc
      case 242: if (isfinite(fv) && isnan(soc)) soc = fv; break;                // bms_batt_soc
      case 3: if (isfinite(fv)) ex.ps_in = fabsf(fv); break;                    // pow_in_sum_w
      case 4: if (isfinite(fv)) ex.ps_out = fabsf(fv); break;                   // pow_out_sum_w
      case 54: if (isfinite(fv)) ex.ps_ac_in = fabsf(fv); break;                // pow_get_ac_in
      case 368: if (isfinite(fv)) ex.ps_ac_out = fabsf(fv); break;              // pow_get_ac_out (negativ)
      case 361: if (isfinite(fv)) ex.ps_dc_in = fabsf(fv); break;               // pow_get_pv
      case 37: if (isfinite(fv)) ex.ps_dc_out = fabsf(fv); break;               // pow_get_12v
      case 367: ex.ac_on = (v & 3) >= 2; break;                                 // flow_info_ac_out
      case 33: ex.dc_on = (v & 3) >= 2; break;                                  // flow_info_12v
      case 268: ex.remain = v ? (int)v : -1; break;                             // cms_dsg_rem_time (min)
      case 259: temp = (float)(int32_t)v; break;                                // bms_max_cell_temp
      default: break;
    }
  }
  return any;
}

static bool ecoflow_connect(Bat &b, NimBLEClient *c) {
  NimBLERemoteCharacteristic *nt = nullptr, *wr = nullptr;
  for (auto *s : c->getServices(true)) {
    if (!nt) nt = s->getCharacteristic(NimBLEUUID("6e400003-b5a3-f393-e0a9-e50e24dcca9e"));
    if (!wr) wr = s->getCharacteristic(NimBLEUUID("6e400002-b5a3-f393-e0a9-e50e24dcca9e"));
  }
  if (!nt || !wr) {
    for (auto *s : c->getServices(true)) {
      if (!nt) nt = s->getCharacteristic(NimBLEUUID("00000003-0000-1000-8000-00805f9b34fb"));
      if (!wr) wr = s->getCharacteristic(NimBLEUUID("00000002-0000-1000-8000-00805f9b34fb"));
    }
  }
  if (!nt || !wr) {
    logf("%s: EcoFlow-Kanaele (Nordic UART bzw. 0002/0003) fehlen", b.name.c_str());
    return false;
  }
  {
    Lock l;
    Bat *x = find_mac(b.mac);
    if (x) {
      x->ef_rx.clear();
      x->ef = EfSess();
      x->ef.enc = x->ef_enc;
    }
  }
  std::string mac = b.mac;
  if (!nt->subscribe(true, [mac](NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) { ef_rxcb(mac, d, n); }, true))
    return false;
  b.wr = wr;
  b.next_poll = millis() + 300;
  if (!b.has) logf("%s: verbunden (EcoFlow, Verschluesselung %u)", b.name.c_str(), b.ef_enc);
  return true;
}

static int ef_rng(uint8_t *dest, unsigned size) {
  esp_fill_random(dest, size);
  return 1;
}

// Anmeldung; true = Benutzer-ID angenommen. Werte, die dabei schon ankommen, landen in ex/soc/temp/volt.
static bool ef_auth(const std::string &mac, NimBLERemoteCharacteristic *wr, EfSess &s, BmsModel m, const std::string &sn,
                    Extra &ex, float &soc, float &temp, float &volt) {
  bool xr = m == BMS_ECOFLOW_PB;
  uint8_t ver = m == BMS_ECOFLOW ? 2 : 3;
  std::vector<std::string> simple;
  std::vector<EfPkt> pkts;
  auto wait_simple = [&](uint8_t type, size_t minlen, std::string &out) -> bool {
    for (uint32_t t0 = millis(); millis() - t0 < 8000;) {
      simple.clear();
      pkts.clear();
      ef_drain(mac, s, xr, &simple, pkts);
      for (auto &x : simple)
        if (!x.empty() && (uint8_t)x[0] == type && x.size() >= minlen) {
          out = x;
          return true;
        }
      delay(40);
    }
    return false;
  };
  if (s.enc == 7) {
    uECC_set_rng(ef_rng);
    uint8_t pub[40], priv[21], z[20];
    if (!uECC_make_key(pub, priv, uECC_secp160r1())) return false;
    std::string req("\x01\x00", 2);
    req.append((const char *)pub, 40);
    ef_write(wr, s, ef_frame(0, req));
    std::string r;
    if (!wait_simple(0x01, 43, r)) {
      logf("%s: EcoFlow antwortet nicht auf den Schluesseltausch", mac.c_str());
      return false;
    }
    if (!uECC_shared_secret((const uint8_t *)r.data() + 3, priv, z, uECC_secp160r1())) {
      logf("%s: EcoFlow-Schluessel ungueltig", mac.c_str());
      return false;
    }
    uint8_t k1[16], iv[16];
    memcpy(k1, z, 16);
    mbedtls_md5(z, 20, iv);
    ef_write(wr, s, ef_frame(0, std::string("\x02", 1)));
    if (!wait_simple(0x02, 33, r)) {
      logf("%s: EcoFlow schickt keinen Sitzungsschluessel", mac.c_str());
      return false;
    }
    std::string enc = r.substr(1);
    enc.resize(enc.size() - enc.size() % 16);
    std::string dec(enc.size(), '\0');
    if (enc.size() < 32 || !bl_cbc(false, k1, 128, iv, (const uint8_t *)enc.data(), (uint8_t *)&dec[0], enc.size()))
      return false;
    const uint8_t *dd = (const uint8_t *)dec.data();
    size_t pos = (size_t)dd[16] * 0x10 + (size_t)((dd[17] - 1) & 0xFF) * 0x100;
    if (pos + 16 > EF_KEYDATA_LEN) {
      logf("%s: EcoFlow-Schluesselposition ausserhalb der Tabelle", mac.c_str());
      return false;
    }
    uint8_t kd[32];
    memcpy(kd, EF_KEYDATA + pos, 16);
    memcpy(kd + 16, dd, 16);
    mbedtls_md5(kd, 32, s.key);
    memcpy(s.iv, iv, 16);
    s.keyed = true;
  } else if (s.enc == 1) {
    std::string rev(sn.rbegin(), sn.rend());
    mbedtls_md5((const uint8_t *)sn.data(), sn.size(), s.key);
    mbedtls_md5((const uint8_t *)rev.data(), rev.size(), s.iv);
    s.keyed = true;
  }
  // Status abfragen, dann mit MD5(Benutzer-ID + Seriennummer) anmelden
  ef_write(wr, s, ef_wrap(s, ef_packet(0x21, 0x35, 0x35, 0x89, "", ver)));
  delay(300);
  uint8_t md[16];
  std::string idsn = s_ef_uid + sn;
  mbedtls_md5((const uint8_t *)idsn.data(), idsn.size(), md);
  char hex[33];
  for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02X", md[i]);
  ef_write(wr, s, ef_wrap(s, ef_packet(0x21, 0x35, 0x35, 0x86, std::string(hex, 32), ver)));
  for (uint32_t t0 = millis(); millis() - t0 < 10000;) {
    pkts.clear();
    ef_drain(mac, s, xr, nullptr, pkts);
    for (auto &p : pkts) {
      if (p.src == 0x35 && p.cmd_set == 0x35 && p.cmd_id == 0x86) {
        int code = p.payload.empty() ? 0 : (uint8_t)p.payload[0];
        if (code == 0) {
          s.authed = true;
          return true;
        }
        static const char *ERR[] = {"", "Token erneuern", "Geraetefehler", "an ein anderes Konto gebunden",
                                    "erst in der App einrichten", "Sendefehler", "falscher Schluessel (Benutzer-ID?)",
                                    "zu viele Geraete"};
        logf("%s: EcoFlow lehnt die Anmeldung ab: %s", mac.c_str(), code < 8 ? ERR[code] : "unbekannt");
        return false;
      }
      if (ef_values(m, p, ex, soc, temp, volt)) {   // Werte kommen: angemeldet
        s.authed = true;
        return true;
      }
    }
    delay(50);
  }
  logf("%s: EcoFlow bestaetigt die Anmeldung nicht", mac.c_str());
  return false;
}

// Ausgang schalten
static void ef_switch(NimBLERemoteCharacteristic *wr, const EfSess &s, BmsModel m, const std::string &sn, bool ac, bool on) {
  if (m == BMS_ECOFLOW) {
    if (ac) {
      uint8_t dst = sn.rfind("R351", 0) == 0 || sn.rfind("R354", 0) == 0 ? 0x04 : 0x05;   // Delta 2 Max: 0x04
      std::string pl(7, (char)0xFF);
      pl[0] = on ? 1 : 0;
      ef_write(wr, s, ef_wrap(s, ef_packet(0x21, dst, 0x20, 0x42, pl, 2)));
    } else {
      ef_write(wr, s, ef_wrap(s, ef_packet(0x21, 0x05, 0x20, 0x51, std::string(1, on ? 1 : 0), 2)));
    }
    return;
  }
  // ConfigWrite: Feld 76 (AC) bzw. 18 (12 V), bool
  std::string pl = ac ? std::string("\xE0\x04", 2) : std::string("\x90\x01", 2);
  pl += (char)(on ? 1 : 0);
  ef_write(wr, s, ef_wrap(s, ef_packet(0x20, 0x02, 0xFE, 0x11, pl, 0x13)));
}

// ---- Anmeldung beim EcoFlow-Konto (holt nur die Benutzer-ID; Passwort wird nicht gespeichert)
#include <ArduinoJson.h>
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
static std::atomic<int> s_ef_login_state{0};   // 0 nichts, 1 laeuft, 2 ok, 3 Fehler
static std::string s_ef_login_text;
namespace {
struct EfLoginReq {
  std::string email, pass;
};
}  // namespace
static std::string b64(const std::string &in) {
  static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
    o += T[v >> 18];
    o += T[(v >> 12) & 63];
    o += T[(v >> 6) & 63];
    o += T[v & 63];
  }
  if (i < in.size()) {
    uint32_t v = (uint8_t)in[i] << 16 | (i + 1 < in.size() ? (uint8_t)in[i + 1] << 8 : 0);
    o += T[v >> 18];
    o += T[(v >> 12) & 63];
    o += i + 1 < in.size() ? T[(v >> 6) & 63] : '=';
    o += '=';
  }
  return o;
}
static void ef_login_task(void *arg) {
  EfLoginReq *r = (EfLoginReq *)arg;
  JsonDocument q;
  q["email"] = r->email;
  q["password"] = b64(r->pass);
  q["scene"] = "IOT_APP";
  q["appVersion"] = "1.0.0";
  q["oauth"]["bundleId"] = "com.ef.EcoFlow";
  q["userType"] = "ECOFLOW";
  std::string body;
  serializeJson(q, body);
  r->pass.assign(r->pass.size(), '\0');
  delete r;
  esp_http_client_config_t cfg = {};
  cfg.url = "https://api.ecoflow.com/auth/login";
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = 20000;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 1024;
  std::string resp, err;
  int status = 0;
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c) {
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_header(c, "Accept", "application/json");
    if (esp_http_client_open(c, body.size()) == ESP_OK && esp_http_client_write(c, body.data(), body.size()) == (int)body.size()) {
      esp_http_client_fetch_headers(c);
      status = esp_http_client_get_status_code(c);
      char buf[512];
      int n;
      while ((n = esp_http_client_read(c, buf, sizeof(buf))) > 0 && resp.size() < 8192) resp.append(buf, n);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
  }
  JsonDocument a;
  std::string uid;
  if (status == 200 && !deserializeJson(a, resp)) {
    JsonVariant id = a["data"]["user"]["userId"];
    if (std::string(a["code"] | "") == "0")
      uid = id.is<const char *>() ? std::string(id.as<const char *>()) : id.is<long long>() ? std::to_string(id.as<long long>()) : "";
    else err = std::string("EcoFlow: ") + (const char *)(a["message"] | "Anmeldung abgelehnt");
  } else {
    err = status ? "EcoFlow-Server antwortet mit Fehler " + std::to_string(status) : "EcoFlow-Server nicht erreichbar";
  }
  if (!uid.empty()) {
    bms_ef_set_uid(uid);
    Lock l;
    s_ef_login_text = "Angemeldet, Benutzer-ID gespeichert";
    s_ef_login_state = 2;
  } else {
    Lock l;
    s_ef_login_text = err.empty() ? "Keine Benutzer-ID erhalten" : err;
    s_ef_login_state = 3;
  }
  s_ver++;
  vTaskDelete(nullptr);
}
#endif
