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
// A Direct3D 11 game works through OptiScaler's Direct3D 11 to 12 bridge (Dx11WithDx12): the runtime
// is a Direct3D 12 one, so the game's textures are copied into shared ones and the result copied
// back, at each of the places below.
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

#include <d3d11.h>
#include <d3d12.h>

#include <with_dx12/dx11_with_dx12.h>
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
    // The same for a Direct3D 11 game, through OptiScaler's Direct3D 11 to 12 bridge: the back
    // buffer goes to the runtime in a shared texture and the result comes back in another.
    static bool WithoutUpscalerDx11(IDXGISwapChain* swapChain, ID3D11Device* device);
    // The same two stages for a Direct3D 11 upscaler, through the bridge: before it, the colour it
    // should read instead of the game's (or nullptr); after it, the buffer it writes into and the
    // composition of that buffer into the output.
    static ID3D11Resource* BeforeUpscaleDx11(ID3D11Device* device, ID3D11DeviceContext* context,
                                             NVSDK_NGX_Parameter* parameters, IFeature* feature);
    static ID3D11Resource* AfterUpscaleBufferDx11(ID3D11Device* device, ID3D11Resource* like);
    static bool AfterUpscaleDx11(ID3D11Device* device, ID3D11DeviceContext* context, NVSDK_NGX_Parameter* parameters,
                                 IFeature* feature, ID3D11Resource* input, ID3D11Resource* output);

    // For the Vulkan bridge (NeuralRenderingVk): counts a present and says whether the finished
    // image is to be enhanced; and makes sure the runtime is up on the bridge's Direct3D 12 device.
    static bool WantsFinishedImage();
    static bool PrepareVulkan(ID3D12Device* device);

    // At Present, in a Direct3D 11 game: the same as Present(queue), on the bridge's queue.
    static void PresentDx11();
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
    friend class NeuralRenderingVk;

    static bool EnsureRuntime(ID3D12Device* device);
    static void PushSettings();
    // Depth, motion vectors and exposure of a Direct3D 11 game, as the bridge's shared textures.
    struct Dx11Inputs
    {
        ID3D12Resource* depth;
        ID3D12Resource* motion;
        ID3D12Resource* exposure;
    };
    // A pair of the bridge's shared textures: the image to read and the result to write.
    struct Dx11Slot
    {
        Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C* input;
        Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C* output;
    };
    static bool Dx11BridgeUnavailable();
    static bool PrepareDx11(ID3D11Device* device, ID3D11DeviceContext* context);
    static bool ProcessDx11(NVSDK_NGX_Parameter* parameters, IFeature* feature, ID3D11Resource* input, Dx11Slot& slot,
                            bool beforeUpscale);
    static bool ComposeFinished(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* source,
                                ID3D12Resource* dest, const dlss5nr_frame* prefilled = nullptr);
    static bool FillFrame(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* parameters, IFeature* feature,
                          dlss5nr_frame& frame, bool beforeUpscale, const Dx11Inputs* dx11 = nullptr);
};
