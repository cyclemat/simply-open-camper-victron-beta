#include "i18n.h"

#include <string.h>
#include <unordered_map>

#define I18N_STR2(x) #x
#define I18N_STR(x) I18N_STR2(x)

struct TrEntry {
  const char *de, *tx;
};

// Tabelle der gewaehlten Sprache; Deutsch braucht keine
static const TrEntry TABLE[] = {
#if __has_include(I18N_STR(PANEL_LANG.h))
#include I18N_STR(PANEL_LANG.h)
#endif
    {nullptr, nullptr}};

namespace {
struct Hash {
  size_t operator()(const char *s) const {
    size_t h = 2166136261u;   // FNV-1a
    while (*s) h = (h ^ (unsigned char)*s++) * 16777619u;
    return h;
  }
};
struct Eq {
  bool operator()(const char *a, const char *b) const { return strcmp(a, b) == 0; }
};
}  // namespace

static std::unordered_map<const char *, const char *, Hash, Eq> *s_map = nullptr;

static void build() {
  s_map = new std::unordered_map<const char *, const char *, Hash, Eq>();
  for (const TrEntry *e = TABLE; e->de; e++)
    if (e->tx && e->tx[0]) (*s_map)[e->de] = e->tx;
}

const char *tr(const char *de) {
  if (!de || !de[0]) return de;
  if (!s_map) build();
  if (s_map->empty()) return de;
  auto it = s_map->find(de);
  return it == s_map->end() ? de : it->second;
}

const char *i18n_lang() { return I18N_STR(PANEL_LANG); }

std::string tr_parts(const std::string &de) {
  if (!s_map) build();
  if (s_map->empty() || de.empty()) return de;
  auto whole = s_map->find(de.c_str());
  if (whole != s_map->end()) return whole->second;
  // Trenner: Mittelpunkt mit Leerraum ("  ·  " bzw. " · ") und Komma mit Leerzeichen
  std::string out, part;
  size_t i = 0;
  auto flush = [&] {
    // Leerraum am Rand erhalten, nur den Kern nachschlagen
    size_t a = part.find_first_not_of(' '), b = part.find_last_not_of(' ');
    if (a == std::string::npos) { out += part; part.clear(); return; }
    std::string core = part.substr(a, b - a + 1);
    out += part.substr(0, a) + tr(core.c_str()) + part.substr(b + 1);
    part.clear();
  };
  while (i < de.size()) {
    if (de.compare(i, 2, "\xC2\xB7") == 0) { flush(); out += "\xC2\xB7"; i += 2; continue; }
    if (de.compare(i, 2, ", ") == 0) { flush(); out += ", "; i += 2; continue; }
    part += de[i++];
  }
  flush();
  return out;
}

std::string tr_lines(const char *de) {
  std::string out, line;
  for (const char *p = de;; p++) {
    if (*p == '\n' || !*p) {
      out += tr(line.c_str());
      if (!*p) break;
      out += '\n';
      line.clear();
    } else {
      line += *p;
    }
  }
  return out;
}
