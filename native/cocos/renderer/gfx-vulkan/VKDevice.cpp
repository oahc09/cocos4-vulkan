/****************************************************************************
 Copyright (c) 2020-2023 Xiamen Yaji Software Co., Ltd.

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

#include "VKDevice.h"
#include <atomic>
#include "VKMeasurementProbe.h"
#include <cstdlib>
#include "VKBuffer.h"
#include "VKCommandBuffer.h"
#include "VKCommands.h"
#include "VKDescriptorSet.h"
#include "VKDescriptorSetLayout.h"
#include "VKFramebuffer.h"
#include "VKGPUObjects.h"
#include "VKInputAssembler.h"
#include "VKPipelineCache.h"
#include "VKPipelineLayout.h"
#include "VKPipelineState.h"
#include "VKQueryPool.h"
#include "VKQueue.h"
#include "VKRenderPass.h"
#include "VKShader.h"
#include "VKSwapchain.h"
#include "VKWSILifetime.h"
#include "VKTexture.h"
#include "VKUtils.h"
#include "base/Utils.h"
#include "gfx-base/GFXDef-common.h"
#include "states/VKBufferBarrier.h"
#include "states/VKGeneralBarrier.h"
#include "states/VKSampler.h"
#include "states/VKTextureBarrier.h"

#include "application/ApplicationManager.h"
#include "gfx-base/SPIRVUtils.h"
#include "platform/interfaces/modules/IXRInterface.h"
#include "profiler/Profiler.h"

#if CC_SWAPPY_ENABLED
    #include "swappy/swappyVk.h"
#endif

CC_DISABLE_WARNINGS()
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#define THSVS_ERROR_CHECK_MIXED_IMAGE_LAYOUT
// remote potential hazard because of programmable blend
// #define THSVS_ERROR_CHECK_POTENTIAL_HAZARD
#define THSVS_SIMPLER_VULKAN_SYNCHRONIZATION_IMPLEMENTATION
#include "thsvs_simpler_vulkan_synchronization.h"
CC_ENABLE_WARNINGS()

namespace cc {
namespace gfx {

static VkResult VKAPI_PTR vkCreateRenderPass2KHRFallback(
    VkDevice device,
    const VkRenderPassCreateInfo2 *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkRenderPass *pRenderPass);

std::shared_ptr<GpuCompletionTracker> CCVKGPUDevice::getCompletionTracker(VkQueue queue) {
    auto &tracker = completionTrackers[queue];
    if (!tracker) {
        tracker = std::make_shared<GpuCompletionTracker>(vkDevice, queue,
            GpuCompletionTracker::Functions{&vkQueueSubmit, &vkGetFenceStatus, &vkWaitForFences,
                                           &vkGetSemaphoreCounterValue, &vkWaitSemaphores,
                                           apiVersionInfo.effectiveDeviceApiVersion >= VK_API_VERSION_1_3 ? &vkQueueSubmit2 : &vkQueueSubmit2KHR});
        if (runtimePaths.useSubmit2 && tracker->configureSubmit2() != VK_SUCCESS) {
            tracker.reset(); return nullptr;
        }
        if (runtimePaths.useTimelineSemaphore) {
            auto &semaphore = completionSemaphores[queue];
            VkSemaphoreTypeCreateInfo typeInfo{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
            typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
            VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            createInfo.pNext = &typeInfo;
            VkResult result = vkCreateSemaphore(vkDevice, &createInfo, nullptr, &semaphore);
            if (result == VK_SUCCESS) result = tracker->configureTimeline(semaphore, maxTimelineSemaphoreValueDifference);
            if (result != VK_SUCCESS) {
                CC_LOG_ERROR("Vulkan completion tracker initialization failed: result=%d", result);
                if (semaphore) vkDestroySemaphore(vkDevice, semaphore, nullptr);
                completionSemaphores.erase(queue);
                tracker.reset();
                return nullptr;
            }
        }
    }
    return tracker;
}

VkResult CCVKGPUDevice::submitTracked(CCVKGPUQueue *queue, const VulkanSubmitBatch &info, VkFence fence, GpuCompletionPoint &point,
                                     const std::vector<std::shared_ptr<CCVKGPURecording>> &recordings) {
    point = {};
    for (const auto &recording : recordings) {
        if (!recording || !recording->valid()) return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::shared_ptr<CCVKGPURecordingSubmission> submission;
    if (!recordings.empty()) {
        // All ownership and pool bookkeeping allocation precedes the driver call.
        submission = std::make_shared<CCVKGPURecordingSubmission>();
        submission->tracker = queue->completionTracker;
        submission->recordings = recordings;
        recordingSubmissions.push_back(submission);
        for (const auto &recording : recordings) recording->trackSubmission(submission);
    }
    // All bookkeeping storage precedes the actual driver submission. A
    // delayed/secondary recording can read instances belonging to other slots.
    std::vector<uint32_t> slots{curBackBufferIndex};
    for (const auto &recording : recordings) recording->appendBufferSlots(slots);
    if (slotCompletions.size() < backBufferCount) slotCompletions.resize(backBufferCount);
    for (auto slot : slots) {
        if (slot >= backBufferCount) {
            if (submission) recordingSubmissions.pop_back();
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    const auto domain = queue->completionTracker->domain();
    for (auto slot : slots) {
        auto &points = slotCompletions[slot];
        if (std::none_of(points.begin(), points.end(), [&](GpuCompletionPoint candidate) { return candidate.domain == domain; })) {
            points.push_back({domain, 0});
        }
    }
    const VkResult result = queue->completionTracker->submit(info, fence, point);
    for (auto slot : slots) {
        auto &points = slotCompletions[slot];
        auto existing = std::find_if(points.begin(), points.end(), [&](GpuCompletionPoint candidate) { return candidate.domain == domain; });
        if (result == VK_SUCCESS) *existing = point;
        else if (!existing->value) points.erase(existing);
    }
    if (result == VK_SUCCESS) {
        if (submission) submission->point = point;
        queue->lastCompletion = point;
        ++trackedSubmitCount;
        if (fence) ++submissionFenceCount;
    } else if (submission && result != VK_ERROR_DEVICE_LOST) {
        recordingSubmissions.pop_back();
    }
    // A lost submit may have executed. Retain its empty-point submission lease
    // until final device teardown proves quiescence; do not mark it completed.
    return result;
}

void CCVKGPUDevice::retireRecordings() {
    for (auto it = recordingSubmissions.begin(); it != recordingSubmissions.end();) {
        const auto &submission = *it;
        if (!submission->point.empty() && submission->tracker->isComplete(submission->point)) {
            it = recordingSubmissions.erase(it);
        } else {
            ++it;
        }
    }
}

VkResult CCVKGPUDevice::observeCompletedFences(const VkFence *fences, uint32_t count) {
    for (auto &entry : completionTrackers) {
        for (uint32_t i = 0; i < count; ++i) {
            const VkResult result = entry.second->observeFenceCompletion(fences[i]);
            if (result != VK_SUCCESS) return result;
        }
    }
    return VK_SUCCESS;
}

bool CCVKGPUDevice::slotComplete(uint32_t slot) {
    if (slot >= slotCompletions.size()) return true;
    for (auto point : slotCompletions[slot]) {
        const auto found = std::find_if(completionTrackers.begin(), completionTrackers.end(), [&](const auto &entry) {
            return entry.second->domain() == point.domain;
        });
        if (found == completionTrackers.end() || !found->second->isComplete(point)) return false;
    }
    return true;
}

VkResult CCVKGPUDevice::waitSlot(uint32_t slot, uint64_t timeout) {
    if (slot >= slotCompletions.size()) return VK_SUCCESS;
    for (const auto point : slotCompletions[slot]) {
        const auto found = std::find_if(completionTrackers.begin(), completionTrackers.end(), [&](const auto &entry) {
            return entry.second && entry.second->domain() == point.domain;
        });
        if (found == completionTrackers.end()) return VK_ERROR_INITIALIZATION_FAILED;
        const auto result = found->second->wait(point, timeout);
        if (result != VK_SUCCESS) return result;
    }
    return VK_SUCCESS;
}

void CCVKGPUDevice::shutdownCompletionTrackers() {
    recordingSubmissions.clear(); // caller already made the device idle
    for (auto &entry : completionTrackers) if (entry.second) entry.second->shutdown();
    for (const auto &entry : completionSemaphores) if (entry.second) vkDestroySemaphore(vkDevice, entry.second, nullptr);
    completionSemaphores.clear();
    completionTrackers.clear();
    slotCompletions.clear();
}

CCVKDevice *CCVKDevice::instance = nullptr;

CCVKDevice *CCVKDevice::getInstance() {
    return CCVKDevice::instance;
}

bool CCVKDevice::configurePipelineCacheForPrewarm(const ccstd::string &path) {
    // Serial internal opt-in, before any factory has populated this cache.
    // A dirty existing cache is deliberately retained rather than replaced.
    if (path.empty() || !_gpuDevice || !_gpuContext || !_pipelineCache || _pipelineCache->isDirty()) return false;
    try {
        auto candidate = std::make_unique<CCVKPipelineCache>();
        candidate->init(_gpuDevice->vkDevice, _gpuContext->physicalDeviceProperties, true, path);
        if (!candidate->getHandle()) return false;
        _pipelineCache = std::move(candidate);
        return true;
    } catch (...) {
        return false;
    }
}

CCVKDevice::CCVKDevice() {
    _api = API::VULKAN;
    _deviceName = "Vulkan";

    _caps.supportQuery = true;
    _caps.clipSpaceMinZ = 0.0F;
    _caps.screenSpaceSignY = -1.0F;
    _caps.clipSpaceSignY = -1.0F;
    CCVKDevice::instance = this;
}

CCVKDevice::~CCVKDevice() {
    CCVKDevice::instance = nullptr;
}

bool CCVKDevice::doInit(const DeviceInfo & /*info*/) {
    VulkanMeasurementScope measurement(VulkanMeasurementKind::BackendInit, this);
    _lastValidationErrorCount = 0;
    _xr = CC_GET_XR_INTERFACE();
    if (_xr) {
        _xr->preGFXDeviceInitialize(_api);
    }
    _gpuContext = std::make_unique<CCVKGPUContext>();
    if (!_gpuContext->initialize()) {
        return false;
    }

    const VkPhysicalDeviceFeatures &deviceFeatures = _gpuContext->physicalDeviceFeatures;

    ///////////////////// Device Creation /////////////////////

    _gpuDevice = std::make_unique<CCVKGPUDevice>();
    _gpuDevice->apiVersionInfo = _gpuContext->apiVersionInfo;
    _gpuDevice->minorVersion = _gpuContext->minorVersion;
    _gpuDevice->capabilities = _gpuContext->capabilities;
    auto &vkCaps = _gpuDevice->capabilities;
    auto failBootstrap = [this]() {
        if (_gpuDevice->vkDevice != VK_NULL_HANDLE) {
            _gpuDevice->shutdownCompletionTrackers(); // bootstrap has not submitted work
            vkDestroyDevice(_gpuDevice->vkDevice, nullptr);
        }
        _gpuDevice.reset();
        _gpuContext->destroy();
        _lastValidationErrorCount = _gpuContext->validationErrorCount.load(std::memory_order_relaxed);
        _gpuContext.reset();
        return false;
    };

    // Request useful tier features, while their renderer paths stay on the
    // existing encoders until the matching migration milestone is validated.
    requestFeature(vkCaps.timelineSemaphore, _gpuDevice->minorVersion >= 2);
    requestFeature(vkCaps.separateDepthStencilLayouts, _gpuDevice->minorVersion >= 2);
    requestFeature(vkCaps.synchronization2, _gpuDevice->minorVersion >= 3);
    requestFeature(vkCaps.dynamicRendering, _gpuDevice->minorVersion >= 3);
    requestFeature(vkCaps.maintenance4, _gpuDevice->minorVersion >= 3);

    // only enable the absolute essentials
    ccstd::vector<const char *> requestedLayers{};
    ccstd::vector<const char *> requestedExtensions{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
#if CC_DEBUG
    requestedExtensions.push_back(VK_EXT_DEBUG_MARKER_EXTENSION_NAME);
#endif
    if (_gpuDevice->minorVersion < 2) {
        requestedExtensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    }
    VkPhysicalDeviceFeatures requestedFeatures{};
    requestedFeatures.textureCompressionASTC_LDR = deviceFeatures.textureCompressionASTC_LDR;
    requestedFeatures.textureCompressionBC = deviceFeatures.textureCompressionBC;
    requestedFeatures.textureCompressionETC2 = deviceFeatures.textureCompressionETC2;
    requestedFeatures.samplerAnisotropy = deviceFeatures.samplerAnisotropy;
    requestedFeatures.depthBounds = deviceFeatures.depthBounds;
    requestedFeatures.multiDrawIndirect = deviceFeatures.multiDrawIndirect;

    // check extensions
    uint32_t availableLayerCount;
    VK_CHECK(vkEnumerateDeviceLayerProperties(_gpuContext->physicalDevice, &availableLayerCount, nullptr));
    _gpuDevice->layers.resize(availableLayerCount);
    VK_CHECK(vkEnumerateDeviceLayerProperties(_gpuContext->physicalDevice, &availableLayerCount, _gpuDevice->layers.data()));

    uint32_t availableExtensionCount;
    VK_CHECK(vkEnumerateDeviceExtensionProperties(_gpuContext->physicalDevice, nullptr, &availableExtensionCount, nullptr));
    _gpuDevice->extensions.resize(availableExtensionCount);
    VK_CHECK(vkEnumerateDeviceExtensionProperties(_gpuContext->physicalDevice, nullptr, &availableExtensionCount, _gpuDevice->extensions.data()));

    if (!isExtensionSupported(VK_KHR_SWAPCHAIN_EXTENSION_NAME, _gpuDevice->extensions)) {
        CC_LOG_ERROR("Vulkan device is missing required VK_KHR_swapchain.");
        return failBootstrap();
    }
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenanceQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    const bool hasMaintenance = isExtensionSupported(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, _gpuDevice->extensions);
    if (hasMaintenance) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &maintenanceQuery;
        vkGetPhysicalDeviceFeatures2(_gpuContext->physicalDevice, &query);
    }
    vkCaps.swapchainMaintenance1.supported = maintenanceQuery.swapchainMaintenance1 == VK_TRUE;
    const bool maintenanceClosure = _gpuContext->checkExtension(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME) &&
                                    _gpuContext->checkExtension(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    requestFeature(vkCaps.swapchainMaintenance1, hasMaintenance && maintenanceClosure && vkCaps.swapchainMaintenance1.supported && !_xr);
    if (vkCaps.swapchainMaintenance1.requested) requestedExtensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
    // FSR requires renderpass2, provided by core 1.2 or the enabled KHR path.
    const bool shadingRateAllowed = _gpuDevice->minorVersion >= 2 ||
                                    isExtensionSupported(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, _gpuDevice->extensions);
    if (shadingRateAllowed && isExtensionSupported(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME, _gpuDevice->extensions)) {
        requestedExtensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
        requestFeature(vkCaps.fragmentShadingRateDetail.pipelineFragmentShadingRate, true);
        requestFeature(vkCaps.fragmentShadingRateDetail.attachmentFragmentShadingRate, true);
        requestFeature(vkCaps.fragmentShadingRate, true);
    }
    // Portability subset is mandatory when advertised and is a device extension.
    constexpr const char *PORTABILITY_SUBSET = "VK_KHR_portability_subset";
    if (isExtensionSupported(PORTABILITY_SUBSET, _gpuDevice->extensions)) {
        requestedExtensions.push_back(PORTABILITY_SUBSET);
    }

#if CC_SWAPPY_ENABLED
    uint32_t swappyRequiredExtensionCount = 0;
    SwappyVk_determineDeviceExtensions(_gpuContext->physicalDevice, availableExtensionCount,
                                       _gpuDevice->extensions.data(), &swappyRequiredExtensionCount, nullptr);
    ccstd::vector<char *> swappyRequiredExtensions(swappyRequiredExtensionCount);
    ccstd::vector<char> swappyRequiredExtensionsData(swappyRequiredExtensionCount * (VK_MAX_EXTENSION_NAME_SIZE + 1));
    for (uint32_t i = 0; i < swappyRequiredExtensionCount; i++) {
        swappyRequiredExtensions[i] = &swappyRequiredExtensionsData[i * (VK_MAX_EXTENSION_NAME_SIZE + 1)];
    }
    SwappyVk_determineDeviceExtensions(_gpuContext->physicalDevice, availableExtensionCount,
                                       _gpuDevice->extensions.data(), &swappyRequiredExtensionCount, swappyRequiredExtensions.data());
    ccstd::vector<ccstd::string> swappyRequiredExtList(swappyRequiredExtensionCount);

    for (size_t i = 0; i < swappyRequiredExtensionCount; ++i) {
        swappyRequiredExtList[i] = swappyRequiredExtensions[i];
        requestedExtensions.push_back(swappyRequiredExtList[i].c_str());
    }
#endif

    // just filter out the unsupported layers & extensions
    for (const char *layer : requestedLayers) {
        if (isLayerSupported(layer, _gpuDevice->layers)) {
            _layers.push_back(layer);
        }
    }
    for (const char *extension : requestedExtensions) {
        if (isExtensionSupported(extension, _gpuDevice->extensions)) {
            if (!checkExtension(extension)) _extensions.push_back(extension);
        }
    }
#if CC_DEBUG
    // Test policy only: disable feature enablement without changing supported
    // bits or the API tier. Renderer path switches are independent of this.
    if (const char *disabled = std::getenv("CC_VULKAN_DISABLE_FEATURES")) {
        const ccstd::string names(disabled);
        for (size_t start = 0; start < names.size();) {
            const size_t end = names.find(',', start);
            const auto name = names.substr(start, end == ccstd::string::npos ? end : end - start);
            if (name == "timelineSemaphore") vkCaps.timelineSemaphore.requested = false;
            else if (name == "synchronization2") vkCaps.synchronization2.requested = false;
            else if (name == "dynamicRendering") vkCaps.dynamicRendering.requested = false;
            else if (name == "maintenance4") vkCaps.maintenance4.requested = false;
            else if (name == "swapchainMaintenance1") vkCaps.swapchainMaintenance1.requested = false;
            else {
                CC_LOG_ERROR("Unknown CC_VULKAN_DISABLE_FEATURES entry: %s", name.c_str());
                return failBootstrap();
            }
            if (end == ccstd::string::npos) break;
            start = end + 1;
        }
    }
#endif
    VulkanFeatureChain enableFeatures;
    const auto actuallyEnabled = getVulkanFeatureExtensionFlags([this](const char *name) { return checkExtension(name); });
    if (!enableFeatures.buildEnable(_gpuContext->featureQuery, vkCaps, requestedFeatures, actuallyEnabled)) {
        CC_LOG_ERROR("Vulkan feature enable chain could not be built.");
        return failBootstrap();
    }

    // prepare the device queues
    uint32_t queueFamilyPropertiesCount = utils::toUint(_gpuContext->queueFamilyProperties.size());
    ccstd::vector<VkDeviceQueueCreateInfo> queueCreateInfos(queueFamilyPropertiesCount, {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO});
    ccstd::vector<ccstd::vector<float>> queuePriorities(queueFamilyPropertiesCount);

    for (uint32_t queueFamilyIndex = 0U; queueFamilyIndex < queueFamilyPropertiesCount; ++queueFamilyIndex) {
        const VkQueueFamilyProperties &queueFamilyProperty = _gpuContext->queueFamilyProperties[queueFamilyIndex];

        queuePriorities[queueFamilyIndex].resize(queueFamilyProperty.queueCount, 1.0F);

        VkDeviceQueueCreateInfo &queueCreateInfo = queueCreateInfos[queueFamilyIndex];

        queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
        queueCreateInfo.queueCount = queueFamilyProperty.queueCount;
        queueCreateInfo.pQueuePriorities = queuePriorities[queueFamilyIndex].data();
    }

    VkDeviceCreateInfo deviceCreateInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};

    deviceCreateInfo.queueCreateInfoCount = utils::toUint(queueCreateInfos.size());
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.enabledLayerCount = utils::toUint(_layers.size());
    deviceCreateInfo.ppEnabledLayerNames = _layers.data();
    deviceCreateInfo.enabledExtensionCount = utils::toUint(_extensions.size());
    deviceCreateInfo.ppEnabledExtensionNames = _extensions.data();
    deviceCreateInfo.pNext = enableFeatures.head();
    VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenanceEnable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
    if (vkCaps.swapchainMaintenance1.requested && checkExtension(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) {
        maintenanceEnable.swapchainMaintenance1 = VK_TRUE;
        maintenanceEnable.pNext = const_cast<void *>(deviceCreateInfo.pNext);
        deviceCreateInfo.pNext = &maintenanceEnable;
    }

    VkResult deviceResult = VK_SUCCESS;
    if (_xr) {
        _gpuDevice->vkDevice = _xr->createXRVulkanDevice(&deviceCreateInfo);
    } else {
        VulkanMeasurementScope creation(VulkanMeasurementKind::DeviceCreation, this);
        deviceResult = vkCreateDevice(_gpuContext->physicalDevice, &deviceCreateInfo, nullptr, &_gpuDevice->vkDevice);
    }
    const bool created = deviceResult == VK_SUCCESS && _gpuDevice->vkDevice != VK_NULL_HANDLE;
    enableFeatures.finalizeEnabled(vkCaps, created);
    finalizeFeature(vkCaps.swapchainMaintenance1, created, maintenanceEnable.swapchainMaintenance1 == VK_TRUE);
    if (!created) {
        CC_LOG_ERROR("Vulkan logical device creation failed: result=%d", deviceResult);
        return failBootstrap();
    }
    vkCaps.portabilitySubset = checkExtension(PORTABILITY_SUBSET);
    _gpuDevice->runtimePaths = normalizeRuntimePaths(vkCaps, VulkanRuntimePaths{});
    volkLoadDevice(_gpuDevice->vkDevice);
    if (vkCaps.swapchainMaintenance1.enabled && !vkReleaseSwapchainImagesEXT) {
        CC_LOG_ERROR("Vulkan swapchainMaintenance1 enabled without its image-release entry.");
        return failBootstrap();
    }
    const bool baselineEntries = vkCreateDescriptorUpdateTemplate && vkDestroyDescriptorUpdateTemplate &&
                                 vkUpdateDescriptorSetWithTemplate && vkGetBufferMemoryRequirements2 &&
                                 vkGetImageMemoryRequirements2 && vkBindBufferMemory2 && vkBindImageMemory2;
    const bool timelineEntries = !vkCaps.timelineSemaphore.enabled ||
                                 (vkGetSemaphoreCounterValue && vkWaitSemaphores && vkSignalSemaphore);
    const bool sync2Entries = !vkCaps.synchronization2.enabled ||
                              (_gpuDevice->minorVersion >= 3 ? (vkCmdPipelineBarrier2 && vkQueueSubmit2)
                                                            : (vkCmdPipelineBarrier2KHR && vkQueueSubmit2KHR));
    const bool dynamicEntries = !vkCaps.dynamicRendering.enabled || (vkCmdBeginRendering && vkCmdEndRendering);
    const bool memoryEntries = _gpuDevice->minorVersion < 3 ||
                               (vkGetDeviceBufferMemoryRequirements && vkGetDeviceImageMemoryRequirements);
    if (!baselineEntries || !timelineEntries || !sync2Entries || !dynamicEntries || !memoryEntries) {
        CC_LOG_ERROR("Vulkan device entries missing: baseline=%d timeline=%d sync2=%d dynamic=%d memory=%d",
                     baselineEntries, timelineEntries, sync2Entries, dynamicEntries, memoryEntries);
        return failBootstrap();
    }
    if (vkCaps.maintenance4.enabled && vkCaps.maxBufferSize == 0) {
        CC_LOG_ERROR("Vulkan maintenance4 enabled without a valid maxBufferSize limit.");
        return failBootstrap();
    }

    // Capability enablement and completion encoder choice are separate policies.
    bool useTimeline = vkCaps.timelineSemaphore.enabled;
#if CC_DEBUG
    if (const char *completionMode = std::getenv("CC_VULKAN_COMPLETION")) {
        const ccstd::string mode(completionMode);
        if (mode == "fence") useTimeline = false;
        else if (mode == "timeline") {
            if (!vkCaps.timelineSemaphore.enabled) {
                CC_LOG_ERROR("CC_VULKAN_COMPLETION=timeline requires enabled timelineSemaphore.");
                return failBootstrap();
            }
            useTimeline = true;
        } else if (mode != "auto" && !mode.empty()) {
            CC_LOG_ERROR("Invalid CC_VULKAN_COMPLETION: %s", completionMode);
            return failBootstrap();
        }
    }
#endif
    VulkanRuntimePaths completionPaths;
    completionPaths.useTimelineSemaphore = useTimeline;
#if CC_DEBUG
    if (const char *submitMode = std::getenv("CC_VULKAN_SUBMIT")) {
        const ccstd::string mode(submitMode);
        if (mode == "submit2") {
            if (!vkCaps.synchronization2.enabled) {
                CC_LOG_ERROR("CC_VULKAN_SUBMIT=submit2 requires enabled synchronization2.");
                return failBootstrap();
            }
            completionPaths.useSubmit2 = true;
        } else if (mode != "legacy" && !mode.empty()) {
            CC_LOG_ERROR("Invalid CC_VULKAN_SUBMIT: %s", submitMode);
            return failBootstrap();
        }
    }
#endif
#if CC_DEBUG
    if (const char *barrierMode = std::getenv("CC_VULKAN_BARRIER")) {
        const ccstd::string mode(barrierMode);
        if (mode == "sync2") {
            if (!vkCaps.synchronization2.enabled) {
                CC_LOG_ERROR("CC_VULKAN_BARRIER=sync2 requires enabled synchronization2.");
                return failBootstrap();
            }
            completionPaths.useSynchronization2 = true;
        } else if (mode != "legacy" && !mode.empty()) {
            CC_LOG_ERROR("Invalid CC_VULKAN_BARRIER: %s", barrierMode);
            return failBootstrap();
        }
    }
#endif
#if CC_DEBUG
    if (const char *renderingMode = std::getenv("CC_VULKAN_RENDERING")) {
        const ccstd::string mode(renderingMode);
        if (mode == "dynamic") {
            if (!vkCaps.dynamicRendering.enabled ||
                _gpuContext->apiVersionInfo.effectiveDeviceApiVersion < VK_API_VERSION_1_3 ||
                !vkCmdBeginRendering || !vkCmdEndRendering) {
                CC_LOG_ERROR("CC_VULKAN_RENDERING=dynamic requires core Vulkan 1.3 and enabled dynamicRendering.");
                return failBootstrap();
            }
            completionPaths.useDynamicRendering = true;
        } else if (mode == "classic") {
            completionPaths.forceClassicRenderPass = true;
        } else if (!mode.empty()) {
            CC_LOG_ERROR("Invalid CC_VULKAN_RENDERING: %s", renderingMode);
            return failBootstrap();
        }
    }
#endif
    _gpuDevice->runtimePaths = normalizeRuntimePaths(vkCaps, completionPaths);
    if (_gpuDevice->runtimePaths.useTimelineSemaphore) {
        VkPhysicalDeviceTimelineSemaphoreProperties timelineProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext = &timelineProperties;
        vkGetPhysicalDeviceProperties2(_gpuContext->physicalDevice, &properties);
        _gpuDevice->maxTimelineSemaphoreValueDifference = timelineProperties.maxTimelineSemaphoreValueDifference;
        if (!_gpuDevice->maxTimelineSemaphoreValueDifference) {
            CC_LOG_ERROR("Vulkan timeline completion has no valid value-difference limit.");
            return failBootstrap();
        }
    }
    // All engine queues use index zero of a supported family. Initialize their
    // trackers before constructing resources, so failure has a clean bootstrap path.
    for (uint32_t family = 0; family < queueFamilyPropertiesCount; ++family) {
        VkQueue queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(_gpuDevice->vkDevice, family, 0, &queue);
        if (!_gpuDevice->getCompletionTracker(queue)) return failBootstrap();
    }

    const auto logFeature = [](const char *name, const VulkanCapabilityState &state) {
        CC_LOG_INFO("Vulkan capability %s: supported=%d requested=%d enabled=%d",
                    name, state.supported, state.requested, state.enabled);
    };
    logFeature("timelineSemaphore", vkCaps.timelineSemaphore);
    logFeature("bufferDeviceAddress", vkCaps.bufferDeviceAddress);
    logFeature("separateDepthStencilLayouts", vkCaps.separateDepthStencilLayouts);
    logFeature("synchronization2", vkCaps.synchronization2);
    logFeature("dynamicRendering", vkCaps.dynamicRendering);
    logFeature("maintenance4", vkCaps.maintenance4);
    logFeature("fragmentShadingRate (any)", vkCaps.fragmentShadingRate);
    logFeature("pipelineFragmentShadingRate", vkCaps.fragmentShadingRateDetail.pipelineFragmentShadingRate);
    logFeature("primitiveFragmentShadingRate", vkCaps.fragmentShadingRateDetail.primitiveFragmentShadingRate);
    logFeature("attachmentFragmentShadingRate", vkCaps.fragmentShadingRateDetail.attachmentFragmentShadingRate);
    logFeature("memoryBudget", vkCaps.memoryBudget);
    logFeature("graphicsPipelineLibrary", vkCaps.graphicsPipelineLibrary);
    logFeature("swapchainMaintenance1", vkCaps.swapchainMaintenance1);
#define CC_LOG_DESCRIPTOR_CAP(member) logFeature(#member, vkCaps.descriptorIndexing.member)
    CC_LOG_DESCRIPTOR_CAP(shaderUniformBufferArrayNonUniformIndexing);
    CC_LOG_DESCRIPTOR_CAP(shaderSampledImageArrayNonUniformIndexing);
    CC_LOG_DESCRIPTOR_CAP(shaderStorageBufferArrayNonUniformIndexing);
    CC_LOG_DESCRIPTOR_CAP(shaderStorageImageArrayNonUniformIndexing);
    CC_LOG_DESCRIPTOR_CAP(descriptorBindingUniformBufferUpdateAfterBind);
    CC_LOG_DESCRIPTOR_CAP(descriptorBindingSampledImageUpdateAfterBind);
    CC_LOG_DESCRIPTOR_CAP(descriptorBindingStorageImageUpdateAfterBind);
    CC_LOG_DESCRIPTOR_CAP(descriptorBindingStorageBufferUpdateAfterBind);
    CC_LOG_DESCRIPTOR_CAP(descriptorBindingPartiallyBound);
    CC_LOG_DESCRIPTOR_CAP(descriptorBindingVariableDescriptorCount);
    CC_LOG_DESCRIPTOR_CAP(runtimeDescriptorArray);
#undef CC_LOG_DESCRIPTOR_CAP
    const auto &paths = _gpuDevice->runtimePaths;
    CC_LOG_INFO("Vulkan limits: maxBufferSize=%llu debugUtils=%d portabilitySubset=%d",
                static_cast<unsigned long long>(vkCaps.maxBufferSize), vkCaps.debugUtils, vkCaps.portabilitySubset);
    CC_LOG_INFO("Vulkan paths: timeline=%d sync2=%d submit2=%d dynamicRendering=%d classicOverride=%d maintenance4=%d",
                paths.useTimelineSemaphore, paths.useSynchronization2, paths.useSubmit2, paths.useDynamicRendering,
                paths.forceClassicRenderPass, paths.useMaintenance4);

    SPIRVUtils::getInstance()->initialize(static_cast<int>(_gpuDevice->minorVersion));

    ///////////////////// Gather Device Properties /////////////////////

    auto findPreferredDepthFormat = [this](const VkFormat *formats, uint32_t count, VkFormat *pFormat) {
        for (uint32_t i = 0; i < count; ++i) {
            VkFormat format = formats[i];
            VkFormatProperties formatProperties;
            vkGetPhysicalDeviceFormatProperties(_gpuContext->physicalDevice, format, &formatProperties);
            // Format must support depth stencil attachment for optimal tiling
            if (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                if (formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) {
                    *pFormat = format;
                    break;
                }
            }
        }
    };

    VkFormat depthFormatPriorityList[]{
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_X8_D24_UNORM_PACK32,
        VK_FORMAT_D16_UNORM,
    };
    findPreferredDepthFormat(depthFormatPriorityList, 3, &_gpuDevice->depthFormat);

    VkFormat depthStencilFormatPriorityList[]{
        VK_FORMAT_D24_UNORM_S8_UINT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D16_UNORM_S8_UINT,
    };
    findPreferredDepthFormat(depthStencilFormatPriorityList, 3, &_gpuDevice->depthStencilFormat);

    initDeviceFeature();
    initFormatFeature();

    ccstd::string compressedFmts;

    if (getFormatFeatures(Format::BC1_SRGB_ALPHA) != FormatFeature::NONE) {
        compressedFmts += "dxt ";
    }

    if (getFormatFeatures(Format::ETC2_RGBA8) != FormatFeature::NONE) {
        compressedFmts += "etc2 ";
    }

    if (getFormatFeatures(Format::ASTC_RGBA_4X4) != FormatFeature::NONE) {
        compressedFmts += "astc ";
    }

    if (getFormatFeatures(Format::PVRTC_RGBA2) != FormatFeature::NONE) {
        compressedFmts += "pvrtc ";
    }

    _gpuDevice->useMultiDrawIndirect = deviceFeatures.multiDrawIndirect;
    _gpuDevice->useDescriptorUpdateTemplate = true; // Vulkan 1.1 baseline core.

    if (_gpuDevice->minorVersion > 1) {
        _gpuDevice->createRenderPass2 = vkCreateRenderPass2;
    } else if (checkExtension(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) {
        _gpuDevice->createRenderPass2 = vkCreateRenderPass2KHR;
    } else {
        _gpuDevice->createRenderPass2 = vkCreateRenderPass2KHRFallback;
    }

    const VkPhysicalDeviceLimits &limits = _gpuContext->physicalDeviceProperties.limits;
    _caps.maxVertexAttributes = limits.maxVertexInputAttributes;
    _caps.maxVertexUniformVectors = limits.maxUniformBufferRange / 16;
    _caps.maxFragmentUniformVectors = limits.maxUniformBufferRange / 16;
    _caps.maxUniformBufferBindings = limits.maxDescriptorSetUniformBuffers;
    _caps.maxUniformBlockSize = limits.maxUniformBufferRange;
    _caps.maxShaderStorageBlockSize = limits.maxStorageBufferRange;
    _caps.maxShaderStorageBufferBindings = limits.maxDescriptorSetStorageBuffers;
    _caps.maxTextureUnits = limits.maxDescriptorSetSampledImages;
    _caps.maxVertexTextureUnits = limits.maxPerStageDescriptorSampledImages;
    _caps.maxColorRenderTargets = limits.maxColorAttachments;
    _caps.maxTextureSize = limits.maxImageDimension2D;
    _caps.maxCubeMapTextureSize = limits.maxImageDimensionCube;
    _caps.maxArrayTextureLayers = limits.maxImageArrayLayers;
    _caps.max3DTextureSize = limits.maxImageDimension3D;
    _caps.uboOffsetAlignment = utils::toUint(limits.minUniformBufferOffsetAlignment);
    // compute shaders
    _caps.maxComputeSharedMemorySize = limits.maxComputeSharedMemorySize;
    _caps.maxComputeWorkGroupInvocations = limits.maxComputeWorkGroupInvocations;
    _caps.maxComputeWorkGroupCount = {limits.maxComputeWorkGroupCount[0], limits.maxComputeWorkGroupCount[1], limits.maxComputeWorkGroupCount[2]};
    _caps.maxComputeWorkGroupSize = {limits.maxComputeWorkGroupSize[0], limits.maxComputeWorkGroupSize[1], limits.maxComputeWorkGroupSize[2]};
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    // UNASSIGNED-BestPractices-vkCreateComputePipelines-compute-work-group-size
    _caps.maxComputeWorkGroupInvocations = std::min(_caps.maxComputeWorkGroupInvocations, 64U);
#endif // defined(VK_USE_PLATFORM_ANDROID_KHR)
    initExtensionCapability();

    ///////////////////// Resource Initialization /////////////////////

    QueueInfo queueInfo;
    queueInfo.type = QueueType::GRAPHICS;
    _queue = createQueue(queueInfo);

    QueryPoolInfo queryPoolInfo{QueryType::OCCLUSION, DEFAULT_MAX_QUERY_OBJECTS, false};
    _queryPool = createQueryPool(queryPoolInfo);

    CommandBufferInfo cmdBuffInfo;
    cmdBuffInfo.type = CommandBufferType::PRIMARY;
    cmdBuffInfo.queue = _queue;
    _cmdBuff = createCommandBuffer(cmdBuffInfo);

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = _gpuContext->physicalDevice;
    allocatorInfo.device = _gpuDevice->vkDevice;
    allocatorInfo.instance = _gpuContext->vkInstance;
    allocatorInfo.vulkanApiVersion = _gpuDevice->apiVersionInfo.effectiveDeviceApiVersion;

    VmaVulkanFunctions vmaVulkanFunc{};
    vmaVulkanFunc.vkAllocateMemory = vkAllocateMemory;
    vmaVulkanFunc.vkBindBufferMemory = vkBindBufferMemory;
    vmaVulkanFunc.vkBindImageMemory = vkBindImageMemory;
    vmaVulkanFunc.vkCreateBuffer = vkCreateBuffer;
    vmaVulkanFunc.vkCreateImage = vkCreateImage;
    vmaVulkanFunc.vkDestroyBuffer = vkDestroyBuffer;
    vmaVulkanFunc.vkDestroyImage = vkDestroyImage;
    vmaVulkanFunc.vkFlushMappedMemoryRanges = vkFlushMappedMemoryRanges;
    vmaVulkanFunc.vkFreeMemory = vkFreeMemory;
    vmaVulkanFunc.vkGetBufferMemoryRequirements = vkGetBufferMemoryRequirements;
    vmaVulkanFunc.vkGetImageMemoryRequirements = vkGetImageMemoryRequirements;
    vmaVulkanFunc.vkGetPhysicalDeviceMemoryProperties = vkGetPhysicalDeviceMemoryProperties;
    vmaVulkanFunc.vkGetPhysicalDeviceProperties = vkGetPhysicalDeviceProperties;
    vmaVulkanFunc.vkInvalidateMappedMemoryRanges = vkInvalidateMappedMemoryRanges;
    vmaVulkanFunc.vkMapMemory = vkMapMemory;
    vmaVulkanFunc.vkUnmapMemory = vkUnmapMemory;
    vmaVulkanFunc.vkCmdCopyBuffer = vkCmdCopyBuffer;

    // VMA's table retains KHR member names; they bind to Vulkan 1.1 core entries.
    vmaVulkanFunc.vkGetBufferMemoryRequirements2KHR = vkGetBufferMemoryRequirements2;
    vmaVulkanFunc.vkGetImageMemoryRequirements2KHR = vkGetImageMemoryRequirements2;
    vmaVulkanFunc.vkBindBufferMemory2KHR = vkBindBufferMemory2;
    vmaVulkanFunc.vkBindImageMemory2KHR = vkBindImageMemory2;
    vmaVulkanFunc.vkGetPhysicalDeviceMemoryProperties2KHR = vkGetPhysicalDeviceMemoryProperties2;
    if (_gpuDevice->minorVersion >= 3) {
        vmaVulkanFunc.vkGetDeviceBufferMemoryRequirements = vkGetDeviceBufferMemoryRequirements;
        vmaVulkanFunc.vkGetDeviceImageMemoryRequirements = vkGetDeviceImageMemoryRequirements;
    }

    allocatorInfo.pVulkanFunctions = &vmaVulkanFunc;

    VK_CHECK(vmaCreateAllocator(&allocatorInfo, &_gpuDevice->memoryAllocator));

    uint32_t backBufferCount = _gpuDevice->backBufferCount;
    for (uint32_t i = 0U; i < backBufferCount; i++) {
        _gpuFencePools.push_back(std::make_unique<CCVKGPUFencePool>(_gpuDevice.get()));
        _gpuSemaphorePools.push_back(std::make_unique<CCVKGPUSemaphorePool>(_gpuDevice.get()));
        _gpuRecycleBins.push_back(std::make_unique<CCVKGPURecycleBin>(_gpuDevice.get()));
        _gpuStagingBufferPools.push_back(std::make_unique<CCVKGPUStagingBufferPool>(_gpuDevice.get()));
    }

    _gpuBufferHub = std::make_unique<CCVKGPUBufferHub>(_gpuDevice.get());
    _gpuIAHub = std::make_unique<CCVKGPUInputAssemblerHub>(_gpuDevice.get());
    _gpuTransportHub = std::make_unique<CCVKGPUTransportHub>(_gpuDevice.get(), static_cast<CCVKQueue *>(_queue)->gpuQueue());
    _gpuDescriptorHub = std::make_unique<CCVKGPUDescriptorHub>(_gpuDevice.get());
    _gpuBarrierManager = std::make_unique<CCVKGPUBarrierManager>(_gpuDevice.get());
    _gpuDescriptorSetHub = std::make_unique<CCVKGPUDescriptorSetHub>(_gpuDevice.get());

    _gpuDevice->defaultSampler = ccnew CCVKGPUSampler();
    _gpuDevice->defaultSampler->init();

    _gpuDevice->defaultTexture = ccnew CCVKGPUTexture();
    _gpuDevice->defaultTexture->format = Format::RGBA8;
    _gpuDevice->defaultTexture->usage = TextureUsageBit::SAMPLED | TextureUsage::STORAGE;
    _gpuDevice->defaultTexture->width = _gpuDevice->defaultTexture->height = 1U;
    _gpuDevice->defaultTexture->size = formatSize(Format::RGBA8, 1U, 1U, 1U);
    _gpuDevice->defaultTexture->init();

    _gpuDevice->defaultTextureView = ccnew CCVKGPUTextureView();
    _gpuDevice->defaultTextureView->gpuTexture = _gpuDevice->defaultTexture;
    _gpuDevice->defaultTextureView->format = Format::RGBA8;
    _gpuDevice->defaultTextureView->init();

    ThsvsImageBarrier barrier{};
    barrier.nextAccessCount = 1;
    barrier.pNextAccesses = getAccessType(AccessFlagBit::VERTEX_SHADER_READ_TEXTURE);
    barrier.image = _gpuDevice->defaultTexture->vkImage;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    gpuTransportHub()->checkIn(
        [&barrier](const CCVKGPUCommandBuffer *gpuCommandBuffer) {
            cmdFuncCCVKImageMemoryBarrier(gpuCommandBuffer, barrier);
        },
        true);

    _gpuDevice->defaultBuffer = ccnew CCVKGPUBuffer();
    _gpuDevice->defaultBuffer->usage = BufferUsage::UNIFORM | BufferUsage::STORAGE;
    _gpuDevice->defaultBuffer->memUsage = MemoryUsage::HOST | MemoryUsage::DEVICE;
    _gpuDevice->defaultBuffer->size = _gpuDevice->defaultBuffer->stride = 16U;
    _gpuDevice->defaultBuffer->count = 1U;
    _gpuDevice->defaultBuffer->init();

    getAccessTypes(AccessFlagBit::COLOR_ATTACHMENT_WRITE, _gpuDevice->defaultColorBarrier.nextAccesses);
    cmdFuncCCVKCreateGeneralBarrier(this, &_gpuDevice->defaultColorBarrier);

    getAccessTypes(AccessFlagBit::DEPTH_STENCIL_ATTACHMENT_WRITE, _gpuDevice->defaultDepthStencilBarrier.nextAccesses);
    cmdFuncCCVKCreateGeneralBarrier(this, &_gpuDevice->defaultDepthStencilBarrier);

    _pipelineCache = std::make_unique<CCVKPipelineCache>();
    _pipelineCache->init(_gpuDevice->vkDevice, _gpuContext->physicalDeviceProperties);

    ///////////////////// Print Debug Info /////////////////////

    ccstd::string instanceLayers;
    ccstd::string instanceExtensions;
    ccstd::string deviceLayers;
    ccstd::string deviceExtensions;
    for (const char *layer : _gpuContext->layers) {
        instanceLayers += layer + ccstd::string(" ");
    }
    for (const char *extension : _gpuContext->extensions) {
        instanceExtensions += extension + ccstd::string(" ");
    }
    for (const char *layer : _layers) {
        deviceLayers += layer + ccstd::string(" ");
    }
    for (const char *extension : _extensions) {
        deviceExtensions += extension + ccstd::string(" ");
    }

    uint32_t apiVersion = _gpuContext->physicalDeviceProperties.apiVersion;
    _renderer = _gpuContext->physicalDeviceProperties.deviceName;
    _vendor = mapVendorName(_gpuContext->physicalDeviceProperties.vendorID);
    _version = StringUtil::format("%d.%d.%d", VK_VERSION_MAJOR(apiVersion),
                                  VK_VERSION_MINOR(apiVersion), VK_VERSION_PATCH(apiVersion));

    CC_LOG_INFO("Vulkan device initialized.");
    CC_LOG_INFO("RENDERER: %s", _renderer.c_str());
    CC_LOG_INFO("VENDOR: %s", _vendor.c_str());
    CC_LOG_INFO("VERSION: %s", _version.c_str());
    CC_LOG_INFO("INSTANCE_LAYERS: %s", instanceLayers.c_str());
    CC_LOG_INFO("INSTANCE_EXTENSIONS: %s", instanceExtensions.c_str());
    CC_LOG_INFO("DEVICE_LAYERS: %s", deviceLayers.c_str());
    CC_LOG_INFO("DEVICE_EXTENSIONS: %s", deviceExtensions.c_str());
    CC_LOG_INFO("COMPRESSED_FORMATS: %s", compressedFmts.c_str());

    if (_xr) {
        cc::gfx::CCVKGPUQueue *vkQueue = static_cast<cc::gfx::CCVKQueue *>(getQueue())->gpuQueue();
        _xr->setXRConfig(xr::XRConfigKey::VK_QUEUE_FAMILY_INDEX, static_cast<int>(vkQueue->queueFamilyIndex));
        _xr->postGFXDeviceInitialize(_api);
    }
    return true;
}

void CCVKDevice::doDestroy() {
    // Preserve the existing synchronous final-shutdown contract. Ordinary
    // acquire/resize/destroySurface paths keep finite, retryable waits; only
    // final device shutdown waits until work really finishes (or device loss).
    constexpr uint64_t shutdownTimeout = UINT64_MAX;
    if (_queue && _gpuTransportHub && _gpuDevice && _gpuDevice->vkDevice) {
        // Drain any transaction already in progress first.
        for (auto *swapchain : _gpuDevice->swapchains) {
            if (!swapchain->abortPending) continue;
            const VkResult result = abortAcquiredSwapchain(swapchain, shutdownTimeout);
            if (!wsiTeardownSafe(result)) {
                CC_LOG_ERROR("Vulkan final device destruction cannot drain WSI safely: %d", result);
                std::abort();
            }
        }
        for (auto *swapchain : _gpuDevice->swapchains) {
            const VkResult result = retireSwapchain(swapchain, shutdownTimeout);
            if (!wsiTeardownSafe(result)) {
                CC_LOG_ERROR("Vulkan final device destruction cannot retire WSI safely: %d", result);
                std::abort();
            }
        }
        _gpuDevice->swapchains.clear();
        _gpuDevice->deferredSwapchains.clear();
    }
    const VkResult waited = waitAllFences(shutdownTimeout);
    if (!wsiTeardownSafe(waited)) {
        // Device::~Device/Destroy return void and all pools are about to die.
        // There is no legal retry owner beyond this boundary. Do not turn a
        // failed final wait into Vulkan destruction of pending resources.
        CC_LOG_ERROR("Vulkan final device destruction cannot prove completion: %d", waited);
        std::abort();
    }

    if (_gpuDevice && _gpuDevice->vkDevice) {
        const VkResult result = vkDeviceWaitIdle(_gpuDevice->vkDevice);
        if (!wsiTeardownSafe(result)) {
            CC_LOG_ERROR("Vulkan final device idle failed: %d", result);
            std::abort();
        }
    }

    // Release typed recording references while descriptor/IA hubs and bins live.
    if (_gpuDevice) _gpuDevice->recordingSubmissions.clear();

    SPIRVUtils::getInstance()->destroy();

    if (_gpuDevice) {
        _gpuDevice->defaultBuffer = nullptr;
        _gpuDevice->defaultTexture = nullptr;
        _gpuDevice->defaultTextureView = nullptr;
        _gpuDevice->defaultSampler = nullptr;
    }

    CC_SAFE_DESTROY_AND_DELETE(_queryPool)
    CC_SAFE_DESTROY_AND_DELETE(_queue)
    CC_SAFE_DESTROY_AND_DELETE(_cmdBuff)

    _gpuStagingBufferPools.clear();
    _gpuFencePools.clear();

    _gpuBufferHub = nullptr;
    _gpuTransportHub = nullptr;
    _gpuSemaphorePools.clear();
    _gpuDescriptorHub = nullptr;
    _gpuBarrierManager = nullptr;
    _gpuDescriptorSetHub = nullptr;
    _gpuIAHub = nullptr;

    if (_gpuDevice) {
        uint32_t backBufferCount = _gpuDevice->backBufferCount;
        for (uint32_t i = 0U; i < backBufferCount; i++) {
            _gpuRecycleBins[i]->clear();
        }
    }
    _gpuStagingBufferPools.clear();
    _gpuRecycleBins.clear();
    _gpuFencePools.clear();

    if (_gpuDevice) {
        _pipelineCache.reset();

        if (_gpuDevice->memoryAllocator != VK_NULL_HANDLE) {
            VmaTotalStatistics stats{};
            vmaCalculateStatistics(_gpuDevice->memoryAllocator, &stats);
            CC_LOG_INFO("Total device memory leaked: %llu bytes.", static_cast<unsigned long long>(stats.total.statistics.allocationBytes));
            CC_ASSERT(!_memoryStatus.bufferSize);  // Buffer memory leaked.
            CC_ASSERT(!_memoryStatus.textureSize); // Texture memory leaked.

            vmaDestroyAllocator(_gpuDevice->memoryAllocator);
            _gpuDevice->memoryAllocator = VK_NULL_HANDLE;
        }

        for (auto it = _gpuDevice->_commandBufferPools.begin(); it != _gpuDevice->_commandBufferPools.end(); ++it) {
            CC_SAFE_DELETE(it->second)
        }
        _gpuDevice->_commandBufferPools.clear();
        _gpuDevice->_descriptorSetPools.clear();

        if (_gpuDevice->vkDevice != VK_NULL_HANDLE) {
            _gpuDevice->shutdownCompletionTrackers();
            vkDestroyDevice(_gpuDevice->vkDevice, nullptr);
            _gpuDevice->vkDevice = VK_NULL_HANDLE;
        }

        _gpuDevice = nullptr;
    }

    if (_gpuContext) {
        _gpuContext->destroy();
        _lastValidationErrorCount = _gpuContext->validationErrorCount.load(std::memory_order_relaxed);
    }
    _gpuContext = nullptr;
}

namespace {
ccstd::vector<VkSwapchainKHR> vkSwapchains;
ccstd::vector<uint32_t> vkSwapchainIndices;
ccstd::vector<CCVKGPUSwapchain *> gpuSwapchains;
ccstd::vector<VkImageMemoryBarrier> vkAcquireBarriers;
ccstd::vector<VkImageMemoryBarrier> vkPresentBarriers;

VkImageMemoryBarrier acquireBarrier{
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    nullptr,
    0,
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_QUEUE_FAMILY_IGNORED,
    VK_QUEUE_FAMILY_IGNORED,
    0, // NOLINT(modernize-use-nullptr) platform dependent type
    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
};
VkImageMemoryBarrier presentBarrier{
    VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
    nullptr,
    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    0,
    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    VK_QUEUE_FAMILY_IGNORED,
    VK_QUEUE_FAMILY_IGNORED,
    0, // NOLINT(modernize-use-nullptr) platform dependent type
    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
};
} // namespace

VkResult CCVKDevice::retryPendingWSIAborts() {
    while (true) {
        const auto pending = std::find_if(gpuSwapchains.begin(), gpuSwapchains.end(), [](const auto *swapchain) { return swapchain->abortPending; });
        if (pending == gpuSwapchains.end()) return _gpuDevice->deviceLost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
        const VkResult result = abortAcquiredSwapchain(*pending);
        if (!wsiTeardownSafe(result)) return result;
    }
}

bool CCVKDevice::hasPendingWSIAbort() const {
    return std::any_of(gpuSwapchains.begin(), gpuSwapchains.end(), [](const auto *swapchain) {
        return swapchain->abortPending;
    });
}

VkResult CCVKDevice::abortAcquiredSwapchain(CCVKGPUSwapchain *swapchain, uint64_t timeout) {
    const auto found = std::find(gpuSwapchains.begin(), gpuSwapchains.end(), swapchain);
    if (found == gpuSwapchains.end()) return VK_SUCCESS; // independent XR path
    // Finish the existing transaction before touching any other batch wait.
    for (const auto *other : gpuSwapchains) {
        if (other != swapchain && other->abortPending) return VK_NOT_READY;
    }
    const size_t slot = static_cast<size_t>(found - gpuSwapchains.begin());
    const uint32_t imageIndex = vkSwapchainIndices[slot];
    auto *queue = static_cast<CCVKQueue *>(_queue);
    auto *gpuQueue = queue->gpuQueue();
    swapchain->abortPending = true;

    if (!_gpuDevice->deviceLost && swapchain->abortCompletion.empty()) {
        // Complete transport, never an abandoned user's CB. A failed transport
        // submit retains its recordings and original binary waits for retry.
        if (!gpuQueue->presentSemaphoresSignaled || !_gpuTransportHub->empty(false) || !_gpuTransportHub->empty(true) ||
            !queue->_pendingEarlyCommands.empty() || !queue->_pendingLateCommands.empty()) {
            const VkResult result = queue->submitChecked(nullptr, 0);
            if (result != VK_SUCCESS) {
                if (result != VK_ERROR_DEVICE_LOST) return result;
                _gpuDevice->deviceLost = true;
            }
        }
        if (!_gpuDevice->deviceLost) {
            VulkanSubmitBatch drain;
            drain.waits.push_back({gpuQueue->lastSignaledSemaphores[slot], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT});
            if (!gpuQueue->completionTracker->usesTimeline() && !swapchain->abortFence) {
                const VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
                const VkResult result = vkCreateFence(_gpuDevice->vkDevice, &info, nullptr, &swapchain->abortFence);
                if (result != VK_SUCCESS) return result;
            }
            const VkResult result = _gpuDevice->submitTracked(gpuQueue, drain, swapchain->abortFence, swapchain->abortCompletion);
            if (result != VK_SUCCESS) {
                if (result == VK_ERROR_DEVICE_LOST) {
                    // This submit may have executed: keep the fence until the
                    // lost-device idle wait below proves it is no longer in use.
                    _gpuDevice->deviceLost = true;
                } else {
                    // OOM guarantees the submit did not modify its inputs.
                    if (swapchain->abortFence) vkDestroyFence(_gpuDevice->vkDevice, swapchain->abortFence, nullptr);
                    swapchain->abortFence = VK_NULL_HANDLE;
                    return result;
                }
            }
        }
    }
    if (!_gpuDevice->deviceLost) {
        const VkResult result = gpuQueue->completionTracker->wait(swapchain->abortCompletion, timeout);
        if (result != VK_SUCCESS) {
            if (result != VK_ERROR_DEVICE_LOST) return result;
            _gpuDevice->deviceLost = true;
        }
    }
    bool retireChain = !_gpuDevice->capabilities.swapchainMaintenance1.enabled || swapchain->wsi.needsSurfaceRecreate();
    if (_gpuDevice->deviceLost) {
        // Lost-device teardown is distinct from normal completion/reuse.
        // vkDeviceWaitIdle is required to return in finite time after loss.
        const VkResult result = waitAllFences(timeout);
        if (!wsiTeardownSafe(result)) return result;
    } else if (!retireChain) {
        VkReleaseSwapchainImagesInfoEXT release{VK_STRUCTURE_TYPE_RELEASE_SWAPCHAIN_IMAGES_INFO_EXT};
        release.swapchain = swapchain->vkSwapchain;
        release.imageIndexCount = 1;
        release.pImageIndices = &imageIndex;
        const VkResult result = vkReleaseSwapchainImagesEXT(_gpuDevice->vkDevice, &release);
        if (result == VK_ERROR_SURFACE_LOST_KHR) {
            // Retrying release cannot repair a lost surface. The drain is
            // already complete; retire its generation instead, without present.
            swapchain->wsi.onRecreateFailure(result);
            retireChain = true;
        } else if (result != VK_SUCCESS) {
            return result; // keep ownership and the completed drain for retry
        } else {
            swapchain->wsi.onAcquiredImageReleased();
        }
    }
    if (!_gpuDevice->deviceLost && retireChain) {
        const VkResult submitted = waitAllFences(timeout);
        if (!wsiTeardownSafe(submitted)) return submitted;
        const VkResult presented = swapchain->surfaceOwner->retireGenerations(timeout);
        if (!wsiTeardownSafe(presented)) return presented;
        if (presented == VK_ERROR_DEVICE_LOST) _gpuDevice->deviceLost = true;
#if CC_SWAPPY_ENABLED
        SwappyVk_destroySwapchain(_gpuDevice->vkDevice, swapchain->vkSwapchain);
#endif
        swapchain->imageGeneration.reset();
        swapchain->swapchainImages.clear();
        swapchain->vkSwapchain = VK_NULL_HANDLE;
        swapchain->createInfo.oldSwapchain = VK_NULL_HANDLE;
        swapchain->wsi.requestRecreate();
    }
    if (swapchain->imageGeneration) swapchain->imageGeneration->imageAcquired[imageIndex] = false;
    if (swapchain->abortFence) vkDestroyFence(_gpuDevice->vkDevice, swapchain->abortFence, nullptr);
    swapchain->abortFence = VK_NULL_HANDLE;
    swapchain->abortCompletion = {};
    swapchain->abortPending = false;
    if (_gpuDevice->deviceLost) swapchain->wsi.onRecreateFailure(VK_ERROR_DEVICE_LOST);
    swapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;

    gpuQueue->lastSignaledSemaphores.erase(gpuQueue->lastSignaledSemaphores.begin() + slot);
    gpuQueue->acquiredSwapchains.erase(gpuQueue->acquiredSwapchains.begin() + slot);
    gpuSwapchains.erase(gpuSwapchains.begin() + slot);
    vkSwapchains.erase(vkSwapchains.begin() + slot);
    vkSwapchainIndices.erase(vkSwapchainIndices.begin() + slot);
    gpuQueue->presentSemaphoresSignaled = !gpuSwapchains.empty();
    if (!_gpuDevice->deviceLost) prepareWSISubmit(gpuQueue);
    return _gpuDevice->deviceLost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

VkResult CCVKDevice::retireSwapchain(CCVKGPUSwapchain *swapchain, uint64_t timeout) {
    const VkResult aborted = abortAcquiredSwapchain(swapchain, timeout);
    if (!wsiTeardownSafe(aborted)) return aborted;
    const VkResult submitted = waitAllFences(timeout);
    if (!wsiTeardownSafe(submitted)) return submitted;
    if (swapchain->surfaceOwner) {
        const VkResult presented = swapchain->surfaceOwner->retireGenerations(timeout);
        if (!wsiTeardownSafe(presented)) return presented;
        if (presented == VK_ERROR_DEVICE_LOST) _gpuDevice->deviceLost = true;
    }
#if CC_SWAPPY_ENABLED
    if (swapchain->vkSwapchain) SwappyVk_destroySwapchain(_gpuDevice->vkDevice, swapchain->vkSwapchain);
#endif
    swapchain->imageGeneration.reset();
    swapchain->swapchainImages.clear();
    swapchain->vkSwapchain = VK_NULL_HANDLE;
    swapchain->createInfo.oldSwapchain = VK_NULL_HANDLE;
    swapchain->surfaceOwner.reset();
    swapchain->vkSurface = VK_NULL_HANDLE;
    swapchain->surfaceRetirementPending = false;
    swapchain->wsi.setSurfaceAvailable(false);
    swapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
    return _gpuDevice->deviceLost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

VkResult CCVKDevice::retireDeferredSwapchains() {
    for (auto it = _gpuDevice->deferredSwapchains.begin(); it != _gpuDevice->deferredSwapchains.end();) {
        const VkResult result = retireSwapchain(it->get());
        if (!wsiTeardownSafe(result)) return result;
        _gpuDevice->swapchains.erase(it->get());
        it = _gpuDevice->deferredSwapchains.erase(it);
    }
    // Surface-only teardown retains its public facade. Retry it from either
    // acquire or present as well, including failures after the image drain.
    for (auto *swapchain : _gpuDevice->swapchains) {
        if (!swapchain->surfaceRetirementPending) continue;
        const VkResult result = retireSwapchain(swapchain);
        if (!wsiTeardownSafe(result)) return result;
    }
    return _gpuDevice->deviceLost ? VK_ERROR_DEVICE_LOST : VK_SUCCESS;
}

void CCVKDevice::prepareWSISubmit(CCVKGPUQueue *queue) {
    if (!_options.enableBarrierDeduce || queue->acquiredSwapchains.empty() ||
        !queue->presentSemaphoresSignaled || queue->wsiTransitionsQueued) return;

    // A previous submit consumed the batch's late PRESENT transitions. Each
    // further actual submit needs a new matching pair, including independent
    // A/B draws after an abort. Keep both transitions in that existing submit.
    ccstd::vector<VkImageMemoryBarrier> restore, finish;
    for (const auto *swapchain : queue->acquiredSwapchains) {
        restore.push_back(acquireBarrier);
        finish.push_back(presentBarrier);
        restore.back().image = finish.back().image = swapchain->swapchainImages[swapchain->curImageIndex];
    }
    _gpuTransportHub->checkIn([&](const CCVKGPUCommandBuffer *cmd) {
        cmdPipelineBarrier(CCVKDevice::getInstance()->gpuDevice(), cmd->vkCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, utils::toUint(restore.size()), restore.data());
    });
    _gpuTransportHub->checkIn([&](const CCVKGPUCommandBuffer *cmd) {
        cmdPipelineBarrier(CCVKDevice::getInstance()->gpuDevice(), cmd->vkCommandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, utils::toUint(finish.size()), finish.data());
    }, false, true);
    queue->wsiTransitionsQueued = true;
}

void CCVKDevice::acquire(Swapchain *const *swapchains, uint32_t count) {
    // A repeated acquire is an abandoned prior frame, not permission to drop
    // its semaphore waits or leave images owned by the application.
    while (!gpuSwapchains.empty()) {
        const auto pending = std::find_if(gpuSwapchains.begin(), gpuSwapchains.end(), [](const auto *swapchain) { return swapchain->abortPending; });
        auto *swapchain = pending == gpuSwapchains.end() ? gpuSwapchains.back() : *pending;
        const VkResult result = abortAcquiredSwapchain(swapchain);
        if (result != VK_SUCCESS) {
            CC_LOG_ERROR("Vulkan acquire deferred: previous image drain returned %d", result);
            return;
        }
    }
    if (retireDeferredSwapchains() != VK_SUCCESS) return;
    if (_onAcquire) _onAcquire->execute();

    auto *queue = static_cast<CCVKQueue *>(_queue);
    queue->gpuQueue()->lastSignaledSemaphores.clear();
    queue->gpuQueue()->acquiredSwapchains.clear();
    queue->gpuQueue()->presentSemaphoresSignaled = false;
    queue->gpuQueue()->wsiTransitionsQueued = false;
    vkSwapchainIndices.clear();
    gpuSwapchains.clear();
    vkSwapchains.clear();
    vkAcquireBarriers.clear();
    vkPresentBarriers.clear();
    for (uint32_t i = 0U; i < count; ++i) {
        auto *swapchain = static_cast<CCVKSwapchain *>(swapchains[i]);
        auto *gpuSwapchain = swapchain->gpuSwapchain();
        if (!gpuSwapchain || gpuSwapchain->retiring || _gpuDevice->deviceLost) continue;
        if (gpuSwapchain->runtimeOwnedImages) {
            // XR owns its images/acquire/release, never pair them with the
            // window-system VkSwapchain vectors.
            if (gpuSwapchain->lastPresentResult == VK_NOT_READY && !swapchain->checkSwapchainStatus()) continue;
            const xr::XRSwapchain acquired = _xr->doGFXDeviceAcquire(_api);
            gpuSwapchain->curImageIndex = acquired.swapchainImageIndex;
            if (!gpuSwapchain->hasRenderableImage()) {
                gpuSwapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
                CC_LOG_ERROR("XR acquired an invalid swapchain image index.");
            }
            continue;
        }
        gpuSwapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
        if (gpuSwapchain->wsi.hasFatalError() || gpuSwapchain->wsi.needsSurfaceRecreate()) continue;
        if (!gpuSwapchain->wsi.canAcquire() || gpuSwapchain->lastPresentResult != VK_SUCCESS) {
            if (!swapchain->checkSwapchainStatus()) {
                continue;
            }
        }
        if (!gpuSwapchain->vkSwapchain || !gpuSwapchain->wsi.beginAcquire()) continue;
        uint32_t imageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
        VkSemaphore acquireSemaphore = gpuSemaphorePool()->alloc();
        const VkResult res = vkAcquireNextImageKHR(_gpuDevice->vkDevice, gpuSwapchain->vkSwapchain, ~0ULL,
                                                 acquireSemaphore, VK_NULL_HANDLE, &imageIndex);
        if (!gpuSwapchain->wsi.completeAcquire(res, imageIndex)) {
            if (res == VK_ERROR_DEVICE_LOST) _gpuDevice->deviceLost = true;
            gpuSwapchain->lastPresentResult = res;
            if (gpuSwapchain->wsi.hasFatalError()) CC_LOG_ERROR("Vulkan acquire failed fatally: %d", gpuSwapchain->wsi.fatalResult());
            continue;
        }
        gpuSwapchain->curImageIndex = imageIndex;
        // Append all paired arrays together, exclusively after acquisition.
        vkSwapchains.push_back(gpuSwapchain->vkSwapchain);
        gpuSwapchains.push_back(gpuSwapchain);
        vkSwapchainIndices.push_back(imageIndex);
        queue->gpuQueue()->lastSignaledSemaphores.push_back(acquireSemaphore);
        queue->gpuQueue()->acquiredSwapchains.push_back(gpuSwapchain);
        auto &generation = *gpuSwapchain->imageGeneration;
        generation.imageAcquired[imageIndex] = true;
        ++generation.acquireSerials[imageIndex];
        // Reacquisition followed by the submit wait protects reuse of this
        // image's present semaphore. A separate present fence protects host
        // reset/destruction when maintenance1 was actually enabled.
        if (!generation.presentFences.empty() && generation.presentPending[imageIndex]) {
            const VkFence fence = generation.presentFences[imageIndex];
            const VkResult waited = vkWaitForFences(_gpuDevice->vkDevice, 1, &fence, VK_TRUE, DEFAULT_TIMEOUT);
            if (waited != VK_SUCCESS) {
                gpuSwapchain->abortPending = true;
                if (waited == VK_ERROR_DEVICE_LOST) _gpuDevice->deviceLost = true;
                CC_LOG_ERROR("Vulkan present fence reuse deferred: %d", waited);
                break;
            }
            const VkResult reset = vkResetFences(_gpuDevice->vkDevice, 1, &fence);
            if (reset != VK_SUCCESS) {
                gpuSwapchain->abortPending = true;
                if (reset == VK_ERROR_DEVICE_LOST) _gpuDevice->deviceLost = true;
                break;
            }
            generation.presentPending[imageIndex] = false;
        }
        if (_options.enableBarrierDeduce || !generation.imageInitialized[imageIndex]) {
            vkAcquireBarriers.push_back(acquireBarrier);
            vkAcquireBarriers.back().image = gpuSwapchain->swapchainImages[imageIndex];
            if (!generation.imageInitialized[imageIndex]) vkAcquireBarriers.back().oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            if (!_options.enableBarrierDeduce) {
                vkAcquireBarriers.back().newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                vkAcquireBarriers.back().dstAccessMask = 0;
            }
        }
        generation.imageInitialized[imageIndex] = true;
        vkPresentBarriers.push_back(presentBarrier);
        vkPresentBarriers.back().image = gpuSwapchain->swapchainImages[imageIndex];
    }
    _gpuDescriptorSetHub->flush();

    if (!vkAcquireBarriers.empty()) {
        _gpuTransportHub->checkIn(
            [&](const CCVKGPUCommandBuffer *gpuCommandBuffer) {
                cmdPipelineBarrier(CCVKDevice::getInstance()->gpuDevice(), gpuCommandBuffer->vkCommandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                     0, 0, nullptr, 0, nullptr, utils::toUint(vkAcquireBarriers.size()), vkAcquireBarriers.data());
            },
            false, false);

    }
    if (this->_options.enableBarrierDeduce && !vkPresentBarriers.empty()) {
        _gpuTransportHub->checkIn(
            [&](const CCVKGPUCommandBuffer *gpuCommandBuffer) {
                cmdPipelineBarrier(CCVKDevice::getInstance()->gpuDevice(), gpuCommandBuffer->vkCommandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                     0, 0, nullptr, 0, nullptr, utils::toUint(vkPresentBarriers.size()), vkPresentBarriers.data());
            },
            false, true);
        queue->gpuQueue()->wsiTransitionsQueued = true;
    }
}

namespace {
std::atomic<VulkanMeasurementCallback> measurementCallback{nullptr};
std::atomic<VulkanMeasurementPresentPolicy> measurementPresentPolicy{VulkanMeasurementPresentPolicy::Keep};
}
VulkanMeasurementCallback setVulkanMeasurementCallback(VulkanMeasurementCallback callback) noexcept {
    return measurementCallback.exchange(callback, std::memory_order_acq_rel);
}
VulkanMeasurementCallback getVulkanMeasurementCallback() noexcept {
    return measurementCallback.load(std::memory_order_acquire);
}
VulkanMeasurementPresentPolicy setVulkanMeasurementPresentPolicy(VulkanMeasurementPresentPolicy policy) noexcept {
    return measurementPresentPolicy.exchange(policy, std::memory_order_acq_rel);
}
VulkanMeasurementPresentPolicy getVulkanMeasurementPresentPolicy() noexcept {
    return measurementPresentPolicy.load(std::memory_order_acquire);
}

void CCVKDevice::present() {
    // A failed abort retains its exact semaphore/point pairing. Never hand
    // that batch to presentation or rotate/reset its frame slot.
    if (!wsiTeardownSafe(retryPendingWSIAborts())) return;
    if (retireDeferredSwapchains() != VK_SUCCESS || _gpuDevice->deviceLost) return;
    VulkanMeasurementScope measurement(VulkanMeasurementKind::Present, this);
    CC_PROFILE(CCVKDevicePresent);
    auto *queue = static_cast<CCVKQueue *>(_queue);
    _numDrawCalls = queue->_numDrawCalls;
    _numInstances = queue->_numInstances;
    _numTriangles = queue->_numTriangles;
    queue->_numDrawCalls = 0;
    queue->_numInstances = 0;
    queue->_numTriangles = 0;

    // Frames with an acquired image and no user command buffer still need
    // their acquisition waits and layout transport submitted before present.
    // Existing submitted frames do not get a second signal-only submit.
    if ((!vkSwapchains.empty() && !queue->gpuQueue()->presentSemaphoresSignaled) ||
        !_gpuTransportHub->empty(false) || !_gpuTransportHub->empty(true)) {
        const VkResult result = queue->submitChecked(nullptr, 0);
        if (result != VK_SUCCESS) return;
    }

#if CC_SWAPPY_ENABLED
    // tripple buffer?
    // static vector<uint8_t> queueFmlIdxBuff(_gpuDevice->backBufferCount);
    // std::iota(std::begin(queueFmlIdxBuff), std::end(queueFmlIdxBuff), 0);
    SwappyVk_setQueueFamilyIndex(_gpuDevice->vkDevice, queue->gpuQueue()->vkQueue, queue->gpuQueue()->queueFamilyIndex);
    auto vkCCPresentFunc = SwappyVk_queuePresent;
#else
    auto vkCCPresentFunc = vkQueuePresentKHR;
#endif

    if (!vkSwapchains.empty()) { // don't present if not acquired
        VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        presentInfo.waitSemaphoreCount = utils::toUint(queue->gpuQueue()->lastSignaledSemaphores.size());
        presentInfo.pWaitSemaphores = queue->gpuQueue()->lastSignaledSemaphores.data();
        presentInfo.swapchainCount = utils::toUint(vkSwapchains.size());
        presentInfo.pSwapchains = vkSwapchains.data();
        presentInfo.pImageIndices = vkSwapchainIndices.data();
        ccstd::vector<VkResult> results(vkSwapchains.size(), VK_SUCCESS);
        presentInfo.pResults = results.data();
        ccstd::vector<VkFence> presentFences;
        VkSwapchainPresentFenceInfoEXT fenceInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        if (_gpuDevice->capabilities.swapchainMaintenance1.enabled) {
            for (size_t i = 0; i < gpuSwapchains.size(); ++i) {
                presentFences.push_back(gpuSwapchains[i]->imageGeneration->presentFences[vkSwapchainIndices[i]]);
            }
            fenceInfo.swapchainCount = utils::toUint(presentFences.size());
            fenceInfo.pFences = presentFences.data();
            presentInfo.pNext = &fenceInfo;
        }

        // Window-system images are released independently of the XR runtime.
        VkResult res = vkCCPresentFunc(queue->gpuQueue()->vkQueue, &presentInfo);
        if (res == VK_ERROR_DEVICE_LOST) _gpuDevice->deviceLost = true;
        for (size_t i = 0; i < gpuSwapchains.size(); ++i) {
            // OOM/device loss may fail to enqueue the entire operation; WSI
            // per-swapchain results otherwise supersede the aggregate result.
            const VkResult result = res == VK_ERROR_DEVICE_LOST || res == VK_ERROR_OUT_OF_HOST_MEMORY ||
                                            res == VK_ERROR_OUT_OF_DEVICE_MEMORY ? res : results[i];
            gpuSwapchains[i]->lastPresentResult = result;
            gpuSwapchains[i]->imageGeneration->imageAcquired[vkSwapchainIndices[i]] = false;
            if (!presentFences.empty() &&
                (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR)) {
                gpuSwapchains[i]->imageGeneration->presentPending[vkSwapchainIndices[i]] = true;
            }
            gpuSwapchains[i]->wsi.onPresentResult(result);
            gpuSwapchains[i]->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
        }
    }
    for (auto *swapchain : _gpuDevice->swapchains) {
        if (swapchain->runtimeOwnedImages) swapchain->curImageIndex = VulkanWSIStatus::INVALID_IMAGE_INDEX;
    }
    // Present has consumed this frame's ownership. Offscreen work submitted
    // before the next acquire must not wait on a semaphore already handed to
    // presentation, or index an image using INVALID_IMAGE_INDEX.
    queue->gpuQueue()->lastSignaledSemaphores.clear();
    queue->gpuQueue()->acquiredSwapchains.clear();
    queue->gpuQueue()->presentSemaphoresSignaled = false;
    queue->gpuQueue()->wsiTransitionsQueued = false;
    vkSwapchains.clear();
    vkSwapchainIndices.clear();
    gpuSwapchains.clear();

    _gpuDevice->curBackBufferIndex = (_gpuDevice->curBackBufferIndex + 1) % _gpuDevice->backBufferCount;

    uint32_t fenceCount = gpuFencePool()->size();
    if (fenceCount) {
        const VkResult result = vkWaitForFences(_gpuDevice->vkDevice, fenceCount,
                                                gpuFencePool()->data(), VK_TRUE, DEFAULT_TIMEOUT);
        if (result != VK_SUCCESS) {
            if (result == VK_ERROR_DEVICE_LOST) _gpuDevice->deviceLost = true;
            return;
        }
    }

    const VkResult observed = _gpuDevice->observeCompletedFences(gpuFencePool()->data(), fenceCount);
    if (observed != VK_SUCCESS) {
        if (observed == VK_ERROR_DEVICE_LOST) _gpuDevice->deviceLost = true;
        return;
    }
    // Bulk fence observation covers ordinary submissions from this slot.
    // Cross-slot buffer-instance points must be waited with either backend.
    const auto slotResult = _gpuDevice->waitSlot(_gpuDevice->curBackBufferIndex, DEFAULT_TIMEOUT);
    if (slotResult != VK_SUCCESS) {
        if (slotResult == VK_ERROR_DEVICE_LOST) _gpuDevice->deviceLost = true;
        CC_LOG_ERROR("Vulkan frame-slot retirement deferred: %d", slotResult);
        return;
    }
    if (!_gpuDevice->slotComplete(_gpuDevice->curBackBufferIndex)) {
        CC_LOG_ERROR("Vulkan slot retirement rejected: GPU completion is not proven.");
        return;
    }
    gpuFencePool()->reset();
    gpuSemaphorePool()->reset(); // the slot's submission waits have completed
    _gpuDevice->retireRecordings();
    gpuRecycleBin()->clear();
    gpuStagingBufferPool()->reset();
    gpuStagingBufferPool()->shrinkSize();
    if (_pipelineCache) _pipelineCache->checkpoint();
    if (_xr) {
        _xr->postGFXDevicePresent(_api);
    }
}

void CCVKDevice::frameSync() {
}

VulkanCompletionObjectCounts CCVKDevice::completionObjectCounts() const {
    VulkanCompletionObjectCounts counts;
    for (const auto &pool : _gpuFencePools) counts.fences += pool->allocatedCount();
    for (const auto &pool : _gpuSemaphorePools) counts.semaphores += pool->allocatedCount();
    if (_gpuTransportHub) counts.fences += _gpuTransportHub->allocatedFenceCount();
    if (_gpuDevice) counts.semaphores += static_cast<uint32_t>(_gpuDevice->completionSemaphores.size());
    return counts;
}

CCVKGPUFencePool *CCVKDevice::gpuFencePool() { return _gpuFencePools[_gpuDevice->curBackBufferIndex].get(); }
CCVKGPURecycleBin *CCVKDevice::gpuRecycleBin() { return _gpuRecycleBins[_gpuDevice->curBackBufferIndex].get(); }
CCVKGPUStagingBufferPool *CCVKDevice::gpuStagingBufferPool() { return _gpuStagingBufferPools[_gpuDevice->curBackBufferIndex].get(); }
CCVKGPUSemaphorePool *CCVKDevice::gpuSemaphorePool() const { return _gpuSemaphorePools[_gpuDevice->curBackBufferIndex].get(); }

VkResult CCVKDevice::waitAllFences(uint64_t timeout) {
    if (!_gpuDevice || !_gpuDevice->vkDevice) return VK_SUCCESS;
    if (_gpuDevice->deviceLost) {
        const VkResult result = vkDeviceWaitIdle(_gpuDevice->vkDevice);
        if (!wsiTeardownSafe(result)) return result;
        for (auto semaphore : _gpuDevice->lostSubmissionSemaphores) vkDestroySemaphore(_gpuDevice->vkDevice, semaphore, nullptr);
        _gpuDevice->lostSubmissionSemaphores.clear();
        return VK_ERROR_DEVICE_LOST;
    }
    for (auto &entry : _gpuDevice->completionTrackers) {
        const auto value = entry.second->submittedValue();
        if (!value) continue;
        const VkResult result = entry.second->wait({entry.second->domain(), value}, timeout);
        if (result != VK_SUCCESS) {
            if (result == VK_ERROR_DEVICE_LOST) {
                _gpuDevice->deviceLost = true;
                return waitAllFences(timeout);
            }
            return result;
        }
    }
    // Every successful actual submit is tracked, including transport. Only
    // now can pools reset; a failed submit's unsignaled fence is not in use.
    if (!_gpuDevice->runtimePaths.useTimelineSemaphore) {
        for (auto &fencePool : _gpuFencePools) fencePool->reset();
    }
    _gpuDevice->retireRecordings();
    return VK_SUCCESS;
}

void CCVKDevice::updateBackBufferCount(uint32_t backBufferCount) {
    if (backBufferCount <= _gpuDevice->backBufferCount) return;
    for (uint32_t i = _gpuDevice->backBufferCount; i < backBufferCount; i++) {
        _gpuFencePools.push_back(std::make_unique<CCVKGPUFencePool>(_gpuDevice.get()));
        _gpuSemaphorePools.push_back(std::make_unique<CCVKGPUSemaphorePool>(_gpuDevice.get()));
        _gpuRecycleBins.push_back(std::make_unique<CCVKGPURecycleBin>(_gpuDevice.get()));
        _gpuStagingBufferPools.push_back(std::make_unique<CCVKGPUStagingBufferPool>(_gpuDevice.get()));
    }
    _gpuBufferHub->updateBackBufferCount(backBufferCount);
    _gpuDescriptorSetHub->updateBackBufferCount(backBufferCount);
    _gpuDevice->backBufferCount = backBufferCount;
}

void CCVKDevice::initDeviceFeature() {
    _features[toNumber(Feature::ELEMENT_INDEX_UINT)] = true;
    _features[toNumber(Feature::INSTANCED_ARRAYS)] = true;
    _features[toNumber(Feature::MULTIPLE_RENDER_TARGETS)] = true;
    _features[toNumber(Feature::BLEND_MINMAX)] = true;
    _features[toNumber(Feature::COMPUTE_SHADER)] = true;
    _features[toNumber(Feature::INPUT_ATTACHMENT_BENEFIT)] = true;
    _features[toNumber(Feature::SUBPASS_COLOR_INPUT)] = true;
    _features[toNumber(Feature::SUBPASS_DEPTH_STENCIL_INPUT)] = true;
    _features[toNumber(Feature::RASTERIZATION_ORDER_NOCOHERENT)] = true;
    _features[toNumber(Feature::MULTI_SAMPLE_RESOLVE_DEPTH_STENCIL)] = checkExtension("VK_KHR_depth_stencil_resolve");

    _gpuContext->debugReport =
        _gpuContext->checkExtension(VK_EXT_DEBUG_REPORT_EXTENSION_NAME) &&
        checkExtension(VK_EXT_DEBUG_MARKER_EXTENSION_NAME) &&
        (vkCmdDebugMarkerBeginEXT != nullptr) &&
        (vkCmdDebugMarkerInsertEXT != nullptr) &&
        (vkCmdDebugMarkerEndEXT != nullptr);
    _gpuContext->debugUtils =
        _gpuContext->checkExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
        (vkCmdBeginDebugUtilsLabelEXT != nullptr) &&
        (vkCmdInsertDebugUtilsLabelEXT != nullptr) &&
        (vkCmdEndDebugUtilsLabelEXT != nullptr);
}

void CCVKDevice::initFormatFeature() {
    const auto formatLen = static_cast<size_t>(Format::COUNT);
    VkFormatProperties properties = {};
    VkFormat format = {};
    VkFormatFeatureFlags formatFeature = {};
    for (uint32_t i = toNumber(Format::R8); i < formatLen; ++i) {
        if (static_cast<Format>(i) == Format::ETC_RGB8) continue;
        format = mapVkFormat(static_cast<Format>(i), _gpuDevice.get());
        vkGetPhysicalDeviceFormatProperties(_gpuContext->physicalDevice, format, &properties);

        // render buffer support
        formatFeature = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (properties.optimalTilingFeatures & formatFeature) {
            _formatFeatures[i] |= FormatFeature::RENDER_TARGET;
        }
        // texture storage support
        formatFeature = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if (properties.optimalTilingFeatures & formatFeature) {
            _formatFeatures[i] |= FormatFeature::STORAGE_TEXTURE;
        }
        // sampled render target support
        formatFeature = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if (properties.optimalTilingFeatures & formatFeature) {
            _formatFeatures[i] |= FormatFeature::SAMPLED_TEXTURE;
        }
        // linear filter support
        formatFeature = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if (properties.optimalTilingFeatures & formatFeature) {
            _formatFeatures[i] |= FormatFeature::LINEAR_FILTER;
        }
        // vertex attribute support
        formatFeature = VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT;
        if (properties.bufferFeatures & formatFeature) {
            _formatFeatures[i] |= FormatFeature::VERTEX_ATTRIBUTE;
        }
        // shading reate support
        formatFeature = VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;
        if (properties.optimalTilingFeatures & formatFeature) {
            _formatFeatures[i] |= FormatFeature ::SHADING_RATE;
        }
    }
}

void CCVKDevice::initExtensionCapability() {
    _caps.supportVariableRateShading = checkExtension(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
    _caps.supportVariableRateShading &= _gpuDevice->capabilities.fragmentShadingRateDetail.pipelineFragmentShadingRate.enabled &&
                                        _gpuDevice->capabilities.fragmentShadingRateDetail.attachmentFragmentShadingRate.enabled;
    _caps.supportVariableRateShading &= vkCmdSetFragmentShadingRateKHR != nullptr;
    _caps.supportVariableRateShading &= hasFlag(_formatFeatures[static_cast<uint32_t>(Format::R8UI)], FormatFeatureBit::SHADING_RATE);

    _caps.supportSubPassShading = checkExtension(VK_HUAWEI_SUBPASS_SHADING_EXTENSION_NAME);
}

CommandBuffer *CCVKDevice::createCommandBuffer(const CommandBufferInfo & /*info*/, bool /*hasAgent*/) {
    return ccnew CCVKCommandBuffer;
}

Queue *CCVKDevice::createQueue() {
    return ccnew CCVKQueue;
}

QueryPool *CCVKDevice::createQueryPool() {
    return ccnew CCVKQueryPool;
}

Swapchain *CCVKDevice::createSwapchain() {
    if (_xr) {
        _xr->createXRSwapchains();
    }
    return ccnew CCVKSwapchain;
}

Buffer *CCVKDevice::createBuffer() {
    return ccnew CCVKBuffer;
}

Texture *CCVKDevice::createTexture() {
    return ccnew CCVKTexture;
}

Shader *CCVKDevice::createShader() {
    return ccnew CCVKShader;
}

InputAssembler *CCVKDevice::createInputAssembler() {
    return ccnew CCVKInputAssembler;
}

RenderPass *CCVKDevice::createRenderPass() {
    return ccnew CCVKRenderPass;
}

Framebuffer *CCVKDevice::createFramebuffer() {
    return ccnew CCVKFramebuffer;
}

DescriptorSet *CCVKDevice::createDescriptorSet() {
    return ccnew CCVKDescriptorSet;
}

DescriptorSetLayout *CCVKDevice::createDescriptorSetLayout() {
    return ccnew CCVKDescriptorSetLayout;
}

PipelineLayout *CCVKDevice::createPipelineLayout() {
    return ccnew CCVKPipelineLayout;
}

PipelineState *CCVKDevice::createPipelineState() {
    return ccnew CCVKPipelineState;
}

Sampler *CCVKDevice::createSampler(const SamplerInfo &info) {
    return ccnew CCVKSampler(info);
}

GeneralBarrier *CCVKDevice::createGeneralBarrier(const GeneralBarrierInfo &info) {
    return ccnew CCVKGeneralBarrier(info);
}

TextureBarrier *CCVKDevice::createTextureBarrier(const TextureBarrierInfo &info) {
    return ccnew CCVKTextureBarrier(info);
}

BufferBarrier *CCVKDevice::createBufferBarrier(const BufferBarrierInfo &info) {
    return ccnew CCVKBufferBarrier(info);
}

void CCVKDevice::copyBuffersToTexture(const uint8_t *const *buffers, Texture *dst, const BufferTextureCopy *regions, uint32_t count) {
    CC_PROFILE(CCVKDeviceCopyBuffersToTexture);
    gpuTransportHub()->checkIn([this, buffers, dst, regions, count](CCVKGPUCommandBuffer *gpuCommandBuffer) {
        cmdFuncCCVKCopyBuffersToTexture(this, buffers, static_cast<CCVKTexture *>(dst)->gpuTexture(), regions, count, gpuCommandBuffer);
    });
}

void CCVKDevice::copyTextureToBuffers(Texture *srcTexture, uint8_t *const *buffers, const BufferTextureCopy *regions, uint32_t count) {
    CC_PROFILE(CCVKDeviceCopyTextureToBuffers);
    uint32_t totalSize = 0U;
    Format format = srcTexture->getFormat();
    ccstd::vector<std::pair<uint32_t, uint32_t>> regionOffsetSizes(count);
    for (size_t i = 0U; i < count; ++i) {
        const BufferTextureCopy &region = regions[i];
        uint32_t w = region.buffStride > 0 ? region.buffStride : region.texExtent.width;
        uint32_t h = region.buffTexHeight > 0 ? region.buffTexHeight : region.texExtent.height;
        uint32_t regionSize = formatSize(format, w, h, region.texExtent.depth);
        regionOffsetSizes[i] = {totalSize, regionSize};
        totalSize += regionSize;
    }

    uint32_t texelSize = GFX_FORMAT_INFOS[toNumber(format)].size;
    IntrusivePtr<CCVKGPUBufferView> stagingBuffer = gpuStagingBufferPool()->alloc(totalSize, texelSize);

    // make sure the src texture is up-to-date
    if (waitAllFences() != VK_SUCCESS) return;

    _gpuTransportHub->checkIn(
        [&](CCVKGPUCommandBuffer *cmdBuffer) {
            cmdFuncCCVKCopyTextureToBuffers(this, static_cast<const CCVKTexture *>(srcTexture)->gpuTexture(), stagingBuffer, regions, count, cmdBuffer);
        },
        true);

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t regionOffset = 0;
        uint32_t regionSize = 0;
        std::tie(regionOffset, regionSize) = regionOffsetSizes[i];
        memcpy(buffers[i], stagingBuffer->mappedData() + regionOffset, regionSize);
    }
}

void CCVKDevice::getQueryPoolResults(QueryPool *queryPool) {
    CC_PROFILE(CCVKDeviceGetQueryPoolResults);
    auto *vkQueryPool = static_cast<CCVKQueryPool *>(queryPool);
    auto queryCount = static_cast<uint32_t>(vkQueryPool->_ids.size());
    CC_ASSERT(queryCount <= vkQueryPool->getMaxQueryObjects());

    const bool bWait = queryPool->getForceWait();
    uint32_t width = bWait ? 1U : 2U;
    uint64_t stride = sizeof(uint64_t) * width;
    VkQueryResultFlagBits flag = bWait ? VK_QUERY_RESULT_WAIT_BIT : VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    ccstd::vector<uint64_t> results(queryCount * width, 0);

    if (queryCount > 0U) {
        VkResult result = vkGetQueryPoolResults(
            gpuDevice()->vkDevice,
            vkQueryPool->_gpuQueryPool->vkPool,
            0,
            queryCount,
            static_cast<size_t>(queryCount * stride),
            results.data(),
            stride,
            VK_QUERY_RESULT_64_BIT | flag);
        CC_ASSERT(result == VK_SUCCESS || result == VK_NOT_READY);
    }

    ccstd::unordered_map<uint32_t, uint64_t> mapResults;
    for (auto queryId = 0U; queryId < queryCount; queryId++) {
        uint32_t offset = queryId * width;
        if (bWait || results[offset + 1] > 0) {
            uint32_t id = vkQueryPool->_ids[queryId];
            auto iter = mapResults.find(id);
            if (iter != mapResults.end()) {
                iter->second += results[offset];
            } else {
                mapResults[id] = results[offset];
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(vkQueryPool->_mutex);
        vkQueryPool->_results = std::move(mapResults);
    }
}

//////////////////////////// Function Fallbacks /////////////////////////////////////////

static VkResult VKAPI_PTR vkCreateRenderPass2KHRFallback(
    VkDevice device,
    const VkRenderPassCreateInfo2 *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkRenderPass *pRenderPass) {
    static ccstd::vector<VkAttachmentDescription> attachmentDescriptions;
    static ccstd::vector<VkSubpassDescription> subpassDescriptions;
    static ccstd::vector<VkAttachmentReference> attachmentReferences;
    static ccstd::vector<VkSubpassDependency> subpassDependencies;
    static ccstd::vector<size_t> inputs;
    static ccstd::vector<size_t> colors;
    static ccstd::vector<size_t> resolves;
    static ccstd::vector<size_t> depths;

    attachmentDescriptions.resize(pCreateInfo->attachmentCount);
    for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
        VkAttachmentDescription &desc{attachmentDescriptions[i]};
        const VkAttachmentDescription2 &desc2{pCreateInfo->pAttachments[i]};
        desc.flags = desc2.flags;
        desc.format = desc2.format;
        desc.samples = desc2.samples;
        desc.loadOp = desc2.loadOp;
        desc.storeOp = desc2.storeOp;
        desc.stencilLoadOp = desc2.stencilLoadOp;
        desc.stencilStoreOp = desc2.stencilStoreOp;
        desc.initialLayout = desc2.initialLayout;
        desc.finalLayout = desc2.finalLayout;
    }

    subpassDescriptions.resize(pCreateInfo->subpassCount);
    attachmentReferences.clear();
    inputs.assign(pCreateInfo->subpassCount, std::numeric_limits<size_t>::max());
    colors.assign(pCreateInfo->subpassCount, std::numeric_limits<size_t>::max());
    resolves.assign(pCreateInfo->subpassCount, std::numeric_limits<size_t>::max());
    depths.assign(pCreateInfo->subpassCount, std::numeric_limits<size_t>::max());
    for (uint32_t i = 0; i < pCreateInfo->subpassCount; ++i) {
        const VkSubpassDescription2 &desc2{pCreateInfo->pSubpasses[i]};
        if (desc2.inputAttachmentCount) {
            inputs[i] = attachmentReferences.size();
            for (uint32_t j = 0; j < desc2.inputAttachmentCount; ++j) {
                attachmentReferences.push_back({desc2.pInputAttachments[j].attachment, desc2.pInputAttachments[j].layout});
            }
        }
        if (desc2.colorAttachmentCount) {
            colors[i] = attachmentReferences.size();
            for (uint32_t j = 0; j < desc2.colorAttachmentCount; ++j) {
                attachmentReferences.push_back({desc2.pColorAttachments[j].attachment, desc2.pColorAttachments[j].layout});
            }
            if (desc2.pResolveAttachments) {
                resolves[i] = attachmentReferences.size();
                for (uint32_t j = 0; j < desc2.colorAttachmentCount; ++j) {
                    attachmentReferences.push_back({desc2.pResolveAttachments[j].attachment, desc2.pResolveAttachments[j].layout});
                }
            }
        }
        if (desc2.pDepthStencilAttachment) {
            depths[i] = attachmentReferences.size();
            attachmentReferences.push_back({desc2.pDepthStencilAttachment->attachment, desc2.pDepthStencilAttachment->layout});
        }
    }
    for (uint32_t i = 0; i < pCreateInfo->subpassCount; ++i) {
        VkSubpassDescription &desc{subpassDescriptions[i]};
        const VkSubpassDescription2 &desc2{pCreateInfo->pSubpasses[i]};
        desc.flags = desc2.flags;
        desc.pipelineBindPoint = desc2.pipelineBindPoint;
        desc.inputAttachmentCount = desc2.inputAttachmentCount;
        desc.pInputAttachments = inputs[i] > attachmentReferences.size() ? nullptr : &attachmentReferences[inputs[i]];
        desc.colorAttachmentCount = desc2.colorAttachmentCount;
        desc.pColorAttachments = colors[i] > attachmentReferences.size() ? nullptr : &attachmentReferences[colors[i]];
        desc.pResolveAttachments = resolves[i] > attachmentReferences.size() ? nullptr : &attachmentReferences[resolves[i]];
        desc.pDepthStencilAttachment = depths[i] > attachmentReferences.size() ? nullptr : &attachmentReferences[depths[i]];
        desc.preserveAttachmentCount = desc2.preserveAttachmentCount;
        desc.pPreserveAttachments = desc2.pPreserveAttachments;
    }

    subpassDependencies.resize(pCreateInfo->dependencyCount);
    for (uint32_t i = 0; i < pCreateInfo->dependencyCount; ++i) {
        VkSubpassDependency &desc{subpassDependencies[i]};
        const VkSubpassDependency2 &desc2{pCreateInfo->pDependencies[i]};
        desc.srcSubpass = desc2.srcSubpass;
        desc.dstSubpass = desc2.dstSubpass;
        desc.srcStageMask = desc2.srcStageMask;
        desc.dstStageMask = desc2.dstStageMask;
        desc.srcAccessMask = desc2.srcAccessMask;
        desc.dstAccessMask = desc2.dstAccessMask;
        desc.dependencyFlags = desc2.dependencyFlags;
    }

    VkRenderPassCreateInfo renderPassCreateInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassCreateInfo.attachmentCount = utils::toUint(attachmentDescriptions.size());
    renderPassCreateInfo.pAttachments = attachmentDescriptions.data();
    renderPassCreateInfo.subpassCount = utils::toUint(subpassDescriptions.size());
    renderPassCreateInfo.pSubpasses = subpassDescriptions.data();
    renderPassCreateInfo.dependencyCount = utils::toUint(subpassDependencies.size());
    renderPassCreateInfo.pDependencies = subpassDependencies.data();

    return vkCreateRenderPass(device, &renderPassCreateInfo, pAllocator, pRenderPass);
}

SampleCount CCVKDevice::getMaxSampleCount(Format format, TextureUsage usage, TextureFlags flags) const {
    auto vkFormat = mapVkFormat(format, gpuDevice());
    auto usages = mapVkImageUsageFlags(usage, flags);

    VkImageFormatProperties imageFormatProperties = {};
    vkGetPhysicalDeviceImageFormatProperties(_gpuContext->physicalDevice, vkFormat, VK_IMAGE_TYPE_2D,
                                             VK_IMAGE_TILING_OPTIMAL, usages, 0, &imageFormatProperties);

    if (imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_64_BIT) return SampleCount::X64;
    if (imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_32_BIT) return SampleCount::X32;
    if (imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_16_BIT) return SampleCount::X16;
    if (imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_8_BIT) return SampleCount::X8;
    if (imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_4_BIT) return SampleCount::X4;
    if (imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_2_BIT) return SampleCount::X2;

    return SampleCount::X1;
}

} // namespace gfx
} // namespace cc
