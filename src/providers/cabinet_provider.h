#pragma once
#include "cloud_provider.h"
#include "cloud_provider_base.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Cabinet: the user's self-hosted file store (Bedrock "cabinet" pebble).
// A bearer token selects the Cabinet user; paths map 1:1 onto
// /api/cloud/files/<path>. Contract: cabinet/docs/cloud-api.md,
// "Steam Cloud file store (CloudRedirect)".

namespace Cabinet {

struct Credentials {
    std::string server;  // normalized origin: scheme://host[:port], no trailing slash
    std::string token;   // cab_<43 base64url chars>
    bool allowInsecureHttp = false;
    bool allowInsecureTls = false;
    std::string caCertPath;
};

// cab_ + 32 random bytes as unpadded base64url (43 chars).
bool IsValidToken(const std::string& token);

// Reduce an http(s) URL to the browser-style origin the server compares the
// Origin header against (lowercase scheme/host, default port dropped).
// Rejects userinfo, whitespace and control characters.
bool NormalizeOrigin(const std::string& url, std::string& outOrigin);

// Accepts "https://host/connect#cab_..." (the admin page's connect link) and
// "cabinet://connect?server=<origin>&token=cab_..." (its Open-in-Cabinet button).
bool ParseConnectLink(const std::string& link, std::string& outServer,
                      std::string& outToken);

// Validate a relative path against the server's rules and percent-encode each
// segment separately. Fails (no request is sent) for anything the server
// would answer with 400.
bool EncodeFilePath(const std::string& path, std::string& outEncoded);

// Parse one /api/cloud/list page. outNext is empty when the listing is done.
bool ParseListPage(const std::string& json,
                   std::vector<ICloudProvider::FileInfo>& outFiles,
                   std::string& outNext);

// Parse the credential document. outError never contains the token.
bool ParseCredentials(const std::string& json, Credentials& out,
                      std::string* outError = nullptr);
std::string SerializeCredentials(const Credentials& creds);

// GET /api/me with these credentials. On success returns the user's display
// name/id; on failure outError says why (status or transport), never the token.
bool FetchMe(const Credentials& creds, std::string& outUserId,
             std::string& outUserName, std::string& outError);

// ---- Pairing codes (cloud-api.md, "Pairing codes") ----

// Server a pairing code is redeemed against unless --server is given.
// Only used for codes; connect links always carry their own server.
inline constexpr const char* kDefaultPairingServer = "https://cabinet.mc2.onl";

// Case-insensitive; spaces and '-' are ignored; O reads as 0, I/L as 1. The
// result must be exactly 12 Crockford base32 characters (0-9 A-H J K M N P-T
// V-Z). outCode receives the 12 uppercase characters without separators.
bool NormalizePairingCode(const std::string& input, std::string& outCode);

// Hostname -> deviceName for /api/pair: trimmed, control characters dropped,
// at most 100 bytes (cut on a UTF-8 boundary); "Steam Deck" when empty.
std::string PairingDeviceName(const std::string& hostname);

// What `cabinet-connect` was asked to do, parsed from its arguments.
struct ConnectRequest {
    enum class Kind { Link, ServerToken, PairingCode };
    Kind kind = Kind::Link;
    std::string server;  // normalized origin (all kinds)
    std::string token;   // Link / ServerToken
    std::string code;    // PairingCode: normalized 12 characters
    bool allowInsecureHttp = false;
};

// Arguments after "cabinet-connect":
//   <connect_link>
//   <server> <token>
//   <pairing_code> [--server <origin>]   (the code may be split across args)
// plus --allow-insecure-http anywhere. On failure outError is a user-facing
// message that never contains a token.
bool ParseConnectArgs(const std::vector<std::string>& args, ConnectRequest& out,
                      std::string& outError);

// POST /api/pair. On success fills outToken and the user; on failure outError
// is a user-facing message (never contains the code's token).
bool RedeemPairingCode(const std::string& server, const std::string& code,
                       const std::string& deviceName, bool allowInsecureHttp,
                       std::string& outToken, std::string& outUserId,
                       std::string& outUserName, std::string& outError);

// Human-readable message for a /api/pair HTTP status (0 = transport failure).
std::string PairingErrorMessage(int status, const std::string& server);

} // namespace Cabinet

class CabinetProvider : public ICloudProvider {
public:
    const char* Name() const override { return "Cabinet"; }

    bool Init(const std::string& configPath) override;
    void Shutdown() override;
    bool IsAuthenticated() const override;

    bool Upload(const std::string& path, const uint8_t* data, size_t len) override;
    bool Download(const std::string& path, std::vector<uint8_t>& outData) override;
    bool Remove(const std::string& path) override;
    ExistsStatus CheckExists(const std::string& path) override;
    std::vector<FileInfo> List(const std::string& prefix) override;
    bool ListChecked(const std::string& prefix, std::vector<FileInfo>& outFiles,
                     bool* outComplete = nullptr) override;

private:
    HttpUtil::HttpResp Send(const char* method, const std::string& pathAndQuery,
                            const std::string& body,
                            std::vector<std::string> extraHeaders = {});

    Cabinet::Credentials m_creds;
    bool m_ready = false;
    std::atomic<bool> m_loggedUnauthorized{false};
    std::unique_ptr<IHttpTransport> m_transport;
};
