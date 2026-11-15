#include "cloud_provider.h"

#include <cstdio>
#include <string>
#include <vector>

static int failures = 0;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } \
} while (0)

class ListingProvider final : public ICloudProvider {
public:
    bool fail = false;
    std::vector<FileInfo> files;
    const char* Name() const override { return "test"; }
    bool Init(const std::string&) override { return true; }
    void Shutdown() override {}
    bool IsAuthenticated() const override { return true; }
    bool Upload(const std::string&, const uint8_t*, size_t) override { return false; }
    bool Download(const std::string&, std::vector<uint8_t>&) override { return false; }
    bool Remove(const std::string&) override { return false; }
    ExistsStatus CheckExists(const std::string&) override { return ExistsStatus::Missing; }
    std::vector<FileInfo> List(const std::string&) override { return files; }
    bool ListChecked(const std::string&, std::vector<FileInfo>& out,
                     bool* complete) override {
        if (complete) *complete = !fail;
        if (fail) return false;
        out = files;
        return true;
    }
};

int main() {
    const std::string dir = "/tmp/cloudredirect-config/";

    CHECK(ResolveProviderTokenPath(
              dir,
              R"({"provider":"gdrive","token_paths":{"gdrive":"providers/google.json"}})",
              "gdrive") == dir + "providers/google.json",
          "relative token_paths entries resolve beside config.json");

    CHECK(ResolveProviderTokenPath(
              dir,
              R"({"provider":"onedrive","token_path":"legacy.json"})",
              "onedrive") == dir + "legacy.json",
          "relative legacy token_path resolves beside config.json");

    CHECK(ResolveProviderTokenPath(
              dir,
              R"({"provider":"folder","sync_folder_path":"/mnt/cloud-saves"})",
              "folder") == "/mnt/cloud-saves",
          "folder provider initializes from sync_folder_path");

    CHECK(ResolveProviderTokenPath(dir, "{}", "r2") ==
              dir + "r2_credentials.json",
          "R2 keeps its convention filename");
    CHECK(ResolveProviderTokenPath(dir, "{}", "s3") ==
              dir + "s3_credentials.json",
          "S3 keeps its convention filename");
    CHECK(ResolveProviderTokenPath("/tmp/cloudredirect-config", "{}", "r2") ==
              "/tmp/cloudredirect-config/r2_credentials.json",
          "convention filenames do not require a trailing config separator");

    ListingProvider listing;
    listing.files = { {"77/42/a.sav"}, {"77/42/b.sav"}, {"77/99/c.sav"} };
    std::vector<std::string> folders;
    CHECK(listing.ListSubfoldersChecked("77/", folders),
          "checked folder listing reports a complete provider result");
    CHECK(folders.size() == 2 && folders[0] == "42" && folders[1] == "99",
          "checked folder listing derives unique immediate folders");
    listing.fail = true;
    CHECK(!listing.ListSubfoldersChecked("77/", folders),
          "checked folder listing preserves provider failures");

    if (failures != 0) {
        std::fprintf(stderr, "%d provider config test(s) failed\n", failures);
        return 1;
    }
    std::puts("provider config tests passed");
    return 0;
}
