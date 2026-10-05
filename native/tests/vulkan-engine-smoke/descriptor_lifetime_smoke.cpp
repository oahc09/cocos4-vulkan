// Real offscreen compute descriptor mutation tests for ordinary/template updates.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "gfx-vulkan/VKBuffer.h"
#include "gfx-vulkan/VKDescriptorSet.h"
#include "gfx-vulkan/VKDevice.h"
#include "gfx-vulkan/VKPipelineState.h"
#include "gfx-vulkan/VKQueue.h"
#include "gfx-vulkan/VKGPUObjects.h"
#include "renderer/GFXDeviceManager.h"
#include "platform/FileUtils.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

int cocos_main(int, const char **) { return 0; }
void cocos_destroy() {}
namespace {
unsigned failures = 0;
void check(bool ok, const char *reason) { if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", reason); } }
VkCommandBuffer original = VK_NULL_HANDLE, lastBegin = VK_NULL_HANDLE;
struct Bind { VkCommandBuffer cb; VkDescriptorSet set; };
std::vector<uint32_t> boundDynamicOffsets;
std::vector<Bind> binds;
std::vector<VkDescriptorSet> updated;
unsigned attempted = 0, forwarded = 0, dispatches = 0;
PFN_vkBeginCommandBuffer realBegin = nullptr;
PFN_vkCmdBindDescriptorSets realBind = nullptr;
PFN_vkCmdDispatch realDispatch = nullptr;
PFN_vkUpdateDescriptorSets realUpdate = nullptr;
PFN_vkQueueSubmit realSubmit = nullptr;
PFN_vkQueueSubmit2 realSubmit2 = nullptr;
unsigned legacySubmitCalls=0, modernSubmitCalls=0;
VkSemaphore gateSemaphore = VK_NULL_HANDLE;
unsigned gatedSubmits = 0;
struct PendingGate {
    VkDevice device = VK_NULL_HANDLE;
    VkSemaphore semaphore = VK_NULL_HANDLE;
    std::thread releaser;
    std::atomic<bool> released{false};
    std::atomic<VkResult> result{VK_NOT_READY};
    explicit PendingGate(VkDevice d) : device(d) {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; info.pNext=&type;
        check(vkCreateSemaphore(device,&info,nullptr,&semaphore)==VK_SUCCESS,"test timeline gate creation");
        gateSemaphore=semaphore;
    }
    void releaseLater() {
        releaser=std::thread([this] {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
            info.semaphore=semaphore; info.value=1;
            result.store(vkSignalSemaphore(device,&info));
            released.store(true);
        });
    }
    void close() {
        if(releaser.joinable()) releaser.join();
        gateSemaphore=VK_NULL_HANDLE;
        if(semaphore) {vkDestroySemaphore(device,semaphore,nullptr);semaphore=VK_NULL_HANDLE;}
    }
    ~PendingGate(){close();}
};
VKAPI_ATTR VkResult VKAPI_CALL beginHook(VkCommandBuffer cb, const VkCommandBufferBeginInfo *info) {
    const auto r = realBegin(cb, info); if (r == VK_SUCCESS) lastBegin = cb; return r;
}
VKAPI_ATTR void VKAPI_CALL bindHook(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
    uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t offsets, const uint32_t *values) {
    for (uint32_t i = 0; i < count; ++i) binds.push_back({cb, sets[i]});
    for (uint32_t i = 0; i < offsets; ++i) boundDynamicOffsets.push_back(values[i]);
    realBind(cb, bp, layout, first, count, sets, offsets, values);
}
VKAPI_ATTR void VKAPI_CALL dispatchHook(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    ++dispatches; realDispatch(cb, x, y, z);
}
VKAPI_ATTR void VKAPI_CALL updateHook(VkDevice d, uint32_t n, const VkWriteDescriptorSet *writes,
    uint32_t copies, const VkCopyDescriptorSet *copy) {
    for (uint32_t i = 0; i < n; ++i) updated.push_back(writes[i].dstSet);
    for (uint32_t i = 0; i < copies; ++i) updated.push_back(copy[i].dstSet);
    realUpdate(d, n, writes, copies, copy);
}
VKAPI_ATTR VkResult VKAPI_CALL submitHook(VkQueue q, uint32_t n, const VkSubmitInfo *infos, VkFence f) {
    unsigned hits = 0;
    for (uint32_t i = 0; i < n; ++i) for (uint32_t j = 0; j < infos[i].commandBufferCount; ++j)
        if (original && infos[i].pCommandBuffers[j] == original) ++hits;
    attempted += hits;
    if (hits) std::printf("descriptor submit: originalAttempt=%u\n", hits);
    VkResult r;
    if(hits && gateSemaphore) {
        // Tracker already owns the timeline pNext. Replace that node locally,
        // preserving all values/other pNext nodes and append the test wait.
        const auto *old=static_cast<const VkTimelineSemaphoreSubmitInfo *>(infos[0].pNext);
        if(n!=1 || (old && old->sType!=VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO)) {
            check(false,"gate injection requires the real tracker timeline node");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        std::vector<VkSemaphore> waits;
        std::vector<VkPipelineStageFlags> stages;
        std::vector<uint64_t> values;
        for(uint32_t i=0;i<infos[0].waitSemaphoreCount;++i) {
            waits.push_back(infos[0].pWaitSemaphores[i]); stages.push_back(infos[0].pWaitDstStageMask[i]);
            values.push_back(old ? old->pWaitSemaphoreValues[i] : 0);
        }
        waits.push_back(gateSemaphore); stages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);values.push_back(1);
        VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        std::vector<uint64_t> binarySignalValues(infos[0].signalSemaphoreCount,0);
        if(old) timeline=*old;
        else {timeline.signalSemaphoreValueCount=infos[0].signalSemaphoreCount;timeline.pSignalSemaphoreValues=binarySignalValues.data();}
        timeline.waitSemaphoreValueCount=static_cast<uint32_t>(values.size());timeline.pWaitSemaphoreValues=values.data();
        VkSubmitInfo gated=infos[0];gated.pNext=&timeline;
        gated.waitSemaphoreCount=static_cast<uint32_t>(waits.size());gated.pWaitSemaphores=waits.data();gated.pWaitDstStageMask=stages.data();
        r=realSubmit(q,1,&gated,f);
        if(r==VK_SUCCESS)++gatedSubmits;
    }else r = realSubmit(q, n, infos, f);
    if (r == VK_SUCCESS) {forwarded += hits;++legacySubmitCalls;}
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL submitHook2(VkQueue q,uint32_t n,const VkSubmitInfo2 *infos,VkFence f) {
    unsigned hits=0;
    for(uint32_t i=0;i<n;++i)for(uint32_t j=0;j<infos[i].commandBufferInfoCount;++j)
        if(original && infos[i].pCommandBufferInfos[j].commandBuffer==original)++hits;
    attempted+=hits;
    if(hits)std::printf("descriptor submit: originalAttempt=%u\n",hits);
    VkResult r;
    if(hits && gateSemaphore) {
        if(n!=1){check(false,"one actual Submit2 gate batch required");return VK_ERROR_INITIALIZATION_FAILED;}
        std::vector<VkSemaphoreSubmitInfo> waits;
        for(uint32_t i=0;i<infos[0].waitSemaphoreInfoCount;++i) waits.push_back(infos[0].pWaitSemaphoreInfos[i]);
        VkSemaphoreSubmitInfo gate{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        gate.semaphore=gateSemaphore;gate.value=1;gate.stageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        waits.push_back(gate);
        VkSubmitInfo2 gated=infos[0];gated.waitSemaphoreInfoCount=static_cast<uint32_t>(waits.size());gated.pWaitSemaphoreInfos=waits.data();
        r=realSubmit2(q,1,&gated,f);if(r==VK_SUCCESS)++gatedSubmits;
    }else r=realSubmit2(q,n,infos,f);
    if(r==VK_SUCCESS){forwarded+=hits;++modernSubmitCalls;}
    return r;
}
struct Hooks {
    Hooks() {
        realBegin=vkBeginCommandBuffer; vkBeginCommandBuffer=beginHook;
        realBind=vkCmdBindDescriptorSets; vkCmdBindDescriptorSets=bindHook;
        realDispatch=vkCmdDispatch; vkCmdDispatch=dispatchHook;
        realUpdate=vkUpdateDescriptorSets; vkUpdateDescriptorSets=updateHook;
        realSubmit=vkQueueSubmit; vkQueueSubmit=submitHook;
        realSubmit2=vkQueueSubmit2; if(realSubmit2)vkQueueSubmit2=submitHook2;
    }
    ~Hooks() {
        vkBeginCommandBuffer=realBegin; vkCmdBindDescriptorSets=realBind;
        vkCmdDispatch=realDispatch; vkUpdateDescriptorSets=realUpdate; vkQueueSubmit=realSubmit; vkQueueSubmit2=realSubmit2;
    }
};
template <class T> void destroy(T *&object) { if (object) { object->destroy(); delete object; object=nullptr; } }
}
using namespace cc; using namespace cc::gfx;
int main(int argc, char **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout,nullptr,_IONBF,0); std::setvbuf(stderr,nullptr,_IONBF,0);
    std::string name; bool templateUpdates=false;
    for(int i=1;i<argc;++i) {
        if(std::strcmp(argv[i],"--case")==0 && i+1<argc) name=argv[++i];
        else if(std::strcmp(argv[i],"--template")==0) templateUpdates=true;
    }
    const bool offsetCase=name=="delayed-dynamic-offset";
    const bool dynamicSlot=name=="delayed-dynamic-slot" || offsetCase;
    const bool mutation=name=="update-after-record" || name=="update-during-record";
    if(name!="control" && !mutation && !dynamicSlot) return 2;
    const auto out=std::filesystem::current_path()/("descriptor-life-"+name);
    std::filesystem::create_directories(out);
    std::unique_ptr<FileUtils> files(createFileUtils()); files->setWritablePath(out.generic_string()+"/");
    std::unique_ptr<Device> device(DeviceManager::create());
    if(!device || device->getGfxAPI()!=API::VULKAN) { if(device)device->destroy(); return 1; }
    auto &raw=*CCVKDevice::getInstance(); Device *backend=&raw; auto *gpu=raw.gpuDevice();
    auto errors=[&]{return raw.gpuContext()->validationErrorCount.load(std::memory_order_relaxed);};
    const bool validation=raw.gpuContext()->validationEnabled && raw.gpuContext()->bestPracticesValidationEnabled && raw.gpuContext()->synchronizationValidationEnabled;
    std::printf("validation + best practices + sync: %s\n",validation?"ENABLED":"NOT VERIFIED");
    if(!validation) {device->destroy();return 1;}
    if(dynamicSlot && (!gpu->capabilities.timelineSemaphore.enabled || gpu->backBufferCount<3)) {
        std::fprintf(stderr,"FAIL: delayed-dynamic-slot requires enabled timeline feature and at least three slots\n");
        device->destroy();return 2;
    }
    // Choose once before creating the test layout, not production policy.
    gpu->useDescriptorUpdateTemplate=templateUpdates; backend->enableAutoBarrier(false);
    std::printf("descriptor config: template=%d autoBarrier=0\n", templateUpdates);
    Hooks hooks; const auto initialErrors=errors();
    BufferInfo bi; bi.usage=BufferUsageBit::STORAGE | BufferUsageBit::TRANSFER_DST | BufferUsageBit::TRANSFER_SRC;
    bi.memUsage=MemoryUsageBit::DEVICE; bi.size=64; bi.stride=4;
    const uint32_t extraOffset=offsetCase ? static_cast<uint32_t>(std::max<VkDeviceSize>(64,raw.gpuContext()->physicalDeviceProperties.limits.minStorageBufferOffsetAlignment)) : 0;
    const uint32_t inputSize=extraOffset+64;
    BufferInfo inputInfo=bi;inputInfo.size=inputSize;
    if(dynamicSlot) inputInfo.memUsage=MemoryUsageBit::HOST | MemoryUsageBit::DEVICE;
    Buffer *a=backend->createBuffer(inputInfo), *b=backend->createBuffer(bi), *output=backend->createBuffer(bi);
    Buffer *inputView=nullptr;
    if(offsetCase){BufferViewInfo viewInfo;viewInfo.buffer=a;viewInfo.offset=0;viewInfo.range=64;inputView=backend->createBuffer(viewInfo);}
    BufferInfo hi; hi.usage=BufferUsageBit::TRANSFER_DST;hi.memUsage=MemoryUsageBit::HOST;hi.size=64;hi.stride=4;
    Buffer *host=backend->createBuffer(hi);
    Buffer *secondHost=name=="update-during-record"?backend->createBuffer(hi):nullptr;
    CommandBufferInfo ci; ci.queue=backend->getQueue(); CommandBuffer *cmd=backend->createCommandBuffer(ci);
    std::vector<uint8_t> patternA(inputSize,0xa5), patternB(inputSize,0x5a);
    auto vkBuffer=[](Buffer *buf){return static_cast<CCVKBuffer *>(buf)->gpuBuffer()->vkBuffer;};
    auto barrier=[&](void *handle,Buffer *buf,VkAccessFlags src,VkAccessFlags dst,VkPipelineStageFlags from,VkPipelineStageFlags to){
        VkBufferMemoryBarrier info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};info.buffer=vkBuffer(buf);info.size=64;
        info.srcAccessMask=src;info.dstAccessMask=dst;info.srcQueueFamilyIndex=info.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        vkCmdPipelineBarrier(static_cast<VkCommandBuffer>(handle),from,to,0,0,nullptr,1,&info,0,nullptr);
    };
    std::printf("descriptor phase: upload begin\n");
    if(dynamicSlot) {
        a->update(patternA.data(),inputSize);
        auto *input=static_cast<CCVKBuffer *>(a)->gpuBuffer();
        VkMemoryPropertyFlags properties=0;
        vmaGetAllocationMemoryProperties(gpu->memoryAllocator,input->vmaAllocation,&properties);
        std::printf("dynamic allocation: instanceSize=%llu coherent=%d\n",static_cast<unsigned long long>(input->instanceSize),!!(properties & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT));
        // Isolate lifetime from a separate non-coherent visibility defect.
        check(vmaFlushAllocation(gpu->memoryAllocator,input->vmaAllocation,input->getStartOffset(gpu->curBackBufferIndex),inputSize)==VK_SUCCESS,"diagnostic initial mapped flush");
    }
    cmd->begin(); if(!dynamicSlot)cmd->updateBuffer(a,patternA.data(),64);cmd->updateBuffer(b,patternB.data(),64);
    cmd->customCommand([&](void *handle){
        if(!dynamicSlot)barrier(handle,a,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(handle,b,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    });
    cmd->end(); backend->getQueue()->submit(&cmd,1); raw.waitAllFences();
    std::printf("descriptor phase: upload complete\n");
    ShaderInfo shaderInfo;shaderInfo.name="descriptor-lifetime";
    shaderInfo.stages.push_back({ShaderStageFlagBit::COMPUTE,
        "layout(local_size_x=16) in;\n"
        "layout(set=0,binding=0,std430) readonly buffer Input { uint source[]; };\n"
        "layout(set=0,binding=1,std430) writeonly buffer Output { uint result[]; };\n"
        "void main(){uint i=gl_GlobalInvocationID.x; result[i]=source[i];}\n"});
    UniformStorageBuffer inputReflection;inputReflection.set=0;inputReflection.binding=0;inputReflection.name="Input";inputReflection.count=1;inputReflection.memoryAccess=MemoryAccessBit::READ_ONLY;
    UniformStorageBuffer outputReflection=inputReflection;outputReflection.binding=1;outputReflection.name="Output";outputReflection.memoryAccess=MemoryAccessBit::WRITE_ONLY;
    shaderInfo.buffers={inputReflection,outputReflection};
    Shader *shader=backend->createShader(shaderInfo);
    std::printf("descriptor phase: shader facade\n");
    DescriptorSetLayoutInfo layoutInfo;
    for(uint32_t i=0;i<2;++i){DescriptorSetLayoutBinding bind;bind.binding=i;bind.count=1;bind.descriptorType=(offsetCase && i==0) ? DescriptorType::DYNAMIC_STORAGE_BUFFER : DescriptorType::STORAGE_BUFFER;bind.stageFlags=ShaderStageFlagBit::COMPUTE;layoutInfo.bindings.push_back(bind);}
    DescriptorSetLayout *layout=backend->createDescriptorSetLayout(layoutInfo);
    std::printf("descriptor phase: set layout\n");
    PipelineLayoutInfo plInfo;plInfo.setLayouts={layout};PipelineLayout *pl=backend->createPipelineLayout(plInfo);
    PipelineStateInfo psInfo;psInfo.shader=shader;psInfo.pipelineLayout=pl;psInfo.bindPoint=PipelineBindPoint::COMPUTE;
    std::printf("descriptor phase: creating compute pipeline\n");
    PipelineState *ps=backend->createPipelineState(psInfo);
    std::printf("descriptor phase: compute pipeline ready\n");
    check(static_cast<CCVKPipelineState *>(ps)->gpuPipelineState()->vkPipeline!=VK_NULL_HANDLE,"real compute pipeline must exist");
    DescriptorSetInfo dsInfo;dsInfo.layout=layout;DescriptorSet *set=backend->createDescriptorSet(dsInfo);
    set->bindBuffer(0,inputView ? inputView : a);set->bindBuffer(1,output);set->update();
    std::printf("descriptor phase: descriptors ready\n");
    binds.clear();updated.clear();boundDynamicOffsets.clear();dispatches=0;attempted=forwarded=0;
    cmd->begin();original=lastBegin;
    cmd->bindPipelineState(ps);
    if(offsetCase) cmd->bindDescriptorSet(0,set,1,&extraOffset);
    else cmd->bindDescriptorSet(0,set);
    if(dynamicSlot)cmd->customCommand([&](void *handle){
        barrier(handle,a,VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    });
    DispatchInfo dispatch;dispatch.groupCountX=dispatch.groupCountY=dispatch.groupCountZ=1;cmd->dispatch(dispatch);
    cmd->customCommand([&](void *handle){
        barrier(handle,output,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferCopy region{0,0,64};vkCmdCopyBuffer(static_cast<VkCommandBuffer>(handle),vkBuffer(output),vkBuffer(host),1,&region);
        barrier(handle,host,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
    });
    if(name!="update-during-record")cmd->end();
    const auto bound=std::find_if(binds.begin(),binds.end(),[&](const auto &bind){return bind.cb==original;});
    check(original && bound!=binds.end() && dispatches==1,"original recording must actually bind and dispatch");
    const VkDescriptorSet oldSet=bound==binds.end()?VK_NULL_HANDLE:bound->set;
    if(offsetCase){
        check(boundDynamicOffsets.size()==1 && boundDynamicOffsets[0]==extraOffset,"actual nonzero dynamic offset must reach Vulkan binding");
        std::printf("dynamic offset: requested=%u bound=%u\n",extraOffset,boundDynamicOffsets.empty()?0:boundDynamicOffsets[0]);
    }
    const auto *testGPUSet=static_cast<CCVKDescriptorSet *>(set)->gpuDescriptorSet();
    check((testGPUSet->gpuLayout->vkDescriptorUpdateTemplate!=VK_NULL_HANDLE)==templateUpdates,"selected ordinary/template layout must agree with configuration");
    std::printf("descriptor update path: template=%d cachedTemplatePFN=%d\n",templateUpdates,templateUpdates);
    const auto slot=gpu->curBackBufferIndex;
    bool mutationObserved=false,sameSet=false;
    if(mutation){
        set->bindBuffer(0,b);set->update();
        const auto *gpuSet=static_cast<CCVKDescriptorSet *>(set)->gpuDescriptorSet();
        VkDescriptorSet destination=VK_NULL_HANDLE;
        if(templateUpdates){
            // The hub caches its template PFN before post-init hooks. Do not
            // claim a hook observation: verify version identity and GPU data.
            check(gpuSet->gpuLayout->vkDescriptorUpdateTemplate!=VK_NULL_HANDLE,"template layout must actually exist");
            destination=gpuSet->instances[slot].vkDescriptorSet;
            mutationObserved=destination!=VK_NULL_HANDLE;
            sameSet=destination==oldSet;
        }else{
            mutationObserved=!updated.empty();
            destination=updated.empty()?VK_NULL_HANDLE:updated.back();
            sameSet=std::find(updated.begin(),updated.end(),oldSet)!=updated.end();
        }
        check(mutationObserved && gpu->curBackBufferIndex==slot,"update must select a real descriptor target without slot rotation");
        std::printf("descriptor mutation: oldSet=%p lastWritten=%p sameSet=%d slotStable=%d\n",(void*)oldSet,(void*)destination,sameSet,gpu->curBackBufferIndex==slot);
    }
    if(name=="update-during-record"){
        cmd->customCommand([&](void *handle){
            barrier(handle,output,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        });
        // The same facade is still selected: version refresh must rebind it.
        cmd->dispatch(dispatch);
        cmd->customCommand([&](void *handle){
            barrier(handle,output,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferCopy region{0,0,64};vkCmdCopyBuffer(static_cast<VkCommandBuffer>(handle),vkBuffer(output),vkBuffer(secondHost),1,&region);
            barrier(handle,secondHost,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT);
        });
        const bool rebound=binds.size()==2 && binds.back().cb==original && binds.back().set!=oldSet;
        check(rebound,"same-facade second dispatch must bind the new descriptor version");
        std::printf("descriptor refresh: rebound=%d\n",rebound);
        cmd->end();
    }
    std::unique_ptr<PendingGate> gate;
    if(dynamicSlot) {
        auto *input=static_cast<CCVKBuffer *>(a)->gpuBuffer();
        raw.present();
        const auto submitSlot=gpu->curBackBufferIndex;
        check(submitSlot!=slot,"delayed recording must submit from another frame slot");
        a->update(patternB.data(),inputSize);
        check(vmaFlushAllocation(gpu->memoryAllocator,input->vmaAllocation,input->getStartOffset(submitSlot),inputSize)==VK_SUCCESS,"diagnostic new-instance mapped flush");
        gate=std::make_unique<PendingGate>(gpu->vkDevice);
        backend->getQueue()->submit(&cmd,1);
        auto *queue=static_cast<CCVKQueue *>(backend->getQueue())->gpuQueue();
        const auto point=queue->lastCompletion;
        check(gatedSubmits==1 && forwarded==1 && !point.empty(),"old CB must actually submit with the test gate");
        auto hasPoint=[&](uint32_t index){
            return index<gpu->slotCompletions.size() && std::any_of(gpu->slotCompletions[index].begin(),gpu->slotCompletions[index].end(),
                [&](const GpuCompletionPoint &p){return p.domain==point.domain && p.value==point.value;});
        };
        const auto untouchedSlot=(submitSlot+1)%gpu->backBufferCount;
        const bool recorded=hasPoint(slot),submitted=hasPoint(submitSlot),unrelated=hasPoint(untouchedSlot);
        check(recorded && submitted && !unrelated,"completion must cover used/submitting slots, excluding an unrelated slot");
        std::printf("dynamic points: recording=%d submitting=%d unrelated=%d\n",recorded,submitted,unrelated);
        const bool pending=!queue->completionTracker->isComplete(point);
        const auto completed=queue->completionTracker->completedValue();
        check(pending && completed<point.value,"real GPU point must be pending behind unsignaled gate");
        std::printf("dynamic gate: injected=%u pending=%d recordedSlot=%u submitSlot=%u point=%llu completed=%llu\n",gatedSubmits,pending,slot,submitSlot,static_cast<unsigned long long>(point.value),static_cast<unsigned long long>(completed));
        gate->releaseLater(); // also bounds a correct implementation's slot wait
        while(gpu->curBackBufferIndex!=slot)raw.present();
        backend->getQueue()->submit(nullptr,0); // real Hub flush, no test memcpy
        std::printf("dynamic reuse: originalSlot=%u currentSlot=%u byte=0x%02x gateReleased=%d\n",slot,gpu->curBackBufferIndex,input->mappedData[input->getStartOffset(slot)],gate->released.load());
        gate->releaser.join();
        check(gate->result.load()==VK_SUCCESS,"host must actually release the test timeline gate");
        std::printf("dynamic release: signaled=%d\n",gate->result.load()==VK_SUCCESS);
    } else backend->getQueue()->submit(&cmd,1);
    raw.waitAllFences();
    if(gate)gate->close();
    auto *mapped=static_cast<CCVKBuffer *>(host)->gpuBuffer();
    check(mapped->mappedData && mapped->instanceSize==0,"single mapped readback instance required");
    check(vmaInvalidateAllocation(gpu->memoryAllocator,mapped->vmaAllocation,0,64)==VK_SUCCESS,"invalidate after real completion");
    unsigned mismatches=0;for(unsigned i=0;i<64;++i)if(mapped->mappedData[i]!=0xa5)++mismatches;
    check(mismatches==0,"recorded input must yield original 64 bytes 0xA5");
    check(attempted==1 && forwarded==1,"original recording must be actually successfully submitted once");
    std::printf("descriptor execution: bound=%d dispatches=%u attempted=%u forwarded=%u\n",oldSet!=VK_NULL_HANDLE,dispatches,attempted,forwarded);
    std::printf("descriptor data: mismatches=%u/64 first=0x%02x\n",mismatches,mapped->mappedData[0]);
    if(mutation)check(!sameSet,"recorded descriptor instance must not be mutated in place");
    if(secondHost){
        auto *second=static_cast<CCVKBuffer *>(secondHost)->gpuBuffer();
        check(vmaInvalidateAllocation(gpu->memoryAllocator,second->vmaAllocation,0,64)==VK_SUCCESS,"second readback invalidate after completion");
        unsigned secondMismatches=0;for(unsigned i=0;i<64;++i)if(second->mappedData[i]!=0x5a)++secondMismatches;
        check(secondMismatches==0 && dispatches==2,"updated same-facade dispatch must read all 64 new bytes");
        std::printf("descriptor second data: mismatches=%u/64 first=0x%02x\n",secondMismatches,second->mappedData[0]);
    }
    const bool modern=gpu->runtimePaths.useSubmit2;
    check(modern ? (modernSubmitCalls>0 && legacySubmitCalls==0) : (legacySubmitCalls>0 && modernSubmitCalls==0), "actual descriptor submit dispatch matches policy");
    std::printf("descriptor encoder: submit=%s legacyCalls=%u submit2Calls=%u\n",modern?"submit2":"legacy",legacySubmitCalls,modernSubmitCalls);
    const auto liveErrors=errors()-initialErrors;
    destroy(cmd); destroy(set);destroy(ps);destroy(pl);destroy(layout);destroy(shader);
    destroy(secondHost);destroy(host);destroy(output);destroy(b);destroy(inputView);destroy(a);original=VK_NULL_HANDLE;
    device->destroy();const auto postErrors=raw.getLastValidationErrorCount();
    std::printf("%s completed: failures=%u liveErrors=%u postErrors=%u outcome=%s\n",name.c_str(),failures,liveErrors,postErrors,name=="control"?"CLEAN":(dynamicSlot && mismatches ? "OVERWRITTEN":"PRESERVED"));
    return failures||liveErrors||postErrors?1:0;
}
