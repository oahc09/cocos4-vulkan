# Cocos Vulkan Backend 1.1 Baseline → 1.3 Modernization 任务计划书 v1.1

> 仓库：`https://github.com/oahc09/cocos4-vulkan`  
> 目标分支：`improvement/vulkan-performance-tuning`  
> 设计参考：Filament Vulkan Backend  
> 核心策略：**Vulkan 1.1 作为硬最低基线；删除 Vulkan 1.0 兼容路径；Vulkan 1.2 / 1.3 按实际 Capability 渐进启用；Classic RenderPass 始终保留为兼容路径。**  
> 本版本根据源码复核与 Vulkan 规范复核重新调整阶段依赖、同步生命周期和 Dynamic Rendering 接入契约。  
> 状态：实施计划，不代表任何阶段已经完成。

---

# 0. 最终结论

本项目不定义为“将 Cocos Vulkan 版本号升级到 1.3”，而定义为：

> **Cocos Vulkan Backend Modernization：采用 Filament 风格的 Vulkan 1.1 明确 Baseline，清除 1.0 历史兼容债；在此基础上建立严格的版本协商和 Capability 模型，再分别引入 Vulkan 1.2/1.3 的现代能力。**

核心架构：

```text
Vulkan Backend
│
├── Required Baseline
│   └── Vulkan 1.1
│
├── Capability Negotiation
│   ├── supported
│   ├── requested
│   ├── enabled
│   └── runtime path
│
├── Baseline Path
│   ├── Classic RenderPass
│   ├── vkQueueSubmit
│   ├── legacy barriers
│   └── WSI binary semaphores
│
├── Enhanced Path — Vulkan 1.2
│   ├── Timeline Semaphore
│   ├── Separate Depth/Stencil Layouts
│   ├── Descriptor Indexing capability
│   └── Buffer Device Address capability
│
└── Modern Path — Vulkan 1.3
    ├── Synchronization2
    ├── vkQueueSubmit2
    ├── Dynamic Rendering
    └── Maintenance4
```

实施原则固定为：

```text
先稳定版本/能力模型
→ 再闭环 WSI 生命周期
→ 再迁移 Timeline/资源回收
→ 再迁移 Sync2/Submit2
→ 再准备 Rendering Contract
→ 最后启用 Dynamic Rendering
→ 最后做缓存/预热/性能收口
```

---

# 1. 当前源码事实基线

## 1.1 当前已有多帧槽轮转

当前 `VKDevice::present()` 的逻辑本质是：

```text
present
→ curBackBufferIndex 切换到下一帧槽
→ 等待“即将复用槽”的 fence pool
→ reset / recycle
```

因此：

> 当前实现不是“每帧等待全部在途帧完成”，已经具备基于 back-buffer slot 的多帧轮转。

所以 Timeline Semaphore 的目标修正为：

```text
错误目标：
“消灭每帧全局等待，让 Frame-in-flight 首次真正并行”

正确目标：
“统一 GPU submission completion progress，
降低 Fence Pool 管理与 Host Wait 成本，
把资源回收条件收敛为明确的完成序号。”
```

是否有效必须实测：

```text
vkWaitForFences 调用次数
CPU wait duration
Render Thread CPU
P50 / P95 / P99
GPU overlap
frames-in-flight occupancy
```

---

## 1.2 当前版本协商

当前已有：

```text
volkInitialize
→ vkEnumerateInstanceVersion
→ XR runtime version adjustment
→ VkApplicationInfo.apiVersion
```

同时仍保留 Vulkan 1.0：

```text
VK_KHR_get_physical_device_properties2
vkGetPhysicalDeviceFeatures2KHR
vkGetPhysicalDeviceProperties2KHR
```

这些属于本次删除目标。

---

## 1.3 Vulkan 1.1 Feature Chain 不能使用错误聚合前提

必须明确：

> `VkPhysicalDeviceVulkan11Features` 是由 Vulkan 1.2 提供的聚合结构。

所以不能因为名字里有 `11`，就在纯 Vulkan 1.1 路径默认挂：

```text
VkPhysicalDeviceVulkan11Features
```

Vulkan 1.1 路径要使用实际需要的独立 feature structure；Vulkan 1.2+ 才可使用 Vulkan11/Vulkan12 聚合结构。

同时必须保证：

> 同一组 promoted feature 的 aggregate structure 与独立 structure 不重复进入 enable chain。

参考：
`https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceVulkan11Features.html`

---

## 1.4 WSI 生命周期是同步迁移前置

必须先闭环：

```text
VK_ERROR_OUT_OF_DATE_KHR
VK_SUBOPTIMAL_KHR
VK_ERROR_SURFACE_LOST_KHR
zero-size surface
surface destroy / recreate
failed acquire 后跳帧
```

不能在这些异常路径尚未闭环时扩大 Timeline / Submit2 / Dynamic Rendering 的同步范围。

---

## 1.5 Present Semaphore 生命周期必须与 Submission Completion 解耦

必须明确：

```text
GPU submission completion
≠
presentation engine 已消费完 present wait semaphore
```

因此：

```text
Fence/timeline signaled
```

不能单独证明：

```text
VkPresentInfoKHR::pWaitSemaphores
```

对应的 binary semaphore 已安全可复用。

基础安全方案：

```text
render-complete binary semaphore
按 swapchain image 关联
```

当该 image 再次 acquire，并且 acquire synchronization 正确进入等待链后，再复用该 image 对应的 render-complete semaphore。

参考：
`https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html`

---

## 1.6 Dynamic Rendering 不是 Begin/End 替换

当前 Classic RenderPass 负责表达：

```text
initial/final layout
subpass dependency
load/store
resolve
depth/stencil
input attachment
preserve attachment
fragment shading rate attachment
secondary CB inheritance
```

Dynamic Rendering 后，其中一部分必须显式化。

因此启用 Dynamic Rendering 前必须先完成：

```text
Rendering Signature
Pipeline Contract
pNext composition
Secondary CB inheritance contract
Explicit layout/dependency contract
Load/store/resolve contract
```

---

# 2. 目标与非目标

## 2.1 目标

### API 基线

```text
Vulkan 1.0 → 不支持
Vulkan 1.1 → Required Baseline
Vulkan 1.2 → Optional Enhanced
Vulkan 1.3 → Optional Modern
Vulkan >1.3 → 引擎最高 contract 仍限制到 1.3
```

### 删除 Vulkan 1.0 compatibility debt

包括：

```text
旧 properties2 KHR fallback
旧 memory requirements KHR fallback
旧 descriptor update template KHR fallback
只为 Vulkan 1.0 存在的 Core/KHR 双路径
```

### 建立严格 Capability 状态

每项能力至少区分：

```text
supported
requested
enabled
runtimePathSelected
```

### Vulkan 1.2

重点：

```text
Timeline Semaphore
Separate Depth/Stencil Layouts
Descriptor Indexing capability
Buffer Device Address capability
```

### Vulkan 1.3

重点：

```text
Synchronization2
QueueSubmit2
Dynamic Rendering
Maintenance4
```

### Robustness

在同步迁移前完成：

```text
Swapchain
Surface
Acquire
Present
WSI binary semaphore lifetime
```

### Correctness

要求同一设备支持强制关闭单项能力，以验证 fallback。

---

## 2.2 非目标

本轮不直接实现：

```text
Vulkan 1.4 Backend
dynamicRenderingLocalRead
Ray Tracing
Mesh Shader
DGC
Descriptor Heap
全面 Bindless
全面 Async Compute
删除 Classic RenderPass
强制所有 Pass Dynamic Rendering
```

---

# 3. 最小版本模型

版本模型必须先于 Vulkan 1.0 删除逻辑落地。

```cpp
struct VulkanApiVersionInfo {
    uint32_t loaderApiVersion{VK_API_VERSION_1_0};
    uint32_t engineRequestedApiVersion{VK_API_VERSION_1_1};
    uint32_t xrConstrainedApiVersion{VK_API_VERSION_1_1};
    uint32_t instanceApiVersion{VK_API_VERSION_1_1};
    uint32_t physicalDeviceApiVersion{VK_API_VERSION_1_0};
    uint32_t effectiveDeviceApiVersion{VK_API_VERSION_1_0};
};
```

固定：

```cpp
constexpr uint32_t BACKEND_BASELINE_API_VERSION = VK_API_VERSION_1_1;
constexpr uint32_t ENGINE_MAX_API_VERSION       = VK_API_VERSION_1_3;
```

---

## 3.1 Loader Query

错误示例：

```cpp
loaderApiVersion = vkEnumerateInstanceVersion();
```

正确逻辑：

```cpp
uint32_t loaderApiVersion = VK_API_VERSION_1_0;

if (vkEnumerateInstanceVersion) {
    uint32_t reportedVersion = VK_API_VERSION_1_0;
    VkResult result = vkEnumerateInstanceVersion(&reportedVersion);
    if (result == VK_SUCCESS) {
        loaderApiVersion = reportedVersion;
    } else {
        // initialization failure
    }
}
```

函数不存在：

```text
Loader = Vulkan 1.0
→ 不满足新 Baseline
→ Vulkan Backend unavailable
```

参考：
`https://docs.vulkan.org/refpages/latest/refpages/source/vkEnumerateInstanceVersion.html`

---

## 3.2 Engine Requested API

```cpp
engineRequestedApiVersion =
    std::min(loaderApiVersion, ENGINE_MAX_API_VERSION);
```

结果：

```text
Loader 1.1 → 1.1
Loader 1.2 → 1.2
Loader 1.3 → 1.3
Loader 1.4 → 1.3
未来版本  → 1.3
```

禁止未来 Driver/Loader 升级后，引擎自动宣称自己是未经验证的 Vulkan 1.4/1.5 application。

---

## 3.3 XR Constraint

必须保留当前：

```text
xr->getXRVkApiVersion(...)
xr->createXRVulkanInstance(...)
xr->createXRVulkanDevice(...)
```

协商顺序：

```text
Loader Version
    ↓
Engine Max = 1.3
    ↓
XR runtime constraint
    ↓
Instance API
```

如果 XR runtime 约束后：

```text
< Vulkan 1.1
```

则直接不满足 Vulkan Backend Baseline，不允许静默回退 Vulkan 1.0。

---

## 3.4 Physical Device

```cpp
physicalDeviceApiVersion =
    physicalDeviceProperties.apiVersion;
```

要求：

```text
physicalDeviceApiVersion >= Vulkan 1.1
```

否则 reject。

---

## 3.5 Effective API

```cpp
effectiveDeviceApiVersion =
    std::min(instanceApiVersion, physicalDeviceApiVersion);
```

所有 Core Feature 判断统一基于：

```text
effectiveDeviceApiVersion
```

---

# 4. Capability 数据模型

复杂 feature family 不能用单个 bool。

基础：

```cpp
struct VulkanCapabilityState {
    bool supported{false};
    bool requested{false};
    bool enabled{false};
};
```

Descriptor Indexing 单独展开：

```cpp
struct VulkanDescriptorIndexingCaps {
    VulkanCapabilityState shaderUniformBufferArrayNonUniformIndexing;
    VulkanCapabilityState shaderSampledImageArrayNonUniformIndexing;
    VulkanCapabilityState shaderStorageBufferArrayNonUniformIndexing;
    VulkanCapabilityState shaderStorageImageArrayNonUniformIndexing;

    VulkanCapabilityState descriptorBindingUniformBufferUpdateAfterBind;
    VulkanCapabilityState descriptorBindingSampledImageUpdateAfterBind;
    VulkanCapabilityState descriptorBindingStorageImageUpdateAfterBind;
    VulkanCapabilityState descriptorBindingStorageBufferUpdateAfterBind;
    VulkanCapabilityState descriptorBindingPartiallyBound;
    VulkanCapabilityState descriptorBindingVariableDescriptorCount;
    VulkanCapabilityState runtimeDescriptorArray;
};
```

总体：

```cpp
struct GfxVKCapabilities {
    VulkanApiVersionInfo version;

    VulkanCapabilityState timelineSemaphore;
    VulkanCapabilityState bufferDeviceAddress;
    VulkanCapabilityState separateDepthStencilLayouts;

    VulkanDescriptorIndexingCaps descriptorIndexing;

    VulkanCapabilityState synchronization2;
    VulkanCapabilityState dynamicRendering;
    VulkanCapabilityState maintenance4;

    VulkanCapabilityState fragmentShadingRate;
    VulkanCapabilityState memoryBudget;
    VulkanCapabilityState graphicsPipelineLibrary;

    bool debugUtils{false};
    bool portabilitySubset{false};

    VkDeviceSize maxBufferSize{0};
};
```

运行路径单独保存：

```cpp
struct VulkanRuntimePaths {
    bool useTimelineSemaphore{false};
    bool useSynchronization2{false};
    bool useSubmit2{false};
    bool useDynamicRendering{false};
    bool forceClassicRenderPass{false};
};
```

这样允许：

```text
enabled == true
但 runtime path 临时关闭
```

用于回退和 A/B。

---

# 5. Feature Query / Enable Chain

## 5.1 Vulkan 1.1 Path

当：

```text
effectiveDeviceApiVersion == Vulkan 1.1
```

使用：

```text
VkPhysicalDeviceFeatures2
+
实际需要的独立 Vulkan 1.1 feature structures
```

例如按需：

```text
VkPhysicalDeviceMultiviewFeatures
VkPhysicalDevice16BitStorageFeatures
VkPhysicalDeviceSamplerYcbcrConversionFeatures
VkPhysicalDeviceShaderDrawParametersFeatures
...
```

不为了“完整”挂所有 structure。

---

## 5.2 Vulkan 1.2+ Path

当：

```text
effectiveDeviceApiVersion >= Vulkan 1.2
```

可使用：

```text
VkPhysicalDeviceVulkan11Features
VkPhysicalDeviceVulkan12Features
```

当：

```text
effectiveDeviceApiVersion >= Vulkan 1.3
```

再加入：

```text
VkPhysicalDeviceVulkan13Features
```

---

## 5.3 禁止 Aggregate / Independent 重复

Feature Chain Builder 必须保证：

```text
VkPhysicalDeviceVulkan11Features
```

与其对应的独立 promoted feature structure 不同时进入同一 enable chain。

参考：
`https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceCreateInfo.html`

---

## 5.4 状态推进

严格：

```text
Query Supported
→ Engine Policy produces Requested
→ Build Enable Chain
→ vkCreateDevice succeeds
→ Enabled finalized
→ Runtime Path selected
```

不能把：

```text
supported
```

当成：

```text
enabled
```

---

# 6. Dependency Lock

M0 必须新增：

```text
ai-docs/to-do/VULKAN_DEPENDENCY_LOCK.md
```

记录：

| Dependency | Selected version/tag/commit | Source | Platform artifact | Reason |
|---|---|---|---|---|
| Vulkan-Headers | exact | Khronos | source | API |
| Volk | exact/generated | zeux/volk | source | dispatch |
| VMA | exact | GPUOpen | source | allocator |
| glslang | exact | Khronos | native libs/source | shader |
| SPIRV-Tools | exact | Khronos | native libs/source | validate |
| Validation Layers | exact SDK | Khronos/LunarG | dev only | validation |

原则：

```text
不写“升级到最新”
而写“升级到已验证并锁定的精确版本”
```

当前 SPIR-V 1.6 路径必须通过真实编译验证：

```text
effective 1.1 → expected target
effective 1.2 → Vulkan 1.2 / SPIR-V 1.5
effective 1.3 → Vulkan 1.3 / SPIR-V 1.6
```

并执行：

```text
spirv-val --target-env vulkan1.1
spirv-val --target-env vulkan1.2
spirv-val --target-env vulkan1.3
```

---

# 7. 最终实施顺序

```text
M0  依赖升级 + 最小版本模型 + Validation 基线
 ↓
M1  Vulkan 1.1 Baseline + 完整 Capability 协商
 ↓
M2  Swapchain / WSI 生命周期闭环
 ↓
M3  Timeline Semaphore + 资源回收成套迁移
 ↓
M4  Synchronization2 / Submit2
    M4-A：语义等价
    M4-B：同步范围优化
 ↓
M5  Rendering Signature + Pipeline / Secondary CB 适配
 ↓
M6  Hybrid Dynamic Rendering
 ↓
M7  Pipeline Cache / Prewarm / Performance & Platform Gate
```

---

# 8. M0 — Dependencies + Minimal Version Model + Validation

## 工作项

完成：

```text
Vulkan-Headers
Volk
VMA
glslang
SPIRV-Tools
Validation Layers
```

以及：

```text
loaderApiVersion
engineRequestedApiVersion
xrConstrainedApiVersion
instanceApiVersion
physicalDeviceApiVersion
effectiveDeviceApiVersion
```

本阶段暂不删除 Vulkan 1.0 fallback，避免“依赖升级 + 行为改变”叠加。

VMA 使用：

```cpp
allocatorInfo.vulkanApiVersion =
    effectiveDeviceApiVersion;
```

Validation 开启：

```text
VK_LAYER_KHRONOS_validation
Best Practices
Synchronization Validation
```

## Gate M0

```text
Renderer behavior unchanged
Windows build PASS
Android build PASS
XR build PASS（项目支持时）
SPIR-V target validation PASS
Vulkan 1.3 core entry loading：
- 在支持 Vulkan 1.3 Modern Path 的设备/模拟 Profile 上 PASS；
- Vulkan 1.1 / 1.2 真实设备不要求 1.3 Core entry 可用；
- 1.1 / 1.2 设备必须验证 Baseline / Enhanced compatibility path 正常；
- 不允许因为 1.3 entry 缺失而错误拒绝合法的 Vulkan 1.1 / 1.2 设备。

Validation 无新增错误
Dependency lock 完整
```

---

# 9. M1 — Vulkan 1.1 Baseline + Capability Negotiation

## 删除 Vulkan 1.0 Path

删除只为 1.0 存在的：

```text
VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2 fallback
vkGetPhysicalDeviceFeatures2KHR
vkGetPhysicalDeviceProperties2KHR

VK_KHR_DEDICATED_ALLOCATION compatibility path
VK_KHR_GET_MEMORY_REQUIREMENTS_2 compatibility path
VK_KHR_DESCRIPTOR_UPDATE_TEMPLATE compatibility path
```

统一 Vulkan 1.1 Core API。

### Gate

```text
Loader <1.1 → backend reject
XR constrained version <1.1 → backend reject
Device <1.1 → reject device
```

---

## Maintenance4 明确任务

Vulkan 1.3：

查询：

```text
maintenance4
VkPhysicalDeviceMaintenance4Properties
maxBufferSize
```

如果 enabled：

```text
maxBufferSize
→ 纳入 Vulkan device limits
→ Buffer 创建统一做上限检查
```

同时为未来：

```text
vkGetDeviceBufferMemoryRequirements
vkGetDeviceImageMemoryRequirements
```

预留接口。

本轮不要求为 Maintenance4 强行重构 VMA。

## Gate M1

```text
Vulkan 1.0 compatibility path = 0
1.1 feature chain compliant
1.2 aggregate chain compliant
1.3 aggregate chain compliant
No duplicate promoted feature structs
Capability supported/requested/enabled logs 完整
Maintenance4 DoD 完成
```

---

# 10. M2 — Swapchain / WSI Lifecycle Closure

## 10.1 State Machine

```cpp
enum class VKSwapchainState {
    READY,
    SUBOPTIMAL,
    OUT_OF_DATE,
    SUSPENDED_ZERO_SIZE,
    SURFACE_LOST,
    RECREATE_PENDING,
};
```

---

## 10.2 Acquire

显式处理：

```text
VK_SUCCESS
VK_SUBOPTIMAL_KHR
VK_ERROR_OUT_OF_DATE_KHR
VK_ERROR_SURFACE_LOST_KHR
```

### SUCCESS

正常进入 rendering。

### SUBOPTIMAL

本帧可继续，标记 deferred recreate。

### OUT_OF_DATE

```text
本帧跳过该 swapchain rendering
不得使用未成功 acquire 的 image index
标记 recreate
```

### ZERO SIZE

```text
不 acquire
不 submit swapchain rendering
不 present
等待恢复有效尺寸
```

### SURFACE LOST

进入 surface recreation。

---

## 10.3 Present

`vkQueuePresentKHR` / Swappy 返回值进入同一 State Machine。

多 swapchain 时评估使用：

```text
VkPresentInfoKHR::pResults
```

避免一个整体返回值覆盖多个 swapchain 状态。

---

## 10.4 Present Binary Semaphore Ownership

改成：

```text
Swapchain
├── Image0 → renderFinishedSemaphore0
├── Image1 → renderFinishedSemaphore1
└── ImageN → renderFinishedSemaphoreN
```

禁止简单：

```text
FrameSlot → Present Semaphore
```

作为唯一 ownership。

安全复用依据：

```text
image 再次 acquire
+
acquire synchronization 正确进入 wait 链
```

不是 submission fence/timeline。

---

## 10.5 Acquire Semaphore

Acquire semaphore 可继续使用 frame-slot/pool，但必须证明其 signal 已被 queue wait 消费，再进入安全回收。

---

## 10.6 同一 Submit 同时 Signal

正确：

```text
Submit
├── signal timeline value N
└── signal render-complete binary semaphore
```

禁止为此额外加第二个串行 submit。

---

## 10.7 Portability

修正：

```text
VK_KHR_portability_enumeration → Instance
VK_KHR_portability_subset      → Device
```

## Gate M2

压力覆盖：

```text
resize
rotation
minimize/restore
Android background/foreground
zero-size
surface destroy/recreate
OUT_OF_DATE
SUBOPTIMAL
SURFACE_LOST（可模拟时）
```

要求：

```text
Validation Error = 0
Semaphore reuse error = 0
无 stale image index
无未 acquire image 使用
无死锁
```

---

# 11. M3 — Timeline Semaphore + Resource Retirement

## 正确目标

不再写：

```text
“让 Frame-in-flight 真正并行”
```

而定义为：

```text
统一 GPU completion tracking
简化 fence management
统一 resource retirement point
减少 Host Wait / bookkeeping 成本
```

---

## 11.1 Completion Model

```cpp
using GpuCompletionValue = uint64_t;
```

submission：

```text
submission N → signal timeline value N
```

### Completion Signal Stage Scope — 硬规则

用于以下生命周期回收的 timeline completion value：

```text
Command Pool / Command Buffer
Descriptor Set / Descriptor Pool
Dynamic / Uniform / Upload transient allocations
Staging Buffer
Recycle Bin
Transient GPU objects
```

必须代表**整个该次 submission 的 GPU 工作已经完成**。

在 `VkSubmitInfo2` / `VkSemaphoreSubmitInfo` 路径中，默认规则：

```cpp
completionSignal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
```

即：

> 用于“submission completion / resource retirement”的 timeline signal 默认放在 `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`，确保该 completion value 覆盖本次提交内需要纳入资源生命周期判断的全部命令。

如果未来为了性能将 completion signal 缩窄到更早的 stage，必须同时提供证明：

```text
1. 该 submission 中所有引用待回收资源的 GPU 工作均已完成；
2. signal stage scope 覆盖这些工作；
3. 后续 stage 不再访问这些资源；
4. Synchronization Validation 与生命周期测试通过。
```

否则禁止缩窄。

这里使用：

```text
VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT
```

属于**提交完成生命周期语义**，不属于 M4-B 中禁止使用宽泛 Barrier 掩盖资源依赖错误的情形。

必须明确区分：

```text
Completion signal stage scope
    → 用于定义“整个 submission 已完成”的生命周期边界

Barrier stage/access scope
    → 用于资源依赖和 hazard 同步，应尽量精确
```

仅在较早 pipeline stage signal timeline semaphore：

```text
不能自动证明整个 submission 已完成
不能作为通用 command pool / descriptor / staging 回收依据
```

参考：
`https://docs.vulkan.org/refpages/latest/refpages/source/VkSemaphoreSubmitInfo.html`

资源：

```cpp
struct RetiredResource {
    GpuCompletionValue retireAfter;
};
```

回收条件：

```text
completedTimelineValue >= retireAfter
```

---

## 11.2 Resource Lifetime Inventory

必须覆盖：

```text
Command Pool
Command Buffer
Dynamic Buffer / ring allocations
Uniform / upload transient allocations
Descriptor Set instances
Descriptor Pool chunks
Staging Buffer
Recycle Bin
Transient GPU objects
Fence objects
Acquire semaphore pool
```

特别说明：

```text
Present wait binary semaphore
```

不由 timeline completion 单独回收。

---

## 11.3 统一 Tracker

Baseline：

```text
Fence-based completion
```

Enhanced：

```text
Timeline-based completion
```

统一接口：

```cpp
class GpuCompletionTracker {
public:
    virtual CompletionPoint submit(...) = 0;
    virtual bool isComplete(CompletionPoint) const = 0;
    virtual void wait(CompletionPoint) = 0;
};
```

---

## 11.4 Benchmark

记录：

```text
Host wait count/frame
Host wait duration/frame
Fence object count
Semaphore object count
Render Thread CPU
P50/P95/P99
GPU frame time
frames-in-flight occupancy
```

如果性能提升不明显，也允许通过，但必须将价值表述为：

```text
生命周期统一 / 可维护性 / 后续现代同步基础
```

不能虚构“消除 stall”。

---

# 12. M4 — Synchronization2 + Submit2

## 12.1 M4-A：语义等价迁移

先：

```text
vkCmdPipelineBarrier → vkCmdPipelineBarrier2
VkSubmitInfo         → VkSubmitInfo2
vkQueueSubmit        → vkQueueSubmit2
```

但保持：

```text
原 stage/access 语义
原 dependency topology
原 submit count
```

### Gate

```text
Synchronization Validation = 0
Render output identical
Submit count unchanged
Performance non-regression
```

---

## 12.2 M4-B：同步范围优化

M4-A PASS 后才开始：

```text
Global Barrier
→ Buffer/Image specific

Broad stage
→ precise stage

Broad access
→ precise access
```

禁止长期使用：

```text
ALL_COMMANDS
MEMORY_READ | MEMORY_WRITE
```

掩盖错误。

---

## 12.3 Unified Submit Model

```cpp
struct VKSubmitBatch {
    Span<CommandBuffer> commandBuffers;
    Span<WaitDependency> waits;
    Span<SignalDependency> signals;
    CompletionPoint completion;
};
```

编码器：

```text
Submit1Encoder → VkSubmitInfo
Submit2Encoder → VkSubmitInfo2
```

业务层只构造一种模型。

---

# 13. M5 — Rendering Contract Preparation

本阶段不默认启用 Dynamic Rendering。

---

## 13.1 Rendering Signature

```cpp
struct VKRenderingSignature {
    ccstd::vector<VkFormat> colorFormats;
    VkFormat depthFormat{VK_FORMAT_UNDEFINED};
    VkFormat stencilFormat{VK_FORMAT_UNDEFINED};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    uint32_t viewMask{0};
};
```

用途：

```text
Pipeline key
Dynamic rendering compatibility
Secondary CB inheritance
A/B validation
```

---

## 13.2 Pipeline Contract

Dynamic Rendering pipeline：

```text
VkGraphicsPipelineCreateInfo.renderPass = VK_NULL_HANDLE
```

并链入：

```text
VkPipelineRenderingCreateInfo
```

包含：

```text
viewMask
colorAttachmentFormats
depthAttachmentFormat
stencilAttachmentFormat
```

注意顺序：

> Rendering Signature 和 Pipeline 适配必须先于 Dynamic Rendering 启用。

---

## 13.3 pNext Composition

必须实现统一 composer，避免：

```text
VkPipelineRenderingCreateInfo
VkPipelineFragmentShadingRateStateCreateInfoKHR
其他 pipeline pNext
```

互相覆盖。

禁止散落：

```cpp
createInfo.pNext = ...
```

---

## 13.4 Secondary Command Buffer Contract

Primary：

```text
VkRenderingInfo.flags
包含 VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT
```

Secondary begin：

```text
VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT
```

Inheritance：

```text
VkCommandBufferInheritanceInfo.renderPass = VK_NULL_HANDLE
```

`pNext`：

```text
VkCommandBufferInheritanceRenderingInfo
```

必须匹配：

```text
flags
viewMask
colorAttachmentFormats
depthAttachmentFormat
stencilAttachmentFormat
rasterizationSamples
```

参考：
`https://docs.vulkan.org/refpages/latest/refpages/source/VkCommandBufferInheritanceRenderingInfo.html`

---

## 13.5 Layout / Dependency Contract

Classic RenderPass 中：

```text
initialLayout
finalLayout
subpass dependency
```

对应行为必须迁移到：

```text
pre-render explicit transition
rendering
post-render explicit transition
```

并统一进入 Barrier Model。

---

## 13.6 Load / Store / Resolve

映射：

```text
VkAttachmentDescription load/store
→ VkRenderingAttachmentInfo loadOp/storeOp
```

Resolve：

```text
resolveImageView
resolveImageLayout
resolveMode
```

Depth / Stencil 分别验证。

---

## 13.7 首批保留 Classic

以下默认保持 Classic：

```text
Multi Subpass
Input Attachment
Preserve Attachment
未验证 FSR Dynamic Path
未验证 Secondary CB 场景
复杂 multiview/XR
```

## Gate M5

完成但不必默认启用：

```text
Rendering Signature
PipelineRenderingCreateInfo adapter
pNext composer
Explicit transition model
Load/store/resolve mapping
Secondary CB inheritance adapter
```

---

# 14. M6 — Hybrid Dynamic Rendering

## 14.1 Eligibility

```cpp
bool canUseDynamicRendering(const RenderPassInfo &info);
```

首批白名单：

```text
single subpass
no input attachment
no preserve attachment
no unverified FSR
no unverified secondary CB
simple color/depth/stencil/resolve
```

原则：

```text
不能证明等价 → Classic
```

---

## 14.2 Begin / End

Modern：

```text
vkCmdBeginRendering
vkCmdEndRendering
```

Classic：

```text
vkCmdBeginRenderPass
vkCmdEndRenderPass
```

高层 RenderPass description 继续保留。

---

## 14.3 A/B

同一设备支持：

```text
forceClassicRenderPass = true
vs
Dynamic Rendering enabled
```

对同 Scene 比较：

```text
visual
validation
CPU
GPU
bandwidth
```

---

## 14.4 Mobile TBDR Gate

重点：

```text
Adreno
Mali
PowerVR（如覆盖）
```

关注：

```text
GPU frame time
external memory bandwidth
tile load/store
vendor counters
```

如果某类 Pass Dynamic Path 回归，则保留 Classic。

---

# 15. M7 — Pipeline Cache / Prewarm / Final Performance Gate

## 15.1 Pipeline Cache

完成：

```text
Pipeline State hash
Pipeline Layout hash
Descriptor Set Layout hash
Rendering Signature hash
Classic RenderPass compatibility key
Dynamic Rendering compatibility key
```

---

## 15.2 Cache Persistence

当前只在析构/退出路径保存的模式需要改成可恢复设计：

```text
startup load
periodic/checkpoint save
clean shutdown save
corruption detection
driver/device/pipelineCacheUUID compatibility check
atomic replace
```

禁止缓存文件损坏导致启动崩溃。

---

## 15.3 Prewarm

建立：

```text
known PSO manifest
→ prewarm queue
→ pipeline cache
```

第一阶段可以是串行/后台预热实验，不要求直接上复杂异步编译框架。

---

## 15.4 Graphics Pipeline Library

作为 Optional：

```text
supported/requested/enabled/runtime path
```

不作为 Vulkan 1.3 Modern Profile 的强制要求。

---

# 16. Benchmark 方法学

原有 3% 调查阈值保留，但必须补全测试方法。

## 16.1 Cold Start / First-use 指标

固定预热仅用于 steady-state 性能统计，不能掩盖冷启动和首次 Pipeline 创建成本。

必须单独统计：

```text
Process Cold Start Time
Vulkan Backend Initialization Time
VkInstance / VkDevice Creation Time
Pipeline Cache Load Time
First Frame Time
Time To First Present
First PSO Creation Time
First Material / Shader Variant Hitch
Cold Pipeline Compilation Total Time
```

至少分别报告：

```text
Cold Cache
Warm Cache
```

并保留首次 PSO / 首帧耗时原始样本。

这些指标：

```text
不得因为后续 Warm-up 阶段而从报告中消失
```

---

## 16.2 Warm-up

每个 steady-state 场景先执行固定预热：

```text
Desktop：至少 300 frames
Mobile：至少 600 frames 或进入稳定状态
```

预热数据不进入 steady-state 正式统计，但仍保留第 16.1 节的冷启动与首次使用指标。

---

## 16.3 正式采样

建议：

```text
每轮 ≥ 1000 valid frames
重复 ≥ 5 轮
```

记录：

```text
median
P95
P99
stddev / MAD
```

禁止只比较平均 FPS。

---

## 16.4 Frame Pacing

固定：

```text
VSync mode
present mode
Swappy 配置
目标刷新率
分辨率
画质
场景 camera
```

并记录实际：

```text
display refresh
frame pacing state
```

---

## 16.5 Mobile 温控

必须控制：

```text
设备初始温度
电量范围
充电状态
环境温度
thermal throttling state
```

正式对比前要求设备进入可重复热状态。

若发生明显 thermal throttling，该轮结果无效。

---

## 16.6 Pipeline Cache 状态

分别报告：

```text
Cold Cache
Warm Cache
```

不能混用。

Dynamic Rendering / Pipeline 改造必须明确比较同等缓存状态。

---

## 16.7 Regression Threshold

同 GPU / 同 Scene / 同条件：

```text
Median Frame Time regression > 3%
或
P95 Frame Time regression > 3%
```

必须调查。

不是自动 FAIL，而是：

```text
需要原因、profile 证据和接受理由
```

Validation/Correctness 回归则直接 FAIL。

---

# 17. Version / Capability Matrix

## 17.1 API Matrix

| Case | Loader | Device | Expected |
|---|---:|---:|---|
| A | 1.0 | 1.0 | Reject Vulkan backend |
| B | 1.1 | 1.1 | Baseline |
| C | 1.2 | 1.1 | Baseline |
| D | 1.2 | 1.2 | Enhanced candidate |
| E | 1.3 | 1.1 | Baseline |
| F | 1.3 | 1.2 | Enhanced candidate |
| G | 1.3 | 1.3 | Modern candidate |
| H | 1.4 | 1.4 | Engine contract capped at 1.3 |

---

## 17.2 同设备 Feature Fallback Matrix

在 Vulkan 1.3 设备上至少测试：

```text
All modern capabilities ON

Device Feature Disable Cases
├── Timeline feature 未 enable
├── Synchronization2 feature 未 enable
├── Dynamic Rendering feature 未 enable
└── Maintenance4 feature 未 enable

Runtime Path Disable Cases
├── Timeline feature enabled，但使用 Fence completion path
├── Synchronization2 feature enabled，但强制 Legacy Barrier Encoder
├── Submit2 可用，但强制 Submit1 Encoder
├── Dynamic Rendering feature enabled，但 force Classic RenderPass
└── Maintenance4 enabled，但关闭对应 runtime optimization/path
```

### Feature / Runtime Path 依赖规则

必须区分：

```text
supported
requested
enabled
runtimePathSelected
```

其中：

```text
enabled == false
```

表示 Device 创建时没有启用该 feature，此时不得调用依赖该 feature 的命令或编码路径。

硬依赖：

```text
synchronization2.enabled == false
    → useSynchronization2 = false
    → useSubmit2 = false
```

因为 `vkQueueSubmit2` 属于 synchronization2 能力路径，不能在未启用对应 Device Feature 时使用。

但如果：

```text
synchronization2.enabled == true
```

只是运行时人为选择：

```text
useSynchronization2 = false
```

即使用 Legacy Barrier Encoder，则：

```text
useSubmit2
```

仍可以保持 `true`，因为“Barrier 编码选择”和“Submit2 编码选择”是两个独立的 runtime path，前提是 synchronization2 feature 已经 enabled。

建议显式定义：

```cpp
struct VulkanRuntimePaths {
    bool useTimelineSemaphore;
    bool useSynchronization2Barriers;
    bool useSubmit2;
    bool useDynamicRendering;
    bool forceClassicRenderPass;
};
```

并集中执行依赖归一化：

```cpp
if (!caps.synchronization2.enabled) {
    paths.useSynchronization2Barriers = false;
    paths.useSubmit2 = false;
}

if (!caps.dynamicRendering.enabled) {
    paths.useDynamicRendering = false;
}
```

禁止把这种依赖关系散落在 Queue / CommandBuffer / RenderPass 内部。

参考：
`https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit2.html`

并验证：

```text
fallback path 正常
视觉一致
Validation = 0
```

---

## 17.3 Vendor Matrix

Android：

```text
Qualcomm Adreno
ARM Mali
PowerVR（项目覆盖时）
```

Desktop：

```text
NVIDIA
AMD
Intel
```

XR：

```text
至少一个实际支持的 XR runtime/device
```

---

# 18. Rendering Feature Matrix

至少：

```text
Basic Forward
Shadow Map
MSAA
Depth Texture
Depth Resolve
Stencil
Input Attachment
Multi Subpass
Post Process
Compute
GPU Instancing
Indirect Draw
Fragment Shading Rate
Texture Upload
Buffer Upload
Swapchain Resize
Surface Recreate
Multiple Swapchains（如支持）
Secondary Command Buffer
XR / Multiview（如支持）
```

---

# 19. Validation Gate

开启：

```text
VK_LAYER_KHRONOS_validation
Synchronization Validation
Best Practices
```

正式 Gate：

```text
Validation Error                = 0
Synchronization Validation Err  = 0
Lifetime Error                  = 0
Semaphore Reuse Error           = 0
Invalid Feature Chain           = 0
Invalid Dynamic Rendering State = 0
```

新增 Warning：

```text
必须分类
不能靠 suppress 替代修复
驱动已知误报必须有证据和白名单说明
```

---

# 20. Rendering Correctness Gate

对：

```text
Baseline 1.1
Enhanced 1.2
Modern 1.3
Modern 1.3 + forced fallback
```

运行同一 reference scene。

建立：

```text
Golden Screenshot
Image Diff
Threshold
```

重点识别：

```text
Load/Store
Resolve
Depth/Stencil
Layout
Subpass replacement
Dynamic Rendering
Secondary CB
FSR
```

回归。

---

# 21. 推荐提交拆分

推荐：

```text
01-lock-vulkan-dependencies
02-add-minimal-version-model
03-enable-validation-baseline
04-remove-vulkan-1.0
05-capability-state-model
06-feature-chain-builder
07-swapchain-state-machine
08-wsi-semaphore-ownership
09-portability-fix
10-completion-tracker-abstraction
11-timeline-semaphore
12-resource-retirement
13-sync2-semantic-port
14-submit2-semantic-port
15-sync-scope-optimization
16-rendering-signature
17-pipeline-dynamic-rendering-contract
18-secondary-cb-inheritance
19-explicit-render-transitions
20-dynamic-rendering-eligibility
21-dynamic-rendering-path
22-pipeline-cache-persistence
23-pipeline-prewarm
24-final-validation-benchmark
```

禁止把：

```text
依赖升级
+ 1.0 删除
+ WSI 修复
+ Timeline
+ Sync2
+ Dynamic Rendering
```

塞进不可审查的大提交。

---

# 22. Source Cleanup Checklist

M1/M2 后检查：

```text
minorVersion < 1
minorVersion > 0
VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2 compatibility
vkGetPhysicalDeviceFeatures2KHR
vkGetPhysicalDeviceProperties2KHR
vkGetBufferMemoryRequirements2KHR
vkGetImageMemoryRequirements2KHR
vkBindBufferMemory2KHR
vkBindImageMemory2KHR
vkCreateDescriptorUpdateTemplateKHR
vkDestroyDescriptorUpdateTemplateKHR
```

注意：

> `KHR` 后缀本身不是删除条件。

删除条件是：

```text
“该路径是否只因为 Vulkan 1.0 Baseline 才存在”
```

---

# 23. 最终 DoD

## API Baseline

- [ ] Vulkan 1.0 明确不支持。
- [ ] Loader <1.1 安全拒绝。
- [ ] XR constrained API <1.1 安全拒绝。
- [ ] Device <1.1 安全 reject。
- [ ] Engine Max API 固定 1.3。

## Version Model

- [ ] Loader / Engine / XR / Instance / Device / Effective 分离。
- [ ] `vkEnumerateInstanceVersion` 正确处理返回值与函数缺失。
- [ ] Vulkan 1.4+ 不自动改变 Backend contract。

## Feature Chain

- [ ] Vulkan 1.1 使用需要的独立 Feature Struct。
- [ ] Vulkan 1.2+ 可使用 Vulkan11/12 aggregate。
- [ ] Vulkan 1.3 使用 Vulkan13 aggregate。
- [ ] Aggregate/independent promoted structures 不重复。
- [ ] supported/requested/enabled/runtime path 完整。

## Dependency

- [ ] Vulkan dependency lock 完成。
- [ ] Vulkan 1.3 core dispatch 正确。
- [ ] SPIR-V 1.6 实际编译/验证 PASS。

## WSI

- [ ] OUT_OF_DATE 闭环。
- [ ] SUBOPTIMAL 闭环。
- [ ] Zero-size suspend/recover。
- [ ] Surface recreate。
- [ ] Present semaphore 按 image 生命周期安全复用。
- [ ] submission completion 不被错误用于证明 present completion。

## Timeline

- [ ] Completion Tracker 统一。
- [ ] Command Pool / Descriptor / Dynamic Buffer / Staging / Recycle 等资源纳入 retirement。
- [ ] 用于通用资源回收的 completion signal 覆盖整个 submission；Submit2 默认 `VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT`。
- [ ] 若 completion signal stage 缩窄，有完整正确性证明与验证。
- [ ] Present binary semaphore 不由 timeline 单独回收。
- [ ] 无提前复用。

## Sync2 / Submit2

- [ ] 先完成语义等价迁移。
- [ ] 再完成同步范围优化。
- [ ] `synchronization2.enabled == false` 时强制关闭 Sync2 Barrier 与 Submit2。
- [ ] synchronization2 feature 已 enabled 时，Legacy Barrier Encoder 与 Submit2 可以独立 A/B。
- [ ] Runtime path 依赖集中归一化，不散落在业务代码。
- [ ] Synchronization Validation = 0。
- [ ] Submit topology 无意外变化。

## Rendering Contract

- [ ] Rendering Signature 完成。
- [ ] `VkPipelineRenderingCreateInfo` 正确。
- [ ] Dynamic pipeline `renderPass = VK_NULL_HANDLE`。
- [ ] Pipeline `pNext` composer 完成。
- [ ] Secondary CB inheritance 完成。
- [ ] Explicit layout/dependency migration 完成。
- [ ] Load/store/resolve 映射完成。

## Dynamic Rendering

- [ ] Eligibility 白名单明确。
- [ ] Classic fallback 始终存在。
- [ ] FSR/Secondary CB/XR 未验证场景默认 Classic。
- [ ] Dynamic vs Classic A/B 可强制切换。

## Maintenance4

- [ ] supported/requested/enabled 正确。
- [ ] `maxBufferSize` 纳入 capabilities。
- [ ] 关闭能力时 fallback 正常。

## Correctness

- [ ] Golden scene visual PASS。
- [ ] Baseline/Enhanced/Modern/Fallback 一致。
- [ ] Validation Error = 0。

## Performance

- [ ] Cold Start / First Present / First PSO 单独统计。
- [ ] Cold Cache / Warm Cache 分开报告。
- [ ] Warm-up 条件固定。
- [ ] 正式采样 ≥1000 valid frames/轮。
- [ ] ≥5 次重复。
- [ ] P50/P95/P99 均记录。
- [ ] Mobile 温控条件记录。
- [ ] Cold/Warm pipeline cache 分开报告。
- [ ] >3% 回归有分析结论。

---

# 24. 本轮验证边界

本任务计划书是“实施规格”，不是实施结果。

当前状态：

```text
已完成：
- 基于现有源码事实修正 M3 帧同步前提
- 修正 Vulkan 1.1 Feature Chain 设计
- WSI 生命周期提前为同步迁移前置
- 补全 Present Binary Semaphore ownership
- 补全 Dynamic Rendering pipeline/secondary CB/layout 契约
- 修正 M0/M1/M2 阶段依赖
- Capability 拆分 supported/requested/enabled/runtime path
- 补充 Dependency Lock / SPIR-V 实际验证
- 补充 Maintenance4 DoD
- 补充 Benchmark 方法学
- 补充单项能力强制关闭回退矩阵
- 明确 submission completion signal 必须覆盖整个提交及其 `ALL_COMMANDS` 生命周期用途
- 明确 Feature Enable 与 Runtime Encoder 开关之间的依赖关系
- 修正 M0 Vulkan 1.3 Core Entry Gate，仅在支持 Modern Path 的设备要求
- 增加 Cold Start / First Present / First PSO 独立性能统计

未执行：
- G4 Vulkan Validation 实机/实际构建
- G5 RenderDoc/AGI 抓帧与性能测量
- G6 Android/Windows/XR 多平台回归
```

这些只能在实际代码实施后完成。

---

# 25. 最终执行主线

最终只允许按：

```text
Dependencies + Minimal Version Model + Validation
→ Vulkan 1.1 Baseline + Capability Negotiation
→ Swapchain / WSI Lifecycle Closure
→ Timeline + Resource Retirement
→ Sync2 / Submit2 Semantic Port
→ Synchronization Scope Optimization
→ Rendering Signature / Pipeline / Secondary CB Contract
→ Hybrid Dynamic Rendering
→ Pipeline Cache / Prewarm
→ Full Correctness / Performance / Platform Gate
```

执行。

本次 modernization 最终目标不是得到“Vulkan 1.3 only backend”，而是：

> **一个以 Vulkan 1.1 为稳定最低基线、明确约束 Vulkan 1.2/1.3 feature 使用、WSI 与资源生命周期正确、Modern Path 可独立回退、并能长期继续演进的 Vulkan 后端。**
