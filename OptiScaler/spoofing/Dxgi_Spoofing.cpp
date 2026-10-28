#include "pch.h"
#include "Dxgi_Spoofing.h"

#include <Config.h>

#include <detours/detours.h>

#include <string>
#include <algorithm>
#include <cctype>
#include <misc/IdentifyGpu.h>
#include <State.h>

typedef HRESULT (*PFN_GetDesc)(IDXGIAdapter* This, DXGI_ADAPTER_DESC* pDesc);
typedef HRESULT (*PFN_GetDesc1)(IDXGIAdapter1* This, DXGI_ADAPTER_DESC1* pDesc);
typedef HRESULT (*PFN_GetDesc2)(IDXGIAdapter2* This, DXGI_ADAPTER_DESC2* pDesc);
typedef HRESULT (*PFN_GetDesc3)(IDXGIAdapter4* This, DXGI_ADAPTER_DESC3* pDesc);

inline static PFN_GetDesc o_GetDesc = nullptr;
inline static PFN_GetDesc1 o_GetDesc1 = nullptr;
inline static PFN_GetDesc2 o_GetDesc2 = nullptr;
inline static PFN_GetDesc3 o_GetDesc3 = nullptr;

typedef HRESULT (*PFN_CheckInterfaceSupport)(IDXGIAdapter* This, REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion);
inline static PFN_CheckInterfaceSupport o_CheckInterfaceSupport = nullptr;

inline static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

inline static bool iequals(const std::string& a, const std::string& b) { return toLower(a) == toLower(b); }

#pragma region DXGI Adapter methods

// Unreal reads the driver version of an NVIDIA adapter out of this. DXVK has no Windows driver to ask and says 0
HRESULT DxgiSpoofing::hkCheckInterfaceSupport(IDXGIAdapter* This, REFGUID InterfaceName, LARGE_INTEGER* pUMDVersion)
{
    auto result = o_CheckInterfaceSupport(This, InterfaceName, pUMDVersion);

    if (result == S_OK && pUMDVersion != nullptr)
    {
        auto caller = Util::WhoIsTheCaller(_ReturnAddress());

        LOG_INFO("UMD version {}.{}.{}.{}, caller: {}", (int) ((pUMDVersion->QuadPart >> 48) & 0xFFFF),
                 (int) ((pUMDVersion->QuadPart >> 32) & 0xFFFF), (int) ((pUMDVersion->QuadPart >> 16) & 0xFFFF),
                 (int) (pUMDVersion->QuadPart & 0xFFFF), caller);

        // 31.0.19.9999 is NVIDIA's driver 999.99, the one the NVAPI and Streamline answers already give
        if (State::Instance().isRunningOnLinux && pUMDVersion->QuadPart == 0 &&
            Config::Instance()->DxgiSpoofing.value_or_default() &&
            Config::Instance()->SpoofedVendorId.value_or_default() == VendorId::Nvidia)
        {
            pUMDVersion->QuadPart = (31LL << 48) | (19LL << 16) | 9999LL;
            LOG_INFO("UMD version was 0, giving 31.0.19.9999");
        }
    }

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc3(IDXGIAdapter4* This, DXGI_ADAPTER_DESC3* pDesc)
{
    auto result = o_GetDesc3(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (iequals(caller, "vulkan-1.dll") || iequals(caller, "amdvlk64.dll") || iequals(caller, "dxgi.dll") ||
        iequals(caller, "d3d12.dll") || iequals(caller, "d3d12Core.dll"))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        if (pDesc->VendorId != VendorId::Microsoft &&
            (!Config::Instance()->TargetVendorId.has_value() ||
             Config::Instance()->TargetVendorId.value() == pDesc->VendorId) &&
            (!Config::Instance()->TargetDeviceId.has_value() ||
             Config::Instance()->TargetDeviceId.value() == pDesc->DeviceId) &&
            Config::Instance()->DxgiSpoofing.value_or_default() && !SkipSpoofing())
        {
            pDesc->VendorId = Config::Instance()->SpoofedVendorId.value_or_default();
            pDesc->DeviceId = Config::Instance()->SpoofedDeviceId.value_or_default();

            auto szName = Config::Instance()->SpoofedGPUName.value_or_default();
            std::memset(pDesc->Description, 0, sizeof(pDesc->Description));
            std::wcscpy(pDesc->Description, szName.c_str());

#ifdef _DEBUG
            LOG_DEBUG("spoofing");
#endif
        }
    }

    AttachToAdapter(This);

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc2(IDXGIAdapter2* This, DXGI_ADAPTER_DESC2* pDesc)
{
    auto result = o_GetDesc2(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (iequals(caller, "vulkan-1.dll") || iequals(caller, "amdvlk64.dll") || iequals(caller, "dxgi.dll") ||
        iequals(caller, "d3d12.dll") || iequals(caller, "d3d12Core.dll"))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        auto targetVendorIdMatches = !Config::Instance()->TargetVendorId.has_value() ||
                                     Config::Instance()->TargetVendorId.value() == pDesc->VendorId;

        auto targetDeviceIdMatches = !Config::Instance()->TargetDeviceId.has_value() ||
                                     Config::Instance()->TargetDeviceId.value() == pDesc->DeviceId;

        if (pDesc->VendorId != VendorId::Microsoft && targetVendorIdMatches && targetDeviceIdMatches &&
            Config::Instance()->DxgiSpoofing.value_or_default() && !SkipSpoofing())
        {
            pDesc->VendorId = Config::Instance()->SpoofedVendorId.value_or_default();
            pDesc->DeviceId = Config::Instance()->SpoofedDeviceId.value_or_default();

            auto szName = Config::Instance()->SpoofedGPUName.value_or_default();
            std::memset(pDesc->Description, 0, sizeof(pDesc->Description));
            std::wcscpy(pDesc->Description, szName.c_str());

#ifdef _DEBUG
            LOG_DEBUG("spoofing");
#endif
        }
    }

    AttachToAdapter(This);

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc1(IDXGIAdapter1* This, DXGI_ADAPTER_DESC1* pDesc)
{
    auto result = o_GetDesc1(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (iequals(caller, "vulkan-1.dll") || iequals(caller, "amdvlk64.dll") || iequals(caller, "dxgi.dll") ||
        iequals(caller, "d3d12.dll") || iequals(caller, "d3d12Core.dll"))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        auto targetVendorIdMatches = !Config::Instance()->TargetVendorId.has_value() ||
                                     Config::Instance()->TargetVendorId.value() == pDesc->VendorId;

        auto targetDeviceIdMatches = !Config::Instance()->TargetDeviceId.has_value() ||
                                     Config::Instance()->TargetDeviceId.value() == pDesc->DeviceId;

        // amdxcffx64 needs to see AMD vendor id
        auto isFfxDriver = caller.starts_with("amdxcffx64");
        auto keepRealAmd = isFfxDriver && pDesc->VendorId == VendorId::AMD;

        if (pDesc->VendorId != VendorId::Microsoft && targetVendorIdMatches && targetDeviceIdMatches && !keepRealAmd &&
            Config::Instance()->DxgiSpoofing.value_or_default() && !SkipSpoofing())
        {
            pDesc->VendorId = Config::Instance()->SpoofedVendorId.value_or_default();
            pDesc->DeviceId = Config::Instance()->SpoofedDeviceId.value_or_default();

            auto szName = Config::Instance()->SpoofedGPUName.value_or_default();
            std::memset(pDesc->Description, 0, sizeof(pDesc->Description));
            std::wcscpy(pDesc->Description, szName.c_str());

#ifdef _DEBUG
            LOG_DEBUG("spoofing");
#endif
        }

        if (isFfxDriver || caller.starts_with("amd_fidelityfx_upscaler_dx12"))
        {
            const auto primaryGpu = IdentifyGpu::getPrimaryGpu();

            if (primaryGpu.fsr4ForcedSupport && primaryGpu.fsr4Support == FSR4Support::INT8)
            {
                LOG_TRACE("Spoofing vendor AMD for {}", caller);
                pDesc->VendorId = VendorId::AMD;
            }
        }
    }

    AttachToAdapter(This);

    return result;
}

HRESULT DxgiSpoofing::hkGetDesc(IDXGIAdapter* This, DXGI_ADAPTER_DESC* pDesc)
{
    auto result = o_GetDesc(This, pDesc);

    auto caller = Util::WhoIsTheCaller(_ReturnAddress());

    if (iequals(caller, "vulkan-1.dll") || iequals(caller, "amdvlk64.dll") || iequals(caller, "dxgi.dll") ||
        iequals(caller, "d3d12.dll") || iequals(caller, "d3d12Core.dll"))
    {
        return result;
    }

#if _DEBUG
    LOG_TRACE("result: {:X}, caller: {}", (UINT) result, caller);
#endif

    if (result == S_OK)
    {
        if (Config::Instance()->DxgiVRAM.has_value())
        {
            uint64_t newMemSize = (uint64_t) Config::Instance()->DxgiVRAM.value() * 1073741824; // 1024 * 1024 * 1024
            pDesc->DedicatedVideoMemory = newMemSize;
        }

        auto targetVendorIdMatches = !Config::Instance()->TargetVendorId.has_value() ||
                                     Config::Instance()->TargetVendorId.value() == pDesc->VendorId;

        auto targetDeviceIdMatches = !Config::Instance()->TargetDeviceId.has_value() ||
                                     Config::Instance()->TargetDeviceId.value() == pDesc->DeviceId;

        if (pDesc->VendorId != VendorId::Microsoft && targetVendorIdMatches && targetDeviceIdMatches &&
            Config::Instance()->DxgiSpoofing.value_or_default() && !SkipSpoofing())
        {
            pDesc->VendorId = Config::Instance()->SpoofedVendorId.value_or_default();
            pDesc->DeviceId = Config::Instance()->SpoofedDeviceId.value_or_default();

            auto szName = Config::Instance()->SpoofedGPUName.value_or_default();
            std::memset(pDesc->Description, 0, sizeof(pDesc->Description));
            std::wcscpy(pDesc->Description, szName.c_str());

#ifdef _DEBUG
            LOG_DEBUG("spoofing");
#endif
        }
    }

    AttachToAdapter(This);

    return result;
}

#pragma endregion

#pragma region DXGI Attach methods

void DxgiSpoofing::AttachToAdapter(IUnknown* unkAdapter)
{
    static bool logAdded = false;
    if (!Config::Instance()->DxgiSpoofing.value_or_default() && !Config::Instance()->DxgiVRAM.has_value())
    {
        if (!logAdded)
        {
            LOG_WARN("DxgiSpoofing and DxgiVRAM is disabled, skipping hooking");
            logAdded = true;
        }

        return;
    }

    if (o_GetDesc != nullptr && o_GetDesc1 != nullptr && o_GetDesc2 != nullptr && o_GetDesc3 != nullptr &&
        o_CheckInterfaceSupport != nullptr)
        return;

    PVOID* pVTable = *(PVOID**) unkAdapter;

    IDXGIAdapter* adapter = nullptr;
    bool adapterOk = unkAdapter->QueryInterface(__uuidof(IDXGIAdapter), (void**) &adapter) == S_OK;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_GetDesc == nullptr && adapterOk)
    {
        LOG_DEBUG("Attach to GetDesc");
        o_GetDesc = (PFN_GetDesc) pVTable[8];
        DetourAttach(&(PVOID&) o_GetDesc, hkGetDesc);
    }

    if (o_CheckInterfaceSupport == nullptr && adapterOk)
    {
        LOG_DEBUG("Attach to CheckInterfaceSupport");
        o_CheckInterfaceSupport = (PFN_CheckInterfaceSupport) pVTable[9];
        DetourAttach(&(PVOID&) o_CheckInterfaceSupport, hkCheckInterfaceSupport);
    }

    if (adapter != nullptr)
        adapter->Release();

    IDXGIAdapter1* adapter1 = nullptr;
    if (o_GetDesc1 == nullptr && unkAdapter->QueryInterface(__uuidof(IDXGIAdapter1), (void**) &adapter1) == S_OK)
    {
        LOG_DEBUG("Attach to GetDesc1");
        o_GetDesc1 = (PFN_GetDesc1) pVTable[10];
        DetourAttach(&(PVOID&) o_GetDesc1, hkGetDesc1);
    }

    if (adapter1 != nullptr)
        adapter1->Release();

    IDXGIAdapter2* adapter2 = nullptr;
    if (o_GetDesc2 == nullptr && unkAdapter->QueryInterface(__uuidof(IDXGIAdapter2), (void**) &adapter2) == S_OK)
    {
        LOG_DEBUG("Attach to GetDesc2");
        o_GetDesc2 = (PFN_GetDesc2) pVTable[11];
        DetourAttach(&(PVOID&) o_GetDesc2, hkGetDesc2);
    }

    if (adapter2 != nullptr)
        adapter2->Release();

    IDXGIAdapter4* adapter4 = nullptr;
    if (o_GetDesc3 == nullptr && unkAdapter->QueryInterface(__uuidof(IDXGIAdapter4), (void**) &adapter4) == S_OK)
    {
        LOG_DEBUG("Attach to GetDesc3");
        o_GetDesc3 = (PFN_GetDesc3) pVTable[18];
        DetourAttach(&(PVOID&) o_GetDesc3, hkGetDesc3);
    }

    if (adapter4 != nullptr)
        adapter4->Release();

    auto detourResult = DetourTransactionCommit();
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("DetourTransactionCommit error: {:X}", detourResult);
        o_GetDesc = nullptr;
        o_GetDesc1 = nullptr;
        o_GetDesc2 = nullptr;
        o_GetDesc3 = nullptr;
        o_CheckInterfaceSupport = nullptr;
    }
}

#pragma endregion
