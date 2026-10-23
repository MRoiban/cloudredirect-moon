#include "cabinet_provider.h"
#include "cloud_provider.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } \
} while (0)

static const std::string kToken = "cab_" + std::string(40, 'A') + "b-_";

static void ConnectLinkTests() {
    std::string server, token;
    CHECK(Cabinet::ParseConnectLink("https://cabinet.example/connect#" + kToken, server, token) &&
              server == "https://cabinet.example" && token == kToken,
          "admin connect link yields origin and fragment token");
    CHECK(Cabinet::ParseConnectLink("  HTTPS://Cabinet.Example:443/connect#" + kToken + "\n",
                                    server, token) &&
              server == "https://cabinet.example",
          "connect link is trimmed, lowercased and drops the default port");
    CHECK(Cabinet::ParseConnectLink("https://cabinet.example:8443/connect#" + kToken,
                                    server, token) &&
              server == "https://cabinet.example:8443",
          "non-default port is kept in the origin");
    CHECK(Cabinet::ParseConnectLink(
              "cabinet://connect?server=https%3A%2F%2Fcabinet.example&token=" + kToken,
              server, token) &&
              server == "https://cabinet.example" && token == kToken,
          "cabinet:// protocol link is accepted");

    CHECK(!Cabinet::ParseConnectLink("https://cabinet.example/connect", server, token),
          "link without a fragment token is rejected");
    CHECK(!Cabinet::ParseConnectLink("https://cabinet.example/connect#cab_short", server, token),
          "short token is rejected");
    CHECK(!Cabinet::ParseConnectLink("https://cabinet.example/connect#" + kToken + "A",
                                     server, token),
          "overlong token is rejected");
    CHECK(!Cabinet::ParseConnectLink("https://cabinet.example/connect#cab_" +
                                         std::string(42, 'A') + "=", server, token),
          "padded/non-base64url token is rejected");
    CHECK(!Cabinet::ParseConnectLink("ftp://cabinet.example/connect#" + kToken, server, token),
          "non-http scheme is rejected");
    CHECK(!Cabinet::ParseConnectLink("https://user:pw@cabinet.example/connect#" + kToken,
                                     server, token),
          "userinfo in the server URL is rejected");
    CHECK(server.empty() && token.empty(), "failed parse clears outputs");
}

static void PathEncodingTests() {
    std::string enc;
    CHECK(Cabinet::EncodeFilePath("77/42/blobs/save 1.sav", enc) &&
              enc == "77/42/blobs/save%201.sav",
          "segments are encoded and joined with /");
    CHECK(Cabinet::EncodeFilePath("1/2/a%b?c#d&e+f", enc) && enc == "1/2/a%25b%3Fc%23d%26e%2Bf",
          "reserved characters inside a segment are percent-encoded");
    CHECK(Cabinet::EncodeFilePath("1/2/\xC3\xA9t\xC3\xA9.sav", enc) &&
              enc == "1/2/%C3%A9t%C3%A9.sav",
          "UTF-8 bytes are percent-encoded");
    CHECK(Cabinet::EncodeFilePath("a/.hidden/..x", enc) && enc == "a/.hidden/..x",
          "dot-prefixed names that are not . or .. are allowed");

    CHECK(!Cabinet::EncodeFilePath("", enc), "empty path is rejected");
    CHECK(!Cabinet::EncodeFilePath("/77/a", enc), "leading slash (empty segment) is rejected");
    CHECK(!Cabinet::EncodeFilePath("77//a", enc), "empty middle segment is rejected");
    CHECK(!Cabinet::EncodeFilePath("77/a/", enc), "trailing slash is rejected");
    CHECK(!Cabinet::EncodeFilePath("77/../a", enc), ".. segment is rejected");
    CHECK(!Cabinet::EncodeFilePath("77/./a", enc), ". segment is rejected");
    CHECK(!Cabinet::EncodeFilePath("77\\a", enc), "backslash is rejected");
    CHECK(!Cabinet::EncodeFilePath(std::string("77/a\x01"), enc), "control character is rejected");
    CHECK(!Cabinet::EncodeFilePath("77/a\x7F", enc), "DEL is rejected");
    CHECK(Cabinet::EncodeFilePath(std::string(255, 'x'), enc), "255-byte segment is allowed");
    CHECK(!Cabinet::EncodeFilePath(std::string(256, 'x'), enc), "256-byte segment is rejected");
    std::string longPath;
    while (longPath.size() < 1024) longPath += "abcdefg/";
    longPath.resize(1024);
    if (longPath.back() == '/') longPath.back() = 'z';
    CHECK(Cabinet::EncodeFilePath(longPath, enc), "1024-byte path is allowed");
    CHECK(!Cabinet::EncodeFilePath(longPath + "z", enc), "1025-byte path is rejected");
}

static void ListPageTests() {
    std::vector<ICloudProvider::FileInfo> files;
    std::string next;
    CHECK(Cabinet::ParseListPage(
              R"({"files":[{"path":"77/42/a b.sav","size":12,"sha256":"00","modifiedTime":1700000000},)"
              R"({"path":"77/42/c","size":0,"sha256":"11","modifiedTime":1}],"next":"77/42/c"})",
              files, next),
          "page with a cursor parses");
    CHECK(files.size() == 2 && files[0].path == "77/42/a b.sav" && files[0].size == 12 &&
              files[0].modifiedTime == 1700000000 && files[1].path == "77/42/c",
          "FileInfo fields map onto ICloudProvider::FileInfo");
    CHECK(next == "77/42/c", "next cursor is returned");

    CHECK(Cabinet::ParseListPage(R"({"files":[{"path":"x","size":3,"sha256":"","modifiedTime":2}],"next":null})",
                                 files, next) &&
              next.empty() && files.size() == 3,
          "last page (next=null) appends and ends the listing");
    CHECK(Cabinet::ParseListPage(R"({"files":[],"next":null})", files, next) && files.size() == 3,
          "empty page parses");

    files.clear();
    CHECK(!Cabinet::ParseListPage("not json", files, next), "non-JSON page fails");
    CHECK(!Cabinet::ParseListPage(R"({"error":"unauthorized"})", files, next),
          "error envelope fails");
    CHECK(!Cabinet::ParseListPage(R"({"files":[]})", files, next), "missing next fails");
    CHECK(!Cabinet::ParseListPage(R"({"files":[],"next":""})", files, next),
          "empty-string cursor fails instead of looping");
    CHECK(!Cabinet::ParseListPage(R"({"files":[],"next":5})", files, next), "non-string next fails");
    CHECK(!Cabinet::ParseListPage(R"({"files":[{"size":1}],"next":null})", files, next),
          "entry without path fails");
    CHECK(files.empty(), "a malformed page appends nothing");
}

static void CredentialTests() {
    const std::string dir = "/tmp/cloudredirect-config/";
    CHECK(ResolveProviderTokenPath(dir, "{}", "cabinet") == dir + "tokens_cabinet.json",
          "cabinet defaults to tokens_cabinet.json");
    CHECK(ResolveProviderTokenPath(dir,
              R"({"provider":"cabinet","token_paths":{"cabinet":"creds/cab.json"}})",
              "cabinet") == dir + "creds/cab.json",
          "token_paths registry overrides the cabinet default");
    CHECK(ResolveProviderTokenPath(dir, R"({"provider":"gdrive","token_path":"g.json"})",
                                   "cabinet") == dir + "tokens_cabinet.json",
          "another provider's legacy token_path is not used for cabinet");

    std::unique_ptr<ICloudProvider> prov = CreateCloudProvider("cabinet");
    CHECK(prov && std::string(prov->Name()) == "Cabinet", "factory creates the cabinet provider");
    CHECK(prov && !prov->Init("/nonexistent/tokens_cabinet.json") && !prov->IsAuthenticated(),
          "missing credential file fails Init");

    Cabinet::Credentials c;
    std::string err;
    CHECK(Cabinet::ParseCredentials(
              R"({"server":"https://Cabinet.Example/","token":")" + kToken + R"("})", c, &err) &&
              c.server == "https://cabinet.example" && c.token == kToken && !c.allowInsecureHttp,
          "credential document parses and normalizes the server");
    CHECK(!Cabinet::ParseCredentials(R"({"server":"http://10.0.0.2:8080","token":")" + kToken +
                                         R"("})", c, &err),
          "plaintext server is refused by default");
    CHECK(err.find(kToken) == std::string::npos, "errors never contain the token");
    CHECK(Cabinet::ParseCredentials(R"({"server":"http://10.0.0.2:8080","token":")" + kToken +
                                        R"(","allow_insecure_http":true})", c, &err) &&
              c.server == "http://10.0.0.2:8080" && c.allowInsecureHttp,
          "allow_insecure_http permits a plaintext server");
    CHECK(!Cabinet::ParseCredentials(R"({"server":"https://cabinet.example","token":"cab_x"})",
                                     c, &err),
          "malformed token is refused");
    CHECK(!Cabinet::ParseCredentials(R"({"token":")" + kToken + R"("})", c, &err),
          "missing server is refused");

    Cabinet::Credentials round;
    c.server = "https://cabinet.example";
    c.token = kToken;
    c.allowInsecureHttp = false;
    CHECK(Cabinet::ParseCredentials(Cabinet::SerializeCredentials(c), round, &err) &&
              round.server == c.server && round.token == c.token,
          "serialized credentials round-trip");
}

static void PairingCodeTests() {
    std::string code;
    CHECK(Cabinet::NormalizePairingCode("ABCD-EFGH-JKMN", code) && code == "ABCDEFGHJKMN",
          "dashed display form normalizes to 12 characters");
    CHECK(Cabinet::NormalizePairingCode("abcdefghjkmn", code) && code == "ABCDEFGHJKMN",
          "lowercase is accepted");
    CHECK(Cabinet::NormalizePairingCode(" 7pq9 - rstv -wxyz\n", code) && code == "7PQ9RSTVWXYZ",
          "spaces, dashes and a pasted newline are ignored");
    CHECK(Cabinet::NormalizePairingCode("OoIi-Ll00-1234", code) && code == "001111001234",
          "O reads as 0, I and L read as 1 (any case)");
    CHECK(Cabinet::NormalizePairingCode("0123456789AB", code) && code == "0123456789AB",
          "digits are accepted");

    CHECK(!Cabinet::NormalizePairingCode("ABCD-EFGH-JKM", code), "11 characters are rejected");
    CHECK(!Cabinet::NormalizePairingCode("ABCD-EFGH-JKMNP", code), "13 characters are rejected");
    CHECK(!Cabinet::NormalizePairingCode("ABCD-EFGH-JKMU", code), "U is not Crockford base32");
    CHECK(!Cabinet::NormalizePairingCode("ABCD.EFGH.JKMN", code), "punctuation other than - is rejected");
    CHECK(!Cabinet::NormalizePairingCode("ABCD_EFGH_JKMN", code), "underscore is rejected");
    CHECK(!Cabinet::NormalizePairingCode("ABCD-EFGH-JKM\xC3\x89", code), "non-ASCII is rejected");
    CHECK(!Cabinet::NormalizePairingCode("", code), "empty input is rejected");
    CHECK(!Cabinet::NormalizePairingCode("------------", code), "separators alone are rejected");
    CHECK(code.empty(), "failed normalization clears the output");

    CHECK(Cabinet::PairingDeviceName("steamdeck\n") == "steamdeck", "hostname is trimmed");
#ifdef _WIN32
    const std::string defaultDeviceName = "Windows PC";
#else
    const std::string defaultDeviceName = "Steam Deck";
#endif
    CHECK(Cabinet::PairingDeviceName("") == defaultDeviceName,
          "empty hostname falls back to the platform device name");
    CHECK(Cabinet::PairingDeviceName(" \t") == defaultDeviceName,
          "blank hostname falls back to the platform device name");
    CHECK(Cabinet::PairingDeviceName(std::string(150, 'h')) == std::string(100, 'h'),
          "device name is capped at 100 bytes");
    const std::string accented = std::string(99, 'h') + "\xC3\xA9xyz";
    CHECK(Cabinet::PairingDeviceName(accented) == std::string(99, 'h'),
          "device name is not cut inside a UTF-8 character");

    CHECK(Cabinet::PairingErrorMessage(401, "https://c.example") == "That code is wrong or has expired.",
          "401 maps to the wrong/expired message");
    CHECK(Cabinet::PairingErrorMessage(429, "https://c.example") == "Too many attempts, wait a minute.",
          "429 maps to the rate-limit message");
    CHECK(Cabinet::PairingErrorMessage(0, "https://c.example").find("Could not reach https://c.example") == 0,
          "transport failure names the server");
}

static void ConnectArgsTests() {
    using Kind = Cabinet::ConnectRequest::Kind;
    Cabinet::ConnectRequest r;
    std::string err;

    CHECK(Cabinet::ParseConnectArgs({"abcd-efgh-jkmn"}, r, err) && r.kind == Kind::PairingCode &&
              r.code == "ABCDEFGHJKMN" && r.server == Cabinet::kDefaultPairingServer &&
              r.token.empty(),
          "a bare code uses the default server");
    CHECK(Cabinet::ParseConnectArgs({"ABCD", "EFGH", "JKMN"}, r, err) &&
              r.kind == Kind::PairingCode && r.code == "ABCDEFGHJKMN",
          "an unquoted code split by spaces is joined");
    CHECK(Cabinet::ParseConnectArgs({"ABCDEFGHJKMN", "--server", "https://Cab.Example:443/"}, r, err) &&
              r.kind == Kind::PairingCode && r.server == "https://cab.example",
          "--server overrides and is normalized");
    CHECK(Cabinet::ParseConnectArgs({"--server=https://cab.example", "ABCDEFGHJKMN"}, r, err) &&
              r.server == "https://cab.example",
          "--server=<origin> form works before the code");
    CHECK(!Cabinet::ParseConnectArgs({"ABCDEFGHJKMN", "--server", "http://10.0.0.2:8080"}, r, err) &&
              err.find("--allow-insecure-http") != std::string::npos,
          "plaintext --server needs --allow-insecure-http");
    CHECK(Cabinet::ParseConnectArgs({"ABCDEFGHJKMN", "--server", "http://10.0.0.2:8080",
                                     "--allow-insecure-http"}, r, err) &&
              r.server == "http://10.0.0.2:8080" && r.allowInsecureHttp,
          "--allow-insecure-http permits a plaintext --server");
    CHECK(!Cabinet::ParseConnectArgs({"ABCDEFGHJKMN", "--server"}, r, err), "--server without a value fails");
    CHECK(!Cabinet::ParseConnectArgs({"ABCDEFGHJKMN", "--server", "cab.example"}, r, err),
          "--server without a scheme fails");
    CHECK(!Cabinet::ParseConnectArgs({"ABCD-EFGH-JKMU"}, r, err) &&
              err.find("pairing code") != std::string::npos,
          "an invalid code reports a pairing-code error");
    CHECK(!Cabinet::ParseConnectArgs({"ABCD-EFGH-JKM"}, r, err), "an 11-character code fails");

    CHECK(Cabinet::ParseConnectArgs({"https://cabinet.example/connect#" + kToken}, r, err) &&
              r.kind == Kind::Link && r.server == "https://cabinet.example" && r.token == kToken,
          "a connect link is still accepted");
    CHECK(!Cabinet::ParseConnectArgs({"https://cabinet.example/connect#cab_short"}, r, err) &&
              err.find("connect link") != std::string::npos && err.find("cab_short") == std::string::npos,
          "a bad link reports a link error without echoing the token");
    CHECK(Cabinet::ParseConnectArgs({"https://cabinet.example", kToken}, r, err) &&
              r.kind == Kind::ServerToken && r.server == "https://cabinet.example" && r.token == kToken,
          "<server> <token> is still accepted");
    CHECK(!Cabinet::ParseConnectArgs({"https://cabinet.example", "cab_short"}, r, err) &&
              err.find("Malformed Cabinet token") != std::string::npos,
          "<server> <bad token> reports a token error");
    CHECK(!Cabinet::ParseConnectArgs({"http://10.0.0.2", kToken}, r, err),
          "plaintext <server> needs --allow-insecure-http");
    CHECK(Cabinet::ParseConnectArgs({"--allow-insecure-http", "http://10.0.0.2", kToken}, r, err) &&
              r.allowInsecureHttp,
          "--allow-insecure-http still works for <server> <token>");
    CHECK(!Cabinet::ParseConnectArgs({"https://cabinet.example/connect#" + kToken, "--server",
                                      "https://other.example"}, r, err),
          "--server is refused with a connect link");

    CHECK(!Cabinet::ParseConnectArgs({}, r, err), "no arguments fails");
    CHECK(!Cabinet::ParseConnectArgs({"ABCDEFGHJKMN", "--bogus"}, r, err), "unknown option fails");
    CHECK(r.code.empty() && r.token.empty(), "failed parse clears the request");
}

static void LuaToolsContractTests() {
    const std::string dir = "/tmp/cloudredirect-config/";
    std::ifstream fixture(CR_TEST_FIXTURE_DIR "/luatools-config.json", std::ios::binary);
    const std::string config((std::istreambuf_iterator<char>(fixture)), {});
    CHECK(fixture && !config.empty(), "LuaTools config fixture can be read");
    CHECK(ResolveProviderTokenPath(dir, config, "cabinet") ==
              R"(C:\Users\Alex\AppData\Roaming\CloudRedirect\tokens_cabinet.json)",
          "LuaTools absolute Cabinet token path is preserved");

    Cabinet::Credentials creds;
    std::string err;
    CHECK(Cabinet::ParseCredentials(
              R"({"server":"https://cabinet.mc2.onl","token":")" + kToken + R"("})",
              creds, &err) && creds.server == "https://cabinet.mc2.onl" &&
              creds.token == kToken && !creds.allowInsecureHttp && !creds.allowInsecureTls,
          "LuaTools plaintext credential shape is accepted after DPAPI decryption");
}

int main() {
    ConnectLinkTests();
    PathEncodingTests();
    ListPageTests();
    CredentialTests();
    PairingCodeTests();
    ConnectArgsTests();
    LuaToolsContractTests();
    if (failures != 0) {
        std::fprintf(stderr, "%d cabinet provider test(s) failed\n", failures);
        return 1;
    }
    std::puts("cabinet provider tests passed");
    return 0;
}
