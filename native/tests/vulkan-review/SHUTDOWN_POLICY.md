# WSI failure and final shutdown policy

## Recoverable lifecycle operations

Acquire, resize, image abandonment, surface destruction, and public swapchain
destruction use finite waits. A failed submit or wait does not count as normal
completion. Ownership, the original semaphore pairing, pending presentation
fences, and an already-submitted drain point survive for retry. Public swapchain
destruction transfers its owning lease to the device before the facade disappears.
Ordinary acquire/present calls also retry deferred surface and swapchain cleanup.

A successful submission's binary wait must not be submitted again after a host
wait timeout. The retry waits the same completion point. A failed OOM submission
is different: Vulkan guarantees that its referenced state is unchanged, so it may
be retried after keeping the original recordings and semaphore pairing.

## Device loss

Device loss is a terminal device state, not successful rendering. In particular,
a **submit** returning `VK_ERROR_DEVICE_LOST` does not prove quiescence. The code
first latches device loss and obtains a device-idle wait result before destroying
possibly submitted drain fences or replacement semaphores. An unexpected idle
error retains those owners for another teardown attempt. No normal completion
counter is advanced on device loss. Each pending presentation fence is checked
separately; one fence result does not stand in for the other fences.

## Final device shutdown

The existing `Device::destroy()` / `doDestroy()` API returns `void` and is part
of a synchronous shutdown contract. This change preserves that contract rather
than widening the Engine, Device, Agent, and ownership APIs. Final device shutdown
explicitly waits without a finite timeout, including pending presentation fences,
and retains the existing device-idle wait. A hung GPU can therefore delay final
shutdown. This is not used to pace ordinary frames or recover resize timeouts.

If final teardown still returns an error other than success or device loss, it
logs and terminates rather than destroying resources whose use is unproven.
This explicit behavior also applies in release builds. A generation reaching
its last reference without going through the retirement owner is treated as an
ownership invariant violation. Neither path is a replacement for normal
recoverable timeout handling.

## Specification basis

- [Queue submission failures](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueSubmit.html): OOM preserves input state; device loss does not make that guarantee.
- [Lost device](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html#devsandqueues-lost-device): distinguish the semantics of submission from those of completion waits.
- [Fence wait results](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForFences.html): timeout or OOM is not completion.
- [Swapchain image release](https://docs.vulkan.org/refpages/latest/refpages/source/vkReleaseSwapchainImagesKHR.html): release only after image use has completed; surface loss needs retirement.
- [Present fences](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html): presentation-completion requirements are separate from submission completion.

Core WSI without present fences retains the existing conventional device-idle
retirement fallback. This does not claim that core WSI has gained a normative
host presentation-completion primitive.
