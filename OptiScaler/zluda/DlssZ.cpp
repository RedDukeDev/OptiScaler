#include <pch.h>

#include "DlssZ.h"

#include <Config.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <nr/NeuralRendering.h>
#include <upscalers/dlssz/DLSSZFeature_Dx12.h>

#include "d4r_commands/d3d12_command_hooks.h"

#include <detours/detours.h>

namespace
{

HMODULE _module = nullptr;
DlssZ::Api _api;
bool _loadTried = false;
bool _loaded = false;

std::mutex _hooksMutex;
bool _hooksInstalled = false;
bool _hooksFailed = false;
ULONGLONG _hooksInstalledAt = 0;
bool _hooksLate = false;

// A late installation settles after this long: every command list the game keeps has been reset since.
constexpr ULONGLONG kSettleMilliseconds = 1500;

typedef VOID(WINAPI* PFN_ExitProcess)(UINT);
PFN_ExitProcess o_ExitProcess = nullptr;
bool _fastExitInstalled = false;

VOID WINAPI hk_ExitProcess(UINT exitCode)
{
    LOG_INFO("ExitProcess({}): ending the process without unloading ZLUDA and HIP", exitCode);

    // The process is gone after this: nothing below runs.
    TerminateProcess(GetCurrentProcess(), exitCode);
}

} // namespace

void DlssZ::InstallFastExit()
{
    if (_fastExitInstalled || !Config::Instance()->ZludaFastExit.value_or_default())
        return;

    _fastExitInstalled = true;

    // kernel32's ExitProcess forwards here.
    auto kernelBase = GetModuleHandleW(L"kernelbase.dll");
    o_ExitProcess = kernelBase ? (PFN_ExitProcess) GetProcAddress(kernelBase, "ExitProcess") : nullptr;
    if (o_ExitProcess == nullptr)
        return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&) o_ExitProcess, hk_ExitProcess);

    if (DetourTransactionCommit() != NO_ERROR)
    {
        LOG_WARN("DLSS on AMD: could not hook ExitProcess; the process may take long to end");
        o_ExitProcess = nullptr;
    }
}

std::filesystem::path DlssZ::RuntimeDirectory() { return NeuralRendering::RuntimeDirectory(); }

std::filesystem::path DlssZ::SnippetPath(bool rayReconstruction)
{
    auto& configured = rayReconstruction ? Config::Instance()->ZludaDlssRrSnippetPath
                                         : Config::Instance()->ZludaDlssSnippetPath;

    if (configured.has_value())
        return configured.value();

    return RuntimeDirectory() / (rayReconstruction ? L"nvngx_dlssd.dll" : L"nvngx_dlss.dll");
}

bool DlssZ::Available(bool rayReconstruction)
{
    std::error_code error;
    return std::filesystem::exists(RuntimeDirectory() / L"dlss5nr_runtime.dll", error) &&
           std::filesystem::exists(SnippetPath(rayReconstruction), error);
}

bool DlssZ::Usable(bool rayReconstruction)
{
    const auto gpu = IdentifyGpu::getPrimaryGpu();
    if (gpu.dlssCapable || gpu.vendorId == VendorId::Nvidia)
        return false;

    if (!Config::Instance()->ZludaDlssEnabled.value_or(true))
        return false;

    return Available(rayReconstruction);
}

bool DlssZ::Load()
{
    if (_loaded)
        return true;

    if (_loadTried)
        return false;

    _loadTried = true;

    auto library = RuntimeDirectory() / L"dlss5nr_runtime.dll";

    // Its own dependencies are looked up beside it, not in the game's folder.
    _module = LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (_module == nullptr)
    {
        LOG_ERROR("DLSS on AMD needs {}, which could not be loaded ({})", wstring_to_string(library.wstring()),
                  GetLastError());
        return false;
    }

#define BIND(field, name)                                                                                              \
    _api.field = (decltype(_api.field)) GetProcAddress(_module, name);                                                 \
    if (_api.field == nullptr)                                                                                         \
    {                                                                                                                  \
        LOG_ERROR("dlss5nr_runtime.dll does not export " name " (an older runtime?)");                                 \
        return false;                                                                                                  \
    }

    BIND(create, "dlss5nr_upscaler_create");
    BIND(destroy, "dlss5nr_upscaler_destroy");
    BIND(evaluate, "dlss5nr_upscaler_evaluate");
    BIND(getStatus, "dlss5nr_upscaler_get_status");
    BIND(lastError, "dlss5nr_upscaler_last_error");
    BIND(loaderThread, "dlss5nr_upscaler_loader_thread");
#undef BIND

    _loaded = true;
    LOG_INFO("DLSS on AMD: runtime library loaded from {}", wstring_to_string(library.wstring()));
    InstallFastExit();
    return true;
}

const DlssZ::Api& DlssZ::Functions() { return _api; }

bool DlssZ::WantsEarlyHooks()
{
    if (IdentifyGpu::getPrimaryGpu().dlssCapable)
        return false;

    auto& config = *Config::Instance();

    // Asked for outright, or the upscaler the configuration names is DLSS.
    const bool asked = config.ZludaDlssEnabled.value_or(config.Dx12Upscaler.has_value() &&
                                                        config.Dx12Upscaler.value() == Upscaler::DLSS);

    return asked && Usable(false);
}

void DlssZ::TrackCommandSignatures(ID3D12Device* device)
{
    if (device == nullptr || IdentifyGpu::getPrimaryGpu().dlssCapable || !Usable(false))
        return;

    try
    {
        d4r::win::commands::track_signatures(device);
    }
    catch (const std::exception& failure)
    {
        LOG_WARN("DLSS on AMD: command signatures could not be tracked: {}", failure.what());
    }
}

bool DlssZ::InstallCommandHooks(ID3D12Device* device, bool late)
{
    std::lock_guard<std::mutex> lock(_hooksMutex);

    if (_hooksInstalled)
        return true;

    if (_hooksFailed || device == nullptr)
        return false;

    try
    {
        d4r::win::commands::install(device);
    }
    catch (const std::exception& failure)
    {
        LOG_ERROR("DLSS on AMD: the command list hooks could not be installed: {}", failure.what());
        _hooksFailed = true;
        return false;
    }

    // The game's list is released at once when the network has been queued behind it, and the GPU does the waiting
    // (d4r's default makes the CPU wait for every frame's submission).
    if (std::getenv("DLSSZ_SYNC_SUBMIT") == nullptr)
        SetEnvironmentVariableA("D4R_ASYNC_INTEROP", "1");

    _hooksInstalled = true;
    _hooksLate = late;
    _hooksInstalledAt = GetTickCount64();
    LOG_INFO("DLSS on AMD: command list hooks installed{}", late ? " (late: the first frames are left out)" : "");
    return true;
}

bool DlssZ::CommandHooksInstalled() { return _hooksInstalled; }

bool DlssZ::HooksAreSettled()
{
    return _hooksInstalled && (!_hooksLate || GetTickCount64() - _hooksInstalledAt > kSettleMilliseconds);
}

bool DlssZ::IsDlssZ(const IFeature* feature)
{
    return dynamic_cast<const DLSSZFeatureDx12*>(feature) != nullptr;
}

bool DlssZ::BypassLibraryHooks()
{
    if (!_loaded)
        return false;

    const DWORD loader = _api.loaderThread();
    return loader != 0 && GetCurrentThreadId() == loader;
}
