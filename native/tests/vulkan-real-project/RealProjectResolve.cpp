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

#include "RealProjectResolve.h"
#include "RealProjectScopeObservation.h"
#include <windows.h>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include "base/Log.h"
#include "base/threading/MessageQueue.h"
#include "gfx-agent/DeviceAgent.h"
#include "renderdoc/renderdoc_app.h"
namespace cc::vulkan_test {
namespace {
bool prepared = false, enabled = false, started = false, valid = false, scopes = false;
std::string mode;
unsigned samples = 2;
bool depth = false;
int result = 1;
bool present(const wchar_t *name) {
    SetLastError(ERROR_SUCCESS);
    return GetEnvironmentVariableW(name, nullptr, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
}
void run() noexcept {
    started = true;
    RENDERDOC_API_1_4_1 *api = nullptr;
    bool capturing = false;
    try {
        if (present(L"CC_VULKAN_REAL_PROJECT_RESOLVE_CAPTURE")) {
            auto module = GetModuleHandleW(L"renderdoc.dll");
            auto get = module ? reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI")) : nullptr;
            const auto *path = std::getenv("CC_VULKAN_REAL_PROJECT_RESOLVE_CAPTURE");
            if (!get || !path || !*path || !get(eRENDERDOC_API_Version_1_4_1, reinterpret_cast<void **>(&api)) || !api || api->IsFrameCapturing()) throw std::runtime_error("capture API");
            api->SetCaptureFilePathTemplate(path);
            api->StartFrameCapture(nullptr, nullptr);
            capturing = api->IsFrameCapturing();
            if (!capturing) throw std::runtime_error("capture start");
        }
        RealProjectScopeObservation observation(scopes);
        result = depth ? runDepthResolve(samples, mode == "skip-depth") : runColorResolve(samples, mode == "skip-color", mode == "wrong-uniform");
        if (!observation.finish()) result = 1;
        if (capturing) {
            const auto ok = api->EndFrameCapture(nullptr, nullptr);
            capturing = false;
            if (!ok) throw std::runtime_error("capture end");
            CC_LOG_INFO("RenderDoc resolve capture: started=1 ended=1");
        }
    } catch (const std::exception &e) {
        CC_LOG_ERROR("project-resolve failure: %s", e.what());
        result = 1;
    } catch (...) {
        CC_LOG_ERROR("project-resolve failure: exception");
        result = 1;
    }
    if (capturing) api->DiscardFrameCapture(nullptr, nullptr);
}
} // namespace
bool prepareResolve() noexcept {
    if (prepared) return !enabled || valid;
    prepared = true;
    enabled = present(L"CC_VULKAN_REAL_PROJECT_RESOLVE");
    scopes = present(L"CC_VULKAN_REAL_PROJECT_SCOPES");
    if (scopes && (!enabled || !std::getenv("CC_VULKAN_REAL_PROJECT_SCOPES") || std::strcmp(std::getenv("CC_VULKAN_REAL_PROJECT_SCOPES"), "post") != 0)) {
        CC_LOG_ERROR("project-resolve failure: scopes require resolve and post mode");
        return false;
    }
    if (!enabled) {
        CC_LOG_INFO("project-resolve mode: off enabled=0");
        return true;
    }
    try {
        const auto *value = std::getenv("CC_VULKAN_REAL_PROJECT_RESOLVE");
        mode = value ? value : "";
        valid = mode == "color2" || mode == "color4" || mode == "depth2" || mode == "depth4" || mode == "skip-color" || mode == "skip-depth" || mode == "wrong-uniform";
        for (const auto *n : {L"CC_VULKAN_REAL_PROJECT_PREWARM", L"CC_VULKAN_REAL_PROJECT_CACHE_PATH", L"CC_VULKAN_REAL_PROJECT_INDIRECT", L"CC_VULKAN_REAL_PROJECT_SUBPASS", L"CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT"}) valid &= !present(n);
        if (!valid) {
            CC_LOG_ERROR("project-resolve failure: unknown/empty/conflicting mode");
            return false;
        }
        const auto *forcedApi = std::getenv("CC_VULKAN_FORCE_API");
        const auto *rendering = std::getenv("CC_VULKAN_RENDERING");
        if (forcedApi && rendering && (std::string(forcedApi) == "1.1" || std::string(forcedApi) == "1.2") && std::string(rendering) == "dynamic") {
            valid = false;
            CC_LOG_ERROR("project-resolve failure: forced low API conflicts with Dynamic policy");
            return false;
        }
        depth = mode == "depth2" || mode == "depth4" || mode == "skip-depth";
        samples = (mode == "color4" || mode == "depth4") ? 4 : 2;
        CC_LOG_INFO("project-resolve mode: %s enabled=1", mode.c_str());
        return true;
    } catch (...) {
        valid = false;
        return false;
    }
}
void startResolve() noexcept {
    if (!enabled || !valid) return;
    if (started) {
        result = 1;
        CC_LOG_ERROR("project-resolve failure: start once");
        return;
    }
    if (auto *a = gfx::DeviceAgent::getInstance()) {
        auto *q = a->getMessageQueue();
        if (!q) {
            result = 1;
            return;
        }
        ENQUEUE_MESSAGE_0(q, RealProjectResolve, { run(); });
        q->kickAndWait();
    } else
        run();
}
int finishResolve(int platformResult) noexcept {
    if (!enabled) return platformResult;
    if (!started) return 1;
    const auto final = depth ? finishDepthResolve(platformResult, result) : finishColorResolve(platformResult, result);
    CC_LOG_INFO("project-resolve completed: kind=%s samples=%u diagnosticResult=%d platformResult=%d result=%d outcome=%s", depth ? "depth" : "color", samples, result, platformResult, final, final == 0 ? "CLEAN" : final == 3 ? "UNSUPPORTED"
                                                                                                                                                                                                                                 : "FAILED");
    return final;
}
} // namespace cc::vulkan_test
