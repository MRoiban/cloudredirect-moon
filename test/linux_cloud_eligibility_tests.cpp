#include "cloud_intercept.h"
#include <cstdio>

// Built twice: with a strong slsteam_native_license_v1 (a current SLSsteam)
// and without one, where the weak fallback must keep today's behaviour.

static int failures = 0;
#define CHECK(condition, message) do { \
    if (!(condition)) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; } \
} while (0)

static uint32_t account = 39734273;
static uint32_t answer = 1;
static int calls = 0;
static uint32_t lastRefresh = 99;

#ifdef CLOUD_TEST_WITH_BRIDGE
extern "C" uint32_t slsteam_native_license_v1(uint32_t app, uint32_t id, uint32_t refresh) noexcept {
    ++calls;
    lastRefresh = refresh;
    return (id == 39734273 && app == 111) ? answer : 0;
}
#endif

namespace CloudIntercept {
// 111 = managed and owned, 222 = managed only, 333 = not managed.
bool IsNamespaceApp(uint32_t id) { return id == 111 || id == 222; }
uint32_t GetAccountId() { return account; }
}

int main() {
    using CloudIntercept::ShouldRedirect;

    CHECK(!ShouldRedirect(0, true), "app 0 never redirects");
    CHECK(!ShouldRedirect(333, true), "unmanaged apps never redirect");
    CHECK(ShouldRedirect(222, true), "managed apps without a license redirect");

#ifdef CLOUD_TEST_WITH_BRIDGE
    calls = 0;
    CHECK(!ShouldRedirect(333, true) && calls == 0,
          "unmanaged apps do not query SLSsteam");

    CHECK(!ShouldRedirect(111, true) && lastRefresh == 1,
          "owned managed app passes through; Steam hook threads may refresh");
    CHECK(!ShouldRedirect(111, false) && lastRefresh == 0,
          "background callers query without refresh");

    answer = 2;
    CHECK(ShouldRedirect(111, true), "only an explicit 1 counts as license evidence");
    answer = 1;

    account = 0;
    calls = 0;
    CHECK(ShouldRedirect(111, true) && calls == 0,
          "unknown account keeps redirecting without querying");
    account = 39734273;
#else
    CHECK(ShouldRedirect(111, true) && ShouldRedirect(111, false),
          "without the SLSsteam export every managed app keeps redirecting");
#endif

    if (failures) {
        std::fprintf(stderr, "%d cloud eligibility test(s) failed\n", failures);
        return 1;
    }
    std::puts("cloud eligibility tests passed");
    return 0;
}
