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
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
****************************************************************************/

// Default-off diagnostic inside the real Windows test-cases application.
#include "RealProjectIndirect.h"
#include <windows.h>
#include "renderdoc/renderdoc_app.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "base/threading/MessageQueue.h"
#include "gfx-agent/DeviceAgent.h"
#include "gfx-base/GFXCommandBuffer.h"
#include "gfx-base/GFXQueue.h"
#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKFramebuffer.h"
#include "gfx-vulkan/VKPipelineLayout.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKRenderPass.h"
#include "gfx-vulkan/VKShader.h"
#include "gfx-vulkan/VKTexture.h"
#include "gfx-vulkan/VKUtils.h"

namespace cc::vulkan_test {
namespace {
using namespace gfx;
constexpr uint32_t W = 32, H = 16, PIXELS = W * H, BYTES = PIXELS * 4;
enum class Kind { Buffer,
                  Image,
                  Pipeline,
                  Module,
                  Layout,
                  Pass,
                  Framebuffer };
struct Object {
    Kind kind{};
    uint64_t handle = 0;
    unsigned destroyed = 0;
};
struct Draw {
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint32_t count = 0, stride = 0;
    bool indexed = false;
};
struct Phase {
    std::array<Draw, 4> draws{};
    unsigned calls = 0, submitted = 0;
    VkCommandBuffer copyCB = VK_NULL_HANDLE;
    VkBuffer source = VK_NULL_HANDLE;
    VkDeviceSize sourceOffset = 0;
    uint32_t stride = 0, packedBytes = 0, mismatches = 0;
    bool chain = false, packed = false;
};
struct State {
    bool prepared = false, enabled = false, started = false, armed = false, creating = false, installed = false;
    unsigned failures = 0, phase = 0, objects = 0, devices = 0, liveErrors = 0, postErrors = 0;
    unsigned bufferFacades = 0, imageFacades = 0, bufferDrops = 0, imageDrops = 0;
    bool multi = false, restored = false, unsupported = false;
    std::string mode;
    VkDevice device = VK_NULL_HANDLE;
    CCVKGPUContext *context = nullptr; // Cleared before returning from vkDestroyDevice.
    std::array<Object, 32> object{};
    std::array<Phase, 2> phases{};
    PFN_vkCmdDrawIndirect draw = nullptr;
    PFN_vkCmdDrawIndexedIndirect indexed = nullptr;
    PFN_vkQueueSubmit submit = nullptr;
    PFN_vkQueueSubmit2 submit2 = nullptr;
    PFN_vkQueueSubmit2KHR submit2KHR = nullptr;
    PFN_vkCreateShaderModule createModule = nullptr;
    PFN_vkCreateGraphicsPipelines createPipeline = nullptr;
    PFN_vkDestroyBuffer destroyBuffer = nullptr;
    PFN_vkDestroyImage destroyImage = nullptr;
    PFN_vkDestroyPipeline destroyPipeline = nullptr;
    PFN_vkDestroyShaderModule destroyModule = nullptr;
    PFN_vkDestroyPipelineLayout destroyLayout = nullptr;
    PFN_vkDestroyRenderPass destroyPass = nullptr;
    PFN_vkDestroyFramebuffer destroyFramebuffer = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
} s;

// The Windows CRT treats an empty environment value as absent. Query the
// named Win32 environment entry as well so an explicit empty opt-in is rejected.
bool environmentPresent(const wchar_t *name) noexcept {
    SetLastError(ERROR_SUCCESS);
    const DWORD count = GetEnvironmentVariableW(name, nullptr, 0);
    return count != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}
void check(bool ok, const char *label) noexcept {
    if (!ok) {
        ++s.failures;
        CC_LOG_ERROR("project-indirect failure: %s", label);
    }
}
void require(bool ok, const char *label) {
    check(ok, label);
    if (!ok) throw std::runtime_error(label);
}
template <class T>
uint64_t bits(T h) noexcept {
    static_assert(sizeof(T) <= sizeof(uint64_t));
    uint64_t v = 0;
    std::memcpy(&v, &h, sizeof(T));
    return v;
}
template <class T>
void remember(Kind kind, T h) noexcept {
    const auto id = bits(h);
    if (!id || s.objects == s.object.size()) {
        check(false, "bounded live object handle");
        return;
    }
    for (unsigned i = 0; i < s.objects; ++i)
        if (s.object[i].kind == kind && s.object[i].handle == id) {
            check(false, "unique diagnostic object");
            return;
        }
    s.object[s.objects++] = {kind, id, 0};
}
template <class T>
void destroyed(Kind kind, T h) noexcept {
    for (unsigned i = 0; i < s.objects; ++i)
        if (s.object[i].kind == kind && s.object[i].handle == bits(h)) {
            ++s.object[i].destroyed;
            check(s.object[i].destroyed == 1, "destroy exactly once");
            return;
        }
}
void drawRecord(VkCommandBuffer cb, VkBuffer b, VkDeviceSize offset, uint32_t count, uint32_t stride, bool indexed) noexcept {
    if (!s.armed) return;
    auto &p = s.phases[s.phase];
    if (p.calls == p.draws.size()) {
        check(false, "draw hook capacity");
        return;
    }
    p.draws[p.calls++] = {cb, b, offset, count, stride, indexed};
}
VKAPI_ATTR void VKAPI_CALL drawHook(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride) {
    if (!s.draw) {
        check(false, "draw PFN");
        return;
    }
    s.draw(cb, b, o, n, stride);
    drawRecord(cb, b, o, n, stride, false);
}
VKAPI_ATTR void VKAPI_CALL indexedHook(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride) {
    if (!s.indexed) {
        check(false, "indexed draw PFN");
        return;
    }
    s.indexed(cb, b, o, n, stride);
    drawRecord(cb, b, o, n, stride, true);
}
void forwarded(VkCommandBuffer cb) noexcept {
    if (s.armed && cb == s.phases[s.phase].copyCB) ++s.phases[s.phase].submitted;
}
VKAPI_ATTR VkResult VKAPI_CALL submitHook(VkQueue q, uint32_t n, const VkSubmitInfo *i, VkFence f) {
    if (!s.submit) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.submit(q, n, i, f);
    if (s.armed && r == VK_SUCCESS && i)
        for (uint32_t a = 0; a < n; ++a)
            if (i[a].pCommandBuffers)
                for (uint32_t b = 0; b < i[a].commandBufferCount; ++b) forwarded(i[a].pCommandBuffers[b]);
    return r;
}
VkResult submit2Forward(PFN_vkQueueSubmit2 fn, VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f) {
    if (!fn) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = fn(q, n, i, f);
    if (s.armed && r == VK_SUCCESS && i)
        for (uint32_t a = 0; a < n; ++a)
            if (i[a].pCommandBufferInfos)
                for (uint32_t b = 0; b < i[a].commandBufferInfoCount; ++b) forwarded(i[a].pCommandBufferInfos[b].commandBuffer);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL submit2Hook(VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f) { return submit2Forward(s.submit2, q, n, i, f); }
VKAPI_ATTR VkResult VKAPI_CALL submit2KHRHook(VkQueue q, uint32_t n, const VkSubmitInfo2 *i, VkFence f) { return submit2Forward(s.submit2KHR, q, n, i, f); }
VKAPI_ATTR VkResult VKAPI_CALL moduleHook(VkDevice d, const VkShaderModuleCreateInfo *i, const VkAllocationCallbacks *a, VkShaderModule *o) {
    if (!s.createModule) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.createModule(d, i, a, o);
    if (s.creating && r == VK_SUCCESS && o) remember(Kind::Module, *o);
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL pipelineHook(VkDevice d, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o) {
    if (!s.createPipeline) return VK_ERROR_INITIALIZATION_FAILED;
    const auto r = s.createPipeline(d, cache, n, i, a, o);
    if (s.creating && r == VK_SUCCESS && o)
        for (uint32_t j = 0; j < n; ++j) remember(Kind::Pipeline, o[j]);
    return r;
}
#define DESTROY_HOOK(Name, Handle, Member, K)                                               \
    VKAPI_ATTR void VKAPI_CALL Name(VkDevice d, Handle h, const VkAllocationCallbacks *a) { \
        if (!s.Member) {                                                                    \
            check(false, "destroy PFN");                                                    \
            return;                                                                         \
        }                                                                                   \
        s.Member(d, h, a);                                                                  \
        destroyed(Kind::K, h);                                                              \
    }
DESTROY_HOOK(bufferDestroy, VkBuffer, destroyBuffer, Buffer)
DESTROY_HOOK(imageDestroy, VkImage, destroyImage, Image)
DESTROY_HOOK(pipelineDestroy, VkPipeline, destroyPipeline, Pipeline)
DESTROY_HOOK(moduleDestroy, VkShaderModule, destroyModule, Module)
DESTROY_HOOK(layoutDestroy, VkPipelineLayout, destroyLayout, Layout)
DESTROY_HOOK(passDestroy, VkRenderPass, destroyPass, Pass)
DESTROY_HOOK(framebufferDestroy, VkFramebuffer, destroyFramebuffer, Framebuffer)
#undef DESTROY_HOOK
VKAPI_ATTR void VKAPI_CALL deviceDestroy(VkDevice d, const VkAllocationCallbacks *a) {
    if (!s.destroyDevice) {
        check(false, "device destroy PFN");
        return;
    }
    check(d == s.device && !s.devices, "expected device shutdown");
    if (s.context) s.liveErrors = s.context->validationErrorCount.load();
    s.destroyDevice(d, a);
    if (s.context) s.postErrors = s.context->validationErrorCount.load();
    s.context = nullptr;
    ++s.devices;
}
void install() {
    s.draw = vkCmdDrawIndirect;
    s.indexed = vkCmdDrawIndexedIndirect;
    s.submit = vkQueueSubmit;
    s.submit2 = vkQueueSubmit2;
    s.submit2KHR = vkQueueSubmit2KHR;
    s.createModule = vkCreateShaderModule;
    s.createPipeline = vkCreateGraphicsPipelines;
    s.destroyBuffer = vkDestroyBuffer;
    s.destroyImage = vkDestroyImage;
    s.destroyPipeline = vkDestroyPipeline;
    s.destroyModule = vkDestroyShaderModule;
    s.destroyLayout = vkDestroyPipelineLayout;
    s.destroyPass = vkDestroyRenderPass;
    s.destroyFramebuffer = vkDestroyFramebuffer;
    s.destroyDevice = vkDestroyDevice;
    require(s.draw && s.indexed && s.submit && s.createModule && s.createPipeline && s.destroyDevice &&
                s.destroyBuffer && s.destroyImage && s.destroyPipeline && s.destroyModule && s.destroyLayout && s.destroyPass && s.destroyFramebuffer,
            "required hook PFNs");
    vkCmdDrawIndirect = drawHook;
    vkCmdDrawIndexedIndirect = indexedHook;
    vkQueueSubmit = submitHook;
    if (s.submit2) vkQueueSubmit2 = submit2Hook;
    if (s.submit2KHR) vkQueueSubmit2KHR = submit2KHRHook;
    vkCreateShaderModule = moduleHook;
    vkCreateGraphicsPipelines = pipelineHook;
    vkDestroyBuffer = bufferDestroy;
    vkDestroyImage = imageDestroy;
    vkDestroyPipeline = pipelineDestroy;
    vkDestroyShaderModule = moduleDestroy;
    vkDestroyPipelineLayout = layoutDestroy;
    vkDestroyRenderPass = passDestroy;
    vkDestroyFramebuffer = framebufferDestroy;
    vkDestroyDevice = deviceDestroy;
    s.installed = true;
}
void restore() noexcept {
    if (!s.installed) return;
    vkCmdDrawIndirect = s.draw;
    vkCmdDrawIndexedIndirect = s.indexed;
    vkQueueSubmit = s.submit;
    vkQueueSubmit2 = s.submit2;
    vkQueueSubmit2KHR = s.submit2KHR;
    vkCreateShaderModule = s.createModule;
    vkCreateGraphicsPipelines = s.createPipeline;
    vkDestroyBuffer = s.destroyBuffer;
    vkDestroyImage = s.destroyImage;
    vkDestroyPipeline = s.destroyPipeline;
    vkDestroyShaderModule = s.destroyModule;
    vkDestroyPipelineLayout = s.destroyLayout;
    vkDestroyRenderPass = s.destroyPass;
    vkDestroyFramebuffer = s.destroyFramebuffer;
    vkDestroyDevice = s.destroyDevice;
    s.installed = false;
}
template <class T>
struct DeleteFacade {
    void operator()(T *p) const {
        if (p) {
            if constexpr (std::is_same_v<T, Buffer>) ++s.bufferDrops;
            if constexpr (std::is_same_v<T, Texture>) ++s.imageDrops;
            p->destroy();
            delete p;
        }
    }
};
template <class T>
using Owned = std::unique_ptr<T, DeleteFacade<T>>;
template <class T>
Owned<T> owned(T *p) {
    require(p != nullptr, "created facade");
    if constexpr (std::is_same_v<T, Buffer>) ++s.bufferFacades;
    if constexpr (std::is_same_v<T, Texture>) ++s.imageFacades;
    return Owned<T>(p);
}

void runPhase(CCVKDevice &d, RenderPass *rp, PipelineState *pso, unsigned index) {
    require(d.gpuContext()->validationErrorCount.load() < 10, "validation error budget");
    s.phase = index;
    auto &p = s.phases[index];
    auto texture = owned(d.createTexture({TextureType::TEX2D, TextureUsageBit::COLOR_ATTACHMENT | TextureUsageBit::TRANSFER_SRC, Format::RGBA8, W, H}));
    FramebufferInfo fb;
    fb.renderPass = rp;
    fb.colorTextures = {texture.get()};
    auto framebuffer = owned(d.createFramebuffer(fb));
    remember(Kind::Framebuffer, static_cast<CCVKFramebuffer *>(framebuffer.get())->gpuFBO()->vkFramebuffer);
    auto indirect = owned(d.createBuffer({BufferUsageBit::INDIRECT | BufferUsageBit::TRANSFER_DST | BufferUsageBit::TRANSFER_SRC,
                                          MemoryUsageBit::DEVICE, uint32_t(2 * sizeof(DrawInfo)), uint32_t(sizeof(DrawInfo))}));
    auto *gpuIndirect = static_cast<CCVKBuffer *>(indirect.get())->gpuBuffer();
    p.source = gpuIndirect->vkBuffer;
    auto pixels = owned(d.createBuffer({BufferUsageBit::TRANSFER_DST, MemoryUsageBit::HOST | MemoryUsageBit::DEVICE, BYTES, 1}));
    auto commands = owned(d.createBuffer({BufferUsageBit::TRANSFER_DST, MemoryUsageBit::HOST | MemoryUsageBit::DEVICE, 40, 1}));
    auto *gpuPixels = static_cast<CCVKBuffer *>(pixels.get())->gpuBuffer();
    auto *gpuCommands = static_cast<CCVKBuffer *>(commands.get())->gpuBuffer();
    require(gpuPixels->mappedData && gpuCommands->mappedData, "host buffers mapped");
    Owned<Buffer> indices;
    InputAssemblerInfo iaInfo;
    iaInfo.indirectBuffer = indirect.get();
    if (index) {
        indices = owned(d.createBuffer({BufferUsageBit::INDEX | BufferUsageBit::TRANSFER_DST, MemoryUsageBit::DEVICE, 12, 2}));
        iaInfo.indexBuffer = indices.get();
    }
    auto ia = owned(d.createInputAssembler(iaInfo));
    CommandBufferInfo cbInfo;
    cbInfo.queue = d.getQueue();
    auto cmd = owned(d.createCommandBuffer(cbInfo));
    std::array<DrawInfo, 2> info{};
    for (unsigned i = 0; i < 2; ++i) {
        info[i].instanceCount = 1;
        if (index) {
            info[i].indexCount = (s.mode == "skip-second" && i == 1) ? 0 : 3;
            info[i].firstIndex = i * 3;
        } else {
            info[i].vertexCount = (s.mode == "skip-second" && i == 1) ? 0 : 3;
            info[i].firstVertex = i * 3;
        }
    }
    const unsigned errorsBefore = d.gpuContext()->validationErrorCount.load();
    s.armed = true;
    cmd->begin();
    cmd->updateBuffer(indirect.get(), info.data(), sizeof(info));
    const uint16_t indexData[6] = {0, 1, 2, 3, 4, 5};
    if (indices) cmd->updateBuffer(indices.get(), indexData, sizeof(indexData));
    const Color clear{0, 0, 1, 1};
    cmd->beginRenderPass(rp, framebuffer.get(), {0, 0, W, H}, &clear, 1, 0);
    cmd->bindPipelineState(pso);
    cmd->bindInputAssembler(ia.get());
    cmd->draw(DrawInfo{});
    cmd->endRenderPass();
    p.stride = index ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
    p.packedBytes = p.stride * 2;
    cmd->customCommand([&](void *raw) {
        auto cb = reinterpret_cast<VkCommandBuffer>(raw);
        p.copyCB = cb;
        const VkDeviceSize src = gpuIndirect->getStartOffset(d.gpuDevice()->curBackBufferIndex);
        p.sourceOffset = src;
        VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        image.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        image.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        image.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image.image = static_cast<CCVKTexture *>(texture.get())->gpuTexture()->vkImage;
        image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cmdPipelineBarrier(d.gpuDevice(), cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image);
        VkBufferMemoryBarrier available{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        available.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        available.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        available.srcQueueFamilyIndex = available.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        available.buffer = gpuIndirect->vkBuffer;
        available.offset = src;
        available.size = p.packedBytes;
        cmdPipelineBarrier(d.gpuDevice(), cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &available, 0, nullptr);
        VkBufferCopy packed{src, 0, p.packedBytes};
        vkCmdCopyBuffer(cb, gpuIndirect->vkBuffer, gpuCommands->vkBuffer, 1, &packed);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {W, H, 1};
        vkCmdCopyImageToBuffer(cb, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gpuPixels->vkBuffer, 1, &region);
        VkBufferMemoryBarrier host[2]{};
        for (auto &b : host) {
            b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.size = VK_WHOLE_SIZE;
        }
        host[0].buffer = gpuPixels->vkBuffer;
        host[1].buffer = gpuCommands->vkBuffer;
        cmdPipelineBarrier(d.gpuDevice(), cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 2, host, 0, nullptr);
    });
    cmd->end();
    auto *command = cmd.get();
    d.getQueue()->submit(&command, 1);
    d.waitAllFences();
    s.armed = false;
    bool chain = p.copyCB && p.submitted == 1 && p.calls == (s.multi ? 1U : 2U);
    for (unsigned i = 0; i < p.calls; ++i) {
        const auto &call = p.draws[i];
        chain &= call.cb == p.copyCB && call.buffer == p.source && call.indexed == bool(index) &&
                 call.offset == p.sourceOffset + (s.multi ? 0 : i * p.stride) && call.count == (s.multi ? 2U : 1U) && call.stride == p.stride;
    }
    p.chain = chain && d.gpuContext()->validationErrorCount.load() == errorsBefore;
    require(p.chain, "actual indirect/copy/submit chain");
    require(vmaInvalidateAllocation(d.gpuDevice()->memoryAllocator, gpuPixels->vmaAllocation, 0, BYTES) == VK_SUCCESS &&
                vmaInvalidateAllocation(d.gpuDevice()->memoryAllocator, gpuCommands->vmaAllocation, 0, p.packedBytes) == VK_SUCCESS,
            "invalidate readback");
    std::array<uint32_t, 10> expected{};
    for (unsigned i = 0; i < 2; ++i) {
        const auto &x = info[i];
        const unsigned n = i * (p.stride / 4);
        if (index) {
            expected[n] = x.indexCount;
            expected[n + 1] = x.instanceCount;
            expected[n + 2] = x.firstIndex;
            expected[n + 3] = uint32_t(x.vertexOffset);
            expected[n + 4] = x.firstInstance;
        } else {
            expected[n] = x.vertexCount;
            expected[n + 1] = x.instanceCount;
            expected[n + 2] = x.firstVertex;
            expected[n + 3] = x.firstInstance;
        }
    }
    p.packed = std::memcmp(expected.data(), gpuCommands->mappedData, p.packedBytes) == 0;
    check(p.packed, "actual GPU packed commands");
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) {
            const uint8_t golden[4] = {uint8_t(x < W / 2 ? 255 : 0), uint8_t(x < W / 2 ? 0 : 255), 0, 255};
            const auto *value = gpuPixels->mappedData + (y * W + x) * 4;
            for (unsigned c = 0; c < 4; ++c) p.mismatches += value[c] != golden[c];
        }
    check(p.mismatches == 0, "red/green golden contents");
    CC_LOG_INFO("project-indirect phase: indexed=%u calls=%u commands=2 stride=%u packedBytes=%u packed=%u submits=%u chain=%u checkedBytes=%u mismatches=%u",
                index, p.calls, p.stride, p.packedBytes, unsigned(p.packed), p.submitted, unsigned(p.chain), BYTES, p.mismatches);
}
// Only used with an explicitly injected, already loaded RenderDoc module.
// No library is loaded and no capture is requested in normal runs.
class CaptureScope final {
public:
    CaptureScope() {
        const char *path = std::getenv("CC_VULKAN_REAL_PROJECT_INDIRECT_CAPTURE");
        if (!environmentPresent(L"CC_VULKAN_REAL_PROJECT_INDIRECT_CAPTURE")) return;
        require(path && *path != '\0', "nonempty capture path");
        const auto module = GetModuleHandleW(L"renderdoc.dll");
        const auto getAPI = module ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI")) : nullptr;
        require(getAPI && getAPI(eRENDERDOC_API_Version_1_4_1, reinterpret_cast<void **>(&api_)) == 1 && api_, "injected RenderDoc API");
        require(!api_->IsFrameCapturing(), "no overlapping capture");
        api_->SetCaptureFilePathTemplate(path);
        api_->StartFrameCapture(nullptr, nullptr);
        active_ = api_->IsFrameCapturing() == 1;
        require(active_, "offscreen capture started");
    }
    ~CaptureScope() {
        if (active_) api_->DiscardFrameCapture(nullptr, nullptr);
    }
    void finish() {
        if (!active_) return;
        const auto result = api_->EndFrameCapture(nullptr, nullptr);
        active_ = false;
        require(result == 1, "offscreen capture ended");
        CC_LOG_INFO("RenderDoc indirect capture: started=1 ended=1");
    }
    CaptureScope(const CaptureScope &) = delete;
    CaptureScope &operator=(const CaptureScope &) = delete;

private:
    RENDERDOC_API_1_4_1 *api_ = nullptr;
    bool active_ = false;
};
void run() noexcept {
    s.started = true;
    try {
        auto *d = CCVKDevice::getInstance();
        require(d && d->gpuDevice() && d->gpuContext(), "live backend");
        s.device = d->gpuDevice()->vkDevice;
        s.context = d->gpuContext();
        require(s.context->validationEnabled && s.context->synchronizationValidationEnabled && s.context->bestPracticesValidationEnabled, "validation triad");
        CC_LOG_INFO("project-indirect validation: ENABLED");
        require(vkGetPhysicalDeviceImageFormatProperties && vkCmdCopyBuffer && vkCmdCopyImageToBuffer, "query and copy PFNs");
        VkImageFormatProperties props{};
        const auto supported = vkGetPhysicalDeviceImageFormatProperties(s.context->physicalDevice, VK_FORMAT_R8G8B8A8_UNORM,
                                                                        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, &props);
        if (supported == VK_ERROR_FORMAT_NOT_SUPPORTED || (supported == VK_SUCCESS && !(props.sampleCounts & VK_SAMPLE_COUNT_1_BIT))) {
            s.unsupported = true;
            return;
        }
        require(supported == VK_SUCCESS && props.maxExtent.width >= W && props.maxExtent.height >= H, "RGBA8 image support");
        const bool oldMulti = d->gpuDevice()->useMultiDrawIndirect;
        if (s.mode == "multi" && !oldMulti) {
            s.unsupported = true;
            return;
        }
        struct RestoreMulti {
            CCVKGPUDevice *d;
            bool old;
            ~RestoreMulti() {
                d->useMultiDrawIndirect = old;
                s.restored = d->useMultiDrawIndirect == old;
            }
        } restoreMulti{d->gpuDevice(), oldMulti};
        s.multi = s.mode == "multi" || (s.mode == "skip-second" && oldMulti);
        d->gpuDevice()->useMultiDrawIndirect = s.multi;
        install();
        s.creating = true;
        auto layout = owned(d->createPipelineLayout(PipelineLayoutInfo{}));
        remember(Kind::Layout, static_cast<CCVKPipelineLayout *>(layout.get())->gpuPipelineLayout()->vkPipelineLayout);
        ShaderInfo info;
        info.name = "real-project-indirect";
        ShaderStage vs;
        vs.stage = ShaderStageFlagBit::VERTEX;
        vs.source = "layout(location=0) flat out int halfId;void main(){int i=gl_VertexIndex%3;vec2 p=vec2(float((i<<1)&2),float(i&2));gl_Position=vec4(p*2.0-1.0,0.0,1.0);halfId=gl_VertexIndex/3;}";
        ShaderStage fs;
        fs.stage = ShaderStageFlagBit::FRAGMENT;
        fs.source = "layout(location=0) flat in int halfId;layout(location=0) out vec4 o;void main(){if((gl_FragCoord.x<16.0)!=(halfId==0))discard;o=halfId==0?vec4(1,0,0,1):vec4(0,1,0,1);}";
        info.stages = {vs, fs};
        auto shader = owned(d->createShader(info));
        GeneralBarrierInfo barrier;
        barrier.prevAccesses = AccessFlagBit::NONE;
        barrier.nextAccesses = AccessFlagBit::COLOR_ATTACHMENT_WRITE;
        RenderPassInfo passInfo;
        passInfo.colorAttachments.resize(1);
        auto &color = passInfo.colorAttachments[0];
        color.format = Format::RGBA8;
        color.loadOp = LoadOp::CLEAR;
        color.storeOp = StoreOp::STORE;
        color.barrier = d->getGeneralBarrier(barrier);
        passInfo.subpasses.resize(1);
        passInfo.subpasses[0].colors = {0};
        auto rp = owned(d->createRenderPass(passInfo));
        remember(Kind::Pass, static_cast<CCVKRenderPass *>(rp.get())->gpuRenderPass()->vkRenderPass);
        PipelineStateInfo ps;
        ps.shader = shader.get();
        ps.pipelineLayout = layout.get();
        ps.renderPass = rp.get();
        ps.primitive = PrimitiveMode::TRIANGLE_LIST;
        ps.rasterizerState.cullMode = CullMode::NONE;
        ps.depthStencilState.depthTest = ps.depthStencilState.depthWrite = false;
        auto pso = owned(d->createPipelineState(ps));
        require(static_cast<CCVKPipelineState *>(pso.get())->gpuPipelineState()->vkPipeline != VK_NULL_HANDLE, "created graphics pipeline");
        s.creating = false;
        CC_LOG_INFO("project-indirect path: multi=%u fault=%u firstInstance=0", unsigned(s.multi), unsigned(s.mode == "skip-second"));
        CaptureScope capture;
        runPhase(*d, rp.get(), pso.get(), 0);
        runPhase(*d, rp.get(), pso.get(), 1);
        capture.finish();
    } catch (const std::exception &e) {
        check(false, e.what());
    } catch (...) {
        check(false, "diagnostic exception");
    }
    s.creating = s.armed = false;
}
} // namespace
bool prepareIndirect() noexcept {
    if (s.prepared) return !s.failures;
    s.prepared = true;
    try {
        const char *mode = std::getenv("CC_VULKAN_REAL_PROJECT_INDIRECT");
        s.enabled = environmentPresent(L"CC_VULKAN_REAL_PROJECT_INDIRECT");
        if (!s.enabled) {
            CC_LOG_INFO("project-indirect mode: off enabled=0");
            return true;
        }
        require(mode && *mode, "known nonempty indirect mode");
        s.mode = mode;
        require(s.mode == "multi" || s.mode == "loop" || s.mode == "skip-second", "known nonempty indirect mode");
        require(!environmentPresent(L"CC_VULKAN_REAL_PROJECT_PREWARM") && !environmentPresent(L"CC_VULKAN_REAL_PROJECT_CACHE_PATH"), "diagnostic modes exclusive");
        CC_LOG_INFO("project-indirect mode: %s enabled=1", s.mode.c_str());
        return true;
    } catch (...) {
        return false;
    }
}
void startIndirect() noexcept {
    if (!s.enabled || s.failures) return;
    if (s.started) {
        check(false, "start once");
        return;
    }
    if (auto *agent = DeviceAgent::getInstance()) {
        auto *messages = agent->getMessageQueue();
        if (!messages) {
            check(false, "render queue");
            return;
        }
        ENQUEUE_MESSAGE_0(messages, RealProjectIndirect, { run(); });
        messages->kickAndWait();
    } else
        run();
}
int finishIndirect(int platformResult) noexcept {
    if (!s.enabled) return platformResult;
    if (s.unsupported) {
        restore();
        CC_LOG_INFO("project-indirect completed: UNSUPPORTED");
        return 3;
    }
    check(platformResult == 0 && s.started && s.devices == 1 && s.restored, "normal shutdown and policy restoration");
    unsigned dead = 0;
    for (unsigned i = 0; i < s.objects; ++i) {
        dead += s.object[i].destroyed;
        check(s.object[i].destroyed == 1, "diagnostic object retired");
    }
    check(s.objects > 0 && dead == s.objects && !s.liveErrors && !s.postErrors, "validation and retirement through device destruction");
    check(s.bufferFacades == 7 && s.bufferDrops == 7 && s.imageFacades == 2 && s.imageDrops == 2, "diagnostic facade release counts");
    CC_LOG_INFO("project-indirect allocations: bufferFacades=%u bufferDrops=%u imageFacades=%u imageDrops=%u allocatorDestroyObserved=0", s.bufferFacades, s.bufferDrops, s.imageFacades, s.imageDrops);
    CC_LOG_INFO("project-indirect retirement: created=%u destroyed=%u devices=%u restored=%u", s.objects, dead, s.devices, unsigned(s.restored));
    CC_LOG_INFO("project-indirect completed: failures=%u liveErrors=%u postErrors=%u outcome=%s", s.failures, s.liveErrors, s.postErrors, s.failures ? "FAILED" : "CLEAN");
    restore();
    return platformResult || s.failures ? 1 : 0;
}
} // namespace cc::vulkan_test
