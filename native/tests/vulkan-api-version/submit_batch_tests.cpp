#include "VKCompletionTracker.h"
#include <cstdio>
#include <type_traits>

using namespace cc::gfx;
namespace {
unsigned failures=0;
void check(bool ok,const char *text) { if (!ok) {++failures;std::fprintf(stderr,"FAIL: %s\n",text);} }
template<class T>T handle(uintptr_t v) {return (T)(void*)v;}
struct State {
    unsigned legacy=0,modern=0,replaced=0,statusCalls=0;
    VkResult result=VK_SUCCESS,status=VK_NOT_READY;
    uint64_t counter=0;
    VkQueue queue=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    VkSubmitFlags flags=0;
    std::vector<VkSemaphoreSubmitInfo> waits,signals;
    std::vector<VkCommandBufferSubmitInfo> commands;
} *state;
VKAPI_ATTR VkResult VKAPI_CALL submit1(VkQueue,uint32_t,const VkSubmitInfo*,VkFence) {++state->legacy;return state->result;}
VKAPI_ATTR VkResult VKAPI_CALL submit2(VkQueue q,uint32_t n,const VkSubmitInfo2 *s,VkFence f) {
    ++state->modern; state->queue=q;state->fence=f;
    check(n==1 && s && s->sType==VK_STRUCTURE_TYPE_SUBMIT_INFO_2 && !s->pNext,"one canonical Submit2 batch");
    state->flags=s->flags;
    state->waits.clear();if(s->waitSemaphoreInfoCount)state->waits.assign(s->pWaitSemaphoreInfos,s->pWaitSemaphoreInfos+s->waitSemaphoreInfoCount);
    state->signals.clear();if(s->signalSemaphoreInfoCount)state->signals.assign(s->pSignalSemaphoreInfos,s->pSignalSemaphoreInfos+s->signalSemaphoreInfoCount);
    state->commands.clear();if(s->commandBufferInfoCount)state->commands.assign(s->pCommandBufferInfos,s->pCommandBufferInfos+s->commandBufferInfoCount);
    return state->result;
}
VKAPI_ATTR VkResult VKAPI_CALL replacement(VkQueue,uint32_t,const VkSubmitInfo2*,VkFence) {++state->replaced;return VK_ERROR_OUT_OF_HOST_MEMORY;}
VKAPI_ATTR VkResult VKAPI_CALL status(VkDevice,VkFence) {++state->statusCalls;return state->status;}
VKAPI_ATTR VkResult VKAPI_CALL counter(VkDevice,VkSemaphore,uint64_t *out) {*out=state->counter;return VK_SUCCESS;}
struct Fixture {
    State data;
    PFN_vkQueueSubmit legacy=&submit1;
    PFN_vkQueueSubmit2 modern=&submit2;
    PFN_vkGetFenceStatus getStatus=&status;
    PFN_vkGetSemaphoreCounterValue getCounter=&counter;
    PFN_vkWaitSemaphores waitSem=nullptr;
    VkDevice device=handle<VkDevice>(1);VkQueue queue=handle<VkQueue>(2);
    VkFence fence=handle<VkFence>(3);VkSemaphore timeline=handle<VkSemaphore>(4);
    GpuCompletionTracker tracker;
    Fixture():tracker(device,queue,{&legacy,&getStatus,nullptr,&getCounter,&waitSem,&modern}) {state=&data;}
    static VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice,const VkSemaphoreWaitInfo*,uint64_t) {return VK_TIMEOUT;}
    void enable(bool tl) {
        check(tracker.configureSubmit2()==VK_SUCCESS,"select Submit2");
        if (tl) {waitSem=&wait;check(tracker.configureTimeline(timeline,UINT64_MAX)==VK_SUCCESS,"select timeline completion independently");}
    }
    VulkanSubmitBatch batch() const {
        VulkanSubmitBatch b;
        b.commands={handle<VkCommandBuffer>(10),handle<VkCommandBuffer>(11)};
        b.waits={{handle<VkSemaphore>(20),VK_PIPELINE_STAGE_2_TRANSFER_BIT},{handle<VkSemaphore>(21),VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,7,true}};
        b.signals={{handle<VkSemaphore>(30)}};
        return b;
    }
};
}
int main() {
    static_assert(!std::is_copy_constructible<VulkanSubmitEncoding>::value && !std::is_move_constructible<VulkanSubmitEncoding>::value,"owning pointer state cannot move/copy");
    { // Same model; binary values zero, external timeline value preserved.
        Fixture f;auto b=f.batch();VulkanSubmitEncoding e;
        check(e.build(b,false),"legacy encoding");auto s=e.legacy();
        auto *v=static_cast<const VkTimelineSemaphoreSubmitInfo*>(s.pNext);
        check(v && v->waitSemaphoreValueCount==2 && v->pWaitSemaphoreValues[0]==0 && v->pWaitSemaphoreValues[1]==7 && v->pSignalSemaphoreValues[0]==0,"legacy positional binary/timeline values");
        check(s.pWaitDstStageMask[0]==VK_PIPELINE_STAGE_TRANSFER_BIT && s.pCommandBuffers[1]==b.commands[1],"legacy stage/order preserved");
        check(e.build(b,true),"modern encoding");b.commands.clear();b.waits.clear();b.signals.clear();
        const auto &m=e.modern();
        check(m.commandBufferInfoCount==2 && m.pCommandBufferInfos[0].deviceMask==1 && m.pCommandBufferInfos[1].commandBuffer==handle<VkCommandBuffer>(11),"owned command array independent of caller mutation");
        check(m.pWaitSemaphoreInfos[0].value==0 && m.pWaitSemaphoreInfos[1].value==7 && m.pWaitSemaphoreInfos[1].stageMask==VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT && m.pWaitSemaphoreInfos[1].deviceIndex==0,"modern wait fields");
        check(m.pSignalSemaphoreInfos[0].value==0 && m.pSignalSemaphoreInfos[0].stageMask==VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT && !m.flags,"modern full signal scope and ordinary flags");
    }
    { // Reject lossy legacy narrowing and unknown pNext; rebuilding clears old data.
        Fixture f;auto b=f.batch();VulkanSubmitEncoding e;
        b.waits[0].stages=VK_PIPELINE_STAGE_2_COPY_BIT;
        check(!e.build(b,false) && e.build(b,true),"64-bit-only stage is not silently truncated");
        VkProtectedSubmitInfo next{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};b.legacyNext=&next;
        check(!e.build(b,true) && e.modern().commandBufferInfoCount==0,"unmodelled Submit2 pNext rejected, old encoding cleared");
        b.waits[0].stages=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        check(e.build(b,false) && static_cast<const VkTimelineSemaphoreSubmitInfo*>(e.legacy().pNext)->pNext==&next,"legacy compatibility chain preserved");
        b.signals[0].stages=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        check(!e.build(b,true),"completion/signal narrowing prohibited in semantic migration");
    }
    { // Fence completion, exactly one modern dispatch, no fallback or point loss.
        Fixture f;f.enable(false);auto b=f.batch();GpuCompletionPoint p{999,999};
        check(f.tracker.submit(b,f.fence,p)==VK_SUCCESS && p.value==1 && f.data.modern==1 && f.data.legacy==0,"fence Submit2 point and one driver call");
        check(f.data.queue==f.queue && f.data.fence==f.fence && f.data.signals.size()==1,"queue/fence/binary-only signals preserved");
        check(f.tracker.submit(b,f.fence,p)!=VK_SUCCESS && p.empty() && f.data.modern==1,"pending fence reuse refused before driver");
        f.data.status=VK_SUCCESS;check(f.tracker.observeFenceCompletion(f.fence)==VK_SUCCESS,"fence actual completion");
        check(f.tracker.submit(b,f.fence,p)==VK_SUCCESS && p.value==2,"reused handle never resurrects old point");
        check(f.tracker.configureSubmit2()!=VK_SUCCESS && f.tracker.usesSubmit2(),"encoding configuration cannot change after submissions");
    }
    { // Completion timeline appended in THIS batch, full scope, no extra submit.
        Fixture f;f.enable(true);auto b=f.batch();GpuCompletionPoint p;
        check(f.tracker.submit(b,VK_NULL_HANDLE,p)==VK_SUCCESS && p.value==1 && f.data.modern==1 && f.data.legacy==0,"timeline Submit2 one batch");
        check(f.data.signals.size()==2 && f.data.signals[0].semaphore==b.signals[0].semaphore && f.data.signals[0].value==0 &&
              f.data.signals[1].semaphore==f.timeline && f.data.signals[1].value==p.value && f.data.signals[1].stageMask==VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,"binary present and full-completion timeline signal alignment");
        check(b.signals.size()==1 && f.data.commands.size()==2 && f.data.waits[1].value==7,"caller topology remains immutable");
        f.data.counter=1;check(f.tracker.isComplete(p),"timeline observation completes modern batch");
    }
    for (bool tl:{false,true}) {
        Fixture f;f.enable(tl);auto b=f.batch();GpuCompletionPoint p{999,999};f.data.result=VK_ERROR_OUT_OF_DEVICE_MEMORY;
        check(f.tracker.submit(b,tl?VK_NULL_HANDLE:f.fence,p)==VK_ERROR_OUT_OF_DEVICE_MEMORY && p.empty() && f.tracker.submittedValue()==0,"failed Submit2 publishes no point or sequence");
        f.data.result=VK_SUCCESS;
        check(f.tracker.submit(b,tl?VK_NULL_HANDLE:f.fence,p)==VK_SUCCESS && p.value==1 && f.data.modern==2,"failure rolls back borrowed fence state and consecutive retry");
    }
    { // Invalid model must not query a fence/counter or reach the driver.
        Fixture f;f.enable(true);auto b=f.batch();GpuCompletionPoint p;
        b.signals[0].value=1;check(f.tracker.submit(b,VK_NULL_HANDLE,p)!=VK_SUCCESS,"nonzero binary value rejected");
        b=f.batch();b.waits[0].semaphore=f.timeline;check(f.tracker.submit(b,VK_NULL_HANDLE,p)!=VK_SUCCESS,"tracker self-wait rejected");
        b=f.batch();b.signals[0].semaphore=f.timeline;check(f.tracker.submit(b,VK_NULL_HANDLE,p)!=VK_SUCCESS,"tracker self-signal rejected");
        b=f.batch();VkProtectedSubmitInfo unknown{VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO};b.legacyNext=&unknown;
        check(f.tracker.submit(b,VK_NULL_HANDLE,p)!=VK_SUCCESS && f.data.modern==0 && f.data.statusCalls==0,"unmodelled extensions rejected before any submission");
    }
    { // PFN addresses stay live after configuration, absent modern dispatch fails.
        Fixture f;f.enable(false);auto b=f.batch();GpuCompletionPoint p;
        f.modern=&replacement;
        check(f.tracker.submit(b,f.fence,p)==VK_ERROR_OUT_OF_HOST_MEMORY && f.data.replaced==1 && f.data.modern==0,"late PFN replacement observed");
        f.modern=nullptr;
        check(f.tracker.submit(b,f.fence,p)!=VK_SUCCESS && f.data.legacy==0 && p.empty(),"missing Submit2 entry never silently falls back");
    }
    for (bool tl:{false,true}) {
        Fixture f;f.enable(tl);auto b=f.batch();GpuCompletionPoint p;
        f.data.result=VK_ERROR_DEVICE_LOST;
        check(f.tracker.submit(b,tl?VK_NULL_HANDLE:f.fence,p)==VK_ERROR_DEVICE_LOST,"modern device loss");
        f.data.result=VK_SUCCESS;
        check(f.tracker.submit(b,tl?VK_NULL_HANDLE:f.fence,p)==VK_ERROR_DEVICE_LOST && f.data.modern==1 && p.empty(),"device loss never resurrects modern backend");
        check(f.tracker.configureSubmit2()==VK_ERROR_DEVICE_LOST,"configuration preserves terminal loss");
    }
    { // A completed timeline counter cannot prove a borrowed fence has signaled.
        Fixture f;f.enable(true);auto b=f.batch();GpuCompletionPoint p;
        check(f.tracker.submit(b,f.fence,p)==VK_SUCCESS,"borrowed fence modern timeline batch");
        f.data.counter=1;check(f.tracker.isComplete(p),"timeline complete");
        check(f.tracker.submit(b,f.fence,p)==VK_NOT_READY && f.data.modern==1,"actual fence signal still required before reuse");
        f.data.status=VK_SUCCESS;
        check(f.tracker.submit(b,f.fence,p)==VK_SUCCESS && p.value==2 && f.data.modern==2,"actual borrowed fence observation permits reuse");
    }
    std::printf("Submit batch contracts: failures=%u (mock PFNs; no driver/Validation evidence)\n",failures);
    return failures?1:0;
}
