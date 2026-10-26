#pragma once
#include <d3d12.h>
#include <dxgiformat.h>
#include <cstdint>

// NVIDIA's frame generation (through the Nvidia provider of Nvngx_FG) driven by OptiScaler's own FG pipeline, where no
// Streamline is there to fill in the library's parameters: what a frame of the game gives OptiScaler goes in, the
// generated frame comes out. Used by the FSR-FG output (its swapchain does the pacing) in place of FSR's interpolation.
class NvInterpolator
{
  public:
    struct Frame
    {
        ID3D12Device* device = nullptr;
        ID3D12GraphicsCommandList* commandList = nullptr;

        // The frame being presented, with the interface; the same without it, when there is one
        ID3D12Resource* backbuffer = nullptr;
        ID3D12Resource* hudless = nullptr;
        // The interface on its own, with its alpha: the game's where it gives one, else an empty one is used
        ID3D12Resource* ui = nullptr;
        ID3D12Resource* depth = nullptr;
        ID3D12Resource* motion = nullptr;
        // Where the generated frame goes
        ID3D12Resource* output = nullptr;

        uint32_t displayWidth = 0, displayHeight = 0;
        uint32_t renderWidth = 0, renderHeight = 0;
        DXGI_FORMAT backbufferFormat = DXGI_FORMAT_UNKNOWN;

        float jitterX = 0, jitterY = 0;
        float motionScaleX = 1, motionScaleY = 1;
        float cameraNear = 0, cameraFar = 0, cameraFov = 0, cameraAspect = 0;
        float cameraPosition[3] {}, cameraUp[3] {}, cameraRight[3] {}, cameraForward[3] {};
        // The frame before's, for the matrix between the two; all zeros where the game does not say
        float previousPosition[3] {}, previousUp[3] {}, previousRight[3] {}, previousForward[3] {};

        // The queue the game presents on, which the library is told of
        ID3D12CommandQueue* queue = nullptr;

        bool depthInverted = false;
        bool hdr = false;
        bool jitteredMotion = false;
        bool reset = false;
        uint64_t frameId = 0;
    };

    // True when the generated frame is in `frame.output`
    static bool Evaluate(const Frame& frame);

    // The library's feature goes with the device or the size it was made for
    static void Release();
};
