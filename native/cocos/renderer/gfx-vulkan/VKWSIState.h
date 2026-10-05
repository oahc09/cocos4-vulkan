/****************************************************************************
 Copyright (c) 2026 Xiamen Yaji Software Co., Ltd.

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
****************************************************************************/

// ============================================================================
// Pure WSI swapchain state model + acquire image-index validity contract
// (M2-B, plan section 10).
//
// This header owns NO Vulkan objects, calls NO Vulkan functions, and proves
// NOTHING about WSI image/semaphore completion or release. "Renderer-allowed
// index" is a different concept from Vulkan object ownership: clearing the
// renderable index here NEVER retires or releases any VkImage/VkSemaphore -
// the caller must complete/retire its Vulkan work separately.
//
// XR is intentionally NOT modeled here (the XR runtime owns its images and
// acquire/release). Fatal results (VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_
// DEVICE_MEMORY, ...) are recorded in a dedicated fatal flag and are never
// disguised as SURFACE_LOST or silently retried.
// ============================================================================

#pragma once

#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace cc {
namespace gfx {

// Plan section 10.1 states (names kept verbatim).
enum class VKSwapchainState : uint8_t {
    READY,
    SUBOPTIMAL,
    OUT_OF_DATE,
    SUSPENDED_ZERO_SIZE,
    SURFACE_LOST,
    RECREATE_PENDING,
};

class VulkanWSIStatus final {
public:
    static constexpr uint32_t INVALID_IMAGE_INDEX = 0xFFFFFFFFu;

    VulkanWSIStatus() = default;

    // ---- inputs ---------------------------------------------------------------

    // Surface gone: everything drops; re-availability is signalled separately.
    void setSurfaceAvailable(bool available) {
        if (!available) {
            surfaceAvailable_ = false;
            state_ = VKSwapchainState::SURFACE_LOST;
            invalidateImage_();
            return;
        }
        if (surfaceAvailable_) return; // no-op when already available
        surfaceAvailable_ = true;
        // first availability: a real swapchain (re)create is required before
        // any acquire, unless the extent is (still) zero.
        if (extentIsZero_()) {
            state_ = VKSwapchainState::SUSPENDED_ZERO_SIZE;
        } else {
            state_ = VKSwapchainState::RECREATE_PENDING;
        }
        invalidateImage_();
    }

    // Explicit extent: 0 means 0 (minimized/hidden). There is NO "unknown"
    // sentinel mixed into this path.
    void setExtent(uint32_t width, uint32_t height) {
        const bool changed = (width != width_) || (height != height_);
        width_ = width;
        height_ = height;
        if (!surfaceAvailable_) {
            state_ = VKSwapchainState::SURFACE_LOST; // extent alone never revives a surface
            invalidateImage_();
            return;
        }
        if (extentIsZero_()) {
            state_ = VKSwapchainState::SUSPENDED_ZERO_SIZE;
            invalidateImage_();
            return;
        }
        const bool recovered = changed; // any change (incl. from 0) invalidates layout
        if (state_ == VKSwapchainState::SUSPENDED_ZERO_SIZE || recovered) {
            if (state_ != VKSwapchainState::RECREATE_PENDING) {
                state_ = VKSwapchainState::RECREATE_PENDING;
            }
            invalidateImage_(); // old index may point outside the new extent layout
        }
    }

    // Explicit request (e.g. detected mode change). No-surface / zero-size /
    // fatal keep their (non-renderable) states.
    void requestRecreate() {
        if (!surfaceAvailable_ || extentIsZero_() || hasFatalError()) {
            invalidateImage_();
            return;
        }
        state_ = VKSwapchainState::RECREATE_PENDING;
        invalidateImage_();
    }

    // Swapchain (re)creation finished. Succeeds only with a live surface, a
    // non-zero extent, count > 0 and no fatal error; on success the old
    // index/pending state is cleared. NEVER clears a fatal error.
    bool onRecreated(uint32_t imageCount) {
        if (!surfaceAvailable_ || extentIsZero_() || imageCount == 0 || hasFatalError()) {
            requestRecreate();
            return false;
        }
        imageCount_ = imageCount;
        state_ = VKSwapchainState::READY;
        invalidateImage_();
        return true;
    }

    // ---- acquire contract -----------------------------------------------------

    // Failed surface queries/swapchain creation cannot leave a READY generation.
    void onRecreateFailure(VkResult result) {
        invalidateImage_();
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            surfaceAvailable_ = false;
            state_ = VKSwapchainState::SURFACE_LOST;
        } else if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            state_ = VKSwapchainState::OUT_OF_DATE;
        } else if (result == VK_NOT_READY || result == VK_TIMEOUT) {
            requestRecreate();
        } else {
            setFatal_(result == VK_SUCCESS ? VK_ERROR_UNKNOWN : result);
        }
    }

    bool canAcquire() const {
        return state_ == VKSwapchainState::READY &&
               imageIndex_ == INVALID_IMAGE_INDEX &&
               !acquirePending_ && !hasFatalError();
    }

    // A begin that returns false must not be followed by a real vkAcquire.
    bool beginAcquire() {
        if (!canAcquire()) return false;
        invalidateImage_();
        acquirePending_ = true;
        return true;
    }

    // Returns true only when the frame may render into the given index.
    // Guarantees: no begin -> reject; index >= imageCount on a "successful"
    // result -> model-level fatal, index rejected.
    bool completeAcquire(VkResult result, uint32_t imageIndex) {
        if (!acquirePending_) return false;
        acquirePending_ = false;
        lastAcquireResult_ = result;

        const bool indexInRange = imageIndex < imageCount_;
        switch (result) {
            case VK_SUCCESS:
                if (indexInRange) {
                    state_ = VKSwapchainState::READY;
                    imageIndex_ = imageIndex;
                    return true;
                }
                setFatal_(VK_ERROR_UNKNOWN); // driver returned an out-of-range index
                return false;
            case VK_SUBOPTIMAL_KHR:
                if (indexInRange) {
                    state_ = VKSwapchainState::SUBOPTIMAL; // renderable, deferred recreate
                    imageIndex_ = imageIndex;
                    return true;
                }
                setFatal_(VK_ERROR_UNKNOWN);
                return false;
            case VK_ERROR_OUT_OF_DATE_KHR:
                state_ = VKSwapchainState::OUT_OF_DATE;
                invalidateImage_();
                return false;
            case VK_ERROR_SURFACE_LOST_KHR:
                surfaceAvailable_ = false;
                state_ = VKSwapchainState::SURFACE_LOST;
                invalidateImage_();
                return false;
            case VK_TIMEOUT:
            case VK_NOT_READY:
                state_ = VKSwapchainState::READY; // retry next frame
                invalidateImage_();
                return false;
            default:
                setFatal_(result); // DEVICE_LOST / OOM / ... never retried here
                return false;
        }
    }

    // ---- present contract -----------------------------------------------------

    // Called only after the backend has consumed acquire synchronization and
    // released the acquired image. This is not a synthetic present result.
    bool onAcquiredImageReleased() {
        if (!hasRenderableImage()) return false;
        const bool deferredRecreate = state_ == VKSwapchainState::SUBOPTIMAL;
        invalidateImage_();
        state_ = deferredRecreate ? VKSwapchainState::RECREATE_PENDING : VKSwapchainState::READY;
        return true;
    }

    // Only valid when THIS frame holds a renderable (acquired) image; a
    // present without a successful acquire is rejected. Return value is false
    // ONLY for that rejection; every processed result returns true (the
    // result itself is visible via state/getters).
    bool onPresentResult(VkResult result) {
        if (!hasRenderableImage()) return false;
        lastPresentResult_ = result;
        const bool wasSuboptimal = (state_ == VKSwapchainState::SUBOPTIMAL);
        invalidateImage_(); // renderer must stop using the index after present

        switch (result) {
            case VK_SUCCESS:
                // A deferred recreate from SUBOPTIMAL is NOT consumed by a
                // successful present.
                state_ = wasSuboptimal ? VKSwapchainState::RECREATE_PENDING
                                       : VKSwapchainState::READY;
                return true;
            case VK_SUBOPTIMAL_KHR:
                state_ = VKSwapchainState::RECREATE_PENDING;
                return true;
            case VK_ERROR_OUT_OF_DATE_KHR:
                state_ = VKSwapchainState::OUT_OF_DATE;
                return true;
            case VK_ERROR_SURFACE_LOST_KHR:
                surfaceAvailable_ = false;
                state_ = VKSwapchainState::SURFACE_LOST;
                return true;
            default:
                setFatal_(result);
                return true;
        }
    }

    // ---- observation ------------------------------------------------------------

    VKSwapchainState state() const { return state_; }
    bool surfaceAvailable() const { return surfaceAvailable_; }
    uint32_t surfaceWidth() const { return width_; }
    uint32_t surfaceHeight() const { return height_; }
    uint32_t imageCount() const { return imageCount_; }
    VkResult lastAcquireResult() const { return lastAcquireResult_; }
    VkResult lastPresentResult() const { return lastPresentResult_; }
    VkResult fatalResult() const { return fatalResult_; }

    // True only while an acquired image may be used by the renderer this
    // frame. Stale indices from OUT_OF_DATE / zero-size / surface-lost /
    // fatal states can never become valid again through getters.
    bool hasRenderableImage() const {
        return imageIndex_ != INVALID_IMAGE_INDEX &&
               (state_ == VKSwapchainState::READY || state_ == VKSwapchainState::SUBOPTIMAL) &&
               !hasFatalError();
    }

    uint32_t currentImageIndex() const { return imageIndex_; }

    bool needsSwapchainRecreate() const {
        return state_ == VKSwapchainState::RECREATE_PENDING ||
               state_ == VKSwapchainState::OUT_OF_DATE;
    }
    bool needsSurfaceRecreate() const {
        return state_ == VKSwapchainState::SURFACE_LOST;
    }
    bool hasFatalError() const { return fatalResult_ != VK_SUCCESS; }

private:
    // Clears the renderer-allowed index. This does NOT release or retire any
    // Vulkan resource - the caller must complete/retire its Vk work itself.
    void invalidateImage_() {
        imageIndex_ = INVALID_IMAGE_INDEX;
        acquirePending_ = false;
    }
    void setFatal_(VkResult result) {
        fatalResult_ = result;
        invalidateImage_();
    }
    bool extentIsZero_() const { return width_ == 0 || height_ == 0; }

    VKSwapchainState state_{VKSwapchainState::SURFACE_LOST};
    bool surfaceAvailable_{false};
    uint32_t width_{0};
    uint32_t height_{0};
    uint32_t imageCount_{0};
    uint32_t imageIndex_{INVALID_IMAGE_INDEX};
    bool acquirePending_{false};
    VkResult lastAcquireResult_{VK_SUCCESS};
    VkResult lastPresentResult_{VK_SUCCESS};
    VkResult fatalResult_{VK_SUCCESS};
};

} // namespace gfx
} // namespace cc
