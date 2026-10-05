// CPU contract: sentinel handles are never passed to a Vulkan function.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "benchmark_wait_phase.h"
#include <cstdio>
#include <limits>
#include <type_traits>
#include <utility>

using R = BenchWaitPhaseRecorder;
using P = R::Phase;
using S = R::Snapshot;
static_assert(!std::is_copy_constructible<R>::value && !std::is_move_constructible<R>::value);
static_assert(!std::is_copy_assignable<R>::value && !std::is_move_assignable<R>::value);
static_assert(std::is_nothrow_default_constructible<R>::value);
static_assert(noexcept(std::declval<R &>().begin({}, {}, nullptr, nullptr, nullptr, 0)));
static_assert(noexcept(std::declval<R &>().reset()));
static_assert(noexcept(std::declval<R &>().setPhase(P::Acquire)));
static_assert(noexcept(std::declval<R &>().onAcquire({}, {}, VK_SUCCESS, 0, nullptr)));
static_assert(noexcept(std::declval<R &>().recordFenceWait({}, 0, nullptr, VK_SUCCESS, 0, nullptr, 0)));
static_assert(noexcept(std::declval<R &>().recordSemaphoreWait({}, VK_SUCCESS, 0)));
static_assert(noexcept(std::declval<R &>().end()));
static_assert(noexcept(std::declval<const R &>().snapshot()));
static_assert(noexcept(std::declval<const R &>().isActive()) && noexcept(std::declval<const R &>().isInvalid()));
static_assert(noexcept(std::declval<const R &>().currentPhase()));
static_assert(noexcept(std::declval<const S &>().totalFenceCalls()));
static_assert(noexcept(std::declval<const S &>().totalSemaphoreCalls()));
static_assert(noexcept(std::declval<const S &>().totalFailures()));
static_assert(noexcept(std::declval<const S &>().totalMilliseconds()));

static unsigned checks = 0, failures = 0, groups = 0;
static const char *context = "initial";
static void check(bool condition, const char *label) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s: %s\n", context, label); }
}
template <typename T> static T handle(uintptr_t value) { return reinterpret_cast<T>(value); }
static const VkDevice DEVICE = handle<VkDevice>(1);
static const VkDevice OTHER_DEVICE = handle<VkDevice>(2);
static const VkSwapchainKHR SWAPCHAIN = handle<VkSwapchainKHR>(3);
static const VkSwapchainKHR OTHER_SWAPCHAIN = handle<VkSwapchainKHR>(4);
static int generation, otherGeneration;
static const VkFence F0 = handle<VkFence>(10), F1 = handle<VkFence>(11), OTHER_FENCE = handle<VkFence>(12);
static bool near(double a, double b) { return std::isfinite(a) && std::abs(a - b) < 1e-10; }
static S expectedOwned() {
    S expected;
    expected.fenceCount = 2;
    expected.fences[0] = F0; expected.fences[1] = F1;
    expected.pending[0] = expected.pending[1] = 1;
    return expected;
}
static void verify(const R &r, const S &expected, bool active, P phase) {
    const auto &actual = r.snapshot();
    check(r.isActive() == active && r.currentPhase() == phase, "activity and phase");
    check(r.isInvalid() == expected.invalid && actual.invalid == expected.invalid, "published invalid");
    uint64_t fences = 0, semaphores = 0, failed = 0; double ms = 0;
    for (size_t i = 0; i < expected.buckets.size(); ++i) {
        const auto &a = actual.buckets[i]; const auto &e = expected.buckets[i];
        check(a.fenceCalls == e.fenceCalls, "bucket fence calls");
        check(a.semaphoreCalls == e.semaphoreCalls, "bucket semaphore calls");
        check(a.failures == e.failures, "bucket failures");
        check(near(a.milliseconds, e.milliseconds), "bucket time");
        fences += e.fenceCalls; semaphores += e.semaphoreCalls; failed += e.failures; ms += e.milliseconds;
    }
    check(actual.totalFenceCalls() == fences && actual.totalSemaphoreCalls() == semaphores, "total calls");
    check(actual.totalFailures() == failed && near(actual.totalMilliseconds(), ms), "total failures and time");
    check(actual.acquireCalls == expected.acquireCalls && actual.acquireSuccesses == expected.acquireSuccesses, "acquire results");
    check(actual.acquirePresentFenceCalls == expected.acquirePresentFenceCalls && actual.acquireOtherFenceCalls == expected.acquireOtherFenceCalls, "acquire categories");
    check(near(actual.acquirePresentFenceMilliseconds, expected.acquirePresentFenceMilliseconds), "present fence time");
    check(actual.identityFailures == expected.identityFailures && actual.fenceCount == expected.fenceCount, "identity count and owned count");
    for (size_t i = 0; i < actual.fences.size(); ++i) {
        check(actual.fences[i] == expected.fences[i] && actual.pending[i] == expected.pending[i], "all owned slots including inactive tail");
    }
    check(actual.acquirePresentFenceCalls + actual.acquireOtherFenceCalls == actual.buckets[1].fenceCalls, "acquire category closure");
}
static void prime(R &r) {
    VkFence fences[] = {F0, F1}; uint8_t pending[] = {1, 1};
    check(r.begin(DEVICE, SWAPCHAIN, &generation, fences, pending, 2), "prime begin");
}
static void acquired(R &r, S &expected, uint32_t index = 0) {
    check(r.setPhase(P::Acquire), "enter acquire");
    check(r.onAcquire(DEVICE, SWAPCHAIN, VK_SUCCESS, index, &generation), "real successful acquire");
    ++expected.acquireCalls; ++expected.acquireSuccesses;
}
static void group(const char *label) { context = label; ++groups; }

int main() {
    group("default and inactive calls");
    {
        R r; S expected;
        verify(r, expected, false, P::Unknown);
        check(!r.setPhase(P::Acquire), "inactive setPhase");
        check(!r.onAcquire(DEVICE, SWAPCHAIN, VK_SUCCESS, 0, &generation), "inactive acquire");
        check(!r.recordFenceWait(DEVICE, 1, &F0, VK_SUCCESS, .1, &generation, 0), "inactive fence wait");
        check(!r.recordSemaphoreWait(DEVICE, VK_SUCCESS, .1), "inactive semaphore wait");
        r.end(); verify(r, expected, false, P::Unknown);
    }
    group("all six phases and exact totals");
    {
        R r; prime(r); S e = expectedOwned();
        for (size_t i = 0; i < 6; ++i) {
            const P phase = static_cast<P>(i); check(r.setPhase(phase), "phase accepted");
            if (phase == P::Acquire) { check(r.onAcquire(DEVICE, SWAPCHAIN, VK_SUBOPTIMAL_KHR, 1, &generation), "suboptimal successful acquire"); ++e.acquireCalls; ++e.acquireSuccesses; }
            check(r.recordFenceWait(DEVICE, 1, &F1, VK_SUCCESS, .1, &generation, 1), "successful fence wait");
            check(r.recordFenceWait(DEVICE, 1, &F1, VK_TIMEOUT, .2, &generation, 1), "timeout fence wait recorded");
            check(r.recordSemaphoreWait(DEVICE, VK_ERROR_DEVICE_LOST, .3), "failed semaphore wait recorded");
            e.buckets[i].fenceCalls = 2; e.buckets[i].semaphoreCalls = 1;
            e.buckets[i].failures = 2; e.buckets[i].milliseconds = .6;
            if (phase == P::Acquire) { e.acquirePresentFenceCalls = 2; e.acquirePresentFenceMilliseconds = .3; }
            verify(r, e, true, phase);
        }
        r.end(); verify(r, e, false, P::Unknown);
        check(!r.recordSemaphoreWait(DEVICE, VK_SUCCESS, 100), "ended wait no-op"); verify(r, e, false, P::Unknown);
        prime(r); verify(r, expectedOwned(), true, P::Unknown);
        S clean = expectedOwned(); acquired(r, clean); verify(r, clean, true, P::Acquire);
        r.reset(); verify(r, S{}, false, P::Unknown);
        prime(r); clean = expectedOwned(); acquired(r, clean); verify(r, clean, true, P::Acquire);
    }
    group("nested begin preserves nonzero evidence");
    {
        R r; prime(r); S e = expectedOwned(); acquired(r, e, 1);
        r.recordFenceWait(DEVICE, 1, &F1, VK_SUCCESS, .125, &generation, 1);
        e.buckets[1].fenceCalls = 1; e.buckets[1].milliseconds = .125;
        e.acquirePresentFenceCalls = 1; e.acquirePresentFenceMilliseconds = .125;
        check(!r.begin(OTHER_DEVICE, OTHER_SWAPCHAIN, &otherGeneration, nullptr, nullptr, 0), "nested rejected");
        e.invalid = true; verify(r, e, true, P::Acquire);
    }
    group("bad begin clears ended evidence and recovers");
    for (unsigned variant = 0; variant < 10; ++variant) {
        R r; prime(r); S e = expectedOwned(); acquired(r, e); r.end();
        VkFence fences[] = {F0, F1}; uint8_t pending[] = {1, 1};
        VkDevice d = DEVICE; VkSwapchainKHR sc = SWAPCHAIN; const void *token = &generation;
        const VkFence *array = fences; const uint8_t *bits = pending; uint32_t count = 2;
        switch (variant) {
            case 0: d = VK_NULL_HANDLE; break; case 1: sc = VK_NULL_HANDLE; break;
            case 2: token = nullptr; break; case 3: array = nullptr; break; case 4: bits = nullptr; break;
            case 5: fences[1] = VK_NULL_HANDLE; break; case 6: fences[1] = F0; break;
            case 7: pending[0] = 2; break; case 8: count = 33; break; case 9: pending[1] = 255; break;
        }
        check(!r.begin(d, sc, token, array, bits, count), "bad shape rejected");
        S empty; empty.invalid = true; verify(r, empty, false, P::Unknown);
        prime(r); verify(r, expectedOwned(), true, P::Unknown);
    }
    group("capacity and maintenance off");
    {
        R r; std::array<VkFence, 32> fences{}; std::array<uint8_t, 32> pending{}; S e;
        e.fenceCount = 32;
        for (size_t i = 0; i < 32; ++i) { fences[i] = handle<VkFence>(100 + i); pending[i] = uint8_t(i % 2); e.fences[i] = fences[i]; e.pending[i] = pending[i]; }
        check(r.begin(DEVICE, SWAPCHAIN, &generation, fences.data(), pending.data(), 32), "capacity32 accepted"); verify(r, e, true, P::Unknown);
        r.end(); check(r.begin(DEVICE, SWAPCHAIN, &generation, nullptr, nullptr, 0), "maintenance off"); e = S{};
        acquired(r, e, 17); verify(r, e, true, P::Acquire);
        check(r.recordFenceWait(DEVICE, 1, &OTHER_FENCE, VK_SUCCESS, .1, &generation, 17), "count0 unclassified acquire fence wait");
        e.buckets[1].fenceCalls = 1; e.buckets[1].milliseconds = .1; e.acquireOtherFenceCalls = 1; verify(r, e, true, P::Acquire);
    }
    group("owned arrays survive mutation and destruction");
    {
        R r;
        {
            VkFence fences[] = {F0, F1}; uint8_t pending[] = {1, 1};
            check(r.begin(DEVICE, SWAPCHAIN, &generation, fences, pending, 2), "scoped arrays begin");
            fences[0] = OTHER_FENCE; pending[1] = 0;
            verify(r, expectedOwned(), true, P::Unknown);
        }
        S e = expectedOwned(); acquired(r, e, 1); r.recordFenceWait(DEVICE, 1, &F1, VK_SUCCESS, .25, &generation, 1);
        e.buckets[1].fenceCalls = 1; e.buckets[1].milliseconds = .25; e.acquirePresentFenceCalls = 1; e.acquirePresentFenceMilliseconds = .25;
        verify(r, e, true, P::Acquire);
        R other; prime(other); other.reset(); verify(r, e, true, P::Acquire);
    }
    group("acquire independent identity and duplicate success");
    for (unsigned variant = 0; variant < 5; ++variant) {
        R r; prime(r); S e = expectedOwned(); r.setPhase(P::Acquire);
        VkDevice d = DEVICE; VkSwapchainKHR sc = SWAPCHAIN; const void *token = &generation; uint32_t index = 0;
        if (variant == 0) d = OTHER_DEVICE;
        if (variant == 1) sc = OTHER_SWAPCHAIN;
        if (variant == 2) token = &otherGeneration;
        if (variant == 3) index = 2;
        if (variant == 4) acquired(r, e);
        check(r.onAcquire(d, sc, VK_SUCCESS, index, token), "actual SUCCESS preserved");
        ++e.acquireCalls; ++e.acquireSuccesses; e.identityFailures = 1; e.invalid = true;
        verify(r, e, true, P::Acquire);
    }
    group("present fence identity independent from engine index");
    for (unsigned variant = 0; variant < 7; ++variant) {
        R r; VkFence fences[] = {F0, F1}; uint8_t pending[] = {1, 1}; if (variant == 5) pending[0] = 0;
        r.begin(DEVICE, SWAPCHAIN, &generation, fences, pending, 2); S e = expectedOwned(); e.pending[0] = pending[0]; r.setPhase(P::Acquire);
        if (variant != 6) acquired(r, e);
        VkDevice d = DEVICE; const void *token = &generation; uint32_t engineIndex = 0; VkFence waitFence = F0;
        if (variant == 0) d = OTHER_DEVICE;
        if (variant == 1) token = &otherGeneration;
        if (variant == 2) engineIndex = 1;
        if (variant == 3) waitFence = F1;
        if (variant == 4) { waitFence = F1; engineIndex = 1; }
        check(r.recordFenceWait(d, 1, &waitFence, VK_SUCCESS, .25, token, engineIndex), "actual wait recorded");
        e.buckets[1].fenceCalls = 1; e.buckets[1].milliseconds = .25;
        e.acquirePresentFenceCalls = 1; e.acquirePresentFenceMilliseconds = .25; e.identityFailures = 1; e.invalid = true;
        verify(r, e, true, P::Acquire);
    }
    group("failed acquire does not create usable index");
    {
        R r; prime(r); S e = expectedOwned(); r.setPhase(P::Acquire);
        check(!r.onAcquire(DEVICE, SWAPCHAIN, VK_ERROR_OUT_OF_DATE_KHR, 0, &generation), "failed acquire preserved"); e.acquireCalls = 1;
        verify(r, e, true, P::Acquire);
        r.recordFenceWait(DEVICE, 1, &F0, VK_SUCCESS, .1, &generation, 0);
        e.buckets[1].fenceCalls = 1; e.buckets[1].milliseconds = .1; e.acquirePresentFenceCalls = 1; e.acquirePresentFenceMilliseconds = .1; e.invalid = true; e.identityFailures = 1;
        verify(r, e, true, P::Acquire);
    }
    group("other acquire fence categories and wait failures");
    {
        R r; prime(r); S e = expectedOwned(); acquired(r, e);
        const VkFence both[] = {F0, F1};
        r.recordFenceWait(DEVICE, 2, both, VK_ERROR_DEVICE_LOST, .2, &generation, 0);
        r.recordFenceWait(DEVICE, 1, &OTHER_FENCE, VK_TIMEOUT, .3, &generation, 0);
        r.recordSemaphoreWait(DEVICE, VK_TIMEOUT, .4);
        e.buckets[1].fenceCalls = 2; e.buckets[1].semaphoreCalls = 1; e.buckets[1].failures = 3; e.buckets[1].milliseconds = .9; e.acquireOtherFenceCalls = 2;
        verify(r, e, true, P::Acquire);
    }
    group("invalid wait inputs never publish invalid duration");
    for (bool semaphore : {false, true}) {
        for (unsigned variant = 0; variant < 5; ++variant) {
            R r; prime(r); S e = expectedOwned();
            r.setPhase(P::Observe); VkDevice d = DEVICE; double ms = .1;
            if (variant == 0) ms = std::numeric_limits<double>::quiet_NaN();
            if (variant == 1) ms = std::numeric_limits<double>::infinity();
            if (variant == 2) ms = -std::numeric_limits<double>::infinity();
            if (variant == 3) ms = -.1;
            if (variant == 4) d = VK_NULL_HANDLE;
            check(!(semaphore ? r.recordSemaphoreWait(d, VK_SUCCESS, ms) : r.recordFenceWait(d, 1, &F0, VK_SUCCESS, ms, &generation, 0)), "bad wait rejected");
            e.invalid = true; verify(r, e, true, P::Observe);
        }
    }
    for (unsigned variant = 0; variant < 3; ++variant) {
        R r; prime(r); S e = expectedOwned(); VkFence nullFence = VK_NULL_HANDLE;
        check(!r.recordFenceWait(DEVICE, variant == 0 ? 0 : 1, variant == 1 ? nullptr : (variant == 2 ? &nullFence : &F0), VK_SUCCESS, .1, &generation, 0), "bad fence array/count rejected");
        e.invalid = true; verify(r, e, true, P::Unknown);
    }
    group("wrong-device semaphore preserves actual evidence");
    {
        R r; prime(r); S e = expectedOwned();
        check(r.recordSemaphoreWait(OTHER_DEVICE, VK_SUCCESS, .5), "non-null actual call recorded");
        e.buckets[0].semaphoreCalls = 1; e.buckets[0].milliseconds = .5; e.identityFailures = 1; e.invalid = true; verify(r, e, true, P::Unknown);
    }
    group("invalid phase rejects without changing phase or accumulated counts");
    {
        R r; prime(r); S e = expectedOwned(); r.setPhase(P::Record);
        r.recordSemaphoreWait(DEVICE, VK_SUCCESS, .25); e.buckets[2].semaphoreCalls = 1; e.buckets[2].milliseconds = .25;
        check(!r.setPhase(static_cast<P>(255)), "invalid enum rejected"); e.invalid = true; verify(r, e, true, P::Record);
    }
    std::printf("wait_phase_profile_tests: groups=%u declared=%u executed=%u passed=%u failures=%u\n", groups, checks, checks, checks - failures, failures);
    return failures ? 1 : 0;
}
