// Pure URL helpers behind NetDll_XHttpCrackUrl / XHttpCreateUrl (WinHttpCrackUrl / WinHttpCreateUrl
// documented behaviour; written from the Microsoft docs, no outside code).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace rex::kernel::xam {

struct UrlSpan {
  uint32_t off = 0, len = 0;
};
struct UrlParts {
  UrlSpan scheme, host, user, pass, path, extra;
  uint32_t scheme_id = 0;  // INTERNET_SCHEME_HTTP=1, HTTPS=2
  uint16_t port = 0;
};

inline bool UrlSchemeIs(std::string_view s, const char* lit) {
  size_t n = std::char_traits<char>::length(lit);
  if (s.size() != n) return false;
  for (size_t i = 0; i < n; ++i)
    if ((s[i] | 0x20) != lit[i]) return false;
  return true;
}

// http/https only, like WinHTTP. Returns false on anything it cannot parse.
inline bool ParseUrl(std::string_view u, UrlParts& p) {
  p = {};
  size_t sep = u.find("://");
  if (sep == std::string_view::npos) return false;
  std::string_view scheme = u.substr(0, sep);
  if (UrlSchemeIs(scheme, "http")) p.scheme_id = 1;
  else if (UrlSchemeIs(scheme, "https")) p.scheme_id = 2;
  else return false;
  p.scheme = {0, uint32_t(sep)};
  size_t a = sep + 3;
  size_t end = u.find_first_of("/?#", a);
  if (end == std::string_view::npos) end = u.size();
  std::string_view auth = u.substr(a, end - a);
  size_t at = auth.rfind('@');
  size_t hs = a;
  if (at != std::string_view::npos) {
    std::string_view ui = auth.substr(0, at);
    size_t c = ui.find(':');
    p.user = {uint32_t(a), uint32_t(c == std::string_view::npos ? ui.size() : c)};
    if (c != std::string_view::npos) p.pass = {uint32_t(a + c + 1), uint32_t(ui.size() - c - 1)};
    hs = a + at + 1;
  }
  std::string_view hp = u.substr(hs, end - hs);
  size_t colon = hp.rfind(':');
  size_t close = hp.rfind(']');
  uint32_t port = p.scheme_id == 2 ? 443 : 80;
  if (colon != std::string_view::npos && (close == std::string_view::npos || colon > close)) {
    std::string_view ps = hp.substr(colon + 1);
    if (ps.empty() || ps.size() > 5) return false;
    port = 0;
    for (char c : ps) {
      if (c < '0' || c > '9') return false;
      port = port * 10 + (c - '0');
    }
    if (port > 65535) return false;
    hp = hp.substr(0, colon);
  }
  if (hp.empty()) return false;
  p.host = {uint32_t(hs), uint32_t(hp.size())};
  p.port = uint16_t(port);
  size_t q = u.find_first_of("?#", end);
  size_t path_end = q == std::string_view::npos ? u.size() : q;
  p.path = {uint32_t(end), uint32_t(path_end - end)};
  if (q != std::string_view::npos) p.extra = {uint32_t(q), uint32_t(u.size() - q)};
  return true;
}

// "scheme://[user[:pass]@]host[:port]path extra"; the port is omitted when it is the scheme default.
inline std::string BuildUrl(uint32_t scheme_id, std::string_view scheme, std::string_view host,
                            uint16_t port, std::string_view user, std::string_view pass,
                            std::string_view path, std::string_view extra) {
  std::string s;
  std::string sc(scheme);
  if (sc.empty()) sc = scheme_id == 2 ? "https" : "http";
  s = sc + "://";
  if (!user.empty()) {
    s += user;
    if (!pass.empty()) s += ":" + std::string(pass);
    s += '@';
  }
  s += host;
  uint32_t def = (scheme_id == 2 || UrlSchemeIs(sc, "https")) ? 443 : 80;
  if (port && port != def) s += ":" + std::to_string(port);
  s += path;
  s += extra;
  return s;
}

}  // namespace rex::kernel::xam
