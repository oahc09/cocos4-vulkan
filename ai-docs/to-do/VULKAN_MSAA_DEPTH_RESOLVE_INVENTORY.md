# Vulkan MSAA Depth/Stencil Resolve — Wiring Inventory

2026-10-03. Orca Worker initial read-only inventory; Lead corrected the
facts below against the workspace and Khronos specifications. This is a
wiring inventory, not proof of rendering correctness or a milestone Gate.

## Existing interfaces and contracts

- `FramebufferInfo` already has **both** `depthStencilTexture` and
  `depthStencilResolveTexture` (`GFXDef-common.h:1444-1445`).
  `VKFramebuffer.cpp:57-59` wires the latter to `gpuDepthStencilResolveView`;
  `VKCommands.cpp` appends that view to the Classic framebuffer.
  No new public framebuffer API is needed for this first tier.
- `VKRenderingAttachmentContract.h:44-49` already carries resolve mode,
  view and layout. `VKRenderingInfoContract.h:162-173` supports optional
  depth/stencil resolves and checks shared views when both aspects resolve.
  These CPU contracts do not query device support or prove image lifetime.
- Before the new M6-A5 wiring, the Dynamic candidate explicitly rejected
  D/S resolves; its existing D/S attachment path was **single-sample**.
  The color resolve transition covered only the 1x color target.

## Actual Classic filtering and local query evidence

- `VKCommands.cpp` filters **both** requested depth and stencil modes
  unconditionally. An unsupported mode (including NONE, whose numeric value
  is zero) becomes SAMPLE_ZERO. Then independentResolve/independentResolveNone
  may couple the modes. There is no CPU depth-only guard around stencil
  filtering. Preserve this existing behavior while saving its actual results
  for Dynamic; do not independently recompute a different policy.
- The corrected query-only probe reports RTX depth/stencil = **0x2f/0x2d**,
  AMD = **0xf/0xd**. SAMPLE_ZERO=1, AVERAGE=2, MIN=4, MAX=8:
  both depth masks contain AVERAGE/MIN/MAX; both stencil masks contain
  MIN/MAX and exclude AVERAGE. Earlier contrary interpretations are withdrawn.
- Both local GPUs report independentResolve=1 and independentResolveNone=1,
  and D32/D32S8 DS+TRANSFER_SRC 1x support. AMD D24S8 is unavailable.
  This is device-query evidence only; AMD has not rendered this workload.

## First narrowly scoped implementation

- Use four attachments: RGBA8 MS color at 0, RGBA8 1x color resolve at 1,
  D32 MS depth at 2, D32 1x depth resolve at 3. Subpass colors={0},
  resolves={1}, depthStencil=2, depthStencilResolve=3. Primary samples=2x/4x;
  resolve samples=1x. CLEAR/STORE primaries, DISCARD/STORE resolve targets.
- Request depth SAMPLE_ZERO and stencil NONE. Save Classic's actual filtered
  per-subpass values. Dynamic consumes the saved depth mode, with no stencil
  attachment. Combined D32S8 and other depth modes remain Classic.
- Reuse existing signature, PSO composer and attachment contracts. Verify real
  views/images, formats, sample counts, extent and ordinary offscreen ownership.
  Select the depth clear value at colorAttachments.size(), not fixed index 1.
- Add a separate owned depth-resolve pre/post transition. Prepare every
  contract before issuing BeginRendering; preserve balanced begin/end and
  recording leases. Synchronization scopes require specification review and
  real synchronization-validation evidence.
- Acceptance still needs true driver creation/draw/resolve/copy/submit chains,
  independent color/depth golden readbacks plus Classic/Dynamic comparison,
  negative controls, strict script criteria and Android compile/link regression.

## Specification boundaries

[VkSubpassDescriptionDepthStencilResolve](https://docs.vulkan.org/refpages/latest/refpages/source/VkSubpassDescriptionDepthStencilResolve.html)
requires an MS primary and 1x resolve target (03179/03180), depth-mode support
only when the resolve format has depth (03183), and stencil-mode support only
when it has stencil (03184). The independent-mode restrictions 03185/03186
apply to formats containing both aspects. This does not make the engine's
unconditional CPU stencil filter inactive for D32.

[VkPhysicalDeviceDepthStencilResolveProperties](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceDepthStencilResolveProperties.html)
requires SAMPLE_ZERO support for both aspects and forbids stencil AVERAGE.
Depth-only SAMPLE_ZERO is the first implementation choice, not a declaration
that every fallback or combined-aspect request is valid.

[Render Pass multisample resolve operations](https://docs.vulkan.org/spec/latest/chapters/renderpass.html#renderpass-resolve-operations)
places fixed-function resolves at COLOR_ATTACHMENT_OUTPUT, with COLOR_ATTACHMENT
access masks even for depth/stencil. The new transitions include these scopes
in addition to EARLY/LATE_FRAGMENT_TESTS D/S load/store/test accesses.
