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

#include "VKStd.h"

#include "VKCommandBuffer.h"
#include "VKCommands.h"
#include "VKDevice.h"
#include "VKQueue.h"

namespace cc {
namespace gfx {

CCVKQueue::CCVKQueue() {
    _typedID = generateObjectID<decltype(this)>();
}

CCVKQueue::~CCVKQueue() {
    destroy();
}

void CCVKQueue::doInit(const QueueInfo & /*info*/) {
    _gpuQueue = std::make_unique<CCVKGPUQueue>();
    _gpuQueue->type = _type;
    cmdFuncCCVKGetDeviceQueue(CCVKDevice::getInstance(), _gpuQueue.get());
}

void CCVKQueue::doDestroy() {
    _pendingEarlyCommands.clear();
    _pendingLateCommands.clear();
    _pendingEarlyRecordings.clear();
    _pendingLateRecordings.clear();
    _gpuQueue = nullptr;
}

void CCVKQueue::submit(CommandBuffer *const *cmdBuffs, uint32_t count) {
    CCVKDevice *device = CCVKDevice::getInstance();
    // An abort may have consumed a binary semaphore even when its host wait
    // timed out. Only the maintenance retry may use this acquired batch now.
    if (device->gpuDevice()->deviceLost || device->hasPendingWSIAbort()) return;
    const VkResult result = submitChecked(cmdBuffs, count);
    if (result != VK_SUCCESS) CC_LOG_ERROR("Vulkan queue submission failed: %d", result);
}

VkResult CCVKQueue::submitChecked(CommandBuffer *const *cmdBuffs, uint32_t count) {
    CCVKDevice *device = CCVKDevice::getInstance();
    if (device->gpuDevice()->deviceLost) return VK_ERROR_DEVICE_LOST;
    _gpuQueue->commandBuffers.clear();
    std::vector<std::shared_ptr<CCVKGPURecording>> recordings;
    ccstd::vector<CCVKCommandBuffer *> selected;

#if BARRIER_DEDUCTION_LEVEL >= BARRIER_DEDUCTION_LEVEL_BASIC
    device->gpuBarrierManager()->update(device->gpuTransportHub());
#endif
    device->gpuBufferHub()->flush(device->gpuTransportHub());
    device->prepareWSISubmit(_gpuQueue.get());

    if (!device->gpuTransportHub()->empty(false)) {
        _pendingEarlyCommands.push_back(device->gpuTransportHub()->packageForFlight(false, _pendingEarlyRecordings));
    }
    _gpuQueue->commandBuffers.insert(_gpuQueue->commandBuffers.end(), _pendingEarlyCommands.begin(), _pendingEarlyCommands.end());
    recordings.insert(recordings.end(), _pendingEarlyRecordings.begin(), _pendingEarlyRecordings.end());

    for (uint32_t i = 0U; i < count; ++i) {
        auto *cmdBuff = static_cast<CCVKCommandBuffer *>(cmdBuffs[i]);
        if (!cmdBuff->_pendingQueue.empty()) {
            if (!cmdBuff->pendingWSIUsageValid()) {
                cmdBuff->_pendingQueue.pop();
                if (!cmdBuff->_pendingWSIUsage.empty()) cmdBuff->_pendingWSIUsage.pop();
                if (!cmdBuff->_pendingRecordings.empty()) cmdBuff->_pendingRecordings.pop();
                ++_gpuQueue->discardedWSICommandBuffers;
                continue; // recorded before an abort/resize, never execute it
            }
            if (!cmdBuff->pendingRecordingValid()) {
                cmdBuff->_pendingQueue.pop();
                cmdBuff->_pendingWSIUsage.pop();
                if (!cmdBuff->_pendingRecordings.empty()) cmdBuff->_pendingRecordings.pop();
                ++_gpuQueue->discardedRecordingCommandBuffers;
                continue;
            }
            // Repeated facade entries must not submit one one-time recording twice.
            if (std::find(selected.begin(), selected.end(), cmdBuff) != selected.end()) continue;
            _gpuQueue->commandBuffers.push_back(cmdBuff->_pendingQueue.front());
            recordings.push_back(cmdBuff->_pendingRecordings.front());
            selected.push_back(cmdBuff);

            _numDrawCalls += cmdBuff->_numDrawCalls;
            _numInstances += cmdBuff->_numInstances;
            _numTriangles += cmdBuff->_numTriangles;
        }
    }

    if (!device->gpuTransportHub()->empty(true)) {
        _pendingLateCommands.push_back(device->gpuTransportHub()->packageForFlight(true, _pendingLateRecordings));
    }
    _gpuQueue->commandBuffers.insert(_gpuQueue->commandBuffers.end(), _pendingLateCommands.begin(), _pendingLateCommands.end());
    recordings.insert(recordings.end(), _pendingLateRecordings.begin(), _pendingLateRecordings.end());

    size_t waitSemaphoreCount = _gpuQueue->lastSignaledSemaphores.size();
    ccstd::vector<VkSemaphore> signals;
    signals.reserve(_gpuQueue->acquiredSwapchains.size());
    for (auto *swapchain : _gpuQueue->acquiredSwapchains) {
        auto signal = swapchain->imageGeneration->renderFinished[swapchain->curImageIndex];
        if (_gpuQueue->presentSemaphoresSignaled) {
            // Multiple actual submits in a frame: the previous signal is
            // consumed by this submit, not by present. Retire it on this
            // submit's completion point and replace the image-owned semaphore. No extra
            // submission is introduced solely to signal a present semaphore.
            VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            const VkResult result = vkCreateSemaphore(device->gpuDevice()->vkDevice, &createInfo, nullptr, &signal);
            if (result != VK_SUCCESS) {
                for (auto created : signals) vkDestroySemaphore(device->gpuDevice()->vkDevice, created, nullptr);
                if (result == VK_ERROR_DEVICE_LOST) device->gpuDevice()->deviceLost = true;
                return result;
            }
        }
        signals.push_back(signal);
    }
    _gpuQueue->submitStageMasks.resize(waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

    VulkanSubmitBatch submitInfo;
    submitInfo.commands.assign(_gpuQueue->commandBuffers.begin(), _gpuQueue->commandBuffers.end());
    for (size_t i=0; i<waitSemaphoreCount; ++i) {
        submitInfo.waits.push_back({_gpuQueue->lastSignaledSemaphores[i], _gpuQueue->submitStageMasks[i]});
    }
    for (auto signal : signals) submitInfo.signals.push_back({signal});

    VkFence vkFence = _gpuQueue->completionTracker->usesTimeline() ? VK_NULL_HANDLE : device->gpuFencePool()->alloc();
    GpuCompletionPoint point;
    const auto result = device->gpuDevice()->submitTracked(_gpuQueue.get(), submitInfo, vkFence, point, recordings);
    if (result != VK_SUCCESS) {
        if (result == VK_ERROR_DEVICE_LOST) {
            device->gpuDevice()->deviceLost = true;
            // Unlike OOM, DEVICE_LOST does not guarantee the submit left its
            // resources untouched. Prove quiescence before rolling them back.
            const VkResult idle = device->waitAllFences();
            if (!wsiTeardownSafe(idle)) {
                if (_gpuQueue->presentSemaphoresSignaled) {
                    // The device-lost entry gate prevents another submit from
                    // populating this owner. Swap transfers without allocating
                    // after the driver may already have consumed the batch.
                    device->gpuDevice()->lostSubmissionSemaphores.swap(signals);
                }
                return idle; // pool fence and recording owners remain retained
            }
        }
        if (vkFence) device->gpuFencePool()->cancel(vkFence);
        // OOM left the submit untouched; DEVICE_LOST was explicitly waited
        // above. Only now may the uncommitted replacement handles be released.
        if (_gpuQueue->presentSemaphoresSignaled) {
            for (auto created : signals) vkDestroySemaphore(device->gpuDevice()->vkDevice, created, nullptr);
        }
        if (result == VK_ERROR_DEVICE_LOST) device->gpuDevice()->deviceLost = true;
        return result;
    }
    if (_gpuQueue->presentSemaphoresSignaled) {
        for (size_t i = 0; i < signals.size(); ++i) {
            auto *swapchain = _gpuQueue->acquiredSwapchains[i];
            auto &old = swapchain->imageGeneration->renderFinished[swapchain->curImageIndex];
            device->gpuRecycleBin()->collectSemaphore(old);
            old = signals[i];
        }
    }
    _pendingEarlyCommands.clear();
    _pendingLateCommands.clear();
    _pendingEarlyRecordings.clear();
    _pendingLateRecordings.clear();
    for (auto *cmdBuff : selected) {
        cmdBuff->_pendingQueue.pop();
        cmdBuff->_pendingWSIUsage.pop();
        cmdBuff->_pendingRecordings.pop();
    }

    _gpuQueue->wsiTransitionsQueued = false;
    _gpuQueue->lastSignaledSemaphores = std::move(signals);
    _gpuQueue->presentSemaphoresSignaled = !_gpuQueue->acquiredSwapchains.empty();
    return VK_SUCCESS;
}

} // namespace gfx
} // namespace cc
