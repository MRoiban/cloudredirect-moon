#include "cabinet_provider.h"
#include "cloud_provider.h"

#include <cstdio>
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

int main() {
    ConnectLinkTests();
    PathEncodingTests();
    ListPageTests();
    CredentialTests();
    if (failures != 0) {
        std::fprintf(stderr, "%d cabinet provider test(s) failed\n", failures);
        return 1;
    }
    std::puts("cabinet provider tests passed");
    return 0;
}
