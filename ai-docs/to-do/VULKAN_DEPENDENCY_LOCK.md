# Vulkan Dependency Lock（依赖锁定记录）

> 隶属：`ai-docs/to-do/Cocos_Vulkan_1.1_to_1.3_Modernization_Task_Plan.md` 第 6 / 8 / 19 / 23 节（M0 Dependency Lock）。
> 状态：**升级前基线已核实（§1）；实施版本已选定并接入 Windows 真实引擎（§3.0，M0-C/D/E/F）。三档引擎 SPIR-V 与匹配 spirv-val 全 PASS；Android 全引擎/XR/画面验收待完成，M0 尚未验收。**
> 原则：只记录"已验证并锁定的精确版本"；候选版本未经真实构建/运行验证前一律标记 pending，不称为锁定。

---

## 1. 升级前基线（M0-A/B 调查快照；当前实施版本见 §3.0）

| Dependency | Current 精确版本 | Source（来源） | Platform artifact（实际链接工件） | Evidence（依据） |
|---|---|---|---|---|
| Vulkan-Headers | **1.2.185**（`VK_HEADER_VERSION=185`，`VK_HEADER_VERSION_COMPLETE=VK_MAKE_API_VERSION(0, 1, 2, 185)`） | `cocos-engine-external` 仓库 tag `v4.0.0-4`（见 `native/external-config.json`），头文件随仓库入库 | 纯头文件，全平台共用：`native/external/sources/khronos/vulkan/`；include 路径 `native/CMakeLists.txt:3571-3573`（`external/sources/khronos` 为 `cocos_engine` PUBLIC include） | `native/external/sources/khronos/vulkan/vulkan_core.h:75,78`；`vulkan_core.h` 中 `VK_API_VERSION_1_3` 出现 0 次 |
| Volk | **VOLK_HEADER_VERSION 179**（volk.h 由 VK header 179 生成；zeux/volk，MIT） | 源码直接 vendored 在引擎源内，非 external 库 | `native/cocos/renderer/gfx-vulkan/volk.c` + `volk.h` 编入 `cocos_engine`：`native/CMakeLists.txt:1949-1950`；调用入口 `VKGPUContext.cpp:110`（`volkInitialize()`） | `native/cocos/renderer/gfx-vulkan/volk.h:18` `#define VOLK_HEADER_VERSION 179` |
| VMA (Vulkan Memory Allocator) | **3.0.0-development 快照，日期 2021-02-16**（正式 3.0.0 发布前的 dev 版） | GPUOpen 上游，头文件 vendored | header-only 实现编入引擎：`VKDevice.cpp:61` `#define VMA_IMPLEMENTATION`；`VKDevice.cpp:352-404` 创建 allocator（`vmaCreateAllocator`，静态函数表手动填入） | `native/cocos/renderer/gfx-vulkan/vk_mem_alloc.h:28` `<b>Version 3.0.0-development</b> (2021-02-16)` |
| glslang | **11.5.0**（按 `versions.txt` 为 `YunHsiao/glslang` `build` 分支构建，commit `1978c76`） | 预编译静态库随 `cocos-engine-external` v4.0.0-4 入库 | win64：`native/external/win64/libs/glslang/*.lib`（release+debug，9 个目标：glslang、glslang-default-resource-limits、MachineIndependent、OGLCompiler、OSDependent、SPIRV、SPIRV-Tools-opt、SPIRV-Tools、GenericCodeGen，`win64/CMakeLists.txt:200-210`）；android：`native/external/android/{arm64-v8a,armeabi-v7a,x86,x86_64}/glslang/lib*.a`；mac：`native/external/mac/libs/glslang/`；ios：`native/external/ios/libs/glslang/` | 声明：`native/external/versions.txt` glslang 段；头：`native/external/sources/glslang/build_info.h`（MAJOR 11 / MINOR 5 / PATCH 0）；**二进制实测**：`grep -aoE "11\.[0-9]+\.[0-9]+"` 在 win64 `MachineIndependent(.d).lib`、android 四 ABI `libMachineIndependent.a`、mac `libMachineIndependent.a` 中均只出现 `11.5.0` |
| SPIRV-Tools | **版本号未在二进制内自声明（确切 revision 待确认）；随 glslang 预编译包捆绑（与 glslang 11.5.0 同期）**。字符串线索：target-env 字符串仅见 vulkan1.0/1.1/1.2（此为支持范围线索，非行为验证；见 §1.1 实测行为） | 同 glslang 预编译包 | win64 `SPIRV-Tools.lib`/`SPIRV-Toolsd.lib`/`SPIRV-Tools-opt(.d).lib`；android 四 ABI `libSPIRV-Tools.a`/`libSPIRV-Tools-opt.a`；mac/ios 同 | 二进制字符串：`grep -aoE "vulkan1\.[0-9]"` 在 win64（release+debug）、android arm64-v8a、mac 的 SPIRV-Tools 库中仅有 `vulkan1.0`/`vulkan1.1`/`vulkan1.2`。注意：引擎自身代码不直接调用 SPIRV-Tools API（`SPIRVUtils.cpp` 仅 include glslang 头），SPIRV-Tools 是 glslang 链接依赖；其确切 revision 与实际校验行为待确认 |
| Validation Layers | **仓库内不捆绑任何 layer**；运行时按优先级请求系统已安装 layer | 系统安装（dev 机器：LunarG Vulkan SDK） | 无仓库工件。dev 机器 SDK：`D:\Software\VulkanSDK\1.4.341.1\Bin\` 下含 `VkLayer_khronos_validation(.dll/.json)`、`VkLayer_khronos_synchronization2`、`VkLayer_khronos_profiles` 等 | 请求逻辑：`VKGPUContext.cpp:159-196`（`CC_DEBUG > 0 && !FORCE_DISABLE_VALIDATION || FORCE_ENABLE_VALIDATION` 时启用，优先 `VK_LAYER_KHRONOS_validation`）；开关宏：`VKGPUContext.cpp:36-37`（`FORCE_ENABLE_VALIDATION 0` / `FORCE_DISABLE_VALIDATION 1`） |

### 1.1 SPIR-V target 能力实测（M0-B harness，2026-10-02 真实运行）

**捆绑 glslang 11.5.0 头文件声明上限**：`EShTargetVulkan_1_0/1_1/1_2`、`EShTargetSpv_1_0..1_5`（`external/sources/glslang/Public/ShaderLang.h:166-181`），**不存在** `EShTargetVulkan_1_3` / `EShTargetSpv_1_6` 枚举。

**引擎回退逻辑**（`SPIRVUtils.cpp:60-98`）：`CC_GLSLANG_VERSION_GREATOR_OR_EQUAL_TO(11, 10, 0)` 不满足时，vulkan minor 3/4 请求**静默回退** Vulkan1.2/SPV1.5。因此捆绑 11.5.0 的引擎路径**无法产出 Vulkan 1.3 / SPIR-V 1.6**，此前若曾表述"上游支持 1.3 目标"为错误表述，以本节实测为准。

**harness 实测结果**（`native/tests/vulkan-modernization/`，链接捆绑 win64 静态库 + 引擎同源编译流程，含 `optimizeSize=true`）：

| target | 结果 | SPIR-V version word | spirv-val（SDK v2026.1） |
|---|---|---|---|
| Vulkan 1.1（vertex/fragment/compute） | PASS ×3 | 0x00010300 ×3 | vulkan1.1 PASS ×3 |
| Vulkan 1.2（vertex/fragment/compute） | PASS ×3 | 0x00010500 ×3 | vulkan1.2 PASS ×3 |
| Vulkan 1.3 | **UNSUPPORTED**（枚举缺失，version guard < 11.10） | 无产物（不回退冒充） | N/A |

harness `--all` exit=2（UNSUPPORTED 语义）；跨 target 顶替已被反向验证拒绝（vk1.2 产物在 `--target-env vulkan1.1` 下 spirv-val 报错退出）。

**SDK 对照（非引擎路径，仅演示升级后形态）**：SDK glslangValidator 16.2.0 `--target-env vulkan1.3` 编译 compute 样例 → version word `0x00010600`，`spirv-val --target-env vulkan1.3` PASS。

### 1.2 相关但非六项锁定对象的工件（一并记录）

- **Swappy / android-games-sdk**（Android 帧调节）：`native/external/sources/Swappy`、`native/external/sources/android-games-sdk`；引擎使用见 `VKSwapchain.cpp:410-417`（`SwappyVk_initAndGetRefreshCycleDuration` / `setSwapIntervalNS` / `setWindow`）。Vulkan swapchain 路径在 Android 上依赖它。
- **GLSL 目标环境映射现状**：`native/cocos/renderer/gfx-base/SPIRVUtils.cpp:60-98` 写有 vulkan 1.3/1.4 → 1.3 目标的 case，但受 `CC_GLSLANG_VERSION_GREATOR_OR_EQUAL_TO(11, 10, 0)` guard：glslang < 11.10 时**回退** Vulkan1.2/SPV1.5。捆绑 11.5.0 即处于回退分支（harness 已实测证实，见 §1.1）。

### 1.3 当前基线的关键缺口（决策输入，均为实测事实）

1. **Vulkan-Headers 1.2.185 不含 Vulkan 1.3 声明**：
   - 存在：`VK_KHR_synchronization2`（KHR 扩展形态）、`vkCmdPipelineBarrier2` / `vkQueueSubmit2`（KHR 变体）、`VkPhysicalDeviceVulkan11Features` / `Vulkan12Features`、`VK_KHR_timeline_semaphore`、`VK_KHR_fragment_shading_rate`、`VK_EXT_extended_dynamic_state`。
   - **缺失**：`VK_API_VERSION_1_3`、`VkRenderingInfo` / `VK_KHR_dynamic_rendering`（Dynamic Rendering 全部声明）、`vkGetDeviceBufferMemoryRequirements` / Maintenance4、`VK_KHR_portability_enumeration`。
   - 结论：M5/M6/M7 涉及的 1.3 API 在现头文件下**无法编译**，头文件升级是 M0 硬前置。
2. **Volk（179）与 Headers（185）版本号不同**：版本号不同**并不自动代表错误**（volk 对较新 header 有容错，未使用的 API 不受影响；是否存在实际问题未验证）。两者是**成套关系**：升级 Headers 时应重新生成 volk 并成套验证，而非单独断言必须版本号相等。
3. **VMA 3.0.0-dev 快照**：`VmaAllocatorCreateInfo::vulkanApiVersion` 字段已存在，但 `VKDevice.cpp:352-357` 创建时**未设置**，默认 0 等价于 `VK_API_VERSION_1_0`（VMA 按 1.0 假设工作，dedicated allocation 靠手动 flag 补偿）。计划第 8 节要求 `allocatorInfo.vulkanApiVersion = effectiveDeviceApiVersion`。
4. **glslang/SPIRV-Tools 预编译库来自 `YunHsiao/glslang` build 分支**（非 Khronos 官方发布物），是否有 cocos 定制补丁：待确认（需对照 YunHsiao/glslang `build` 分支与官方 11.5.0 差异）。glslang 侧 target 能力已由 harness 实测（§1.1）；SPIRV-Tools 确切 revision 仍待确认。

---

## 2. Evidence 附录（可重复检查命令）

M0-B 新增：`native/tests/vulkan-modernization/`（harness + `verify-spirv.ps1` + README，含完整复现命令与真实验证边界）。

```powershell
# Vulkan-Headers 版本
Select-String -Path native/external/sources/khronos/vulkan/vulkan_core.h -Pattern 'VK_HEADER_VERSION'
# → :75 #define VK_HEADER_VERSION 185
# → :78 #define VK_HEADER_VERSION_COMPLETE VK_MAKE_API_VERSION(0, 1, 2, VK_HEADER_VERSION)

# 1.3 声明缺失验证（预期 0）
(Select-String -Path native/external/sources/khronos/vulkan/vulkan_core.h -Pattern 'VK_API_VERSION_1_3\b').Count
(Select-String -Path native/external/sources/khronos/vulkan/vulkan_core.h -Pattern 'VkRenderingInfo|VK_KHR_dynamic_rendering').Count

# Volk 版本
Select-String -Path native/cocos/renderer/gfx-vulkan/volk.h -Pattern 'VOLK_HEADER_VERSION'
# → :18 #define VOLK_HEADER_VERSION 179

# VMA 版本
Select-String -Path native/cocos/renderer/gfx-vulkan/vk_mem_alloc.h -Pattern 'Version 3' | Select-Object -First 1
# → :28 <b>Version 3.0.0-development</b> (2021-02-16)

# glslang 声明版本（头文件）
Select-String -Path native/external/sources/glslang/build_info.h -Pattern 'GLSLANG_VERSION_(MAJOR|MINOR|PATCH)'
# → 11 / 5 / 0

# glslang 二进制实测版本（Git Bash；对 .lib/.a 均适用）
grep -aoE "11\.[0-9]+\.[0-9]+[-~+a-zA-Z0-9]*" native/external/win64/libs/glslang/MachineIndependent.lib | sort -u
grep -aoE "11\.[0-9]+\.[0-9]+[-~+a-zA-Z0-9]*" native/external/android/arm64-v8a/glslang/libMachineIndependent.a | sort -u
# → 均只出现 11.5.0

# SPIRV-Tools target-env 字符串（支持范围线索，非行为验证；确切 revision 待确认）
grep -aoE "vulkan1\.[0-9]" native/external/win64/libs/glslang/SPIRV-Tools.lib | sort -u
grep -aoE "vulkan1\.[0-9]" native/external/android/arm64-v8a/glslang/libSPIRV-Tools.a | sort -u
grep -aoE "vulkan1\.[0-9]" native/external/mac/libs/glslang/libSPIRV-Tools.a | sort -u
# → 均仅 vulkan1.0 / vulkan1.1 / vulkan1.2，无 vulkan1.3（字符串层面）

# SPIR-V target 能力行为验证（M0-B harness，真实编译）
cd native/tests/vulkan-modernization
powershell -ExecutionPolicy Bypass -File verify-spirv.ps1   # 基线预期 exit=2（1.1/1.2 PASS，1.3 UNSUPPORTED）

# Validation layers 请求逻辑
# native/cocos/renderer/gfx-vulkan/VKGPUContext.cpp:36-37, 110-129, 159-196
```

---

## 3. Candidate / Selected（实施组合已指定，完整平台验收未完成）

> Candidate 未经真实构建/运行验证前一律 pending；Chosen 表示已确定的实施版本（Lead 决策），
> 完成引擎/平台验证后才登记 Selected。

### 3.0 Chosen（M0-C/D/E/F 已落地，未验收）

| Dependency | Chosen 精确版本 | 落地位置 | 状态 |
|---|---|---|---|
| Vulkan-Headers | tag `vulkan-sdk-1.3.296.0`，commit **29f979ee5aa58b7b005f805ea8df7a855c39ff37**（Lead 与 Worker 双重 git ls-remote 核实） | `native/cocos/renderer/gfx-vulkan/dependencies/Vulkan-Headers/`（仅 `include/`（vulkan/ + vk_video/）、`LICENSE.md`、`LICENSES/`、`VERSION.txt` 元数据；可追踪、不在 ignored external 内） | 内容核验 PASS：`VK_HEADER_VERSION=296`、`VK_API_VERSION_1_3`、`VkRenderingInfo`/`VK_KHR_dynamic_rendering`、`VkPhysicalDeviceMaintenance4Features`、`VK_KHR_portability_enumeration` 全存在；loader smoke 三档入口加载 PASS；**M0-D 已接入引擎 include 优先级，cocos_engine Release + CC_DEBUG_FORCE 构建 exit 0**；平台运行验证未做 |
| Volk | tag `vulkan-sdk-1.3.296.0`，commit **59d26900f53c7621a8ba8ab0e3f18d3bd883fa9a** | 原位替换 `native/cocos/renderer/gfx-vulkan/volk.c` / `volk.h`（`VOLK_HEADER_VERSION 296`） | 替换前 diff 核查无定制；loader smoke PASS；已随 M0-D 引擎构建验证编译 |
| VMA | tag `v3.1.0`，commit **009ecd192c1289c7529bff248a16cfe896254816** | `native/cocos/renderer/gfx-vulkan/vk_mem_alloc.h` 原样替换；来源见 `dependencies/VMA_VERSION.txt` | Lead 已设置 effective API、显式 dispatch、适配 statistics；Headers/Volk/VMA 的 Windows `cocos_engine` Release + CC_DEBUG_FORCE 构建 exit 0；运行/Android/XR 未验证 |
| glslang | tag `vulkan-sdk-1.3.296.0`，commit **46ef757e048e760b46601e6e77ae0cb72c97bd2f**（实测版本号 **15.0.0**） | **M0-E 已建源码构建项目** `native/cmake/vulkan-shader-toolchain/`（钉 commit+SHA256 的 FetchContent；产物在 `native/build/vulkan-modernization/shader-toolchain/`，不入源目录） | Windows VS18 Release 构建 exit 0；现代 harness 三档全 PASS（1.3→**0x00010600**，spirv-val 匹配 PASS）；Android arm64 交叉编译 exit 0；**M0-F 已切换真实引擎链接**；Windows 引擎初始化/三档 shader module/销毁 PASS（含 Validation）；Android 无设备运行验证 |
| SPIRV-Tools | **6dcc7e350a0b9871a825414d42329e44b0eb8109** | 随 glslang 源码构建（M0-E，静态库 SPIRV-Tools/SPIRV-Tools-opt + link/lint/diff/reduce） | 精确 commit 来自该 glslang 的 `known_good.json`；Win/Android 均已构建 PASS |
| SPIRV-Headers | **2a9b6f951c7d6b04b6c21fe1bf3f475b68b84801** | 随 SPIRV-Tools 构建 | 同上游 `known_good.json`；与 Vulkan-Headers 为不同依赖 |

Lead 下载的 immutable commit codeload `tar.gz` SHA256（2026-10-02）：

| Archive | SHA256 |
|---|---|
| glslang | `ef16e5924e07f49b6280158645051ed5930ee5c5628e573fd2fb49e210aaca71` |
| SPIRV-Tools | `e3665378ab0ae8efb327027ee103d10c3c9e05823dea8b28893c00886ffe1a23` |
| SPIRV-Headers | `1698e1373bd6e59a263acef821c4d955c561b991feb6db8199833ef19ffe8a37` |
| VMA header | `f063056705f2cd0ddd8d9da034f673b09814ca96aa45a958e9e687bf9f3fed15` |

关键文件 sha256（vendored 快照）：

| 文件 | sha256 |
|---|---|
| `dependencies/Vulkan-Headers/include/vulkan/vulkan_core.h` | `50af5a157c8aab7d90dcd929a05758b4dc3e78a619f46552bfd5cde67b2d46c1` |
| `dependencies/Vulkan-Headers/include/vulkan/vulkan.h` | `52297513cc6b6cc8db2921aa849f72e3f55d678b3a58a2c3eb297c02ded7a837` |
| `dependencies/Vulkan-Headers/include/vulkan/vulkan_win32.h` | `fa3a9263d1764b82b634180ec5d3010adc216108bae11c02b157f733073f87f8` |
| `dependencies/Vulkan-Headers/include/vulkan/vulkan_android.h` | `f815593b7d20f291850b8d85e21e3d47fb123932805ba3606df631bdff4cb1e7` |
| `dependencies/Vulkan-Headers/include/vk_video/vulkan_video_codec_h264std.h` | `072ee6eac80e73d93bb4bfe0059f99e581fc8b55b1437236185bb4ff377914b1` |
| `gfx-vulkan/volk.c`（新） | `a7dd4404fa43f31602b9c657d9df0e9106ddf52e3df3e0d3d26d65bc267c5729` |
| `gfx-vulkan/volk.h`（新） | `0cde6969d9447e6043c867c324251ad362e4bea56d45be11301c53b829f94b74` |

**引擎接入进展（M0-F，Lead）**：Headers/Volk/VMA 与 shader 源码工具链已接入真实引擎。`CC_USE_VULKAN` 构建过滤旧 glslang 链接并优先使用新版安装头；SPIRVUtils 公共 API 与反射适配已完成。Windows engine smoke 构建/运行 exit 0，三档 × 三阶段生成及匹配 spirv-val 全 PASS；初始化/模块创建/销毁 Validation errors=0、VMA 0 bytes（进程内隔离宿主失效 layer）。仍有一条默认纹理 Best Practices 警告；不是画面、pipeline 执行、Android/XR 或性能验收。详见 PROGRESS §2F。

### 3.1 Candidate 总表

> 下表保留 M0-A/B 的候选调查记录，已被 §3.0 Chosen 决策取代；不表示当前仍未选型。

| Dependency | Candidate（候选方向，未验证） | Selected（已验证锁定） | 升级工件范围 | 约束/备注 |
|---|---|---|---|---|---|---|---|
| Vulkan-Headers | ~~候选中~~ → **Chosen**：tag `vulkan-sdk-1.3.296.0`（见 §3.0） | **pending（未完成引擎/平台验证）** | 已 vendored 至 `gfx-vulkan/dependencies/Vulkan-Headers/`；引擎 include 已在 M0-D 接入；平台 Gate 待补齐 | 引擎 contract 上限仍锁 1.3；dev 机 loader 1.4.341 / driver 1.4.351 不改变引擎上限 |
| Volk | ~~候选中~~ → **Chosen**：随 Headers 成套 tag `vulkan-sdk-1.3.296.0`（见 §3.0） | **pending（未完成引擎/平台验证）** | 已原位替换 `gfx-vulkan/volk.c/.h`（旧 179 无定制，无需保留适配） | 与 Headers 成套验证；loader smoke 与 Windows 引擎构建/有限运行验证均 PASS |
| VMA | GPUOpen 稳定版（3.0.1+ / 3.1.0 系） | **pending** | `native/cocos/renderer/gfx-vulkan/vk_mem_alloc.h` | 升级后需设置 `vulkanApiVersion` 并复核 `VKDevice.cpp:352-404` 手动函数表与新版 API 兼容性 |
| glslang | 升级路径二选一，**由 Lead 决策**：(a) 沿用 YunHsiao build 分支新构建；(b) 官方 Khronos release（dev 机 SDK 内为 16.2.0） | **pending** | win64 9 个 `.lib`（release+debug）+ android 4 ABI `.a` + mac/ios `.a` + `external/sources/glslang/` 头 | 锁定前验证条件：M0-B harness（`-DCC_GLSLANG_EXTERNAL_ROOT` 指向新树）`--all` exit=0，且 1.3 目标 version word 实测 `0x00010600`；需先确认 YunHsiao 分支相对官方的定制差异；预编译库在仓库内，不走包管理 |
| SPIRV-Tools | 与选定 glslang 成套同版本（捆绑于同一预编译包） | **pending** | 随 glslang 包 | 字符串线索示现捆绑版无 vulkan1.3 env（行为未验，确切 revision 待确认）；计划第 8 节 `spirv-val --target-env vulkan1.1/1.2/1.3` 的**外部工具验证**可用 dev 机 SDK spirv-val（v2026.1，已实测 1.1/1.2 PASS）完成，与引擎内捆绑库升级解耦 |
| Validation Layers | 开发期用 LunarG SDK 1.4.341.1 捆绑 layers（dev only，不入库） | **pending（策略待 Lead 定）** | 无仓库工件 | **本机发现（M0-C）**：注册表 `HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers` 指向不存在的 `C:\VulkanSDK\1.4.341.1\Bin`（实际 SDK 在 `D:\Software\VulkanSDK`），layer 系统级不可见；需设 `VK_LAYER_PATH=D:\Software\VulkanSDK\1.4.341.1\Bin` 才能被引擎/测试枚举（loader smoke 实测：不设→NOT AVAILABLE，设后→ENABLED errors=0）。修复注册表属系统变更，待用户/Lead 决策；Best Practices / Synchronization Validation 子特性开启属 M0 后续 |

### 3.2 升级验证要求（摘自计划第 6/8 节；本节为当前验收状态）

- [x] Windows build PASS（CC_USE_VULKAN=ON）〔真实 cocos_engine + engine smoke，Release + CC_DEBUG_FORCE，exit 0〕
- [x] Android arm64 build / final link PASS〔M0-G，NDK 26.1.10909125、android-26、c++_static；完整 cocos_engine + 导出探针，链接带 --no-undefined；不是设备运行或游戏打包〕
- [ ] XR build PASS（项目支持时）〔未执行；XR 设备信息待确认〕
- [x] 引擎真实 SPIRVUtils 编译三档 SPIR-V〔M0-F：1.1→0x00010300、1.2→0x00010500、1.3→0x00010600，各 vertex/fragment/compute；尚未覆盖真实低版本 GPU 协商〕
- [x] `spirv-val --target-env vulkan1.1` / `vulkan1.2` / `vulkan1.3` 全 PASS〔M0-F，引擎输出，SDK spirv-val v2026.1，9 个产物全部 PASS〕
- [ ] Validation 无新增错误；Renderer behavior unchanged〔未验证〕
- [x] Vulkan 1.3 core entry loading 在支持 1.3 的设备上 PASS〔M0-C，独立 loader smoke，RTX 4060 API 1.4.351；不等于现代 feature/command 执行或平台 Gate〕

---

## 4. 变更记录

| 日期 | 内容 |
|---|---|
| 2026-10-02 | M0-A 初版：Current baseline 六项全部核实；Candidate/Selected 全部 pending（未做任何依赖变更）。同日 M0-B 修正：①纠正"glslang 11.5 上游支持 1.3 目标"错误表述（枚举不存在，引擎 guard 回退，harness 实测）；②SPIRV-Tools 二进制字符串降级为线索、确切 revision 待确认；③Volk/Headers 改为成套验证表述，不再断言必须版本号相等；④VMA 移除"3.0 起必填"表述（改为默认 0 == VK_API_VERSION_1_0）；⑤平台验证能力改为待实际验证并注明 profiles 模拟边界；⑥新增 §1.1 harness 真实运行结果 |
| 2026-10-02 | M0-C：①Chosen 落地：Vulkan-Headers（vulkan-sdk-1.3.296.0，commit 29f979e…）vendored 至 `gfx-vulkan/dependencies/Vulkan-Headers/`（含 sha256 记录与内容核验）；volk（同 tag，commit 59d2690…）原位替换 volk.c/.h（旧 179 无 Cocos 定制，已报告）；②loader smoke 新目标三档入口加载全 PASS（含 1.3 六项 Modern 入口），validation errors=0；③harness 验证脚本加固 + 5 项负例实测；④发现本机 validation layer 注册表失效，VK_LAYER_PATH 规避；均未完成引擎/平台验证，不记 Selected |
| 2026-10-02 | M0-E（Worker）：新建 `native/cmake/vulkan-shader-toolchain/` 源码构建项目（glslang 46ef757e / SPIRV-Tools 6dcc7e35 / SPIRV-Headers 2a9b6f95，钉 commit + URL_HASH，本地缓存复用但 hash 同验）；Windows VS18 Release build+install exit 0；CC_BUILD_SPIRV_HARNESS 现代 harness 三档全 PASS（1.3→0x00010600，spirv-val 匹配 PASS，verify exit 0）；旧基线 harness 保持 1.3 UNSUPPORTED（exit 2 退化测试保留）；Android arm64（NDK 26.1.10909125/Ninja/android-26/c++_static）全库交叉编译 exit 0；安装清单/库名/链接顺序记入 install 树 lib-list.txt；引擎链接切换待 Lead。同轮 §3.0 状态更新为 M0-C/D/E 已落地（保留各时间点事实） |
| 2026-10-02 | M0-G（Worker）：Android 真实引擎集成证据补齐 —— 新增 `native/tests/vulkan-android-build/` 入口，arm64 全量 cocos_engine（CC_USE_VULKAN=ON）交叉编译 exit 0；`libvk_android_probe.so` 最终链接（whole-archive cocos_jni + cocos_engine）exit 0；链接线仅 8 个现代 shader 静态库（vulkan-shader-toolchain Android 专属 child，platform=Android），旧 external glslang/OGLCompiler 0 次；vendored Vulkan-Headers include 优先于 khronos；探针拉入 CCVKDevice/VKGPUContext/VKSwapchain/SPIRVUtils/DeviceManager 链（符号计数见 PROGRESS §2G）；无设备，仅编译/链接 PASS，不声明运行/Validation/画面 |
