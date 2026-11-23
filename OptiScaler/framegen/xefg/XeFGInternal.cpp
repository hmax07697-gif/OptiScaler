#include "pch.h"
#include "XeFGInternal.h"

#include <Util.h>
#include <proxies/KernelBase_Proxy.h>

#include <cstring>
#include <filesystem>
#include <format>
#include <string>
#include <vector>

namespace
{
using PfnCreateContext = XeFGInternal::result_t (*)(ID3D12Device*, void**);
using PfnInit = XeFGInternal::result_t (*)(void*, const XeFGInternal::InitParams*);
using PfnExecute = XeFGInternal::result_t (*)(void*, ID3D12GraphicsCommandList*, const XeFGInternal::ExecParams*);
using PfnDestroy = XeFGInternal::result_t (*)(void*);
using PfnSettings = uint8_t* (*)();

// Offsets into the XefgSettings singleton, per build
struct SettingsLayout
{
    size_t allowModelOverride; // byte, the library never sets it itself
    size_t modelOverrideSet;   // byte
    size_t modelOverride;      // int32
    size_t maxFramesSet;       // byte
    size_t maxFrames;          // uint32, only read when maxFramesSet is set
    size_t sanityFloat;        // float, the scene change threshold, 0.7 in both builds
};

constexpr SettingsLayout Layout_1_3_1 { 77, 100, 104, 360, 364, 112 };
constexpr SettingsLayout Layout_1_3_4 { 110, 132, 136, 392, 396, 144 };

struct KnownBuild
{
    uint32_t timeDateStamp;
    uint32_t sizeOfImage;
    uint32_t createContext, init, execute, destroy, settings;
    const SettingsLayout* layout;
};

// libxess_fg.dll 1.3.1 (XeSS SDK 3.0.2) and 1.3.4, as documented by dlssg-to-xefg (GPL-3.0)
constexpr KnownBuild KnownBuilds[] = {
    { 0x69CB0F4D, 0x015ED000, 0x1853B0, 0x1871C0, 0x185800, 0x1874C0, 0x1A4610, &Layout_1_3_1 },
    { 0x6A95ED0F, 0x015D7000, 0x1856A0, 0x1874B0, 0x185AF0, 0x1877B0, 0x1A4EA0, &Layout_1_3_4 },
};

struct Api
{
    PfnCreateContext createContext = nullptr;
    PfnInit init = nullptr;
    PfnExecute execute = nullptr;
    PfnDestroy destroy = nullptr;

    uint8_t* settings = nullptr;
    const SettingsLayout* layout = nullptr;

    HMODULE module = nullptr;
};

Api api;
std::string status = "not loaded";

bool ParseHeaders(HMODULE module, IMAGE_NT_HEADERS64 const** nt)
{
    auto base = reinterpret_cast<const uint8_t*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 0x1000)
        return false;

    auto headers = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);

    if (headers->Signature != IMAGE_NT_SIGNATURE || headers->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;

    *nt = headers;
    return true;
}

bool Resolve(HMODULE module)
{
    IMAGE_NT_HEADERS64 const* nt = nullptr;

    if (!ParseHeaders(module, &nt))
    {
        status = "libxess_fg.dll has no usable PE headers";
        return false;
    }

    // The version is an export of the wrapper API, the same build as the internal one
    struct LibVersion
    {
        uint16_t major, minor, patch, reserved;
    };
    LibVersion version {};

    if (auto getVersion = reinterpret_cast<int32_t (*)(LibVersion*)>(
            KernelBaseProxy::GetProcAddress_()(module, "xefgSwapChainGetVersion")))
    {
        if (getVersion(&version) != XeFGInternal::RESULT_SUCCESS)
            version = {};
    }

    auto const stamp = nt->FileHeader.TimeDateStamp;
    auto const size = nt->OptionalHeader.SizeOfImage;
    auto const* base = reinterpret_cast<const uint8_t*>(module);
    const KnownBuild* known = nullptr;

    for (const auto& build : KnownBuilds)
    {
        if (build.timeDateStamp == stamp && build.sizeOfImage == size)
            known = &build;
    }

    if (known == nullptr)
    {
        status = std::format("unsupported libxess_fg build (stamp {:08X}, size {:X}, version {}.{}.{})", stamp, size,
                             version.major, version.minor, version.patch);
        LOG_WARN("{}", status);
        return false;
    }

    api.createContext = reinterpret_cast<PfnCreateContext>(base + known->createContext);
    api.init = reinterpret_cast<PfnInit>(base + known->init);
    api.execute = reinterpret_cast<PfnExecute>(base + known->execute);
    api.destroy = reinterpret_cast<PfnDestroy>(base + known->destroy);
    api.layout = known->layout;

    auto settingsInstance = reinterpret_cast<PfnSettings>(base + known->settings);
    api.settings = settingsInstance();

    // A wrong layout would write into unrelated fields, the scene change threshold identifies it
    float sanity = 0.0f;

    if (api.settings != nullptr)
        memcpy(&sanity, api.settings + api.layout->sanityFloat, sizeof(sanity));

    if (api.settings == nullptr || sanity != 0.7f)
    {
        status = std::format("libxess_fg settings layout mismatch ({}.{}.{})", version.major, version.minor,
                             version.patch);
        LOG_WARN("{}", status);
        return false;
    }

    api.module = module;
    auto path = Util::ModulePath(module);

    LOG_INFO("XeFG internal API resolved: {} ({}.{}.{})", wstring_to_string(path.wstring()), version.major,
             version.minor, version.patch);

    // The game can unload its own copy, the addresses and the settings pointer have to stay valid
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(module), &pinned);

    return true;
}

HMODULE TryLoad(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path))
        return nullptr;

    auto module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (module == nullptr)
        LOG_WARN("Can't load {}", wstring_to_string(path.wstring()));

    return module;
}
} // namespace

namespace XeFGInternal
{
bool Load()
{
    static const bool loaded = []
    {
        // A game or OptiScaler may already have it, otherwise the one next to the game or OptiScaler is used
        std::vector<std::filesystem::path> candidates;
        auto exeFolder = Util::ExePath().parent_path();

        candidates.push_back(exeFolder / L"libxess_fg.dll");
        candidates.push_back(exeFolder / L"OptiScaler" / L"libxess_fg.dll");
        candidates.push_back(exeFolder / L"OptiScaler" / L"xess" / L"libxess_fg.dll");

        if (auto loadedModule = GetModuleHandleW(L"libxess_fg.dll"); loadedModule != nullptr && Resolve(loadedModule))
            return true;

        for (const auto& path : candidates)
        {
            auto module = TryLoad(path);

            if (module != nullptr && Resolve(module))
                return true;
        }

        if (api.module == nullptr && status == "not loaded")
            status = "libxess_fg.dll not found";

        LOG_WARN("XeFG internal API unavailable: {}", status);
        return false;
    }();

    return loaded;
}

bool Available() { return api.module != nullptr; }

const char* Status() { return status.c_str(); }

void SetMaxInterpolatedFrames(uint32_t maxGeneratedFrames)
{
    if (api.settings == nullptr || api.layout == nullptr)
        return;

    api.settings[api.layout->maxFramesSet] = 1;
    memcpy(api.settings + api.layout->maxFrames, &maxGeneratedFrames, sizeof(maxGeneratedFrames));

    LOG_INFO("XeFG settings: max generated frames {}", maxGeneratedFrames);
}

void SetModel(int32_t modelId)
{
    if (api.settings == nullptr || api.layout == nullptr)
        return;

    api.settings[api.layout->allowModelOverride] = 1;
    api.settings[api.layout->modelOverrideSet] = 1;
    memcpy(api.settings + api.layout->modelOverride, &modelId, sizeof(modelId));

    LOG_INFO("XeFG settings: model {}", modelId);
}

bool CreateContext(ID3D12Device* device, void** context)
{
    if (api.createContext == nullptr || device == nullptr || context == nullptr)
        return false;

    *context = nullptr;
    auto result = api.createContext(device, context);

    if (result != RESULT_SUCCESS || *context == nullptr)
    {
        LOG_ERROR("xefgD3D12CreateContext failed: {}", result);
        return false;
    }

    return true;
}

result_t Init(void* context, const InitParams& params)
{
    if (api.init == nullptr || context == nullptr)
        return RESULT_ERROR_NOT_IMPLEMENTED;

    return api.init(context, &params);
}

result_t Execute(void* context, ID3D12GraphicsCommandList* cmdList, const ExecParams& params)
{
    if (api.execute == nullptr || context == nullptr || cmdList == nullptr)
        return RESULT_ERROR_NOT_IMPLEMENTED;

    return api.execute(context, cmdList, &params);
}

void Destroy(void* context)
{
    if (api.destroy == nullptr || context == nullptr)
        return;

    api.destroy(context);
}
} // namespace XeFGInternal
