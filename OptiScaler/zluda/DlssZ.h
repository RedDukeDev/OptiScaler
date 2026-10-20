#pragma once

// DLSS super resolution and ray reconstruction on AMD, through the dlss5nr runtime
// (https://github.com/RedDukeDev/dlss5-nr-amd-custom): NVIDIA's own libraries, run on CUDA that ZLUDA
// provides over HIP. This is what the runtime's C API needs of a host that is not NVIDIA's driver:
//
//   - the runtime library, loaded once from the dlss5nr folder, the same one the neural rendering uses;
//   - the game's command lists split where the upscaler runs (the module vendored in zluda/d4r_commands,
//     from d4r https://github.com/countervolts/d4r): the network runs between what the game recorded
//     before the upscale and what it records after;
//   - the runtime's library loads let through OptiScaler's hooks (it needs its own nvngx.dll).
//
// The feature that uses it is upscalers/dlssz/DLSSZFeature_Dx12.h.

#include <d3d12.h>

#include <filesystem>

#include "nr/dlss5nr.h"

class IFeature;

namespace DlssZ
{

struct Api
{
    decltype(&dlss5nr_upscaler_create) create = nullptr;
    decltype(&dlss5nr_upscaler_destroy) destroy = nullptr;
    decltype(&dlss5nr_upscaler_evaluate) evaluate = nullptr;
    decltype(&dlss5nr_upscaler_get_status) getStatus = nullptr;
    decltype(&dlss5nr_upscaler_last_error) lastError = nullptr;
    decltype(&dlss5nr_upscaler_loader_thread) loaderThread = nullptr;
};

// The library and the snippet of that network are where they were looked for (the dlss5nr folder, or
// the path given in the configuration).
bool Available(bool rayReconstruction);
// Whether the upscaler can be offered here: not an NVIDIA GPU (it has the real thing), the configuration
// does not forbid it (Enabled=false) and the files are there.
bool Usable(bool rayReconstruction);
std::filesystem::path SnippetPath(bool rayReconstruction);
std::filesystem::path RuntimeDirectory();

// Loads the runtime (once; false when it cannot be loaded).
bool Load();
const Api& Functions();

// Whether the game's command lists are to be tracked from the creation of the device: the runtime is
// there, the GPU is not NVIDIA's and the configuration asks for DLSS (or leaves the choice to the
// upscaler selected, DLSS).
bool WantsEarlyHooks();

// Installs the command-list hooks on this device's runtime (once; they serve every device of it).
// `late` says it is not done as the device is created: command lists that began before are not tracked and
// cannot be split, so an upscaler leaves out the first few frames after (see HooksAreSettled).
bool InstallCommandHooks(ID3D12Device* device, bool late);
bool CommandHooksInstalled();
bool HooksAreSettled();

// Whether this feature is DLSS run through the runtime.
bool IsDlssZ(const IFeature* feature);

// Library loads made by the runtime while it creates an upscaler must reach the real files.
bool BypassLibraryHooks();

// ZLUDA and HIP do not survive the unloading of their libraries at the end of a process: one thread
// spins in HIP's detach, for minutes, and the process cannot be killed meanwhile. Once the runtime is
// loaded (by this or by the neural rendering), the process ends with TerminateProcess instead of
// ExitProcess, which skips that unloading. [ZludaDlss] FastExit=false keeps the ordinary exit.
void InstallFastExit();

} // namespace DlssZ
