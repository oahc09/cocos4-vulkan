/****************************************************************************
 Copyright (c) 2021-2023 Xiamen Yaji Software Co., Ltd.

 http://www.cocos.com

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do so,
 subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

#include "VKSwapchain.h"
#include "VKCommands.h"
#include "VKDevice.h"
#include "VKMeasurementProbe.h"
#include "VKGPUObjects.h"
#include "VKQueue.h"
#include "VKRenderPass.h"
#include "VKTexture.h"
#include "VKUtils.h"
#include "VKWSILifetime.h"
#include <cstdlib>

#include "application/ApplicationManager.h"
#include "platform/interfaces/modules/ISystemWindow.h"
#include "platform/interfaces/modules/ISystemWindowManager.h"
#include "platform/interfaces/modules/IXRInterface.h"

#if CC_SWAPPY_ENABLED
    #include "platform/android/AndroidPlatform.h"
    #include "swappy/swappyVk.h"
    #include "swappy/swappy_common.h"
#endif

namespace cc {
namespace gfx {

CCVKGPUWSISurface::~CCVKGPUWSISurface() {
    if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
}

VkResult CCVKGPUWSISurface::retireGenerations(uint64_t timeout) {
    VkResult status = VK_SUCCESS;
    for (const auto &weak : generations) {
        if (auto generation = weak.lock()) {
            if (!generation->retired) {
                const VkResult result = generation->waitForPresentCompletion(timeout);
                if (!wsiTeardownSafe(result)) return result;
                if (result == VK_ERROR_DEVICE_LOST) status = result;
                generation->releaseVulkanObjects();
            }
        }
    }
    generations.clear();
    return status;
}

VkResult CCVKGPUWSIGeneration::waitForPresentCompletion(uint64_t timeout) {
    if (retired) return VK_SUCCESS;
    if (presentFences.empty()) {
        // Core WSI has no host presentation-completion primitive. Preserve
        // the conventional retirement fallback, never use it for frame pacing.
        return vkDeviceWaitIdle(device);
    }
    VkResult status = VK_SUCCESS;
    for (size_t i = 0; i < presentFences.size(); ++i) {
        if (presentPending[i]) {
            const VkResult result = vkWaitForFences(device, 1, &presentFences[i], VK_TRUE, timeout);
            if (!wsiTeardownSafe(result)) return result;
            presentPending[i] = false;
            if (result == VK_ERROR_DEVICE_LOST) status = result;
        }
    }
    return status;
}

void CCVKGPUWSIGeneration::releaseVulkanObjects() {
    if (retired) return;
    retired = true;
    // CPU wrappers may still refer to this invalidated generation. All GPU
    // work/presentation has completed; stale generation gates reject reuse.
    // Removing dependent Vulkan objects first also permits a NEW surface to
    // attach to the same native window (otherwise NATIVE_WINDOW_IN_USE).
    for (VkFramebuffer framebuffer : framebuffers) vkDestroyFramebuffer(device, framebuffer, nullptr);
    framebuffers.clear();
    for (VkImageView view : imageViews) vkDestroyImageView(device, view, nullptr);
    imageViews.clear();
    for (VkSemaphore semaphore : renderFinished) vkDestroySemaphore(device, semaphore, nullptr);
    for (VkFence fence : presentFences) vkDestroyFence(device, fence, nullptr);
    renderFinished.clear();
    presentFences.clear();
    if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
    surface.reset();
}

CCVKGPUWSIGeneration::~CCVKGPUWSIGeneration() {
    // Normal retirement explicitly releases before dropping its owning lease.
    // A last-reference bug cannot silently destroy pending presentation work.
    if (!retired && swapchain) {
        CC_LOG_ERROR("Vulkan WSI generation lost its retirement owner.");
        std::abort();
    }
}

CCVKSwapchain::CCVKSwapchain() {
    _typedID = generateObjectID<decltype(this)>();
    _preRotationEnabled = ENABLE_PRE_ROTATION;
}

CCVKSwapchain::~CCVKSwapchain() {
    destroy();
}

void CCVKSwapchain::doInit(const SwapchainInfo &info) {
    // Opt-in measurement uses the normal present-mode selection and pNext
    // construction. Default and XR swapchains keep the application's policy.
    if (!CC_GET_XR_INTERFACE() && getVulkanMeasurementPresentPolicy() == VulkanMeasurementPresentPolicy::Immediate) {
        _vsyncMode = VsyncMode::OFF;
    }
    _xr = CC_GET_XR_INTERFACE();
    if (_xr) {
        _xr->updateXRSwapchainTypedID(getTypedID());
    }
    auto *gpuDevice = CCVKDevice::getInstance()->gpuDevice();
    const auto *gpuContext = CCVKDevice::getInstance()->gpuContext();
    _gpuSwapchain = ccnew CCVKGPUSwapchain;
    _gpuSwapchain->runtimeOwnedImages = _xr != nullptr;
    _gpuSwapchain->wsi.setExtent(info.width, info.height);
    gpuDevice->swapchains.insert(_gpuSwapchain);

    createVkSurface();

    Format colorFmt = selectSurfaceConfig();
    const Format depthStencilFmt = Format::DEPTH_STENCIL;
    ///////////////////// Texture Creation /////////////////////
    auto width = static_cast<int32_t>(info.width);
    auto height = static_cast<int32_t>(info.height);
    if (_xr) {
        colorFmt = _xr->getXRSwapchainFormat();
        width = _xr->getXRConfig(xr::XRConfigKey::SWAPCHAIN_WIDTH).getInt();
        height = _xr->getXRConfig(xr::XRConfigKey::SWAPCHAIN_HEIGHT).getInt();
    }
    _colorTexture = ccnew CCVKTexture;
    _depthStencilTexture = ccnew CCVKTexture;

    SwapchainTextureInfo textureInfo;
    textureInfo.swapchain = this;
    textureInfo.format = colorFmt;
    textureInfo.width = width;
    textureInfo.height = height;
    initTexture(textureInfo, _colorTexture);

    textureInfo.format = depthStencilFmt;
    initTexture(textureInfo, _depthStencilTexture);

#if CC_PLATFORM == CC_PLATFORM_ANDROID
    auto *window = CC_GET_SYSTEM_WINDOW(_windowId);
    auto viewSize = window->getViewSize();
    checkSwapchainStatus(viewSize.width, viewSize.height);
#else
    checkSwapchainStatus();
#endif
}

Format CCVKSwapchain::selectSurfaceConfig() {
    auto *gpuDevice = CCVKDevice::getInstance()->gpuDevice();
    const auto *gpuContext = CCVKDevice::getInstance()->gpuContext();
    ///////////////////// Parameter Selection /////////////////////

    uint32_t queueFamilyPropertiesCount = utils::toUint(gpuContext->queueFamilyProperties.size());
    _gpuSwapchain->queueFamilyPresentables.resize(queueFamilyPropertiesCount);
    for (uint32_t propertyIndex = 0U; propertyIndex < queueFamilyPropertiesCount; propertyIndex++) {
        if (_xr) {
            _gpuSwapchain->queueFamilyPresentables[propertyIndex] = true;
        } else {
            vkGetPhysicalDeviceSurfaceSupportKHR(gpuContext->physicalDevice, propertyIndex,
                                                 _gpuSwapchain->vkSurface, &_gpuSwapchain->queueFamilyPresentables[propertyIndex]);
        }
    }

    // find other possible queues if not presentable
    auto *queue = static_cast<CCVKQueue *>(CCVKDevice::getInstance()->getQueue());
    if (!_gpuSwapchain->queueFamilyPresentables[queue->gpuQueue()->queueFamilyIndex]) {
        auto &indices = queue->gpuQueue()->possibleQueueFamilyIndices;
        indices.erase(std::remove_if(indices.begin(), indices.end(), [this](uint32_t i) {
                          return !_gpuSwapchain->queueFamilyPresentables[i];
                      }),
                      indices.end());
        CC_ASSERT(!_gpuSwapchain->queueFamilyPresentables.empty());
        cmdFuncCCVKGetDeviceQueue(CCVKDevice::getInstance(), queue->gpuQueue());
    }

    Format colorFmt = Format::BGRA8;

    if (_gpuSwapchain->vkSurface != VK_NULL_HANDLE) {
        VkSurfaceCapabilitiesKHR surfaceCapabilities{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpuContext->physicalDevice, _gpuSwapchain->vkSurface, &surfaceCapabilities);

        uint32_t surfaceFormatCount = 0U;
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpuContext->physicalDevice, _gpuSwapchain->vkSurface, &surfaceFormatCount, nullptr));
        ccstd::vector<VkSurfaceFormatKHR> surfaceFormats(surfaceFormatCount);
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpuContext->physicalDevice, _gpuSwapchain->vkSurface, &surfaceFormatCount, surfaceFormats.data()));

        uint32_t presentModeCount = 0U;
        VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(gpuContext->physicalDevice, _gpuSwapchain->vkSurface, &presentModeCount, nullptr));
        ccstd::vector<VkPresentModeKHR> presentModes(presentModeCount);
        VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(gpuContext->physicalDevice, _gpuSwapchain->vkSurface, &presentModeCount, presentModes.data()));

        VkFormat colorFormat = VK_FORMAT_B8G8R8A8_UNORM;
        VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        // If the surface format list only includes one entry with VK_FORMAT_UNDEFINED,
        // there is no preferred format, so we assume VK_FORMAT_B8G8R8A8_UNORM
        if ((surfaceFormatCount == 1) && (surfaceFormats[0].format == VK_FORMAT_UNDEFINED)) {
            colorFormat = VK_FORMAT_B8G8R8A8_UNORM;
            colorSpace = surfaceFormats[0].colorSpace;
        } else {
            // iterate over the list of available surface format and
            // check for the presence of VK_FORMAT_B8G8R8A8_UNORM
            bool imageFormatFound = false;
            for (VkSurfaceFormatKHR &surfaceFormat : surfaceFormats) {
                if (surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM) {
                    colorFormat = surfaceFormat.format;
                    colorSpace = surfaceFormat.colorSpace;
                    imageFormatFound = true;
                    break;
                }
            }

            // in case VK_FORMAT_B8G8R8A8_UNORM is not available
            // select the first available color format
            if (!imageFormatFound) {
                colorFormat = surfaceFormats[0].format;
                colorSpace = surfaceFormats[0].colorSpace;
                switch (colorFormat) {
                    case VK_FORMAT_R8G8B8A8_UNORM: colorFmt = Format::RGBA8; break;
                    case VK_FORMAT_R8G8B8A8_SRGB: colorFmt = Format::SRGB8_A8; break;
                    case VK_FORMAT_R5G6B5_UNORM_PACK16: colorFmt = Format::R5G6B5; break;
                    default: CC_ABORT(); break;
                }
            }
        }

        // Select a present mode for the swapchain

        ccstd::vector<VkPresentModeKHR> presentModePriorityList;

        switch (_vsyncMode) {
            case VsyncMode::OFF: presentModePriorityList.insert(presentModePriorityList.end(), {VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR}); break;
            case VsyncMode::ON: presentModePriorityList.insert(presentModePriorityList.end(), {VK_PRESENT_MODE_FIFO_KHR}); break;
            case VsyncMode::RELAXED: presentModePriorityList.insert(presentModePriorityList.end(), {VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_FIFO_KHR}); break;
            case VsyncMode::MAILBOX: presentModePriorityList.insert(presentModePriorityList.end(), {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_FIFO_KHR}); break;
            case VsyncMode::HALF: presentModePriorityList.insert(presentModePriorityList.end(), {VK_PRESENT_MODE_FIFO_KHR}); break; // no easy fallback
        }

        VkPresentModeKHR swapchainPresentMode = VK_PRESENT_MODE_FIFO_KHR;

        // UNASSIGNED-BestPractices-vkCreateSwapchainKHR-swapchain-presentmode-not-fifo
#if !defined(VK_USE_PLATFORM_ANDROID_KHR)
        for (VkPresentModeKHR presentMode : presentModePriorityList) {
            if (std::find(presentModes.begin(), presentModes.end(), presentMode) != presentModes.end()) {
                swapchainPresentMode = presentMode;
                break;
            }
        }
#endif

        // Determine the number of images
        uint32_t desiredNumberOfSwapchainImages = std::max(gpuDevice->backBufferCount, surfaceCapabilities.minImageCount);

        VkExtent2D imageExtent = {1U, 1U};

        // Find a supported composite alpha format (not all devices support alpha opaque)
        VkCompositeAlphaFlagBitsKHR compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        // Simply select the first composite alpha format available
        ccstd::vector<VkCompositeAlphaFlagBitsKHR> compositeAlphaFlags = {
            VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
            VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        };
        for (VkCompositeAlphaFlagBitsKHR compositeAlphaFlag : compositeAlphaFlags) {
            if (surfaceCapabilities.supportedCompositeAlpha & compositeAlphaFlag) {
                compositeAlpha = compositeAlphaFlag;
                break;
            };
        }
        VkImageUsageFlags imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        // Enable transfer source on swap chain images if supported
        if (surfaceCapabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
            imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        }

        // Enable transfer destination on swap chain images if supported
        if (surfaceCapabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
            imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        }

        _gpuSwapchain->createInfo.surface = _gpuSwapchain->vkSurface;
        _gpuSwapchain->createInfo.minImageCount = desiredNumberOfSwapchainImages;
        _gpuSwapchain->createInfo.imageFormat = colorFormat;
        _gpuSwapchain->createInfo.imageColorSpace = colorSpace;
        _gpuSwapchain->createInfo.imageExtent = imageExtent;
        _gpuSwapchain->createInfo.imageUsage = imageUsage;
        _gpuSwapchain->createInfo.imageArrayLayers = 1;
        _gpuSwapchain->createInfo.preTransform = VkSurfaceTransformFlagBitsKHR::VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        _gpuSwapchain->createInfo.compositeAlpha = compositeAlpha;
        _gpuSwapchain->createInfo.presentMode = swapchainPresentMode;
        _gpuSwapchain->createInfo.clipped = VK_TRUE; // Setting clipped to VK_TRUE allows the implementation to discard rendering outside of the surface area
    }
    return colorFmt;
}

void CCVKSwapchain::doDestroy() {
    if (!_gpuSwapchain) return;
    auto *device = CCVKDevice::getInstance();
    auto *gpuDevice = device->gpuDevice();
    // This void API is also called from the destructor. Transfer the owning
    // lease BEFORE trying a finite wait; timeout must not destroy or leak it.
    _gpuSwapchain->retiring = true;
    gpuDevice->deferredSwapchains.push_back(_gpuSwapchain);
    const VkResult result = device->retireDeferredSwapchains();
    if (!wsiTeardownSafe(result)) CC_LOG_ERROR("Vulkan swapchain destruction deferred: %d", result);
    _depthStencilTexture = nullptr;
    _colorTexture = nullptr;
    _gpuSwapchain = nullptr;
}

void CCVKSwapchain::doResize(uint32_t width, uint32_t height, SurfaceTransform /*transform*/) {
    checkSwapchainStatus(width, height); // the orientation info from system event is not reliable
}

bool CCVKSwapchain::checkSwapchainStatus(uint32_t width, uint32_t height) {
    if (_gpuSwapchain && _gpuSwapchain->surfaceRetirementPending) {
        doDestroySurface();
        return false;
    }
    if (_gpuSwapchain && CCVKDevice::getInstance()->abortAcquiredSwapchain(_gpuSwapchain) != VK_SUCCESS) return false;
    if (!_gpuSwapchain || (!_xr && (_gpuSwapchain->wsi.hasFatalError() || _gpuSwapchain->wsi.needsSurfaceRecreate()))) return false;
    uint32_t newWidth = width;
    uint32_t newHeight = height;
    uint32_t imageCount = 0;
    if (_xr) {
        newWidth = static_cast<CCVKTexture *>(_colorTexture.get())->_info.width;
        newHeight = static_cast<CCVKTexture *>(_colorTexture.get())->_info.height;
        // xr double eyes need six images
        std::vector<VkImage> vkImagesLeft;
        std::vector<VkImage> vkImagesRight;
        _xr->getXRSwapchainVkImages(vkImagesLeft, static_cast<uint32_t>(cc::xr::XREye::LEFT));
        _xr->getXRSwapchainVkImages(vkImagesRight, static_cast<uint32_t>(cc::xr::XREye::RIGHT));
        _gpuSwapchain->swapchainImages.resize(vkImagesLeft.size() + vkImagesRight.size());
        _gpuSwapchain->swapchainImages.clear();
        // 0-1-2
        _gpuSwapchain->swapchainImages.insert(_gpuSwapchain->swapchainImages.end(), vkImagesLeft.begin(), vkImagesLeft.end());
        // 3-4-5
        _gpuSwapchain->swapchainImages.insert(_gpuSwapchain->swapchainImages.end(), vkImagesRight.begin(), vkImagesRight.end());

        imageCount = _gpuSwapchain->swapchainImages.size();
        _gpuSwapchain->createInfo.imageExtent.width = newWidth;
        _gpuSwapchain->createInfo.imageExtent.height = newHeight;
        auto *gpuDevice = CCVKDevice::getInstance()->gpuDevice();
        gpuDevice->curBackBufferIndex = 0;
        _gpuSwapchain->curImageIndex = 0;
        CCVKDevice::getInstance()->updateBackBufferCount(imageCount);
        if (CCVKDevice::getInstance()->waitAllFences() != VK_SUCCESS) return false;
        CC_LOG_INFO("Resizing surface: %dx%d, surface rotation: %d degrees", newWidth, newHeight, static_cast<uint32_t>(_transform) * 90);
    } else {
        if (_gpuSwapchain->vkSurface == VK_NULL_HANDLE) { // vkSurface will be set to VK_NULL_HANDLE after call doDestroySurface
            return false;
        }
        auto *gpuDevice = CCVKDevice::getInstance()->gpuDevice();
        const auto *gpuContext = CCVKDevice::getInstance()->gpuContext();

        VkSurfaceCapabilitiesKHR surfaceCapabilities;
        const VkResult surfaceResult = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpuContext->physicalDevice, _gpuSwapchain->vkSurface, &surfaceCapabilities);
        if (surfaceResult != VK_SUCCESS) {
            _gpuSwapchain->wsi.onRecreateFailure(surfaceResult);
            _gpuSwapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
            CC_LOG_ERROR("Vulkan surface capability query failed: %d", surfaceResult);
            return false;
        }

        // surfaceCapabilities.currentExtent seems to remain the same
        // during any size/orientation change events on android devices
        // so we prefer the system input (oriented size) here
        newWidth = width == UINT32_MAX ? _gpuSwapchain->wsi.surfaceWidth() : width;
        newHeight = height == UINT32_MAX ? _gpuSwapchain->wsi.surfaceHeight() : height;
        // A zero explicitly supplied by resize remains suspended until a
        // non-zero resize/createSurface event arrives. A pending non-zero
        // recreation may adopt a fixed extent advertised by the new surface.
        if (newWidth && newHeight && surfaceCapabilities.currentExtent.width != UINT32_MAX) {
            newWidth = surfaceCapabilities.currentExtent.width;
            newHeight = surfaceCapabilities.currentExtent.height;
        }
        if (newWidth && newHeight && surfaceCapabilities.currentExtent.width == UINT32_MAX) {
            newWidth = std::clamp(newWidth, surfaceCapabilities.minImageExtent.width, surfaceCapabilities.maxImageExtent.width);
            newHeight = std::clamp(newHeight, surfaceCapabilities.minImageExtent.height, surfaceCapabilities.maxImageExtent.height);
        }
        _gpuSwapchain->wsi.setExtent(newWidth, newHeight);
        if (newWidth == 0 || newHeight == 0) {
            _gpuSwapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
            _gpuSwapchain->lastPresentResult = VK_NOT_READY;
            return false;
        }

        if (_gpuSwapchain->createInfo.imageExtent.width == newWidth &&
            _gpuSwapchain->createInfo.imageExtent.height == newHeight &&
            !_gpuSwapchain->wsi.needsSwapchainRecreate() && _gpuSwapchain->wsi.canAcquire()) {
            return true;
        }

        if (newWidth == static_cast<uint32_t>(-1)) {
            _gpuSwapchain->createInfo.imageExtent.width = _colorTexture->getWidth();
            _gpuSwapchain->createInfo.imageExtent.height = _colorTexture->getHeight();
        } else {
            _gpuSwapchain->createInfo.imageExtent.width = newWidth;
            _gpuSwapchain->createInfo.imageExtent.height = newHeight;
        }

        if (newWidth == 0 || newHeight == 0) {
            _gpuSwapchain->lastPresentResult = VK_NOT_READY;
            return false;
        }

        const Format colorFmt = selectSurfaceConfig();
        static_cast<CCVKTexture *>(_colorTexture.get())->_info.format = colorFmt;
        static_cast<CCVKTexture *>(_colorTexture.get())->_viewInfo.format = colorFmt;
        _gpuSwapchain->createInfo.imageExtent = {newWidth, newHeight};
        _gpuSwapchain->createInfo.minImageCount = std::max(gpuDevice->backBufferCount, surfaceCapabilities.minImageCount);
        if (surfaceCapabilities.maxImageCount) {
            _gpuSwapchain->createInfo.minImageCount = std::min(_gpuSwapchain->createInfo.minImageCount, surfaceCapabilities.maxImageCount);
        }
        _gpuSwapchain->createInfo.preTransform = surfaceCapabilities.currentTransform;
        _gpuSwapchain->createInfo.surface = _gpuSwapchain->vkSurface;
        _gpuSwapchain->createInfo.oldSwapchain = _gpuSwapchain->vkSwapchain;

        CC_LOG_INFO("Resizing surface: %dx%d", newWidth, newHeight);

        if (CCVKDevice::getInstance()->waitAllFences() != VK_SUCCESS) return false;
        // Prove old-generation presentation completion BEFORE vkCreateSwapchain
        // retires the old handle; a timeout leaves a retryable old generation.
        if (_gpuSwapchain->imageGeneration) {
            const VkResult result = _gpuSwapchain->imageGeneration->waitForPresentCompletion();
            if (result != VK_SUCCESS) {
                if (result == VK_ERROR_DEVICE_LOST) gpuDevice->deviceLost = true;
                return false;
            }
        }

        VkSwapchainKHR vkSwapchain = VK_NULL_HANDLE;
        // Declare the one selected mode when maintenance1 is enabled. This
        // does not enable dynamic mode switching or retain a stack pNext node
        // in the persistent recreation template.
        VkSwapchainCreateInfoKHR createInfo = _gpuSwapchain->createInfo;
        VkSwapchainPresentModesCreateInfoEXT presentModes{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT};
        if (gpuDevice->capabilities.swapchainMaintenance1.enabled) {
            presentModes.pNext = createInfo.pNext;
            presentModes.presentModeCount = 1;
            presentModes.pPresentModes = &createInfo.presentMode;
            createInfo.pNext = &presentModes;
        }
        const VkResult recreateResult = vkCreateSwapchainKHR(gpuDevice->vkDevice, &createInfo, nullptr, &vkSwapchain);
        if (recreateResult != VK_SUCCESS) {
            // The supplied oldSwapchain becomes retired even on failure. A
            // recoverable retry must not pass that retired handle again.
            if (recreateResult == VK_ERROR_OUT_OF_DATE_KHR || recreateResult == VK_ERROR_SURFACE_LOST_KHR ||
                recreateResult == VK_NOT_READY || recreateResult == VK_TIMEOUT) {
                // The current generation was waited above. Only drop handles
                // after their result-bearing retirement succeeds.
                const VkResult result = destroySwapchain(gpuDevice);
                if (!wsiTeardownSafe(result)) return false;
                _gpuSwapchain->createInfo.oldSwapchain = VK_NULL_HANDLE;
            }
            _gpuSwapchain->wsi.onRecreateFailure(recreateResult);
            _gpuSwapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
            CC_LOG_ERROR("Vulkan swapchain recreation failed: %d", recreateResult);
            return false;
        }

        // No work was enqueued between the successful preflight and creation.
        if (_gpuSwapchain->imageGeneration) _gpuSwapchain->imageGeneration->releaseVulkanObjects();
        _gpuSwapchain->imageGeneration.reset();
        _gpuSwapchain->swapchainImages.clear();
#if CC_SWAPPY_ENABLED
        if (_gpuSwapchain->vkSwapchain) SwappyVk_destroySwapchain(gpuDevice->vkDevice, _gpuSwapchain->vkSwapchain);
#endif
        _gpuSwapchain->vkSwapchain = vkSwapchain;
        _gpuSwapchain->imageGeneration = std::make_shared<CCVKGPUWSIGeneration>();
        auto &generation = *_gpuSwapchain->imageGeneration;
        generation.device = gpuDevice->vkDevice;
        generation.swapchain = vkSwapchain;
        generation.surface = _gpuSwapchain->surfaceOwner;
        auto &generations = generation.surface->generations;
        generations.erase(std::remove_if(generations.begin(), generations.end(), [](const auto &weak) { return weak.expired(); }), generations.end());
        generation.surface->generations.push_back(_gpuSwapchain->imageGeneration);

        VK_CHECK(vkGetSwapchainImagesKHR(gpuDevice->vkDevice, _gpuSwapchain->vkSwapchain, &imageCount, nullptr));
        CCVKDevice::getInstance()->updateBackBufferCount(imageCount);
        _gpuSwapchain->swapchainImages.resize(imageCount);
        VK_CHECK(vkGetSwapchainImagesKHR(gpuDevice->vkDevice, _gpuSwapchain->vkSwapchain, &imageCount, _gpuSwapchain->swapchainImages.data()));
        generation.renderFinished.resize(imageCount, VK_NULL_HANDLE);
        generation.imageInitialized.resize(imageCount, false);
        generation.imageAcquired.resize(imageCount, false);
        generation.acquireSerials.resize(imageCount, 0);
        VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        for (auto &semaphore : generation.renderFinished) VK_CHECK(vkCreateSemaphore(gpuDevice->vkDevice, &semaphoreInfo, nullptr, &semaphore));
        if (gpuDevice->capabilities.swapchainMaintenance1.enabled) {
            generation.presentFences.resize(imageCount, VK_NULL_HANDLE);
            generation.presentPending.resize(imageCount, false);
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            for (auto &fence : generation.presentFences) VK_CHECK(vkCreateFence(gpuDevice->vkDevice, &fenceInfo, nullptr, &fence));
        }
    }
    ++_generation;
    _gpuSwapchain->generation = _generation;

    // should skip size check, since the old swapchain has already been destroyed
    static_cast<CCVKTexture *>(_colorTexture.get())->_info.width = 1;
    static_cast<CCVKTexture *>(_depthStencilTexture.get())->_info.width = 1;
    _colorTexture->resize(newWidth, newHeight);
    _depthStencilTexture->resize(newWidth, newHeight);

    bool hasStencil = GFX_FORMAT_INFOS[toNumber(_depthStencilTexture->getFormat())].hasStencil;
    ccstd::vector<VkImageMemoryBarrier> barriers(imageCount, VkImageMemoryBarrier{});
    VkPipelineStageFlags srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    ThsvsImageBarrier tempBarrier{};
    tempBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tempBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    tempBarrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    tempBarrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    VkPipelineStageFlags tempSrcStageMask = 0;
    VkPipelineStageFlags tempDstStageMask = 0;
    auto *colorGPUTexture = static_cast<CCVKTexture *>(_colorTexture.get())->gpuTexture();
    auto *depthStencilGPUTexture = static_cast<CCVKTexture *>(_depthStencilTexture.get())->gpuTexture();
    for (uint32_t i = 0U; i < imageCount; i++) {
        tempBarrier.nextAccessCount = 1;
        tempBarrier.pNextAccesses = getAccessType(AccessFlagBit::DEPTH_STENCIL_ATTACHMENT_WRITE);
        tempBarrier.image = depthStencilGPUTexture->swapchainVkImages[i];
        tempBarrier.subresourceRange.aspectMask = hasStencil ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        thsvsGetVulkanImageMemoryBarrier(tempBarrier, &tempSrcStageMask, &tempDstStageMask, &barriers[i]);
        srcStageMask |= tempSrcStageMask;
        dstStageMask |= tempDstStageMask;
    }
    CCVKDevice::getInstance()->gpuTransportHub()->checkIn(
        [&](const CCVKGPUCommandBuffer *gpuCommandBuffer) {
            cmdPipelineBarrier(CCVKDevice::getInstance()->gpuDevice(), gpuCommandBuffer->vkCommandBuffer, srcStageMask, dstStageMask, 0, 0, nullptr, 0, nullptr,
                                 utils::toUint(barriers.size()), barriers.data());
        },
        true); // submit immediately

    colorGPUTexture->currentAccessTypes.assign(1, THSVS_ACCESS_PRESENT);
    depthStencilGPUTexture->currentAccessTypes.assign(1, THSVS_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ);

    _gpuSwapchain->lastPresentResult = VK_SUCCESS;
    if (!_xr) {
        _gpuSwapchain->wsi.onRecreated(imageCount);
        _gpuSwapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
    }

    // Android Game Frame Pacing:swappy
#if CC_SWAPPY_ENABLED

    auto *gpuDevice = CCVKDevice::getInstance()->gpuDevice();
    const auto *gpuContext = CCVKDevice::getInstance()->gpuContext();
    int32_t fps = cc::BasePlatform::getPlatform()->getFps();

    uint64_t frameRefreshIntervalNS;
    auto *platform = static_cast<AndroidPlatform *>(cc::BasePlatform::getPlatform());
    auto *window = CC_GET_SYSTEM_WINDOW(_windowId);
    void *windowHandle = reinterpret_cast<void *>(window->getWindowHandle());
    SwappyVk_initAndGetRefreshCycleDuration(static_cast<JNIEnv *>(platform->getEnv()),
                                            static_cast<jobject>(platform->getActivity()),
                                            gpuContext->physicalDevice,
                                            gpuDevice->vkDevice,
                                            _gpuSwapchain->vkSwapchain,
                                            &frameRefreshIntervalNS);
    SwappyVk_setSwapIntervalNS(gpuDevice->vkDevice, _gpuSwapchain->vkSwapchain, fps ? 1000000000L / fps : frameRefreshIntervalNS);
    SwappyVk_setWindow(gpuDevice->vkDevice, _gpuSwapchain->vkSwapchain, static_cast<ANativeWindow *>(windowHandle));
#endif

    return true;
}

VkResult CCVKSwapchain::destroySwapchain(CCVKGPUDevice *gpuDevice) {
    if (_gpuSwapchain->vkSwapchain == VK_NULL_HANDLE) return VK_SUCCESS;
    const VkResult submitted = CCVKDevice::getInstance()->waitAllFences();
    if (!wsiTeardownSafe(submitted)) return submitted;
    const VkResult presented = _gpuSwapchain->imageGeneration->waitForPresentCompletion();
    if (!wsiTeardownSafe(presented)) return presented;
    if (presented == VK_ERROR_DEVICE_LOST) gpuDevice->deviceLost = true;
    _gpuSwapchain->imageGeneration->releaseVulkanObjects();
#if CC_SWAPPY_ENABLED
    SwappyVk_destroySwapchain(gpuDevice->vkDevice, _gpuSwapchain->vkSwapchain);
#endif
    _gpuSwapchain->swapchainImages.clear();
    _gpuSwapchain->imageGeneration.reset();
    _gpuSwapchain->vkSwapchain = VK_NULL_HANDLE;
    _gpuSwapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
    return gpuDevice->deviceLost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

void CCVKSwapchain::doDestroySurface() {
    if (!_gpuSwapchain || _gpuSwapchain->vkSurface == VK_NULL_HANDLE) return;
    _gpuSwapchain->surfaceRetirementPending = true;
    _gpuSwapchain->retiring = true;
    const VkResult result = CCVKDevice::getInstance()->retireSwapchain(_gpuSwapchain);
    if (!wsiTeardownSafe(result)) {
        CC_LOG_ERROR("Vulkan surface destruction deferred: %d", result);
        return;
    }
    _gpuSwapchain->surfaceRetirementPending = false;
    _gpuSwapchain->lastPresentResult = VK_NOT_READY;
}

void CCVKSwapchain::doCreateSurface(void *windowHandle) { // NOLINT
    if (_gpuSwapchain && _gpuSwapchain->surfaceRetirementPending) doDestroySurface();
    if (!_gpuSwapchain || _gpuSwapchain->vkSurface != VK_NULL_HANDLE || CCVKDevice::getInstance()->gpuDevice()->deviceLost) return;
    _gpuSwapchain->retiring = false;
    createVkSurface();
#if CC_PLATFORM == CC_PLATFORM_ANDROID
    auto *window = CC_GET_SYSTEM_WINDOW(_windowId);
    auto viewSize = window->getViewSize();
    checkSwapchainStatus(viewSize.width, viewSize.height);
#else
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    RECT clientRect{};
    if (GetClientRect(reinterpret_cast<HWND>(_windowHandle), &clientRect)) {
        checkSwapchainStatus(clientRect.right - clientRect.left, clientRect.bottom - clientRect.top);
    } else {
        checkSwapchainStatus();
    }
#else
    checkSwapchainStatus();
#endif
#endif
}

void CCVKSwapchain::createVkSurface() {
    if (_xr) {
        // xr do not need VkSurface
        _gpuSwapchain->vkSurface = VK_NULL_HANDLE;
        return;
    }
    const auto *gpuContext = CCVKDevice::getInstance()->gpuContext();

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    VkAndroidSurfaceCreateInfoKHR surfaceCreateInfo{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surfaceCreateInfo.window = reinterpret_cast<ANativeWindow *>(_windowHandle);
    VK_CHECK(vkCreateAndroidSurfaceKHR(gpuContext->vkInstance, &surfaceCreateInfo, nullptr, &_gpuSwapchain->vkSurface));
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    VkWin32SurfaceCreateInfoKHR surfaceCreateInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceCreateInfo.hinstance = static_cast<HINSTANCE>(GetModuleHandle(nullptr));
    surfaceCreateInfo.hwnd = reinterpret_cast<HWND>(_windowHandle);
    VK_CHECK(vkCreateWin32SurfaceKHR(gpuContext->vkInstance, &surfaceCreateInfo, nullptr, &_gpuSwapchain->vkSurface));
#elif defined(VK_USE_PLATFORM_VI_NN)
    VkViSurfaceCreateInfoNN surfaceCreateInfo{VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN};
    surfaceCreateInfo.window = _windowHandle;
    VK_CHECK(vkCreateViSurfaceNN(gpuContext->vkInstance, &surfaceCreateInfo, nullptr, &_gpuSwapchain->vkSurface));
#elif defined(VK_USE_PLATFORM_MACOS_MVK)
    VkMacOSSurfaceCreateInfoMVK surfaceCreateInfo{VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK};
    surfaceCreateInfo.pView = _windowHandle;
    VK_CHECK(vkCreateMacOSSurfaceMVK(gpuContext->vkInstance, &surfaceCreateInfo, nullptr, &_gpuSwapchain->vkSurface));
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
    VkWaylandSurfaceCreateInfoKHR surfaceCreateInfo{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
    surfaceCreateInfo.display = nullptr; // TODO
    surfaceCreateInfo.surface = reinterpret_cast<wl_surface *>(_windowHandle);
    VK_CHECK(vkCreateWaylandSurfaceKHR(gpuContext->vkInstance, &surfaceCreateInfo, nullptr, &_gpuSwapchain->vkSurface));
#elif defined(VK_USE_PLATFORM_XCB_KHR)
    VkXcbSurfaceCreateInfoKHR surfaceCreateInfo{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
    surfaceCreateInfo.connection = nullptr; // TODO
    surfaceCreateInfo.window = reinterpret_cast<uint64_t>(_windowHandle);
    VK_CHECK(vkCreateXcbSurfaceKHR(gpuContext->vkInstance, &surfaceCreateInfo, nullptr, &_gpuSwapchain->vkSurface));
#else
    #pragma error Platform not supported
#endif
    _gpuSwapchain->surfaceOwner = std::make_shared<CCVKGPUWSISurface>();
    _gpuSwapchain->surfaceOwner->instance = gpuContext->vkInstance;
    _gpuSwapchain->surfaceOwner->surface = _gpuSwapchain->vkSurface;
    _gpuSwapchain->wsi.setSurfaceAvailable(_gpuSwapchain->vkSurface != VK_NULL_HANDLE);
}

} // namespace gfx
} // namespace cc
