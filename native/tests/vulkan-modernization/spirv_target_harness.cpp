// ============================================================================
// SPIR-V target-env verification harness (M0-B)
//
// Purpose: prove, with the SAME bundled glslang static libraries and the SAME
// compile flow the engine uses (cocos/renderer/gfx-base/SPIRVUtils.cpp), which
// SPIR-V version is actually produced for each requested Vulkan target:
//
//   Vulkan 1.1 -> client EShTargetVulkan_1_1, SPV target 1.3 (word 0x00010300)
//   Vulkan 1.2 -> client EShTargetVulkan_1_2, SPV target 1.5 (word 0x00010500)
//   Vulkan 1.3 -> client EShTargetVulkan_1_3, SPV target 1.6 (word 0x00010600)
//
// NO silent fallback: if the bundled glslang cannot express the requested
// target (engine guard CC_GLSLANG_VERSION_GREATOR_OR_EQUAL_TO(11,10,0) not
// satisfied), the target is reported UNSUPPORTED and never substituted.
//
// Exit codes:
//   0  = every requested target compiled AND its SPIR-V header version word
//        matched the expectation (real PASS only)
//   1  = at least one shader FAILED (parse/link/emit or version mismatch)
//   2  = at least one target UNSUPPORTED by bundled glslang (no FAIL)
//   64 = usage error
// ============================================================================

#include <glslang/build_info.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Dual-API adapter: this single source serves BOTH the legacy bundled glslang
// 11.5 (old external tree) and the source-built modern toolchain (>= 12.2).
// No stubs, no fake resources - each path uses the real upstream API.
#if __has_include(<glslang/Public/ResourceLimits.h>)
    #include <glslang/Public/ResourceLimits.h> // modern: GetDefaultResources()
    #define CC_HARNESS_MODERN_RESOURCES 1
#else
    #include <glslang/StandAlone/ResourceLimits.h> // bundled 11.5: DefaultTBuiltInResource
#endif

#if __has_include(<SPIRV/GlslangToSpv.h>)
    #include <SPIRV/GlslangToSpv.h> // modern layout (glslang >= 15: top-level SPIRV/)
    #include <SPIRV/Logger.h>
#else
    #include <glslang/SPIRV/GlslangToSpv.h> // legacy layout (bundled 11.5)
    #include <glslang/SPIRV/Logger.h>
#endif

#if __has_include(<spirv/unified1/spirv.h>)
    #include <spirv/unified1/spirv.h> // SPIRV-Headers layout (source-built tree)
#else
    #include "spirv/spirv.h" // legacy bundled khronos/spirv layout
#endif

#include "glslang/Public/ShaderLang.h"

// Same shape as engine macro CC_GLSLANG_VERSION_GREATOR_OR_EQUAL_TO
// (native/cocos/renderer/gfx-base/SPIRVUtils.cpp:44).
#define HARNESS_GLSLANG_AT_LEAST(major, minor, patch)                       \
    (((major) < GLSLANG_VERSION_MAJOR) || ((major) == GLSLANG_VERSION_MAJOR && \
     (((minor) < GLSLANG_VERSION_MINOR) || ((minor) == GLSLANG_VERSION_MINOR && \
      ((patch) <= GLSLANG_VERSION_PATCH)))))

#if HARNESS_GLSLANG_AT_LEAST(11, 10, 0)
    #define HARNESS_HAS_VULKAN_1_3_TARGET 1
#else
    #define HARNESS_HAS_VULKAN_1_3_TARGET 0
#endif

namespace {

constexpr int kExitPass         = 0;
constexpr int kExitFail         = 1;
constexpr int kExitUnsupported  = 2;
constexpr int kExitUsage        = 64;

struct TargetSpec {
    const char *name;          // "1.1" ...
    int vulkanMinor;           // 1/2/3
    uint32_t expectedWord;     // expected SPIR-V header word 1
    glslang::EShTargetClientVersion clientVersion;
    glslang::EShTargetLanguageVersion spvVersion;
};

#if HARNESS_HAS_VULKAN_1_3_TARGET
constexpr glslang::EShTargetClientVersion kClientVulkan13 = glslang::EShTargetVulkan_1_3;
constexpr glslang::EShTargetLanguageVersion kSpv16 = glslang::EShTargetSpv_1_6;
#endif

// Mirrors engine getClientVersion()/getTargetVersion() (SPIRVUtils.cpp:60-98)
// but WITHOUT the <11.10 fallback substitution: unsupported is reported as-is.
bool makeTargetSpec(int vulkanMinor, TargetSpec *out) {
    switch (vulkanMinor) {
        case 1:
            *out = TargetSpec{"1.1", 1, 0x00010300u,
                              glslang::EShTargetVulkan_1_1, glslang::EShTargetSpv_1_3};
            return true;
        case 2:
            *out = TargetSpec{"1.2", 2, 0x00010500u,
                              glslang::EShTargetVulkan_1_2, glslang::EShTargetSpv_1_5};
            return true;
#if HARNESS_HAS_VULKAN_1_3_TARGET
        case 3:
            *out = TargetSpec{"1.3", 3, 0x00010600u, kClientVulkan13, kSpv16};
            return true;
#endif
        default:
            return false;
    }
}

// --- sample shaders ---------------------------------------------------------
// vertex: UBO push through MVP + varying output (real vertex work)
const char *kVertexGLSL = R"GLSL(#version 450
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 0) out vec2 v_uv;
layout(set = 0, binding = 0) uniform Uniforms {
    mat4 u_mvp;
};
void main() {
    v_uv = a_uv;
    gl_Position = u_mvp * vec4(a_position, 0.0, 1.0);
}
)GLSL";

// fragment: texture sample * varying -> real output write
const char *kFragmentGLSL = R"GLSL(#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 1) uniform sampler2D u_sampler;
void main() {
    o_color = texture(u_sampler, v_uv);
}
)GLSL";

// compute: SSBO read -> scaled write (storage buffer write required)
const char *kComputeGLSL = R"GLSL(#version 450
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
layout(std430, set = 0, binding = 0) readonly buffer InBuf {
    vec4 inData[];
};
layout(std430, set = 0, binding = 1) writeonly buffer OutBuf {
    vec4 outData[];
};
void main() {
    uint idx = gl_GlobalInvocationID.y * 64u + gl_GlobalInvocationID.x;
    outData[idx] = inData[idx] * 2.0;
}
)GLSL";

struct StageResult {
    bool ok{false};
    std::string log;
    std::vector<uint32_t> spirv;
};

// Exact engine flow from SPIRVUtils::compileGLSL() (SPIRVUtils.cpp:126-165):
// TShader -> setStrings -> setEnvInput/setEnvClient/setEnvTarget ->
// parse(&glslang::DefaultTBuiltInResource, semantics, false, messages) ->
// TProgram::link -> GlslangToSpv(SpvOptions{disableOptimizer=false,
// optimizeSize=true, stripDebugInfo=true /* CC_DEBUG==0 path */}).
StageResult compileStage(EShLanguage stage, const char *source, const char *stageName,
                         int clientInputSemanticsVersion,
                         glslang::EShTargetClientVersion clientVersion,
                         glslang::EShTargetLanguageVersion spvVersion) {
    StageResult result;

    glslang::TShader shader(stage);
    shader.setStrings(&source, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan,
                       clientInputSemanticsVersion);
    shader.setEnvClient(glslang::EShClientVulkan, clientVersion);
    shader.setEnvTarget(glslang::EShTargetSpv, spvVersion);

    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);

#if CC_HARNESS_MODERN_RESOURCES
    const TBuiltInResource *resources = GetDefaultResources(); // global-namespace in modern headers
    if (resources == nullptr) {
        result.log = std::string("GetDefaultResources() returned null [") + stageName + "]";
        return result;
    }
#else
    const TBuiltInResource *resources = &glslang::DefaultTBuiltInResource;
#endif

    if (!shader.parse(resources, clientInputSemanticsVersion,
                      /*forwardCompatible=*/false, messages)) {
        result.log = std::string("parse failed [") + stageName + "]:\n" +
                     shader.getInfoLog() + "\n" + shader.getInfoDebugLog();
        return result;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        result.log = std::string("link failed [") + stageName + "]:\n" +
                     program.getInfoLog() + "\n" + program.getInfoDebugLog();
        return result;
    }

    spv::SpvBuildLogger logger;
    glslang::SpvOptions options;
    options.disableOptimizer = false; // engine: never disable (see SPIRVUtils.cpp comment)
    options.optimizeSize = true;      // engine: optimizeSize = true
    options.stripDebugInfo = true;    // engine CC_DEBUG==0 path

    const auto *intermediate = program.getIntermediate(stage);
    if (intermediate == nullptr) {
        result.log = std::string("no intermediate for stage [") + stageName + "]";
        return result;
    }
    glslang::GlslangToSpv(*intermediate, result.spirv, &logger, &options);

    const std::string spvLog = logger.getAllMessages();
    if (result.spirv.size() < 2) {
        result.log = std::string("GlslangToSpv produced no output [") + stageName + "]\n" + spvLog;
        return result;
    }
    if (result.spirv[0] != SpvMagicNumber) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "0x%08X", result.spirv[0]);
        result.log = std::string("bad SPIR-V magic ") + buf + " [" + stageName + "]";
        return result;
    }
    result.ok = true;
    result.log = spvLog;
    return result;
}

enum class Verdict { PASS, FAIL, UNSUPPORTED };

struct TargetRun {
    Verdict verdict{Verdict::PASS};
    std::string detail;
    std::vector<std::string> writtenFiles;
};

TargetRun runTarget(const TargetSpec &spec, const std::filesystem::path &outRoot) {
    TargetRun run;
    // Engine: _clientInputSemanticsVersion = 100 + vulkanMinorVersion * 10 (SPIRVUtils.cpp:114)
    const int semanticsVersion = 100 + spec.vulkanMinor * 10;

    struct ShaderCase {
        const char *name;
        EShLanguage stage;
        const char *source;
    };
    const ShaderCase cases[] = {
        {"vertex", EShLangVertex, kVertexGLSL},
        {"fragment", EShLangFragment, kFragmentGLSL},
        {"compute", EShLangCompute, kComputeGLSL},
    };

    std::filesystem::path outDir = outRoot / ("vk" + std::string(spec.name));
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);
    if (ec) {
        run.verdict = Verdict::FAIL;
        run.detail = "cannot create output dir " + outDir.string() + ": " + ec.message();
        return run;
    }

    for (const ShaderCase &c : cases) {
        StageResult r = compileStage(c.stage, c.source, c.name, semanticsVersion,
                                     spec.clientVersion, spec.spvVersion);
        if (!r.ok) {
            run.verdict = Verdict::FAIL;
            run.detail += std::string("[") + c.name + "] " + r.log + "\n";
            continue;
        }
        char got[16], want[16];
        std::snprintf(got, sizeof(got), "0x%08X", r.spirv[1]);
        std::snprintf(want, sizeof(want), "0x%08X", spec.expectedWord);
        if (r.spirv[1] != spec.expectedWord) {
            run.verdict = Verdict::FAIL;
            run.detail += std::string("[") + c.name + "] SPIR-V version word mismatch: got " +
                           got + ", expected " + want + "\n";
            continue;
        }
        const std::filesystem::path file = outDir / (std::string(c.name) + ".spv");
        std::ofstream ofs(file, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            run.verdict = Verdict::FAIL;
            run.detail += "cannot write " + file.string() + "\n";
            continue;
        }
        ofs.write(reinterpret_cast<const char *>(r.spirv.data()),
                  static_cast<std::streamsize>(r.spirv.size()) * 4);
        ofs.close();
        run.writtenFiles.push_back(file.string());
        std::printf("    [%s] %s -> %s (version word %s)\n", spec.name, c.name,
                    file.string().c_str(), got);
    }
    return run;
}

void printUsage(const char *argv0) {
    std::printf(
        "usage: %s <1.1|1.2|1.3|--all> [--out-dir DIR]\n"
        "  Compiles sample vertex/fragment/compute shaders through the bundled\n"
        "  glslang static libraries with the engine compile flow, verifies the\n"
        "  SPIR-V header version word and writes the .spv outputs.\n"
        "  exit: 0=PASS 1=FAIL 2=UNSUPPORTED(target not expressible by bundled glslang) 64=usage\n",
        argv0);
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return kExitUsage;
    }

    std::string targetArg = argv[1];
    std::filesystem::path outDir = std::filesystem::current_path() / "spirv-out";
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--out-dir" && i + 1 < argc) {
            outDir = argv[++i];
        } else {
            std::printf("unknown argument: %s\n", argv[i]);
            printUsage(argv[0]);
            return kExitUsage;
        }
    }

    std::printf("glslang bundled version: %d.%d.%d (flavor \"%s\")\n",
                GLSLANG_VERSION_MAJOR, GLSLANG_VERSION_MINOR, GLSLANG_VERSION_PATCH,
                GLSLANG_VERSION_FLAVOR);
#if HARNESS_HAS_VULKAN_1_3_TARGET
    std::printf("Vulkan 1.3 / SPV 1.6 target enums: present (engine guard >= 11.10 satisfied)\n");
#else
    std::printf("Vulkan 1.3 / SPV 1.6 target enums: ABSENT in bundled headers "
                "(ShaderLang.h only declares EShTargetVulkan_1_0/1_1/1_2, EShTargetSpv_1_0..1_5)\n");
#endif

    std::vector<int> minors;
    if (targetArg == "--all") {
        minors = {1, 2, 3};
    } else if (targetArg == "1.1") {
        minors = {1};
    } else if (targetArg == "1.2") {
        minors = {2};
    } else if (targetArg == "1.3") {
        minors = {3};
    } else {
        printUsage(argv[0]);
        return kExitUsage;
    }

    bool anyFail = false;
    bool anyUnsupported = false;
    std::printf("output dir: %s\n", outDir.string().c_str());

    // Engine calls glslang::InitializeProcess() in SPIRVUtils::initialize()
    // (SPIRVUtils.cpp:113-118); without it parse() fails with empty logs.
    glslang::InitializeProcess();

    for (int minor : minors) {
        TargetSpec spec{};
        if (!makeTargetSpec(minor, &spec)) {
            // Only reachable for minor>=3 when target enums are absent.
            anyUnsupported = true;
            std::printf("TARGET %d.%d: UNSUPPORTED - bundled glslang %d.%d.%d cannot express "
                        "Vulkan 1.3 / SPIR-V 1.6 target (version guard < 11.10).\n"
                        "  Engine SPIRVUtils.cpp would silently fall back to "
                        "Vulkan1.2/SPV1.5 here; this harness does NOT substitute and does NOT "
                        "count the fallback as PASS.\n",
                        1, minor, GLSLANG_VERSION_MAJOR, GLSLANG_VERSION_MINOR,
                        GLSLANG_VERSION_PATCH);
            continue;
        }
        std::printf("TARGET %s: client=Vulkan%s semantics=%d spv_expect=0x%08X\n",
                    spec.name, spec.name, 100 + spec.vulkanMinor * 10, spec.expectedWord);
        TargetRun run = runTarget(spec, outDir);
        switch (run.verdict) {
            case Verdict::PASS:
                std::printf("TARGET %s: PASS (%zu shaders)\n", spec.name,
                            run.writtenFiles.size());
                break;
            case Verdict::FAIL:
                anyFail = true;
                std::printf("TARGET %s: FAIL\n%s\n", spec.name, run.detail.c_str());
                break;
            default:
                break;
        }
    }

    glslang::FinalizeProcess();

    const int exitCode = anyFail ? kExitFail
                                 : (anyUnsupported ? kExitUnsupported : kExitPass);
    std::printf("harness exit: %d (%s)\n", exitCode,
                exitCode == kExitPass ? "all requested targets PASS"
                : exitCode == kExitFail ? "at least one FAIL"
                                        : "UNSUPPORTED target(s) present");
    return exitCode;
}
