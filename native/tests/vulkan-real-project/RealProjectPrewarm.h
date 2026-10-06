// Explicit diagnostic integration for the real Windows test project.
// prepare() precedes Engine::init; start() runs at ON_START, before scripts;
// finish() follows the platform's normal engine/device shutdown.
#pragma once

namespace cc::vulkan_test {
bool prepare() noexcept;
void start() noexcept;
int finish(int platformResult) noexcept;
}
