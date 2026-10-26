#include "pch.h"
#include "Nvngx_Nvidia.h"

#include <zluda/DlssZ.h>
#include <zluda/d4r_commands/d3d12_command_hooks.h>

namespace
{

// The host's entry points, which take the place of the library's own. Each is called from inside the host, which
// is what the library checks.
struct Host
{
    const char* (*load)(const wchar_t*) = nullptr;
    NVSDK_NGX_Result (*init)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, unsigned long long,
                             unsigned) = nullptr;
    NVSDK_NGX_Result (*shutdown)(ID3D12Device*) = nullptr;
    NVSDK_NGX_Result (*scratch)(NVSDK_NGX_Feature, const void*, size_t*) = nullptr;
    NVSDK_NGX_Result (*create)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*,
                               NVSDK_NGX_Handle**) = nullptr;
    NVSDK_NGX_Result (*release)(NVSDK_NGX_Handle*) = nullptr;
    NVSDK_NGX_Result (*requirements)(void*, const void*, void*) = nullptr;
    NVSDK_NGX_Result (*evaluate)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*,
                                 void*) = nullptr;
    NVSDK_NGX_Result (*populate)(NVSDK_NGX_Parameter*) = nullptr;
};

// The NVAPI stand-in (nvapi64.dll of the dlss5nr folder): what the library's kernel launches and texture requests
// reach, and the evaluation of a frame around them. See dlss5nr's runtime/fg.
struct StandIn
{
    int (*attach)(ID3D12Device*,
                  bool (*)(ID3D12GraphicsCommandList*, d4r::win::commands::BoundaryC, void*),
                  uint64_t (*)(ID3D12GraphicsCommandList*),
                  uint32_t (*)(ID3D12GraphicsCommandList*, ID3D12Resource*, uint32_t)) = nullptr;
    void (*scope)(int) = nullptr;
    void (*purge)() = nullptr;
    int32_t (*evaluate)(ID3D12GraphicsCommandList*, const void*, NVSDK_NGX_Parameter*, void*,
                        int32_t (*)(ID3D12GraphicsCommandList*, const void*, NVSDK_NGX_Parameter*, void*)) = nullptr;
};

Host host;
StandIn standIn;
ID3D12Device* attachedDevice = nullptr;

// Every call into the library is bracketed: the stand-in sees what the library creates meanwhile, and the loads the
// library makes (it asks for nvapi64.dll by name) reach the real files.
struct Call
{
    DlssZ::LibraryBypass bypass;
    Call() { standIn.scope(1); }
    ~Call() { standIn.scope(0); }
};

void Attach(ID3D12Device* device)
{
    if (device == attachedDevice)
        return;

    if (!standIn.attach(device, &d4r::win::commands::record_boundary_c, &d4r::win::commands::command_mark,
                        &d4r::win::commands::resource_state_c))
    {
        LOG_ERROR("NVIDIA frame generation: the NVAPI stand-in could not attach to the device");
        return;
    }

    attachedDevice = device;
}

NVSDK_NGX_Result Init_Ext(unsigned long long id, const wchar_t* path, ID3D12Device* device, NVSDK_NGX_Version version,
                          const NVSDK_NGX_FeatureCommonInfo*)
{
    // The stand-in loads the gate when it attaches: that load, like the library's, is not the game's.
    DlssZ::LibraryBypass bypass;

    // The command lists are tracked from here on; a list begun before cannot be split.
    DlssZ::InstallCommandHooks(device, true);
    Attach(device);
    Call call;
    return host.init(id, path, device, version, 0, 0);
}

NVSDK_NGX_Result Init(unsigned long long id, const wchar_t* path, ID3D12Device* device,
                      const NVSDK_NGX_FeatureCommonInfo* info, NVSDK_NGX_Version version)
{
    return Init_Ext(id, path, device, version, info);
}

NVSDK_NGX_Result Shutdown1(ID3D12Device* device)
{
    NVSDK_NGX_Result result;
    {
        Call call;
        result = host.shutdown(device);
    }

    standIn.purge();

    if (device == attachedDevice)
        attachedDevice = nullptr;

    return result;
}

NVSDK_NGX_Result Shutdown() { return Shutdown1(attachedDevice); }

NVSDK_NGX_Result Scratch(NVSDK_NGX_Feature feature, const NVSDK_NGX_Parameter* parameters, size_t* size)
{
    Call call;
    return host.scratch(feature, parameters, size);
}

// What the library is given by whoever drives it, once or twice: the schema to reproduce where nothing but OptiScaler
// drives it.
void LogParameters(const char* where, const NVSDK_NGX_Parameter* parameters)
{
    static int budget = 3;
    if (budget <= 0)
        return;
    --budget;

    const auto* store = dynamic_cast<const NVNGX_Parameters*>(parameters);
    if (store == nullptr)
        return;

    auto keys = store->enumerate();
    std::sort(keys.begin(), keys.end());

    for (const auto& key : keys)
    {
        if (!key.starts_with("DLSSG."))
            continue;

        void* pointer = nullptr;
        double number = 0;

        if (store->Get(key.c_str(), &pointer) == NVSDK_NGX_Result_Success && pointer != nullptr)
            LOG_DEBUG("{}: {} = {:#x} (pointer)", where, key, (size_t) pointer);
        else if (store->Get(key.c_str(), &number) == NVSDK_NGX_Result_Success)
            LOG_DEBUG("{}: {} = {}", where, key, number);
        else
            LOG_DEBUG("{}: {} (no value)", where, key);
    }
}

NVSDK_NGX_Result Create(ID3D12GraphicsCommandList* list, NVSDK_NGX_Feature feature, NVSDK_NGX_Parameter* parameters,
                        NVSDK_NGX_Handle** handle)
{
    LogParameters("CreateFeature", parameters);
    Call call;
    return host.create(list, feature, parameters, handle);
}

NVSDK_NGX_Result Release(NVSDK_NGX_Handle* handle)
{
    Call call;
    const auto result = host.release(handle);
    return result;
}

NVSDK_NGX_Result Requirements(IDXGIAdapter* adapter, const NVSDK_NGX_FeatureDiscoveryInfo* info,
                              NVSDK_NGX_FeatureRequirement* out)
{
    DlssZ::LibraryBypass bypass;
    return host.requirements(adapter, info, out);
}

// What the stand-in calls, in the middle of its own evaluation of the frame, to run the library.
int32_t HostEvaluate(ID3D12GraphicsCommandList* list, const void* handle, NVSDK_NGX_Parameter* parameters,
                     void* progress)
{
    return (int32_t) host.evaluate(list, static_cast<const NVSDK_NGX_Handle*>(handle), parameters, progress);
}

NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList* list, const NVSDK_NGX_Handle* handle,
                          const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback)
{
    LogParameters("EvaluateFeature", parameters);
    DlssZ::LibraryBypass bypass;
    const auto result = (NVSDK_NGX_Result) standIn.evaluate(list, handle, const_cast<NVSDK_NGX_Parameter*>(parameters),
                                                            (void*) callback, &HostEvaluate);

    if (result != NVSDK_NGX_Result_Success)
    {
        static int reported = 0;
        if (reported++ < 5)
            LOG_ERROR("NVIDIA frame generation failed ({:#x}) on a command list of type {}: {}", (unsigned) result,
                      (int) list->GetType(), d4r::win::commands::boundary_failure());
    }

    return result;
}

NVSDK_NGX_Result Populate(NVSDK_NGX_Parameter* parameters)
{
    Call call;
    return host.populate(parameters);
}

} // namespace

void Nvngx_Nvidia::LoadLibraries()
{
    const auto directory = DlssZ::RuntimeDirectory();
    const auto library = directory / L"nvngx_dlssg.dll";
    const auto hostPath = directory / L"zluda" / L"ngx_fg" / L"nvngx.dll";
    const auto standInPath = directory / L"zluda" / L"nvapi64.dll";

    std::error_code error;
    if (!std::filesystem::exists(library, error) || !std::filesystem::exists(hostPath, error) ||
        !std::filesystem::exists(standInPath, error))
    {
        LOG_WARN("NVIDIA frame generation on this GPU needs {}, {} and {}", wstring_to_string(library.wstring()),
                 wstring_to_string(hostPath.wstring()), wstring_to_string(standInPath.wstring()));
        return;
    }

    // These loads, and the library's own, are not the game's: they reach the real files.
    DlssZ::LibraryBypass bypass;

    // The stand-in is loaded by its path, first: the library asks for nvapi64.dll by name, and is given the module
    // of that name that is already there. Beside it are ZLUDA's NVAPI and the gate.
    auto stand = LoadLibraryExW(standInPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (stand == nullptr)
    {
        LOG_ERROR("The NVAPI stand-in could not be loaded ({})", GetLastError());
        return;
    }

#define BIND_STAND(field, name)                                                                                        \
    standIn.field = (decltype(standIn.field)) GetProcAddress(stand, name);                                             \
    if (standIn.field == nullptr)                                                                                      \
    {                                                                                                                  \
        LOG_ERROR("The NVAPI stand-in does not export " name " (an older one?)");                                      \
        return;                                                                                                        \
    }

    BIND_STAND(attach, "fgnv_attach");
    BIND_STAND(scope, "fgnv_scope");
    BIND_STAND(purge, "fgnv_purge");
    BIND_STAND(evaluate, "fgnv_evaluate");
#undef BIND_STAND

    // The host's own imports are beside it.
    dll = LoadLibraryExW(hostPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (dll == nullptr)
    {
        LOG_ERROR("The dlss5nr NGX host could not be loaded ({})", GetLastError());
        return;
    }

#define BIND(field, name)                                                                                              \
    host.field = (decltype(host.field)) GetProcAddress(dll, name);                                                     \
    if (host.field == nullptr)                                                                                         \
    {                                                                                                                  \
        LOG_ERROR("The dlss5nr NGX host does not export " name " (an older host?)");                                   \
        return;                                                                                                        \
    }

    BIND(load, "ngxrt_load");
    BIND(init, "ngxrt_d3d12_init");
    BIND(shutdown, "ngxrt_d3d12_shutdown");
    BIND(scratch, "ngxrt_d3d12_scratch_size");
    BIND(create, "ngxrt_d3d12_create");
    BIND(release, "ngxrt_d3d12_release");
    BIND(requirements, "ngxrt_d3d12_requirements");
    BIND(evaluate, "ngxrt_d3d12_evaluate");
    BIND(populate, "ngxrt_d3d12_populate_into");
#undef BIND

    if (const char* missing = host.load(library.c_str()); missing != nullptr)
    {
        LOG_ERROR("nvngx_dlssg.dll could not be loaded by the host (missing {})", missing);
        return;
    }

    _DLSSG_D3D12_Init = Init;
    _DLSSG_D3D12_Init_Ext = Init_Ext;
    _DLSSG_D3D12_Shutdown = Shutdown;
    _DLSSG_D3D12_Shutdown1 = Shutdown1;
    _DLSSG_D3D12_GetScratchBufferSize = Scratch;
    _DLSSG_D3D12_CreateFeature = Create;
    _DLSSG_D3D12_ReleaseFeature = Release;
    _DLSSG_D3D12_GetFeatureRequirements = Requirements;
    _DLSSG_D3D12_EvaluateFeature = Evaluate;
    _DLSSG_D3D12_PopulateParameters_Impl = Populate;

    LOG_INFO("NVIDIA frame generation loaded from {} through the dlss5nr NGX host",
             wstring_to_string(library.wstring()));
}
