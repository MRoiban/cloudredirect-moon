#include "cloud_intercept.h"
#include "log.h"
#include <dlfcn.h>
#include <mutex>
#include <unordered_set>

// Same bridge as slsteam_local_stats_epoch_v1 (stats_eligibility.cpp): dlsym
// cannot normally see LD_AUDIT objects, so SLSsteam redirects this versioned
// fallback via la_symbind32. Old/missing SLSsteam => no license evidence =>
// every managed app keeps redirecting, as before.
extern "C" __attribute__((weak, visibility("default")))
uint32_t slsteam_native_license_v1(uint32_t, uint32_t, uint32_t) noexcept { return 0; }

namespace CloudIntercept {

static bool HasNativeLicense(uint32_t appId, bool refresh) {
    const auto account = GetAccountId();
    if (!account) return false;
    using Query = uint32_t (*)(uint32_t, uint32_t, uint32_t);
    static const auto query = reinterpret_cast<Query>(
        dlsym(RTLD_DEFAULT, "slsteam_native_license_v1"));
    // Only an explicit 1 is evidence; anything else keeps the app redirected
    // so an unexpected answer can never strand saves on the wrong store.
    return query && query(appId, account, refresh ? 1 : 0) == 1;
}

bool ShouldRedirect(uint32_t appId, bool steamThread) {
    if (!appId || !IsNamespaceApp(appId)) return false;
    if (!HasNativeLicense(appId, steamThread)) return true;

    static std::mutex loggedMtx;
    static std::unordered_set<uint32_t> logged;
    std::lock_guard<std::mutex> lk(loggedMtx);
    if (logged.insert(appId).second)
        LOG("[Linux] app %u has a native license – Steam Cloud passthrough", appId);
    return false;
}

} // namespace CloudIntercept
