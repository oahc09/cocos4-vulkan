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
#pragma once

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/Log.h"
#include "platform/FileUtils.h"
#include "rapidjson/document.h"

namespace cc::vulkan_test {

// Called before platform startup in the real Windows test project. No opt-in means no
// FileUtils mutation. This selects a complete Creator-produced package, never
// overlays a new cc.js on the old package or changes the normal Debug resources.
inline bool installScenePackage() noexcept {
    constexpr auto *ENV = L"CC_VULKAN_REAL_PROJECT_RESOURCE_ROOT";
    try {
        SetLastError(ERROR_SUCCESS);
        const DWORD length = GetEnvironmentVariableW(ENV, nullptr, 0);
        if (!length && GetLastError() == ERROR_ENVVAR_NOT_FOUND) return true;
        if (length < 2 || length > 32767) throw std::runtime_error("empty or oversized resource root");
        std::vector<wchar_t> text(length);
        const DWORD copied = GetEnvironmentVariableW(ENV, text.data(), length);
        if (copied != length - 1) throw std::runtime_error("resource root changed while reading");
        const std::filesystem::path requested(text.data());
        if (!requested.is_absolute()) throw std::runtime_error("resource root must be absolute");
        const auto root = std::filesystem::canonical(requested);
        if (!std::filesystem::is_directory(root)) throw std::runtime_error("resource root is not a directory");
        for (const auto *file : {"main.js", "application.js", "src/settings.json", "src/import-map.json", "src/cocos-js/cc.js", "assets/main/index.js"}) {
            if (!std::filesystem::is_regular_file(root / file) || !std::filesystem::file_size(root / file))
                throw std::runtime_error("incomplete scene package");
        }
        const auto settingsPath = root / "src/settings.json";
        if (std::filesystem::file_size(settingsPath) > 8 * 1024 * 1024) throw std::runtime_error("oversized settings");
        std::ifstream stream(settingsPath, std::ios::binary);
        if (!stream) throw std::runtime_error("cannot open settings");
        const std::string settings((std::istreambuf_iterator<char>(stream)), {});
        rapidjson::Document doc;
        doc.Parse(settings.data(), settings.size());
        if (doc.HasParseError() || !doc.IsObject() || !doc.HasMember("rendering") || !doc["rendering"].IsObject())
            throw std::runtime_error("invalid rendering settings");
        const auto &rendering = doc["rendering"];
        if (!rendering.HasMember("customPipeline") || !rendering["customPipeline"].IsBool() || !rendering["customPipeline"].GetBool())
            throw std::runtime_error("scene package requires customPipeline=true");
        if (!rendering.HasMember("effectSettingsPath") || !rendering["effectSettingsPath"].IsString())
            throw std::runtime_error("scene package has no effect layout data");
        const auto effectRelative = std::filesystem::u8path(rendering["effectSettingsPath"].GetString());
        if (effectRelative.empty() || effectRelative.is_absolute()) throw std::runtime_error("effect path must be package-relative");
        for (const auto &part : effectRelative) {
            if (part == "..") throw std::runtime_error("effect path escapes package");
        }
        const auto effect = std::filesystem::canonical(root / effectRelative);
        if (!std::filesystem::is_regular_file(effect) || !std::filesystem::file_size(effect))
            throw std::runtime_error("effect layout data is empty");
        auto *files = FileUtils::getInstance();
        std::unique_ptr<FileUtils> newlyCreated;
        if (!files) {
            // Windows createFileUtils() reuses the singleton. Engine::init()
            // adopts it in _fs and destroys it normally, as in the simulator.
            newlyCreated.reset(createFileUtils());
            files = newlyCreated.get();
        }
        if (!files) throw std::runtime_error("FileUtils initialization failed");
        // Make this the only search root: missing variant data must not silently
        // resolve to the old legacy package through the default-root fallback.
        const auto resourceRoot = root.generic_u8string() + "/";
        files->setDefaultResourceRootPath(resourceRoot);
        files->setSearchPaths({resourceRoot});
        if (std::filesystem::u8path(files->fullPathForFilename("src/settings.json")) != settingsPath)
            throw std::runtime_error("settings resolved to another package");
        CC_LOG_INFO("real-scene-package: custom=1 effectBytes=%llu root=%s", static_cast<unsigned long long>(std::filesystem::file_size(effect)), resourceRoot.c_str());
        newlyCreated.release();
        return true;
    } catch (const std::exception &error) {
        CC_LOG_ERROR("real-scene-package: REJECTED %s", error.what());
        return false;
    } catch (...) {
        CC_LOG_ERROR("real-scene-package: REJECTED unknown exception");
        return false;
    }
}

} // namespace cc::vulkan_test
