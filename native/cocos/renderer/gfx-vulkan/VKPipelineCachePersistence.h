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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#if defined(_WIN32)
    #include <windows.h>
#else
    #include <unistd.h>
#endif

namespace cc::gfx {

// Optional, bounded application envelope. Legacy version 1 is a cold miss.
// The checksum detects accidental damage, not hostile replacement. All fields
// (including Vulkan's 32-byte header) are decoded explicitly as little endian.
class VulkanPipelineCachePersistence final {
public:
    static constexpr size_t HEADER_SIZE = 48;
    static constexpr size_t MAX_PAYLOAD_SIZE = 64U * 1024U * 1024U;
    using Bytes = std::vector<uint8_t>;
    struct Identity {
        uint32_t driver = 0;
        uint32_t vendor = 0;
        uint32_t device = 0;
        std::array<uint8_t, VK_UUID_SIZE> uuid{};
    };

    static Identity identity(const VkPhysicalDeviceProperties &properties) noexcept {
        Identity result{properties.driverVersion, properties.vendorID, properties.deviceID, {}};
        for (size_t i = 0; i < VK_UUID_SIZE; ++i) result.uuid[i] = properties.pipelineCacheUUID[i];
        return result;
    }

    static uint32_t read32(const uint8_t *p) noexcept {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8U) | (uint32_t(p[2]) << 16U) | (uint32_t(p[3]) << 24U);
    }
    static void write32(uint8_t *p, uint32_t value) noexcept {
        for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(value >> (8U * i));
    }
    static bool validPayload(const uint8_t *data, size_t size, const Identity &id) noexcept {
        if (!data || size < 32 || size > MAX_PAYLOAD_SIZE || read32(data) != 32 ||
            read32(data + 4) != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
            read32(data + 8) != id.vendor || read32(data + 12) != id.device) return false;
        for (size_t i = 0; i < VK_UUID_SIZE; ++i)
            if (data[16 + i] != id.uuid[i]) return false;
        return true;
    }

    static bool encode(const Bytes &payload, const Identity &id, Bytes &out) noexcept {
        try {
            if (!validPayload(payload.data(), payload.size(), id)) {
                out.clear();
                return false;
            }
            Bytes bytes(HEADER_SIZE + payload.size());
            write32(bytes.data(), 0x4343564B);
            write32(bytes.data() + 4, 2);
            write32(bytes.data() + 8, id.driver);
            write32(bytes.data() + 12, id.vendor);
            write32(bytes.data() + 16, id.device);
            for (size_t i = 0; i < VK_UUID_SIZE; ++i) bytes[20 + i] = id.uuid[i];
            write32(bytes.data() + 36, static_cast<uint32_t>(payload.size()));
            const auto checksum = hash(payload.data(), payload.size());
            for (unsigned i = 0; i < 8; ++i) bytes[40 + i] = static_cast<uint8_t>(checksum >> (8U * i));
            std::copy(payload.begin(), payload.end(), bytes.begin() + HEADER_SIZE);
            out.swap(bytes);
            return true;
        } catch (...) {
            out.clear();
            return false;
        }
    }

    static bool decode(const Bytes &bytes, const Identity &id, Bytes &out) noexcept {
        try {
            if (bytes.size() < HEADER_SIZE || bytes.size() > HEADER_SIZE + MAX_PAYLOAD_SIZE ||
                read32(bytes.data()) != 0x4343564B || read32(bytes.data() + 4) != 2 ||
                read32(bytes.data() + 8) != id.driver || read32(bytes.data() + 12) != id.vendor ||
                read32(bytes.data() + 16) != id.device) {
                out.clear();
                return false;
            }
            for (size_t i = 0; i < VK_UUID_SIZE; ++i)
                if (bytes[20 + i] != id.uuid[i]) {
                    out.clear();
                    return false;
                }
            const auto size = read32(bytes.data() + 36);
            if (size != bytes.size() - HEADER_SIZE || !validPayload(bytes.data() + HEADER_SIZE, size, id)) {
                out.clear();
                return false;
            }
            uint64_t checksum = 0;
            for (unsigned i = 0; i < 8; ++i) checksum |= uint64_t(bytes[40 + i]) << (8U * i);
            if (checksum != hash(bytes.data() + HEADER_SIZE, size)) {
                out.clear();
                return false;
            }
            Bytes payload(bytes.begin() + HEADER_SIZE, bytes.end());
            out.swap(payload);
            return true;
        } catch (...) {
            out.clear();
            return false;
        }
    }

    static bool read(const std::filesystem::path &path, const Identity &id, Bytes &out) noexcept {
        out.clear();
        try {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            const auto size = stream.tellg();
            // Bound the actual file length BEFORE allocating or reading metadata.
            if (!stream || size < static_cast<std::streamoff>(HEADER_SIZE) ||
                size > static_cast<std::streamoff>(HEADER_SIZE + MAX_PAYLOAD_SIZE)) return false;
            Bytes bytes(static_cast<size_t>(size));
            stream.seekg(0);
            stream.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (!stream || stream.peek() != std::char_traits<char>::eof()) return false;
            return decode(bytes, id, out);
        } catch (...) {
            return false;
        }
    }

    static bool retrieve(VkDevice device, VkPipelineCache cache, PFN_vkGetPipelineCacheData get,
                         const Identity &id, Bytes &out) noexcept {
        out.clear();
        if (!device || !cache || !get) return false;
        try {
            for (unsigned attempt = 0; attempt < 3; ++attempt) {
                size_t size = 0;
                if (get(device, cache, &size, nullptr) != VK_SUCCESS || size < 32 || size > MAX_PAYLOAD_SIZE) return false;
                Bytes data(size);
                const size_t capacity = size;
                const auto result = get(device, cache, &size, data.data());
                if (result == VK_INCOMPLETE) continue;
                if (result != VK_SUCCESS || size > capacity || !validPayload(data.data(), size, id)) return false;
                data.resize(size);
                out.swap(data);
                return true;
            }
        } catch (...) {
        }
        return false;
    }

    // Serialize first, write/flush/close a unique sibling, then atomically
    // replace. A failed write/rename keeps the previous destination intact.
    // No promise of POSIX power-loss durability (no directory fsync).
    static bool write(const std::filesystem::path &path, const Identity &id, const Bytes &payload) noexcept {
        std::filesystem::path temporary;
        try {
            Bytes bytes;
            if (!encode(payload, id, bytes)) return false;
            static std::atomic<uint64_t> sequence{0};
#if defined(_WIN32)
            const auto pid = GetCurrentProcessId();
#else
            const auto pid = getpid();
#endif
            temporary = path;
            temporary += ".tmp-" + std::to_string(pid) + "-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                         std::to_string(sequence.fetch_add(1));
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
                return false;
            }
            stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            stream.flush();
            const bool written = static_cast<bool>(stream);
            stream.close();
            bool replaced = false;
            if (written && !stream.fail()) {
#if defined(_WIN32)
                replaced = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
                replaced = std::rename(temporary.c_str(), path.c_str()) == 0;
#endif
            }
            if (!replaced) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
            }
            return replaced;
        } catch (...) {
            if (!temporary.empty()) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
            }
            return false;
        }
    }

private:
    static uint64_t hash(const uint8_t *data, size_t size) noexcept {
        uint64_t value = 14695981039346656037ULL;
        for (size_t i = 0; i < size; ++i) {
            value ^= data[i];
            value *= 1099511628211ULL;
        }
        return value;
    }
};
} // namespace cc::gfx
