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
