#include "cabinet_provider.h"
#include "http_util.h"
#include "json.h"
#include "log.h"
#include "sha256_hmac.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

namespace Cabinet {

static constexpr size_t kTokenPayloadLen = 43; // base64url(32 bytes), unpadded
static constexpr size_t kMaxPathBytes = 1024;
static constexpr size_t kMaxSegmentBytes = 255;

static bool IsBase64UrlChar(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_';
}

static bool HasControlOrSpace(const std::string& s) {
    for (unsigned char c : s)
        if (c <= 0x20 || c == 0x7F) return true;
    return false;
}

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool IsValidToken(const std::string& token) {
    if (token.size() != 4 + kTokenPayloadLen || token.compare(0, 4, "cab_") != 0)
        return false;
    for (size_t i = 4; i < token.size(); ++i)
        if (!IsBase64UrlChar((unsigned char)token[i])) return false;
    return true;
}

bool NormalizeOrigin(const std::string& url, std::string& outOrigin) {
    outOrigin.clear();
    if (url.empty() || HasControlOrSpace(url)) return false;

    const size_t sep = url.find("://");
    if (sep == std::string::npos) return false;
    const std::string scheme = Lower(url.substr(0, sep));
    if (scheme != "https" && scheme != "http") return false;

    const size_t hostStart = sep + 3;
    size_t hostEnd = url.find_first_of("/?#", hostStart);
    if (hostEnd == std::string::npos) hostEnd = url.size();
    std::string authority = Lower(url.substr(hostStart, hostEnd - hostStart));
    // Credentials in the URL would end up in logs and the Origin header.
    if (authority.empty() || authority.find('@') != std::string::npos) return false;

    // Split an optional port, keeping bracketed IPv6 literals intact.
    std::string host = authority, port;
    const size_t bracket = authority.rfind(']');
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos && (bracket == std::string::npos || colon > bracket)) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
        if (port.empty()) return false;
        for (char c : port)
            if (c < '0' || c > '9') return false;
    }
    if (host.empty()) return false;

    // Browsers omit default ports from Origin; match that so the server's
    // origin comparison succeeds.
    if ((scheme == "https" && port == "443") || (scheme == "http" && port == "80"))
        port.clear();

    outOrigin = scheme + "://" + host + (port.empty() ? "" : ":" + port);
    return true;
}

// Value of key in an "a=1&b=2" query, percent-decoded.
static std::string QueryParam(const std::string& query, const char* key) {
    const std::string want = std::string(key) + "=";
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        if (query.compare(pos, want.size(), want) == 0)
            return HttpUtil::UrlDecode(query.substr(pos + want.size(),
                                                    amp - pos - want.size()));
        pos = amp + 1;
    }
    return {};
}

bool ParseConnectLink(const std::string& rawLink, std::string& outServer,
                      std::string& outToken) {
    outServer.clear();
    outToken.clear();
    const std::string link = Trim(rawLink);
    std::string server, token;

    if (Lower(link.substr(0, 10)) == "cabinet://") {
        const size_t q = link.find('?');
        if (q == std::string::npos) return false;
        std::string query = link.substr(q + 1);
        const size_t hash = query.find('#');
        if (hash != std::string::npos) query.resize(hash);
        server = QueryParam(query, "server");
        token = QueryParam(query, "token");
    } else {
        // The token lives in the fragment so browsers never send it to the
        // server; the origin is everything before the path.
        const size_t hash = link.find('#');
        if (hash == std::string::npos) return false;
        server = link.substr(0, hash);
        token = link.substr(hash + 1);
    }

    if (!IsValidToken(token)) return false;
    if (!NormalizeOrigin(server, outServer)) return false;
    outToken = token;
    return true;
}

bool EncodeFilePath(const std::string& path, std::string& outEncoded) {
    outEncoded.clear();
    if (path.empty() || path.size() > kMaxPathBytes) return false;

    std::string encoded;
    size_t start = 0;
    for (;;) {
        size_t slash = path.find('/', start);
        if (slash == std::string::npos) slash = path.size();
        const std::string seg = path.substr(start, slash - start);
        if (seg.empty() || seg.size() > kMaxSegmentBytes || seg == "." || seg == "..")
            return false;
        for (unsigned char c : seg)
            if (c < 0x20 || c == 0x7F || c == '\\') return false;
        if (!encoded.empty()) encoded += '/';
        encoded += HttpUtil::UrlEncode(seg, /*preserveSlash=*/false);
        if (slash == path.size()) break;
        start = slash + 1;
    }
    outEncoded = std::move(encoded);
    return true;
}

bool ParseListPage(const std::string& json,
                   std::vector<ICloudProvider::FileInfo>& outFiles,
                   std::string& outNext) {
    outNext.clear();
    const Json::Value root = Json::Parse(json);
    if (root.type != Json::Type::Object || !root.has("files") ||
        root["files"].type != Json::Type::Array || !root.has("next"))
        return false;

    // "next" is string | null. An empty cursor would restart the listing, so
    // treat it as malformed rather than silently looping or truncating.
    const Json::Value& next = root["next"];
    if (next.type == Json::Type::String) {
        if (next.str().empty()) return false;
        outNext = next.str();
    } else if (next.type != Json::Type::Null) {
        return false;
    }

    std::vector<ICloudProvider::FileInfo> page;
    const Json::Value& files = root["files"];
    page.reserve(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        const Json::Value& f = files[i];
        if (f.type != Json::Type::Object || f["path"].type != Json::Type::String ||
            f["path"].str().empty())
            return false;
        ICloudProvider::FileInfo fi;
        fi.path = f["path"].str();
        if (f["size"].type == Json::Type::Number && f["size"].number() >= 0)
            fi.size = (uint64_t)f["size"].number();
        if (f["modifiedTime"].type == Json::Type::Number && f["modifiedTime"].number() >= 0)
            fi.modifiedTime = (uint64_t)f["modifiedTime"].number();
        page.push_back(std::move(fi));
    }
    outFiles.insert(outFiles.end(), std::make_move_iterator(page.begin()),
                    std::make_move_iterator(page.end()));
    return true;
}

static bool JsonBoolField(const Json::Value& root, const char* key) {
    return root.has(key) && root[key].type == Json::Type::Bool && root[key].boolean();
}

bool ParseCredentials(const std::string& json, Credentials& out, std::string* outError) {
    auto fail = [&](const char* why) {
        if (outError) *outError = why;
        return false;
    };
    out = Credentials{};
    const Json::Value root = Json::Parse(json);
    if (root.type != Json::Type::Object) return fail("credentials are not a JSON object");
    if (root["server"].type != Json::Type::String || root["server"].str().empty())
        return fail("missing \"server\"");
    if (root["token"].type != Json::Type::String || !IsValidToken(root["token"].str()))
        return fail("missing or malformed \"token\"");

    Credentials c;
    if (!NormalizeOrigin(root["server"].str(), c.server))
        return fail("\"server\" is not an http(s) origin");
    c.token = root["token"].str();
    c.allowInsecureHttp = JsonBoolField(root, "allow_insecure_http");
    c.allowInsecureTls = JsonBoolField(root, "allow_insecure_tls");
    if (root["ca_cert_path"].type == Json::Type::String)
        c.caCertPath = root["ca_cert_path"].str();

    // The bearer token rides every request; plaintext needs an explicit opt-in.
    if (c.server.compare(0, 7, "http://") == 0 && !c.allowInsecureHttp)
        return fail("\"server\" uses http:// but allow_insecure_http is not set");

    out = std::move(c);
    return true;
}

std::string SerializeCredentials(const Credentials& creds) {
    Json::Value root = Json::Object();
    root.objVal["server"] = Json::String(creds.server);
    root.objVal["token"] = Json::String(creds.token);
    if (creds.allowInsecureHttp) {
        Json::Value v; v.type = Json::Type::Bool; v.boolVal = true;
        root.objVal["allow_insecure_http"] = v;
    }
    if (creds.allowInsecureTls) {
        Json::Value v; v.type = Json::Type::Bool; v.boolVal = true;
        root.objVal["allow_insecure_tls"] = v;
    }
    if (!creds.caCertPath.empty())
        root.objVal["ca_cert_path"] = Json::String(creds.caCertPath);
    return Json::Stringify(root);
}

static std::unique_ptr<IHttpTransport> MakeTransport(const Credentials& creds) {
    auto transport = CreateHttpTransport("[Cabinet]");
    if (!transport || !transport->Init()) return nullptr;
    TransportOptions opts;
    opts.allowInsecureHttp = creds.allowInsecureHttp;
    opts.allowInsecureTls = creds.allowInsecureTls;
    opts.caCertPath = creds.caCertPath;
    transport->SetOptions(opts);
    return transport;
}

// Bedrock rejects writes without a matching Origin (403, before Cabinet runs).
// Sending it on every request keeps one header set for all methods.
static std::vector<std::string> BaseHeaders(const Credentials& creds) {
    return { "Authorization: Bearer " + creds.token, "Accept: application/json",
             "Origin: " + creds.server };
}

// { "id": string|number, "name": string } as returned by /api/me and /api/pair.
static void ReadUser(const Json::Value& user, std::string& outId, std::string& outName) {
    if (user["id"].type == Json::Type::String) outId = user["id"].str();
    else if (user["id"].type == Json::Type::Number)
        outId = std::to_string(user["id"].integer());
    if (user["name"].type == Json::Type::String) outName = user["name"].str();
}

bool FetchMe(const Credentials& creds, std::string& outUserId,
             std::string& outUserName, std::string& outError) {
    outUserId.clear();
    outUserName.clear();
    auto transport = MakeTransport(creds);
    if (!transport) { outError = "HTTP transport unavailable"; return false; }

    HttpUtil::HttpResp r = transport->RequestUrl("GET", creds.server + "/api/me", "",
                                                 BaseHeaders(creds));
    transport->Shutdown();
    if (r.status == 0) { outError = "could not reach " + creds.server; return false; }
    if (r.status == 401) { outError = "server rejected the token (401)"; return false; }
    if (r.status != 200) {
        outError = "unexpected HTTP " + std::to_string(r.status) + " from /api/me";
        return false;
    }
    const Json::Value root = Json::Parse(r.body);
    const Json::Value& user = root["user"];
    if (root.type != Json::Type::Object || user.type != Json::Type::Object) {
        outError = "/api/me returned an unexpected body";
        return false;
    }
    ReadUser(user, outUserId, outUserName);
    return true;
}

// ---- Pairing codes ----

static constexpr size_t kPairingCodeLen = 12;
static constexpr size_t kMaxDeviceNameBytes = 100;
static constexpr const char* kCrockfordAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

bool NormalizePairingCode(const std::string& input, std::string& outCode) {
    outCode.clear();
    std::string code;
    for (unsigned char c : input) {
        // Spaces and dashes are separators. Other whitespace only arrives from
        // a paste (trailing newline, tab) and is treated the same way.
        if (c == '-' || std::isspace(c)) continue;
        if (c >= 0x80) return false;
        c = (unsigned char)std::toupper(c);
        if (c == 'O') c = '0';
        else if (c == 'I' || c == 'L') c = '1';
        if (std::strchr(kCrockfordAlphabet, c) == nullptr || c == '\0') return false;
        code += (char)c;
        if (code.size() > kPairingCodeLen) return false;
    }
    if (code.size() != kPairingCodeLen) return false;
    outCode = std::move(code);
    return true;
}

std::string PairingDeviceName(const std::string& hostname) {
    std::string name;
    for (unsigned char c : hostname)
        if (c >= 0x20 && c != 0x7F) name += (char)c;
    name = Trim(name);
    if (name.size() > kMaxDeviceNameBytes) {
        size_t cut = kMaxDeviceNameBytes;
        // Back up over UTF-8 continuation bytes so a character isn't split.
        while (cut > 0 && ((unsigned char)name[cut] & 0xC0) == 0x80) --cut;
        name.resize(cut);
        name = Trim(name);
    }
#ifdef _WIN32
    return name.empty() ? std::string("Windows PC") : name;
#else
    return name.empty() ? std::string("Steam Deck") : name;
#endif
}

static bool StartsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

bool ParseConnectArgs(const std::vector<std::string>& args, ConnectRequest& out,
                      std::string& outError) {
    out = ConnectRequest{};
    outError.clear();
    auto fail = [&](const std::string& why) {
        out = ConnectRequest{};
        outError = why;
        return false;
    };

    std::vector<std::string> positional;
    bool allowInsecureHttp = false;
    bool haveServerFlag = false;
    std::string serverFlag;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--allow-insecure-http") {
            allowInsecureHttp = true;
        } else if (a == "--server") {
            if (i + 1 >= args.size() || StartsWith(args[i + 1], "--"))
                return fail("--server needs an origin such as https://cabinet.example");
            haveServerFlag = true;
            serverFlag = args[++i];
        } else if (StartsWith(a, "--server=")) {
            haveServerFlag = true;
            serverFlag = a.substr(9);
        } else if (StartsWith(a, "--")) {
            return fail("Unknown option " + a);
        } else {
            positional.push_back(a);
        }
    }
    if (positional.empty())
        return fail("cabinet-connect needs a pairing code, a connect link, or <server> <token>");

    const bool firstIsUrl = positional[0].find("://") != std::string::npos;
    ConnectRequest req;
    req.allowInsecureHttp = allowInsecureHttp;

    if (positional.size() == 1 && firstIsUrl) {
        if (!ParseConnectLink(positional[0], req.server, req.token))
            return fail("Not a Cabinet connect link (expected https://<server>/connect#cab_...)");
        req.kind = ConnectRequest::Kind::Link;
    } else if (positional.size() == 2 && (firstIsUrl || StartsWith(positional[1], "cab_"))) {
        if (!NormalizeOrigin(positional[0], req.server))
            return fail("Server must be an http(s) origin such as https://cabinet.example");
        if (!IsValidToken(positional[1]))
            return fail("Malformed Cabinet token (expected cab_ + 43 base64url characters)");
        req.token = positional[1];
        req.kind = ConnectRequest::Kind::ServerToken;
    } else if (firstIsUrl) {
        return fail("cabinet-connect takes <connect_link> or <server> <token>");
    } else {
        // A pairing code, possibly typed as "ABCD EFGH JKMN" without quotes.
        std::string joined;
        for (const auto& p : positional) joined += p;
        if (!NormalizePairingCode(joined, req.code))
            return fail("Not a pairing code: expected 12 characters like ABCD-EFGH-JKMN");
        const std::string server = haveServerFlag ? serverFlag : kDefaultPairingServer;
        if (!NormalizeOrigin(server, req.server))
            return fail("--server must be an http(s) origin such as https://cabinet.example");
        req.kind = ConnectRequest::Kind::PairingCode;
    }

    if (haveServerFlag && req.kind != ConnectRequest::Kind::PairingCode)
        return fail("--server only applies to pairing codes; a connect link names its server");
    if (StartsWith(req.server, "http://") && !allowInsecureHttp)
        return fail("Refusing plaintext http:// server; pass --allow-insecure-http to override");

    out = std::move(req);
    return true;
}

std::string PairingErrorMessage(int status, const std::string& server) {
    switch (status) {
    case 0:   return "Could not reach " + server + ". Check the network connection and try again.";
    case 400: return "The server did not accept that code's format. Check the code and try again.";
    case 401: return "That code is wrong or has expired.";
    case 403: return server + " refused the request (HTTP 403). Check --server.";
    case 404: return server + " does not support pairing codes (HTTP 404). Check --server or use a connect link.";
    case 429: return "Too many attempts, wait a minute.";
    default: break;
    }
    if (status >= 500)
        return server + " had a problem (HTTP " + std::to_string(status) + "). Try again in a moment.";
    return "Unexpected HTTP " + std::to_string(status) + " from " + server + "/api/pair.";
}

bool RedeemPairingCode(const std::string& server, const std::string& code,
                       const std::string& deviceName, bool allowInsecureHttp,
                       std::string& outToken, std::string& outUserId,
                       std::string& outUserName, std::string& outError) {
    outToken.clear();
    outUserId.clear();
    outUserName.clear();

    Credentials creds;
    creds.server = server;
    creds.allowInsecureHttp = allowInsecureHttp;
    auto transport = MakeTransport(creds);
    if (!transport) { outError = "HTTP transport unavailable"; return false; }

    Json::Value body = Json::Object();
    body.objVal["code"] = Json::String(code);
    body.objVal["deviceName"] = Json::String(deviceName);

    // Unauthenticated write: the Origin header is what Bedrock checks. The
    // transport supplies User-Agent: CloudRedirect/1.0. No retries: a code
    // works once, and failed attempts count toward the server-wide limit.
    HttpUtil::HttpResp r = transport->RequestUrl(
        "POST", server + "/api/pair", Json::Stringify(body),
        { "Content-Type: application/json", "Accept: application/json",
          "Origin: " + server });
    transport->Shutdown();

    if (r.status != 200) {
        outError = PairingErrorMessage(r.status, server);
        return false;
    }
    const Json::Value root = Json::Parse(r.body);
    const Json::Value& user = root["user"];
    if (root.type != Json::Type::Object || root["token"].type != Json::Type::String ||
        !IsValidToken(root["token"].str()) || user.type != Json::Type::Object) {
        outError = server + "/api/pair returned an unexpected response.";
        return false;
    }
    outToken = root["token"].str();
    ReadUser(user, outUserId, outUserName);
    return true;
}

} // namespace Cabinet

bool CabinetProvider::Init(const std::string& configPath) {
    m_ready = false;
    auto store = CreateTokenStore();
    if (!store) return false;
    const std::string json = store->Read(configPath);
    if (json.empty()) return false;

    std::string err;
    if (!Cabinet::ParseCredentials(json, m_creds, &err)) {
        LOG("[Cabinet] invalid credentials in %s: %s", configPath.c_str(), err.c_str());
        return false;
    }

    // No network here: an offline boot must still bring the provider up so
    // work queues and retries behave as for the other static-credential
    // providers. cabinet-connect is where the token is verified.
    m_transport = Cabinet::MakeTransport(m_creds);
    if (!m_transport) return false;
    m_ready = true;
    LOG("[Cabinet] using %s", m_creds.server.c_str());
    return true;
}

void CabinetProvider::Shutdown() {
    if (m_transport) m_transport->Shutdown();
    m_transport.reset();
    m_ready = false;
}

bool CabinetProvider::IsAuthenticated() const {
    return m_ready;
}

HttpUtil::HttpResp CabinetProvider::Send(const char* method, const std::string& pathAndQuery,
                                         const std::string& body,
                                         std::vector<std::string> extraHeaders) {
    HttpUtil::HttpResp resp;
    if (!m_ready || !m_transport) return resp;

    std::vector<std::string> headers = Cabinet::BaseHeaders(m_creds);
    for (auto& h : extraHeaders) headers.push_back(std::move(h));

    const std::string url = m_creds.server + pathAndQuery;
    static constexpr int kMaxAttempts = 5;
    static thread_local std::mt19937 rng{std::random_device{}()};

    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            int baseMs = 1000 * (1 << (attempt - 1));
            int jitter = std::uniform_int_distribution<int>(0, baseMs / 2)(rng);
            std::this_thread::sleep_for(std::chrono::milliseconds(baseMs + jitter));
        }
        resp = m_transport->RequestUrl(method, url, body, headers);

        if (resp.status == 401 && !m_loggedUnauthorized.exchange(true))
            LOG("[Cabinet] server rejected the token (401); re-run cabinet-connect");

        const bool retry = resp.status == 0 || resp.status == 429 ||
                           resp.status == 500 || resp.status == 502 ||
                           resp.status == 503 || resp.status == 504;
        if (!retry) return resp;
        if (resp.status == 429)
            g_rateLimitHits.fetch_add(1, std::memory_order_relaxed);
    }
    return resp;
}

static std::string FilesUrl(const std::string& encodedPath) {
    return "/api/cloud/files/" + encodedPath;
}

bool CabinetProvider::Upload(const std::string& path, const uint8_t* data, size_t len) {
    std::string encoded;
    if (!Cabinet::EncodeFilePath(path, encoded)) {
        LOG("[Cabinet] refusing to upload invalid path '%s'", path.c_str());
        return false;
    }
    std::string body(reinterpret_cast<const char*>(data), len);
    std::vector<std::string> headers = {
        "Content-Type: application/octet-stream",
        "X-Content-SHA256: " + crypto::Sha256Hex(body),
    };
    // libcurl sends no body (and no length) for an empty custom-method PUT.
    if (body.empty()) headers.push_back("Content-Length: 0");

    HttpUtil::HttpResp r = Send("PUT", FilesUrl(encoded), body,
                                std::move(headers));
    if (r.status >= 200 && r.status < 300) return true;
    LOG("[Cabinet] PUT %s failed: HTTP %d%s", path.c_str(), r.status,
        r.status == 507 ? " (quota exceeded)" :
        r.status == 413 ? " (file too large)" : "");
    return false;
}

// ETag: "<sha256>" -> sha256 (lowercase), or "" when absent/unrecognized.
static std::string EtagSha256(const HttpUtil::HttpResp& r) {
    auto it = r.headers.find("etag");
    if (it == r.headers.end()) return {};
    std::string v = it->second;
    if (v.compare(0, 2, "W/") == 0) v = v.substr(2);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (v.size() != 64) return {};
    for (auto& c : v) {
        c = (char)std::tolower((unsigned char)c);
        if (!std::isxdigit((unsigned char)c)) return {};
    }
    return v;
}

bool CabinetProvider::Download(const std::string& path, std::vector<uint8_t>& outData) {
    std::string encoded;
    if (!Cabinet::EncodeFilePath(path, encoded)) return false;
    HttpUtil::HttpResp r = Send("GET", FilesUrl(encoded), "");
    if (r.status != 200) return false;
    // The server advertises the content hash; a mismatch means a truncated or
    // corrupted transfer, which must not be applied as save data.
    const std::string expected = EtagSha256(r);
    if (!expected.empty() && crypto::Sha256Hex(r.body) != expected) {
        LOG("[Cabinet] GET %s: body does not match ETag sha256", path.c_str());
        return false;
    }
    outData.assign(r.body.begin(), r.body.end());
    return true;
}

bool CabinetProvider::Remove(const std::string& path) {
    std::string encoded;
    if (!Cabinet::EncodeFilePath(path, encoded)) return false;
    HttpUtil::HttpResp r = Send("DELETE", FilesUrl(encoded), "");
    return r.status == 200 || r.status == 204 || r.status == 404;
}

ICloudProvider::ExistsStatus CabinetProvider::CheckExists(const std::string& path) {
    std::string encoded;
    if (!Cabinet::EncodeFilePath(path, encoded)) return ExistsStatus::Error;
    // Status only: through Cloudflare HEAD replies carry no Content-Length.
    HttpUtil::HttpResp r = Send("HEAD", FilesUrl(encoded), "");
    if (r.status == 200) return ExistsStatus::Exists;
    if (r.status == 404) return ExistsStatus::Missing;
    return ExistsStatus::Error;
}

bool CabinetProvider::ListChecked(const std::string& prefix, std::vector<FileInfo>& outFiles,
                                  bool* outComplete) {
    if (outComplete) *outComplete = false;
    outFiles.clear();

    std::string cursor;
    for (;;) {
        std::string query = "/api/cloud/list?prefix=" + HttpUtil::UrlEncode(prefix, false);
        if (!cursor.empty()) query += "&cursor=" + HttpUtil::UrlEncode(cursor, false);

        HttpUtil::HttpResp r = Send("GET", query, "");
        if (r.status != 200) {
            LOG("[Cabinet] list '%s' failed: HTTP %d", prefix.c_str(), r.status);
            return false;
        }
        std::string next;
        if (!Cabinet::ParseListPage(r.body, outFiles, next)) {
            LOG("[Cabinet] list '%s': malformed page", prefix.c_str());
            return false;
        }
        if (next.empty()) break;
        // A server that hands back the same cursor would loop forever.
        if (next == cursor) return false;
        cursor = std::move(next);
    }

    if (outComplete) *outComplete = true;
    return true;
}

std::vector<ICloudProvider::FileInfo> CabinetProvider::List(const std::string& prefix) {
    std::vector<FileInfo> files;
    ListChecked(prefix, files, nullptr);
    return files;
}
