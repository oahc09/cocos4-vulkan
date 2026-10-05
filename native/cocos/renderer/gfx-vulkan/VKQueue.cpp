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
    _gpuQueue = nullptr;
}

void CCVKQueue::submit(CommandBuffer *const *cmdBuffs, uint32_t count) {
    CCVKDevice *device = CCVKDevice::getInstance();
    _gpuQueue->commandBuffers.clear();
    std::vector<std::shared_ptr<CCVKGPURecording>> recordings;
    ccstd::vector<CCVKCommandBuffer *> selected;

#if BARRIER_DEDUCTION_LEVEL >= BARRIER_DEDUCTION_LEVEL_BASIC
    device->gpuBarrierManager()->update(device->gpuTransportHub());
#endif
    device->gpuBufferHub()->flush(device->gpuTransportHub());
    device->prepareWSISubmit(_gpuQueue.get());

    if (!device->gpuTransportHub()->empty(false)) {
        _gpuQueue->commandBuffers.push_back(device->gpuTransportHub()->packageForFlight(false, recordings));
    }

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
        _gpuQueue->commandBuffers.push_back(device->gpuTransportHub()->packageForFlight(true, recordings));
    }

    size_t waitSemaphoreCount = _gpuQueue->lastSignaledSemaphores.size();
    ccstd::vector<VkSemaphore> signals;
    for (auto *swapchain : _gpuQueue->acquiredSwapchains) {
        auto &signal = swapchain->imageGeneration->renderFinished[swapchain->curImageIndex];
        if (_gpuQueue->presentSemaphoresSignaled) {
            // Multiple actual submits in a frame: the previous signal is
            // consumed by this submit, not by present. Retire it on this
            // submit's completion point and replace the image-owned semaphore. No extra
            // submission is introduced solely to signal a present semaphore.
            device->gpuRecycleBin()->collectSemaphore(signal);
            VkSemaphoreCreateInfo createInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VK_CHECK(vkCreateSemaphore(device->gpuDevice()->vkDevice, &createInfo, nullptr, &signal));
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
    VK_CHECK(result);
    if (result != VK_SUCCESS) return; // no point, keep pending user recording ownership
    for (auto *cmdBuff : selected) {
        cmdBuff->_pendingQueue.pop();
        cmdBuff->_pendingWSIUsage.pop();
        cmdBuff->_pendingRecordings.pop();
    }

    _gpuQueue->wsiTransitionsQueued = false;
    _gpuQueue->lastSignaledSemaphores = std::move(signals);
    _gpuQueue->presentSemaphoresSignaled = !_gpuQueue->acquiredSwapchains.empty();
}

} // namespace gfx
} // namespace cc
