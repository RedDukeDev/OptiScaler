#pragma once
// The few types of d4r's tools/windows/d3d12_external.h that its command-hooks module uses.
//
// The module comes from d4r (https://github.com/countervolts/d4r, GPL-3.0) and is kept as it is there;
// the one thing that differs is this header, which stands in for d4r's own: that one carries the whole
// of d4r's HIP and shared-memory machinery (hip_api.h and more), none of which the module needs.
// update_d4r_commands.py copies the module over from a d4r checkout and leaves this file alone.
#if defined(__MINGW32__)
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdio>
#include <stdexcept>
#include <string>

namespace d4r::win {
using Microsoft::WRL::ComPtr;

inline void dx(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " HRESULT=" + std::to_string(result));
}

struct Handle {
    HANDLE value = nullptr;
    Handle() = default;
    Handle(const Handle&) = delete;
    ~Handle() { if (value) CloseHandle(value); }
};

inline D3D12_RESOURCE_DESC resource_desc(ID3D12Resource* resource) {
#if defined(__MINGW32__)
    D3D12_RESOURCE_DESC desc{}; resource->GetDesc(&desc); return desc;
#else
    return resource->GetDesc();
#endif
}

// How a texture is accessed at a point of a command list: a legacy resource state, or an enhanced
// barrier's layout, access and sync.
struct ResourceAccess {
    D3D12_RESOURCE_STATES legacy = D3D12_RESOURCE_STATE_COMMON;
    bool enhanced = false, pending_split = false, inherited = false;
    D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_COMMON;
    D3D12_BARRIER_ACCESS access = D3D12_BARRIER_ACCESS_COMMON;
    D3D12_BARRIER_SYNC sync = D3D12_BARRIER_SYNC_ALL;
    ResourceAccess() = default;
    ResourceAccess(D3D12_RESOURCE_STATES state) : legacy(state) {}
};
}
