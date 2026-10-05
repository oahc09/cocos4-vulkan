#include "VKBarrierEncoding.h"
#include <cstdio>
#include <cstring>
#include <type_traits>
using namespace cc::gfx;
namespace {
unsigned failures=0;
void check(bool condition,const char *reason){if(!condition){++failures;std::fprintf(stderr,"FAIL: %s\n",reason);}}
template<class T>T handle(uintptr_t value){return reinterpret_cast<T>(value);}
}
int main(){
    static_assert(!std::is_copy_constructible<VulkanBarrierEncoding>::value && !std::is_move_constructible<VulkanBarrierEncoding>::value,"pointer-owning encoder cannot copy/move");
    VulkanBarrierEncoding e;
    // Empty arrays in legacy STILL create an execution dependency.
    check(e.build(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,0,nullptr),"execution-only build");
    const auto &exec=e.info();
    check(exec.memoryBarrierCount==1 && exec.bufferMemoryBarrierCount==0 && exec.imageMemoryBarrierCount==0 &&
          exec.pMemoryBarriers[0].srcStageMask==VK_PIPELINE_STAGE_2_TRANSFER_BIT &&
          exec.pMemoryBarriers[0].dstStageMask==VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
          !exec.pMemoryBarriers[0].srcAccessMask && !exec.pMemoryBarriers[0].dstAccessMask,"execution-only scopes cannot silently disappear");
    VkMemoryBarrier m[2]={{VK_STRUCTURE_TYPE_MEMORY_BARRIER},{VK_STRUCTURE_TYPE_MEMORY_BARRIER}};
    m[0].srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; m[0].dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    m[1].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; m[1].dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    VkBufferMemoryBarrier b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    b.srcQueueFamilyIndex=3;b.dstQueueFamilyIndex=5;b.buffer=handle<VkBuffer>(10);b.offset=256;b.size=VK_WHOLE_SIZE;
    VkImageMemoryBarrier i{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    i.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;i.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    i.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;i.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    i.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;i.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    i.image=handle<VkImage>(20);i.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,2,VK_REMAINING_MIP_LEVELS,4,VK_REMAINING_ARRAY_LAYERS};
    const auto src=VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const auto dst=VK_PIPELINE_STAGE_VERTEX_INPUT_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_HOST_BIT;
    const auto flags=VK_DEPENDENCY_BY_REGION_BIT|VK_DEPENDENCY_VIEW_LOCAL_BIT;
    check(e.build(src,dst,flags,2,m,1,&b,1,&i),"mixed dependency build");
    const auto &info=e.info();
    check(info.sType==VK_STRUCTURE_TYPE_DEPENDENCY_INFO && !info.pNext && info.dependencyFlags==flags && info.memoryBarrierCount==2 && info.bufferMemoryBarrierCount==1 && info.imageMemoryBarrierCount==1,"topology and flags preserved");
    check(info.pMemoryBarriers[0].srcAccessMask==m[0].srcAccessMask && info.pMemoryBarriers[1].dstAccessMask==m[1].dstAccessMask,"memory access and order preserved");
    const auto &buffer=info.pBufferMemoryBarriers[0];
    check(buffer.srcStageMask==src && buffer.dstStageMask==dst && buffer.srcAccessMask==b.srcAccessMask && buffer.dstAccessMask==b.dstAccessMask && buffer.srcQueueFamilyIndex==3 && buffer.dstQueueFamilyIndex==5 && buffer.buffer==b.buffer && buffer.offset==256 && buffer.size==VK_WHOLE_SIZE,"buffer scopes, ownership transfer and range preserved");
    const auto &image=info.pImageMemoryBarriers[0];
    check(image.srcStageMask==src && image.dstStageMask==dst && image.srcAccessMask==i.srcAccessMask && image.dstAccessMask==i.dstAccessMask && image.oldLayout==i.oldLayout && image.newLayout==i.newLayout && image.image==i.image && image.srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED && image.dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED && std::memcmp(&image.subresourceRange,&i.subresourceRange,sizeof(i.subresourceRange))==0,"image stage/access/layout/aspects/mip/layer sentinels preserved");
    b.offset=0;i.image=VK_NULL_HANDLE;m[0].srcAccessMask=0;
    check(buffer.offset==256 && image.image==handle<VkImage>(20) && info.pMemoryBarriers[0].srcAccessMask==VK_ACCESS_TRANSFER_WRITE_BIT,"encoded pointers own data independent of original arrays");
    // Do not optimize TOP/BOTTOM or broaden/split ambiguous stage/access flags.
    check(e.build(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,0,nullptr) && e.info().pMemoryBarriers[0].srcStageMask==VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT && e.info().pMemoryBarriers[0].dstStageMask==VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,"deprecated aliases retained for semantic port");
    VkBaseInStructure unknown{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr};m[0].pNext=&unknown;
    check(!e.build(src,dst,0,1,m,0,nullptr,0,nullptr) && e.info().memoryBarrierCount==0 && e.info().imageMemoryBarrierCount==0,"unmodelled memory extension rejected and old pointer state cleared");
    m[0].pNext=nullptr;b.pNext=&unknown;
    check(!e.build(src,dst,0,0,nullptr,1,&b,0,nullptr),"buffer extension rejected");b.pNext=nullptr;i.pNext=&unknown;
    check(!e.build(src,dst,0,0,nullptr,0,nullptr,1,&i),"image extension rejected");i.pNext=nullptr;
    check(!e.build(src,dst,0,1,nullptr,0,nullptr,0,nullptr) && !e.build(src,dst,0,0,nullptr,1,nullptr,0,nullptr) && !e.build(src,dst,0,0,nullptr,0,nullptr,1,nullptr),"nonzero count with absent array rejected");
    m[0].sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
    check(!e.build(src,dst,0,1,m,0,nullptr,0,nullptr),"wrong node type rejected");
    check(e.build(0,0,0,0,nullptr,0,nullptr,0,nullptr) && e.info().memoryBarrierCount==1 && !e.info().pMemoryBarriers[0].srcStageMask && !e.info().pMemoryBarriers[0].dstStageMask,"empty scope rebuild has no stale fields");
    std::printf("Barrier encoder contracts: failures=%u (pure CPU; no driver/Validation)\n",failures);
    return failures?1:0;
}
