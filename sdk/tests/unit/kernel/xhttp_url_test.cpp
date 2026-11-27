#include <catch2/catch_test_macros.hpp>
#include "kernel/xam/xhttp_url.h"
using namespace rex::kernel::xam;

TEST_CASE("ParseUrl splits scheme host port path extra") {
  std::string u = "https://user:pw@example.com:8443/a/b?x=1";
  UrlParts p;
  REQUIRE(ParseUrl(u, p));
  CHECK(p.scheme_id == 2);
  CHECK(p.port == 8443);
  CHECK(u.substr(p.host.off, p.host.len) == "example.com");
  CHECK(u.substr(p.user.off, p.user.len) == "user");
  CHECK(u.substr(p.pass.off, p.pass.len) == "pw");
  CHECK(u.substr(p.path.off, p.path.len) == "/a/b");
  CHECK(u.substr(p.extra.off, p.extra.len) == "?x=1");
}
TEST_CASE("ParseUrl defaults and rejects") {
  UrlParts p;
  REQUIRE(ParseUrl("http://h", p));
  CHECK(p.port == 80);
  CHECK(p.path.len == 0);
  REQUIRE(ParseUrl("HTTPS://h/", p));
  CHECK(p.port == 443);
  CHECK_FALSE(ParseUrl("ftp://h/", p));
  CHECK_FALSE(ParseUrl("nourl", p));
  CHECK_FALSE(ParseUrl("http://h:99999/", p));
  CHECK_FALSE(ParseUrl("http:///x", p));
}
TEST_CASE("BuildUrl omits default port and round trips") {
  CHECK(BuildUrl(1, "", "h", 80, "", "", "/p", "?q") == "http://h/p?q");
  CHECK(BuildUrl(2, "https", "h", 8443, "u", "w", "/p", "") == "https://u:w@h:8443/p");
}
