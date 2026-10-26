#pragma once
#include "d3d12_external.h"
#include <functional>
#include <string>

namespace d4r::win::commands {
// Intercept only documented D3D12 COM methods on this device's runtime.
// A boundary becomes prefix -> callback -> suffix at ExecuteCommandLists,
// preserving preceding lists in the caller's batch and current-frame output.
using Boundary = std::function<void(ID3D12CommandQueue*)>;
// Tags every command signature made from now on with its argument description, which a split needs to know what an
// ExecuteIndirect resets. Cheap enough to run before the hooks proper, so an upscaler picked later finds them tagged.
void track_signatures(ID3D12Device* device);
void install(ID3D12Device* device);
void record_boundary(ID3D12GraphicsCommandList* list, Boundary callback);
// The same for a library that cannot share std::function with this module (the frame generation NVAPI):
// false when the list cannot be split. command_mark names a recording and how many commands it holds, other than
// barriers and markers; the same mark twice means nothing was recorded in between.
using BoundaryC = void (*)(void* user, ID3D12CommandQueue* queue);
bool record_boundary_c(ID3D12GraphicsCommandList* list, BoundaryC callback, void* user) noexcept;
uint64_t command_mark(ID3D12GraphicsCommandList* list);
// The state the recording has left a resource in, or `fallback` when it has not seen it change.
uint32_t resource_state_c(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, uint32_t fallback) noexcept;
std::string error();
// Why the last split was refused; unlike error() it does not stay once set.
std::string boundary_failure();
size_t live_recordings();
D3D12_RESOURCE_STATES resource_state(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES fallback);
ResourceAccess resource_access(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, ResourceAccess fallback);
ResourceAccess submitted_resource_access(ID3D12Resource* resource, ResourceAccess fallback);
// Internal copies and callback submissions must bypass logical list routing.
struct InternalScope {
    bool previous;
    InternalScope();
    ~InternalScope();
};
}
