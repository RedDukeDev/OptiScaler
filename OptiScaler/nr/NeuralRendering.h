#pragma once

// DLSS 5 Neural Rendering on AMD, through the dlss5nr runtime
// (https://github.com/RedDukeDev/dlss5-nr-amd-custom).
//
// The runtime lives in the dlss5nr folder beside OptiScaler and is loaded on
// first use. It does the work -- capturing the frame, running the network on a
// thread of its own, composing its result back -- and this class only hands it
// what OptiScaler already knows about the game's frame: the upscaler's inputs
// and output, the command list, and the moment of Present.
//
// Where it runs:
//   before the upscaler (default)  on the upscaler's colour input, at render
//                                  resolution (NrPreUpscale): much cheaper,
//                                  somewhat lower quality
//   after the upscaler             a stage of IFeature_Dx12's shader pipeline,
//                                  between the upscaler's output and the next
//                                  stage, at output resolution
//   without an upscaler            on the finished image, at Present, when no
//                                  upscaler has handed over a frame lately
//                                  (nothing to select: it is what happens
//                                  when none is in use): no depth and no motion
//                                  vectors, and the game's interface included

#include <d3d12.h>
#include <dxgi1_4.h>

#include <filesystem>
#include <string>

#include "dlss5nr.h"

class IFeature;
struct NVSDK_NGX_Parameter;

class NeuralRendering
{
  public:
    // Whether the effect is switched on in the configuration.
    static bool Enabled();
    static bool PreUpscale();

    // Before the upscaler: returns the colour the upscaler should read instead
    // of the game's, or nullptr to leave the game's in place.
    static ID3D12Resource* BeforeUpscale(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                         NVSDK_NGX_Parameter* parameters, IFeature* feature);

    // After the upscaler: the buffer the upscaler writes into instead of the
    // next stage's input -- like `like`, in the unordered access state.
    static ID3D12Resource* AfterUpscaleBuffer(ID3D12Device* device, ID3D12Resource* like);

    // After the upscaler: composes input into output, same size, both in the
    // unordered access state. Without the runtime it copies.
    static bool AfterUpscale(ID3D12Device* device, ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* parameters,
                             IFeature* feature, ID3D12Resource* input, ID3D12Resource* output);

    // At Present, before the frame's fence: enhances the back buffer itself
    // when no upscaler is doing it. Whether it did.
    static bool WithoutUpscaler(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue);
    // True while that is how the effect runs.
    static bool RunsWithoutUpscaler();

    // At Present, with the queue the frame went to.
    static void Present(ID3D12CommandQueue* queue);

    // Library loads the runtime's own thread makes must reach the real files:
    // OptiScaler otherwise hands back its own stand-ins for nvngx.dll and
    // nvapi64.dll, and the network would talk to those.
    static bool BypassLibraryHooks();

    // For the menu.
    static bool RuntimePresent();
    static void GetStatus(dlss5nr_status& status);
    static std::filesystem::path RuntimeDirectory();

    // New settings from the configuration, from the next frame on.
    static void ApplySettings();

  private:
    static bool EnsureRuntime(ID3D12Device* device);
    static void PushSettings();
    static bool FillFrame(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* parameters, IFeature* feature,
                          dlss5nr_frame& frame, bool beforeUpscale);
};
