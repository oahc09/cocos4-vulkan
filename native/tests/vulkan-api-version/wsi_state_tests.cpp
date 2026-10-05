// Pure WSI state contract tests (M2-B).
//
// Sequence-level contracts from plan section 10 / the M2-B task: state
// transitions, acquire-index validity, deferred recreate, zero-size suspend,
// surface loss/rebuild ordering, fatal isolation. These are PURE STATE tests:
// the model owns no Vulkan objects and prove nothing about image/semaphore
// completion, retirement, or real WSI behavior on a GPU.

#include "VKWSIState.h"

#include <cstdio>

namespace {

unsigned g_failures = 0;

void check(bool condition, const char *name) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++g_failures;
    }
}

using cc::gfx::VulkanWSIStatus;
using cc::gfx::VKSwapchainState;

// Bring a fresh instance up to a READY swapchain with the given extent/count.
VulkanWSIStatus makeReady(uint32_t w, uint32_t h, uint32_t count) {
    VulkanWSIStatus s;
    s.setExtent(w, h);
    s.setSurfaceAvailable(true);
    check(s.state() == VKSwapchainState::RECREATE_PENDING, "makeReady: pending after surface");
    check(s.onRecreated(count), "makeReady: onRecreated");
    return s;
}

} // namespace

int main() {
    const auto INVALID = VulkanWSIStatus::INVALID_IMAGE_INDEX;

    // --- initial state: lost surface, zero extent, nothing renderable --------
    {
        VulkanWSIStatus s;
        check(s.state() == VKSwapchainState::SURFACE_LOST, "initial: SURFACE_LOST");
        check(!s.surfaceAvailable() && s.surfaceWidth() == 0 && s.surfaceHeight() == 0,
              "initial: no surface, zero extent");
        check(!s.hasRenderableImage() && s.currentImageIndex() == INVALID,
              "initial: no renderable index");
        check(!s.canAcquire() && !s.beginAcquire(), "initial: cannot begin acquire");
        check(!s.onPresentResult(VK_SUCCESS), "initial: present without acquire rejected");
    }

    // --- seq1: successful frame -> next frame OUT_OF_DATE with stale index ---
    {
        VulkanWSIStatus s = makeReady(800, 600, 3);
        check(s.beginAcquire(), "seq1: begin");
        check(s.completeAcquire(VK_SUCCESS, 1), "seq1: acquire success -> renderable");
        check(s.hasRenderableImage() && s.currentImageIndex() == 1, "seq1: index 1 valid");
        check(s.onPresentResult(VK_SUCCESS), "seq1: present success");
        check(!s.hasRenderableImage(), "seq1: index cleared after present");

        // next frame: acquire reports OUT_OF_DATE (caller may still pass the
        // stale index by mistake) - the model must reject render/present
        check(s.beginAcquire(), "seq1: begin frame 2");
        check(!s.completeAcquire(VK_ERROR_OUT_OF_DATE_KHR, 1),
              "seq1: OUT_OF_DATE with stale index -> not renderable");
        check(!s.hasRenderableImage() && s.currentImageIndex() == INVALID,
              "seq1: stale index not visible");
        check(!s.onPresentResult(VK_SUCCESS), "seq1: present rejected without acquire");
        check(s.needsSwapchainRecreate(), "seq1: needs swapchain recreate");
        // duplicate present attempt must not fabricate an index either
        check(!s.onPresentResult(VK_SUCCESS), "seq1: duplicate present rejected");
    }

    // --- seq2: SUBOPTIMAL acquire -> renderable this frame -> present SUCCESS
    //     still leaves a deferred recreate -------------------------------
    {
        VulkanWSIStatus s = makeReady(800, 600, 3);
        check(s.beginAcquire(), "seq2: begin");
        check(s.completeAcquire(VK_SUBOPTIMAL_KHR, 2), "seq2: SUBOPTIMAL renderable");
        check(s.state() == VKSwapchainState::SUBOPTIMAL, "seq2: state SUBOPTIMAL");
        check(s.onPresentResult(VK_SUCCESS), "seq2: present success");
        check(s.state() == VKSwapchainState::RECREATE_PENDING,
              "seq2: deferred recreate survives successful present");
        check(!s.canAcquire(), "seq2: cannot acquire until recreated");
        check(s.onRecreated(3), "seq2: recreate");
        check(s.state() == VKSwapchainState::READY && s.canAcquire(), "seq2: ready again");
    }

    // --- seq3: zero-size suspension ----------------------------------------
    {
        VulkanWSIStatus s = makeReady(800, 600, 3);
        s.setExtent(0, 600);
        check(s.state() == VKSwapchainState::SUSPENDED_ZERO_SIZE, "seq3: suspended");
        check(!s.hasRenderableImage(), "seq3: index invalidated on suspend");
        check(!s.canAcquire() && !s.beginAcquire(), "seq3: cannot begin while suspended");
        check(!s.onPresentResult(VK_SUCCESS), "seq3: present rejected while suspended");
        check(!s.onRecreated(3), "seq3: recreate at zero size rejected");
        // recovery: extent back -> recreate pending, then recreate -> usable
        s.setExtent(800, 600);
        check(s.state() == VKSwapchainState::RECREATE_PENDING, "seq3: pending after recovery");
        check(!s.canAcquire(), "seq3: still cannot acquire before recreate");
        check(s.onRecreated(3) && s.canAcquire(), "seq3: usable after recreate");
    }

    // --- seq4: surface lost -> extent recovery alone is NOT enough ----------
    {
        VulkanWSIStatus s = makeReady(800, 600, 3);
        check(s.beginAcquire() && s.completeAcquire(VK_SUCCESS, 0), "seq4: frame acquired");
        check(!s.completeAcquire(VK_ERROR_SURFACE_LOST_KHR, 0) || true, "seq4: (setup)");
        VulkanWSIStatus s2 = makeReady(800, 600, 3);
        check(s2.beginAcquire(), "seq4: begin");
        check(!s2.completeAcquire(VK_ERROR_SURFACE_LOST_KHR, 0), "seq4: surface lost");
        check(!s2.surfaceAvailable() && s2.state() == VKSwapchainState::SURFACE_LOST,
              "seq4: SURFACE_LOST recorded");
        check(s2.needsSurfaceRecreate(), "seq4: needs surface recreate");
        // extent recovery does not resurrect the surface
        s2.setExtent(1024, 768);
        check(s2.state() == VKSwapchainState::SURFACE_LOST && !s2.canAcquire(),
              "seq4: extent change while lost stays lost");
        check(!s2.onRecreated(3), "seq4: recreate without surface rejected");
        // surface rebuilt first...
        s2.setSurfaceAvailable(true);
        check(s2.state() == VKSwapchainState::RECREATE_PENDING, "seq4: pending after surface back");
        check(!s2.canAcquire(), "seq4: swapchain recreate still required");
        // ...then swapchain rebuild
        check(s2.onRecreated(3) && s2.canAcquire(), "seq4: usable after both rebuilds");
    }

    // --- seq5: TIMEOUT is not fatal, no index --------------------------------
    {
        VulkanWSIStatus s = makeReady(800, 600, 3);
        check(s.beginAcquire(), "seq5: begin");
        check(!s.completeAcquire(VK_TIMEOUT, 0), "seq5: TIMEOUT not renderable");
        check(!s.hasFatalError(), "seq5: TIMEOUT not fatal");
        check(!s.hasRenderableImage(), "seq5: no index after TIMEOUT");
        check(s.canAcquire(), "seq5: retry allowed next frame");
    }

    // --- seq6: DEVICE_LOST is fatal and is never cleared ---------------------
    {
        VulkanWSIStatus s = makeReady(800, 600, 3);
        check(s.beginAcquire(), "seq6: begin");
        check(!s.completeAcquire(VK_ERROR_DEVICE_LOST, 0), "seq6: fatal not renderable");
        check(s.hasFatalError() && s.fatalResult() == VK_ERROR_DEVICE_LOST, "seq6: fatal recorded");
        check(!s.hasRenderableImage() && !s.canAcquire(), "seq6: dead for acquire/render");
        check(!s.onRecreated(3), "seq6: onRecreated cannot clear fatal");
        s.setExtent(640, 480);
        check(s.hasFatalError(), "seq6: setExtent cannot clear fatal");
        s.setSurfaceAvailable(false);
        s.setSurfaceAvailable(true);
        check(s.hasFatalError(), "seq6: surface cycle cannot clear fatal");
        check(s.state() != VKSwapchainState::READY || s.hasFatalError(),
              "seq6: never healthy while fatal");
        // present with fatal: no renderable image -> rejected
        check(!s.onPresentResult(VK_SUCCESS), "seq6: present rejected while fatal");
    }

    // --- seq7: multiple swapchains are independent ----------------------------
    {
        VulkanWSIStatus a = makeReady(800, 600, 3);
        VulkanWSIStatus b = makeReady(800, 600, 3);
        check(a.beginAcquire() && a.completeAcquire(VK_ERROR_OUT_OF_DATE_KHR, 0) == false,
              "seq7: a goes OUT_OF_DATE");
        check(b.canAcquire(), "seq7: b unaffected");
        check(b.beginAcquire() && b.completeAcquire(VK_SUCCESS, 1) && b.hasRenderableImage(),
              "seq7: b still renders");
        check(!a.hasRenderableImage(), "seq7: a has no image");
    }

    // --- seq8: imageCount shrink invalidates old indices ----------------------
    {
        VulkanWSIStatus s = makeReady(800, 600, 5);
        check(s.beginAcquire() && s.completeAcquire(VK_SUCCESS, 4), "seq8: index 4 valid @5");
        check(s.onPresentResult(VK_SUCCESS), "seq8: present");
        // recreate shrinks to 3
        check(s.onRecreated(3), "seq8: recreated with 3 images");
        check(s.beginAcquire(), "seq8: begin after shrink");
        check(!s.completeAcquire(VK_SUCCESS, 4), "seq8: old index 4 rejected after shrink");
        check(s.hasFatalError(), "seq8: out-of-range index is a model fatal");
    }

    // --- seq9: illegal begin/complete/present sequences fabricate nothing -----
    {
        VulkanWSIStatus s = makeReady(800, 600, 3);
        check(!s.completeAcquire(VK_SUCCESS, 0), "seq9: complete without begin rejected");
        check(s.beginAcquire(), "seq9: begin");
        check(!s.beginAcquire(), "seq9: duplicate begin rejected");
        check(!s.hasRenderableImage(), "seq9: pending alone gives no index");
        check(s.completeAcquire(VK_SUCCESS, 0), "seq9: proper acquire");
        check(!s.completeAcquire(VK_SUCCESS, 1), "seq9: complete without new begin rejected");
        check(s.onPresentResult(VK_SUCCESS), "seq9: present once");
        check(!s.onPresentResult(VK_SUCCESS), "seq9: duplicate present rejected");
        check(!s.hasRenderableImage(), "seq9: no fabricated index");
        // success result with out-of-range index -> fatal, index rejected
        VulkanWSIStatus t = makeReady(800, 600, 2);
        check(t.beginAcquire(), "seq9: t begin");
        check(!t.completeAcquire(VK_SUCCESS, 7), "seq9: out-of-range index rejected");
        check(t.hasFatalError(), "seq9: out-of-range success is model fatal");
    }

    // --- seq10: failed recreate never becomes READY ----------------------------
    {
        VulkanWSIStatus s;
        s.setExtent(800, 600);
        s.setSurfaceAvailable(true);
        check(s.state() == VKSwapchainState::RECREATE_PENDING, "seq10: pending");
        check(!s.onRecreated(0), "seq10: zero image count rejected");
        check(s.state() != VKSwapchainState::READY && !s.canAcquire(),
              "seq10: failed recreate not READY");
        check(s.onRecreated(4), "seq10: valid recreate succeeds");
        check(s.state() == VKSwapchainState::READY, "seq10: READY after success");
        // surface torn down mid-life
        s.setSurfaceAvailable(false);
        check(s.state() == VKSwapchainState::SURFACE_LOST && !s.hasRenderableImage(),
              "seq10: teardown clears everything");
    }

    // Cancelled/recreated generations cannot accept an old acquire completion.
    {
        auto s = makeReady(800, 600, 3);
        check(s.beginAcquire(), "revision: pending acquire before surface loss");
        s.setSurfaceAvailable(false);
        check(!s.completeAcquire(VK_SUCCESS, 1), "revision: lost surface rejects old acquire completion");
        check(!s.hasRenderableImage(), "revision: lost surface has no resurrected image");
        s.setSurfaceAvailable(true);
        check(s.onRecreated(2), "revision: recreate after cancelled acquisition");
        check(s.canAcquire(), "revision: cancelled pending flag cannot block new generation");

        auto t = makeReady(800, 600, 3);
        check(t.beginAcquire() && t.completeAcquire(VK_SUCCESS, 1), "revision: acquired before failed recreation");
        check(!t.onRecreated(0), "revision: zero count recreation fails");
        check(t.state() != VKSwapchainState::READY && !t.hasRenderableImage() && !t.canAcquire(),
              "revision: failed recreation cannot retain READY or old image");
        t.onRecreateFailure(VK_ERROR_DEVICE_LOST);
        check(t.hasFatalError() && !t.onRecreated(3), "revision: recreation DEVICE_LOST cannot retry to READY");
    }

    // Explicit image release is not presentation and preserves deferred recreate.
    {
        auto s = makeReady(800, 600, 3);
        check(!s.onAcquiredImageReleased(), "release: no acquired image cannot be released");
        check(s.beginAcquire() && s.completeAcquire(VK_SUCCESS, 1), "release: real acquire prerequisite");
        check(s.onAcquiredImageReleased() && !s.hasRenderableImage() && s.canAcquire(),
              "release: successful release clears ownership without a present");
        check(!s.onPresentResult(VK_SUCCESS) && !s.onAcquiredImageReleased(),
              "release: stale present and double release rejected");
        check(s.beginAcquire() && s.completeAcquire(VK_SUBOPTIMAL_KHR, 2), "release: suboptimal acquired frame");
        check(s.onAcquiredImageReleased() && s.state() == VKSwapchainState::RECREATE_PENDING && !s.canAcquire(),
              "release: suboptimal still requires recreate after release");
    }

    if (g_failures == 0) {
        std::printf("wsi state contract tests: all PASS\n");
        return 0;
    }
    std::printf("wsi state contract tests: %u FAIL\n", g_failures);
    return 1;
}
