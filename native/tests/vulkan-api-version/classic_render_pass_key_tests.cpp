#include "VKClassicRenderPassKey.h"

#if defined(VKCLASSICKEY_STANDALONE_PROBE)
int main() { return 0; }
#else
    #include <cstdio>
    #include <cstdlib>
    #include <new>
    #include <unordered_map>

using Key = cc::gfx::VulkanClassicRenderPassKey;
static bool counting = false;
static size_t allocs = 0, failAt = 0, injections = 0;
void *operator new(size_t size) {
    if (counting && ++allocs == failAt) {
        ++injections;
        throw std::bad_alloc();
    }
    if (void *p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, size_t) noexcept { std::free(p); }

static unsigned failures = 0;
static void check(bool yes, const char *label) {
    if (!yes) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
struct Fixture {
    VkAttachmentDescription2 attachment{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    VkAttachmentReference2 color{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    VkAttachmentReference2 unused{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    VkSubpassDependency2 dependency{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
    VkRenderPassCreateInfo2 root{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    Fixture() {
        attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_4_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.attachment = 0;
        color.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        color.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        unused.attachment = VK_ATTACHMENT_UNUSED;
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependency.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
        root.attachmentCount = 1;
        root.pAttachments = &attachment;
        root.subpassCount = 1;
        root.pSubpasses = &subpass;
        root.dependencyCount = 1;
        root.pDependencies = &dependency;
    }
    Fixture(const Fixture &) = delete;
};
static void verifyReset(const Key &key) {
    check(key == Key{} && !key.valid() && key.subpassCount() == 0 && key.words().empty(), "full reset");
}
int main() {
    Fixture f;
    Key expected;
    check(Key::build(f.root, expected), "baseline build");
    const std::vector<uint32_t> manual = {0, 1, 0, 37, 4, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0xffffffffU, 0, 0, 0, 0, 1,
                                          0xffffffffU, 0, 0x400, 0x80, 0x100, 0x10, 1, 0, 0};
    check(expected.valid() && expected.subpassCount() == 1 && expected.words() == manual, "independent full word stream");
    check(expected.hash() == 0xfac41a7fb4369aefULL, "independent baseline FNV vector");
    check(Key{}.hash() == 0xf26d36715c6423d1ULL, "independent invalid FNV vector");

    // All omitted layout and load/store fields can change independently.
    for (unsigned field = 0; field < 7; ++field) {
        Fixture changed;
        switch (field) {
            case 0: changed.attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
            case 1: changed.attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; break;
            case 2: changed.attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; break;
            case 3: changed.attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; break;
            case 4: changed.attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL; break;
            case 5: changed.attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; break;
            case 6: changed.color.layout = VK_IMAGE_LAYOUT_GENERAL; break;
        }
        Key key;
        check(Key::build(changed.root, key) && key == expected && key.hash() == expected.hash(), "ignored field equality");
    }
    for (unsigned field = 0; field < 8; ++field) {
        Fixture changed;
        switch (field) {
            case 0: changed.attachment.format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
            case 1: changed.attachment.samples = VK_SAMPLE_COUNT_2_BIT; break;
            case 2: changed.attachment.flags = VK_ATTACHMENT_DESCRIPTION_MAY_ALIAS_BIT; break;
            case 3: changed.subpass.viewMask = 3; break;
            case 4: changed.color.attachment = VK_ATTACHMENT_UNUSED; break;
            case 5: changed.dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT; break;
            case 6: changed.dependency.viewOffset = -1; break;
            case 7: changed.root.flags = 1; break; // opaque field shape, not Vulkan validity
        }
        Key key;
        check(Key::build(changed.root, key) && key != expected && key.hash() != expected.hash(), "retained field mutation");
    }
    Key owned;
    {
        Fixture caller;
        check(Key::build(caller.root, owned), "scoped owning build");
        caller.attachment.format = VK_FORMAT_D32_SFLOAT;
        caller.color.attachment = VK_ATTACHMENT_UNUSED;
    }
    check(owned == expected && owned.words() == manual, "caller destruction preserves all words");
    Key other = owned;
    check(other.words().data() != owned.words().data() && other == owned, "copy independent words");

    // Typed extensions are canonicalized into a fixed order, never pointer bytes.
    VkSubpassDescriptionDepthStencilResolve ds{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    VkFragmentShadingRateAttachmentInfoKHR fsr{VK_STRUCTURE_TYPE_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
    ds.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    ds.pDepthStencilResolveAttachment = &f.unused;
    fsr.pFragmentShadingRateAttachment = &f.unused;
    fsr.shadingRateAttachmentTexelSize = {2, 4};
    f.subpass.pNext = &ds;
    ds.pNext = &fsr;
    Key extensions;
    check(Key::build(f.root, extensions), "both extensions build");
    f.subpass.pNext = &fsr;
    fsr.pNext = &ds;
    ds.pNext = nullptr;
    Key reversed;
    check(Key::build(f.root, reversed) && reversed == extensions, "extension order canonical");
    ds.stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    check(Key::build(f.root, reversed) && reversed != extensions, "resolve mode retained conservatively");
    ds.stencilResolveMode = VK_RESOLVE_MODE_NONE;
    fsr.shadingRateAttachmentTexelSize.height = 8;
    check(Key::build(f.root, reversed) && reversed != extensions, "FSR dimensions retained");
    f.subpass.pNext = nullptr;

    // Each malformed shape primes a valid tuple, clears fully, then recovers.
    for (unsigned bad = 0; bad < 14; ++bad) {
        Fixture broken;
        VkBaseInStructure foreign{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        switch (bad) {
            case 0: broken.root.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; break;
            case 1: broken.root.pNext = &foreign; break;
            case 2: broken.root.pAttachments = nullptr; break;
            case 3: broken.root.pSubpasses = nullptr; break;
            case 4: broken.root.subpassCount = 0; break;
            case 5: broken.root.attachmentCount = Key::MAX_ATTACHMENTS + 1; break;
            case 6: broken.attachment.samples = static_cast<VkSampleCountFlagBits>(3); break;
            case 7: broken.attachment.format = VK_FORMAT_UNDEFINED; break;
            case 8: broken.color.attachment = 1; break;
            case 9: broken.color.pNext = &foreign; break;
            case 10: broken.subpass.pNext = &foreign; break;
            case 11: broken.subpass.pColorAttachments = nullptr; break;
            case 12: broken.root.pDependencies = nullptr; break;
            case 13: broken.dependency.dstSubpass = 1; break;
        }
        Key key = extensions;
        check(!Key::build(broken.root, key), "bad shape refused");
        verifyReset(key);
        check(Key::build(f.root, key) && key == expected, "bad shape full recovery");
    }
    // Cycle is rejected within the two-node cap.
    ds.pNext = &ds;
    f.subpass.pNext = &ds;
    check(!Key::build(f.root, reversed), "extension cycle refused");
    verifyReset(reversed);
    f.subpass.pNext = nullptr;

    Fixture empty;
    empty.root.attachmentCount = 0;
    empty.root.pAttachments = nullptr;
    empty.subpass.colorAttachmentCount = 0;
    empty.subpass.pColorAttachments = nullptr;
    empty.root.dependencyCount = 0;
    empty.root.pDependencies = nullptr;
    check(Key::build(empty.root, other) && other.valid() && other.subpassCount() == 1 && other != expected, "attachmentless valid key");
    empty.subpass.pDepthStencilAttachment = &empty.unused;
    Key nullEquivalent;
    check(Key::build(empty.root, nullEquivalent) && nullEquivalent == other, "null and UNUSED depth canonical");
    check(owned == expected, "rebuilding independent owner preserves original");

    // Subpass count, attachment order, preserve slots and correlation values survive.
    {
        VkAttachmentDescription2 attachments[2] = {f.attachment, f.attachment};
        attachments[1].format = VK_FORMAT_R16G16B16A16_SFLOAT;
        VkAttachmentReference2 colors[2] = {f.color, f.color};
        colors[1].attachment = 1;
        VkSubpassDescription2 passes[2] = {f.subpass, f.subpass};
        passes[1].pColorAttachments = &colors[1];
        auto root = f.root;
        root.attachmentCount = 2;
        root.pAttachments = attachments;
        root.subpassCount = 2;
        root.pSubpasses = passes;
        Key graph;
        check(Key::build(root, graph) && graph.subpassCount() == 2 && graph != expected, "two-subpass graph");
        passes[1].viewMask = 3;
        Key changed;
        check(Key::build(root, changed) && changed != graph, "second subpass view mask retained");
        passes[1].viewMask = 0;
        uint32_t preserve = 1;
        passes[0].preserveAttachmentCount = 1;
        passes[0].pPreserveAttachments = &preserve;
        check(Key::build(root, changed) && changed != graph, "preserve references retained");
        Key preserveOne = changed;
        preserve = 0;
        check(Key::build(root, changed) && changed != preserveOne, "preserve slot mutation");
        passes[0].preserveAttachmentCount = 0;
        uint32_t masks[2] = {1, 2};
        root.correlatedViewMaskCount = 2;
        root.pCorrelatedViewMasks = masks;
        check(Key::build(root, changed) && changed != graph, "correlation masks retained");
        Key firstMasks = changed;
        masks[1] = 4;
        check(Key::build(root, changed) && changed != firstMasks, "correlation mask mutation");
        root.correlatedViewMaskCount = 0;
        passes[0].colorAttachmentCount = 2;
        passes[0].pColorAttachments = colors;
        check(Key::build(root, changed), "two ordered color slots");
        Key ordered = changed;
        std::swap(colors[0], colors[1]);
        check(Key::build(root, changed) && changed != ordered, "color slot order retained");
        check(graph.subpassCount() == 2, "old graph snapshot retained");
    }

    struct ZeroHash {
        size_t operator()(const Key &) const { return 0; }
    };
    std::unordered_map<Key, int, ZeroHash> map;
    map.emplace(expected, 1);
    map.emplace(other, 2);
    check(map.size() == 2 && map.find(expected) != map.end() && map.find(other) != map.end() &&
              map.find(extensions) == map.end(),
          "full equality under forced hash collision");

    size_t points = 0;
    bool built = false;
    Key fresh;
    allocs = failAt = injections = 0;
    counting = true;
    built = Key::build(f.root, fresh);
    counting = false;
    points = allocs;
    check(built && points > 0 && fresh == expected, "actual allocation points measured");
    for (size_t n = 1; n <= points; ++n) {
        Key key = extensions; // prime outside injection
        allocs = injections = 0;
        failAt = n;
        counting = true;
        built = Key::build(f.root, key);
        counting = false;
        failAt = 0;
        check(!built && injections == 1, "each allocation injected exactly once");
        verifyReset(key);
        check(Key::build(f.root, key) && key == expected, "allocation recovery");
    }
    std::printf("classic render pass allocation points: %zu; failures=%u\n", points, failures);
    return failures ? 1 : 0;
}
#endif
