#include "VKPipelineCachePersistence.h"

#include <cstdio>
#include <limits>

using Store = cc::gfx::VulkanPipelineCachePersistence;
static int failures = 0;
static void check(bool value, const char *message) {
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}
static Store::Identity id{77, 0x10DE, 0x1234, {1, 3, 5, 7}};
static Store::Bytes raw() {
    Store::Bytes bytes(64, 0xA5);
    Store::write32(bytes.data(), 32);
    Store::write32(bytes.data() + 4, 1);
    Store::write32(bytes.data() + 8, id.vendor);
    Store::write32(bytes.data() + 12, id.device);
    std::copy(id.uuid.begin(), id.uuid.end(), bytes.begin() + 16);
    return bytes;
}
static int queryCalls = 0, readCalls = 0, mode = 0;
static VKAPI_ATTR VkResult VKAPI_CALL getData(VkDevice, VkPipelineCache, size_t *size, void *data) {
    if (!data) {
        ++queryCalls;
        if (mode == 1) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *size = mode == 2 ? Store::MAX_PAYLOAD_SIZE + 1 : 80;
        return VK_SUCCESS;
    }
    ++readCalls;
    if (mode == 3 || (mode == 4 && readCalls == 1)) return VK_INCOMPLETE;
    if (mode == 5) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (mode == 6) {
        *size += 1;
        return VK_SUCCESS;
    }
    auto bytes = raw();
    std::copy(bytes.begin(), bytes.end(), static_cast<uint8_t *>(data));
    *size = bytes.size(); // legitimate smaller result than queried capacity
    if (mode == 7) static_cast<uint8_t *>(data)[0] = 0;
    return VK_SUCCESS;
}
int main() {
    const auto payload = raw();
    Store::Bytes encoded, decoded;
    check(Store::encode(payload, id, encoded), "encode valid driver payload");
    check(Store::decode(encoded, id, decoded) && decoded == payload, "lossless roundtrip");
    const auto good = encoded;
    const auto reject = [&](Store::Bytes bytes, const Store::Identity &identity) {
        Store::Bytes output{9, 8, 7};
        check(!Store::decode(bytes, identity, output) && output.empty(), "reject damaged/incompatible data, clear stale output");
    };
    // Change each metadata family independently, retaining valid payload.
    for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(16), size_t(20), size_t(36), size_t(40), good.size() - 1}) {
        auto bad = good;
        bad[offset] ^= 1;
        reject(bad, id);
    }
    for (uint32_t version : {0U, 1U, 3U, 0xFFFFFFFFU}) {
        auto bad = good;
        Store::write32(bad.data() + 4, version);
        reject(bad, id);
    }
    for (uint32_t size : {0U, 31U, 63U, 65U, 0xFFFFFFFFU}) {
        auto bad = good;
        Store::write32(bad.data() + 36, size);
        reject(bad, id);
    }
    for (size_t length = 0; length < good.size(); ++length) reject(Store::Bytes(good.begin(), good.begin() + length), id);
    auto trailing = good;
    trailing.push_back(0);
    reject(trailing, id);
    auto other = id;
    ++other.driver;
    reject(good, other);
    other = id;
    ++other.vendor;
    reject(good, other);
    other = id;
    ++other.device;
    reject(good, other);
    other = id;
    other.uuid.back() ^= 1;
    reject(good, other);
    for (size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(16)}) {
        auto bad = payload;
        bad[offset] ^= 1;
        Store::Bytes output{1};
        check(!Store::encode(bad, id, output) && output.empty(), "do not publish invalid Vulkan header");
    }
    // Alias-safe conversion: encode/decode publish only after their input is consumed.
    auto alias = payload;
    check(Store::encode(alias, id, alias) && alias == good, "encode self-input");
    check(Store::decode(alias, id, alias) && alias == payload, "decode self-input");

    const auto device = reinterpret_cast<VkDevice>(uintptr_t(1));
    const auto cache = reinterpret_cast<VkPipelineCache>(uintptr_t(2));
    for (mode = 0; mode <= 7; ++mode) {
        queryCalls = readCalls = 0;
        decoded = {99};
        const auto result = Store::retrieve(device, cache, getData, id, decoded);
        check(result == (mode == 0 || mode == 4), "retrieval result classification");
        check(result ? decoded == payload : decoded.empty(), "retrieval never publishes partial data");
        if (mode == 3) check(queryCalls == 3 && readCalls == 3, "INCOMPLETE retry bounded at three");
        if (mode == 4) check(queryCalls == 2 && readCalls == 2, "INCOMPLETE then fresh successful query");
        if (mode == 1 || mode == 2) check(readCalls == 0, "failed/oversized query never allocates driver output");
    }
    check(!Store::retrieve(device, cache, nullptr, id, decoded), "null dispatcher rejected");
    check(!Store::retrieve(VK_NULL_HANDLE, cache, getData, id, decoded), "null device rejected");
    check(!Store::retrieve(device, VK_NULL_HANDLE, getData, id, decoded), "null cache rejected");

    // Isolated real filesystem tests: remove only explicitly created files.
    const auto folder = std::filesystem::current_path() / ("cache-contract-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(folder);
    const auto path = folder / std::filesystem::u8path(u8"缓存.bin");
    check(!Store::read(path, id, decoded) && decoded.empty(), "missing file is cold miss");
    check(Store::write(path, id, payload), "atomic first save");
    check(Store::read(path, id, decoded) && decoded == payload, "actual file roundtrip and Unicode path");
    auto replacement = payload;
    replacement.back() ^= 3;
    check(Store::write(path, id, replacement), "atomic replace existing file");
    check(Store::read(path, id, decoded) && decoded == replacement, "replace publishes latest whole payload");
    check(!Store::write(path, id, Store::Bytes{0}), "invalid save rejected");
    check(Store::read(path, id, decoded) && decoded == replacement, "failed encode preserves old file");
#if defined(_WIN32)
    HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    check(lock != INVALID_HANDLE_VALUE, "lock destination against atomic replacement");
    if (lock != INVALID_HANDLE_VALUE) {
        check(!Store::write(path, id, payload), "rename failure reported");
        CloseHandle(lock);
        check(Store::read(path, id, decoded) && decoded == replacement, "rename failure preserves previous cache");
    }
#endif
    check(std::distance(std::filesystem::directory_iterator(folder), std::filesystem::directory_iterator{}) == 1,
          "failed replacement leaves no temporary sibling");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char *>(good.data()), 10);
    }
    check(!Store::read(path, id, decoded) && decoded.empty(), "truncated real file rejected");
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.seekp(Store::MAX_PAYLOAD_SIZE + Store::HEADER_SIZE);
        file.put(0);
    }
    check(!Store::read(path, id, decoded) && decoded.empty(), "oversized sparse file rejected before allocation");
    std::filesystem::remove(path);
    std::filesystem::remove(folder);
    std::printf("pipeline-cache contract: failures=%d\n", failures);
    return failures ? 1 : 0;
}
