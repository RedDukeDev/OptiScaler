#include <pch.h>

#include "NeuralRendering.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <upscalers/IFeature.h>
#include <with_dx12/dx11_with_dx12.h>

#include <vector>

namespace
{

struct RuntimeApi
{
    decltype(&dlss5nr_create) create = nullptr;
    decltype(&dlss5nr_destroy) destroy = nullptr;
    decltype(&dlss5nr_default_settings) defaultSettings = nullptr;
    decltype(&dlss5nr_set_settings) setSettings = nullptr;
    decltype(&dlss5nr_process) process = nullptr;
    decltype(&dlss5nr_present) present = nullptr;
    decltype(&dlss5nr_get_status) getStatus = nullptr;
    decltype(&dlss5nr_loader_thread) loaderThread = nullptr;
};

HMODULE _module = nullptr;
RuntimeApi _api;
dlss5nr_context* _context = nullptr;
bool _tried = false;
DWORD _loaderThread = 0;
bool _settingsDirty = true;

// The colour the upscaler reads before the upscaler, and the buffer the
// upscaler writes into after it. Replaced buffers are kept for a few frames:
// the GPU may still be using them.
ID3D12Resource* _preBuffer = nullptr;
ID3D12Resource* _postBuffer = nullptr;
struct Retired
{
    ID3D12Resource* resource;
    int frames;
};
std::vector<Retired> _retired;

// Without an upscaler: Presents since an upscaler last handed over a frame,
// and what the pass on the back buffer needs of its own -- a command list, a
// few allocators in turn (each reused only once the GPU is done with it), a
// copy of the back buffer to read and a buffer to compose into.
constexpr int kPresentsWithoutUpscaler = 8;
constexpr UINT kAloneAllocators = 4;
int _presentsSinceUpscaler = kPresentsWithoutUpscaler;
// Set when the command list for the finished-image path could not be made.
bool _aloneFailed = false;
ID3D12CommandAllocator* _aloneAllocator[kAloneAllocators] = {};
UINT64 _aloneAllocatorFence[kAloneAllocators] = {};
ID3D12GraphicsCommandList* _aloneList = nullptr;
ID3D12Fence* _aloneFence = nullptr;
HANDLE _aloneEvent = nullptr;
UINT64 _aloneFenceValue = 0;
UINT _aloneNext = 0;
ID3D12Resource* _aloneInput = nullptr;
ID3D12Resource* _aloneOutput = nullptr;
UINT64 _aloneWidth = 0;
UINT _aloneHeight = 0;
// With a Direct3D 11 game: the finished image copied into a texture both APIs can open, and the
// texture the result comes back in.
Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C _dx11Input = {};
Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C _dx11Output = {};

void Retire(ID3D12Resource*& resource)
{
    if (resource != nullptr)
        _retired.push_back({ resource, 8 });

    resource = nullptr;
}

void RuntimeLog(void*, int level, const char* message)
{
    switch (level)
    {
    case DLSS5NR_LOG_ERROR:
        LOG_ERROR("dlss5nr: {}", message);
        break;
    case DLSS5NR_LOG_WARNING:
        LOG_WARN("dlss5nr: {}", message);
        break;
    case DLSS5NR_LOG_INFO:
        LOG_INFO("dlss5nr: {}", message);
        break;
    default:
        LOG_DEBUG("dlss5nr: {}", message);
        break;
    }
}

template <typename T> T GetParameter(NVSDK_NGX_Parameter* parameters, const char* name)
{
    T value {};
    if (parameters->Get(name, &value) != NVSDK_NGX_Result_Success)
        parameters->Get(name, (void**) &value);

    return value;
}

D3D12_RESOURCE_STATES StateFromConfig(const CustomOptional<int32_t, NoDefault>& setting)
{
    return setting.has_value() ? (D3D12_RESOURCE_STATES) setting.value()
                               : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
}

// sRGB formats cannot back an unordered access view; their typeless family
// can, and the runtime writes through a UNORM view of it.
DXGI_FORMAT UavCapable(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8X8_TYPELESS;
    default:
        return format;
    }
}

// A texture like `like` (size, format, flags), writable as an unordered access
// view, created in `state`; reused while those stay the same.
bool EnsureBuffer(ID3D12Device* device, ID3D12Resource*& buffer, ID3D12Resource* like, D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC wanted = like->GetDesc();
    wanted.Format = UavCapable(wanted.Format);
    wanted.MipLevels = 1;
    wanted.DepthOrArraySize = 1;
    wanted.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    wanted.Flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;

    if (buffer != nullptr)
    {
        auto current = buffer->GetDesc();
        if (current.Width == wanted.Width && current.Height == wanted.Height && current.Format == wanted.Format &&
            current.Flags == wanted.Flags)
            return true;

        Retire(buffer);
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    if (device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &wanted, state, nullptr,
                                        IID_PPV_ARGS(&buffer)) != S_OK)
    {
        LOG_ERROR("Can't create the neural rendering buffer ({}x{}, format {})", wanted.Width, wanted.Height,
                  (int) wanted.Format);
        buffer = nullptr;
        return false;
    }

    buffer->SetName(L"dlss5nr buffer");
    return true;
}

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
}

} // namespace

bool NeuralRendering::Enabled() { return Config::Instance()->NrEnabled.value_or_default(); }

bool NeuralRendering::PreUpscale() { return Config::Instance()->NrPreUpscale.value_or_default(); }

std::filesystem::path NeuralRendering::RuntimeDirectory()
{
    return std::filesystem::path(Config::Instance()->MainDllPath.value()) / L"dlss5nr";
}

bool NeuralRendering::RuntimePresent()
{
    auto library = RuntimeDirectory() / L"dlss5nr_runtime.dll";
    bool present = std::filesystem::exists(library);

    // Once, with the whole path: the menu asks on every frame.
    static bool logged = false;
    if (!present && !logged)
    {
        LOG_WARN("Neural rendering: dlss5nr runtime not found, expected {}", wstring_to_string(library.wstring()));
        logged = true;
    }

    return present;
}

bool NeuralRendering::BypassLibraryHooks()
{
    return _loaderThread != 0 && GetCurrentThreadId() == _loaderThread;
}

void NeuralRendering::ApplySettings() { _settingsDirty = true; }

void NeuralRendering::PushSettings()
{
    if (!_settingsDirty || _context == nullptr)
        return;

    auto config = Config::Instance();
    dlss5nr_settings settings {};
    _api.defaultSettings(&settings);
    settings.enabled = 1;
    settings.resolution_scale = config->NrResolutionScale.value_or_default();
    settings.style = config->NrStyle.value_or_default();
    settings.intensity = config->NrIntensity.value_or_default();
    settings.local_tone = config->NrLocalTone.value_or_default();
    settings.local_structure = config->NrLocalStructure.value_or_default();
    settings.skin_structure = config->NrSkinStructure.value_or_default();
    settings.detail_strength = config->NrDetailStrength.value_or_default();
    settings.colour_strength = config->NrColourStrength.value_or_default();
    settings.max_ratio = config->NrMaxRatio.value_or_default();
    settings.follow_motion = config->NrFollowMotion.value_or_default() ? 1 : 0;
    settings.network_history = config->NrNetworkHistory.value_or_default() ? 1 : 0;
    settings.show_tracking = config->NrShowTracking.value_or_default() ? 1 : 0;
    settings.wait_for_network = config->NrWaitForNetwork.value_or_default() ? 1 : 0;
    settings.passes = config->NrPasses.value_or_default();
    settings.network_preset = config->NrPreset.value_or_default();
    settings.keep_local_tone = config->NrKeepLocalTone.value_or_default() ? 1 : 0;
    settings.blend_frames = config->NrBlendFrames.value_or_default();
    settings.wait_inside = config->NrWaitInside.value_or_default() ? 1 : 0;
    settings.max_age = config->NrMaxAge.value_or_default();
    settings.encoding = config->NrEncoding.value_or_default();
    settings.exposure_mode = config->NrExposureMode.value_or_default();
    settings.exposure = config->NrExposure.value_or_default();
    _api.setSettings(_context, &settings);
    _settingsDirty = false;
}

ID3D12Resource* NeuralRendering::AfterUpscaleBuffer(ID3D12Device* device, ID3D12Resource* like)
{
    // The upscaler writes it as its output, so it lives as an unordered
    // access view, as OptiScaler's other stages keep theirs.
    if (like == nullptr || !EnsureBuffer(device, _postBuffer, like, D3D12_RESOURCE_STATE_UNORDERED_ACCESS))
        return nullptr;

    return _postBuffer;
}

bool NeuralRendering::EnsureRuntime(ID3D12Device* device)
{
    if (_context != nullptr)
        return true;

    if (_tried)
        return false;

    _tried = true;
    const double began = Util::MillisecondsNow();

    auto directory = RuntimeDirectory();
    auto library = directory / L"dlss5nr_runtime.dll";

    // Its own dependencies are looked up beside it, not in the game's folder.
    _module = LoadLibraryExW(library.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (_module == nullptr)
    {
        LOG_ERROR("Neural rendering needs {}, which could not be loaded ({})", wstring_to_string(library.wstring()),
                  GetLastError());
        return false;
    }

    LOG_INFO("Neural rendering: runtime library loaded in {:.0f} ms", Util::MillisecondsNow() - began);

#define BIND(field, name)                                                                                              \
    _api.field = (decltype(_api.field)) GetProcAddress(_module, name);                                                 \
    if (_api.field == nullptr)                                                                                         \
    {                                                                                                                  \
        LOG_ERROR("dlss5nr_runtime.dll does not export " name);                                                        \
        return false;                                                                                                  \
    }

    BIND(create, "dlss5nr_create");
    BIND(destroy, "dlss5nr_destroy");
    BIND(defaultSettings, "dlss5nr_default_settings");
    BIND(setSettings, "dlss5nr_set_settings");
    BIND(process, "dlss5nr_process");
    BIND(present, "dlss5nr_present");
    BIND(getStatus, "dlss5nr_get_status");
    BIND(loaderThread, "dlss5nr_loader_thread");
#undef BIND

    std::wstring snippet;
    if (Config::Instance()->NrSnippetPath.has_value())
        snippet = Config::Instance()->NrSnippetPath.value();

    dlss5nr_create_info info {};
    info.struct_size = sizeof(info);
    info.device = device;
    info.runtime_dir = directory.c_str();
    info.snippet_path = snippet.empty() ? nullptr : snippet.c_str();
    info.log = &RuntimeLog;

    if (_api.create(&info, &_context) != 0)
    {
        LOG_ERROR("dlss5nr_create failed");
        _context = nullptr;
        return false;
    }

    // Before the runtime loads anything: from here its thread's library
    // loads pass the hooks untouched.
    _loaderThread = _api.loaderThread(_context);
    LOG_INFO("Neural rendering runtime started from {} in {:.0f} ms", wstring_to_string(directory.wstring()),
             Util::MillisecondsNow() - began);
    return true;
}

bool NeuralRendering::FillFrame(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* parameters, IFeature* feature,
                                dlss5nr_frame& frame, bool beforeUpscale, const Dx11Inputs* dx11)
{
    auto config = Config::Instance();

    frame.struct_size = sizeof(frame);
    frame.command_list = cmd;

    // From a Direct3D 11 game they are the bridge's shared copies, in the common state.
    frame.depth = dx11 ? dx11->depth : GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_Depth);
    frame.motion = dx11 ? dx11->motion : GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_MotionVectors);
    frame.depth_state = dx11 ? D3D12_RESOURCE_STATE_COMMON : StateFromConfig(config->DepthResourceBarrier);
    frame.motion_state = dx11 ? D3D12_RESOURCE_STATE_COMMON : StateFromConfig(config->MVResourceBarrier);
    frame.depth_width = feature->RenderWidth();
    frame.depth_height = feature->RenderHeight();

    if (feature->LowResMV())
    {
        frame.motion_width = feature->RenderWidth();
        frame.motion_height = feature->RenderHeight();
    }
    else
    {
        frame.motion_width = feature->TargetWidth();
        frame.motion_height = feature->TargetHeight();
    }

    frame.motion_scale_x = 1.0f;
    frame.motion_scale_y = 1.0f;
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.motion_scale_x);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.motion_scale_y);
    frame.depth_inverted = feature->DepthInverted() ? 1 : 0;

    frame.exposure = feature->AutoExposure() ? nullptr
                     : dx11                  ? dx11->exposure
                                             : GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_ExposureTexture);
    frame.exposure_state = dx11 ? D3D12_RESOURCE_STATE_COMMON : StateFromConfig(config->ExposureResourceBarrier);
    frame.pre_exposure = 1.0f;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &frame.pre_exposure);

    frame.hdr = feature->IsHdr() ? 1 : 0;
    int reset = 0;
    parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    frame.reset = reset;

    if (beforeUpscale)
    {
        frame.width = feature->RenderWidth();
        frame.height = feature->RenderHeight();
    }
    else
    {
        frame.width = feature->TargetWidth();
        frame.height = feature->TargetHeight();
    }

    return frame.depth != nullptr && frame.motion != nullptr;
}

ID3D12Resource* NeuralRendering::BeforeUpscale(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                               NVSDK_NGX_Parameter* parameters, IFeature* feature)
{
    if (!Enabled() || !PreUpscale() || !EnsureRuntime(device))
        return nullptr;

    PushSettings();

    auto color = GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_Color);
    if (color == nullptr)
        return nullptr;

    dlss5nr_frame frame {};
    if (!FillFrame(cmd, parameters, feature, frame, true))
        return nullptr;

    // Kept in the state the upscaler expects its colour in, so whatever
    // barriers it records around the colour stay valid for the replacement.
    auto colorState = StateFromConfig(Config::Instance()->ColorResourceBarrier);
    if (!EnsureBuffer(device, _preBuffer, color, colorState))
        return nullptr;

    frame.input = color;
    frame.input_state = colorState;
    frame.output = _preBuffer;
    frame.output_state = colorState;

    if (_api.process(_context, &frame) != 0)
        return nullptr;

    _presentsSinceUpscaler = 0;
    return _preBuffer;
}

bool NeuralRendering::AfterUpscale(ID3D12Device* device, ID3D12GraphicsCommandList* cmd,
                                   NVSDK_NGX_Parameter* parameters, IFeature* feature, ID3D12Resource* input,
                                   ID3D12Resource* output)
{
    if (EnsureRuntime(device))
    {
        PushSettings();

        dlss5nr_frame frame {};
        if (FillFrame(cmd, parameters, feature, frame, false))
        {
            frame.input = input;
            frame.input_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            frame.output = output;
            frame.output_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

            if (_api.process(_context, &frame) == 0)
            {
                _presentsSinceUpscaler = 0;
                return true;
            }
        }
    }

    // No runtime, or no frame to give it: the stage still has to fill its
    // output, which the next stage (or the game) reads.
    Barrier(cmd, input, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(output, input);
    Barrier(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(cmd, input, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return true;
}

bool NeuralRendering::RunsWithoutUpscaler()
{
    return Enabled() && !_aloneFailed && _presentsSinceUpscaler >= kPresentsWithoutUpscaler;
}

bool NeuralRendering::WantsFinishedImage()
{
    if (_presentsSinceUpscaler < kPresentsWithoutUpscaler)
        ++_presentsSinceUpscaler;

    return RunsWithoutUpscaler() && RuntimePresent();
}

bool NeuralRendering::PrepareVulkan(ID3D12Device* device) { return RuntimePresent() && EnsureRuntime(device); }

bool NeuralRendering::WithoutUpscaler(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
    if (!WantsFinishedImage() || swapChain == nullptr || queue == nullptr)
        return false;

    ID3D12Device* device = nullptr;
    if (queue->GetDevice(IID_PPV_ARGS(&device)) != S_OK)
        return false;

    device->Release();

    if (!EnsureRuntime(device))
        return false;

    IDXGISwapChain3* swapChain3 = nullptr;
    if (swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3)) != S_OK)
        return false;

    ID3D12Resource* backBuffer = nullptr;
    const HRESULT got = swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer));
    swapChain3->Release();

    if (got != S_OK || backBuffer == nullptr)
        return false;

    const bool composed = ComposeFinished(device, queue, backBuffer, backBuffer);
    backBuffer->Release();
    return composed;
}

bool NeuralRendering::WithoutUpscalerDx11(IDXGISwapChain* swapChain, ID3D11Device* device)
{
    if (!WantsFinishedImage() || swapChain == nullptr || device == nullptr)
        return false;

    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    if (context == nullptr)
        return false;

    // OptiScaler's own bridge from Direct3D 11 to Direct3D 12: the device and queue the runtime
    // will work on, shared textures and the fences between the two.
    Dx11WithDx12::Init(device, context);
    context->Release();

    auto device12 = Dx11WithDx12::GetD3D12Device();
    auto queue12 = Dx11WithDx12::GetD3D12CommandQueue();
    auto context11 = Dx11WithDx12::GetD3D11DeviceContext();
    if (device12 == nullptr || queue12 == nullptr || context11 == nullptr || !EnsureRuntime(device12))
        return false;

    ID3D11Texture2D* backBuffer = nullptr;
    if (swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)) != S_OK || backBuffer == nullptr)
        return false;

    // The back buffer's content, in a texture both APIs can open; and a second one, for the
    // result to come back in.
    const UINT64 frameId = Dx11WithDx12::NextUpscalerFrameId();
    bool done = false;

    if (Dx11WithDx12::PrepareTextureFrom11To12("nr finished image", device12, backBuffer, &_dx11Input, true, false,
                                               false, frameId) &&
        Dx11WithDx12::PrepareTextureFrom11To12("nr composed image", device12, backBuffer, &_dx11Output, false, false,
                                               false, frameId) &&
        Dx11WithDx12::SyncDx11ToDx12())
    {
        if (ComposeFinished(device12, queue12, _dx11Input.Dx12Resource, _dx11Output.Dx12Resource) &&
            Dx11WithDx12::SyncDx12ToDx11())
        {
            context11->CopyResource(backBuffer, _dx11Output.SharedTexture);
            done = true;
        }
    }

    backBuffer->Release();
    return done;
}

void NeuralRendering::PresentDx11()
{
    // The queue the bridge's work went to; there is none until a Direct3D 11 game has used it.
    if (auto queue = Dx11WithDx12::GetD3D12CommandQueue(); queue != nullptr)
        Present(queue);
}

// ---------------------------------------------------------------- with a Direct3D 11 upscaler
//
// The upscaler's own inputs go through the same bridge as the finished image does: shared copies of
// colour, depth and motion vectors for the runtime, and the result in a shared texture.

namespace
{
Dx11WithDx12::D3D11_UPSCALER_RESOURCE_CACHE_C _dx11Cache = {};
Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C _dx11PreInput = {};
Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C _dx11PreOutput = {};
Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C _dx11PostInput = {};
Dx11WithDx12::D3D11_TEXTURE2D_RESOURCE_C _dx11PostOutput = {};
ID3D11Texture2D* _dx11PostBuffer = nullptr;
UINT _dx11Frame = 0;

ID3D11Resource* Dx11Parameter(NVSDK_NGX_Parameter* parameters, const char* name)
{
    ID3D11Resource* resource = nullptr;
    if (parameters->Get(name, &resource) != NVSDK_NGX_Result_Success)
        parameters->Get(name, (void**) &resource);

    return resource;
}
} // namespace

// The bridge's objects, and the runtime on its device; false when any is missing.
bool NeuralRendering::PrepareDx11(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if (!RuntimePresent() || device == nullptr || context == nullptr)
        return false;

    Dx11WithDx12::Init(device, context);

    auto device12 = Dx11WithDx12::GetD3D12Device();
    return device12 != nullptr && Dx11WithDx12::GetD3D12CommandQueue() != nullptr &&
           Dx11WithDx12::GetD3D11DeviceContext() != nullptr && EnsureRuntime(device12);
}

// Colour (or the upscaler's output), depth and motion through the network, between two Direct3D 11
// textures. `result` is the shared texture the answer is in, once SyncDx12ToDx11 has been called.
bool NeuralRendering::ProcessDx11(NVSDK_NGX_Parameter* parameters, IFeature* feature, ID3D11Resource* input,
                                  Dx11Slot& slot, bool beforeUpscale)
{
    auto device12 = Dx11WithDx12::GetD3D12Device();
    auto queue12 = Dx11WithDx12::GetD3D12CommandQueue();

    const bool dontUseNTShared = Config::Instance()->DontUseNTShared.value_or_default();
    auto mask = Dx11WithDx12::ResourceMask::Mv | Dx11WithDx12::ResourceMask::Depth;
    if (!feature->AutoExposure())
        mask |= Dx11WithDx12::ResourceMask::Exposure;

    const UINT64 frameId = Dx11WithDx12::NextUpscalerFrameId();
    const UINT frameIndex = _dx11Frame++;

    // Depth, motion vectors and exposure are the game's, copied to shared textures.
    if (!Dx11WithDx12::PrepareUpscalerResources(_dx11Cache, parameters, mask, frameIndex, frameId, dontUseNTShared,
                                                false, false)
             .Success)
        return false;

    if (!Dx11WithDx12::PrepareTextureFrom11To12("nr image", device12, input, slot.input, true, false, dontUseNTShared,
                                                frameId) ||
        !Dx11WithDx12::PrepareTextureFrom11To12("nr result", device12, input, slot.output, false, false,
                                                dontUseNTShared, frameId))
        return false;

    if (!Dx11WithDx12::SyncDx11ToDx12())
        return false;

    Dx11Inputs inputs {};
    inputs.depth = _dx11Cache.CurrentDepth().Dx12Resource;
    inputs.motion = _dx11Cache.CurrentMv().Dx12Resource;
    inputs.exposure = _dx11Cache.Exposure.Dx12Resource;

    dlss5nr_frame frame {};
    if (!FillFrame(nullptr, parameters, feature, frame, beforeUpscale, &inputs))
        return false;

    PushSettings();

    if (!ComposeFinished(device12, queue12, slot.input->Dx12Resource, slot.output->Dx12Resource, &frame))
        return false;

    _presentsSinceUpscaler = 0;
    return Dx11WithDx12::SyncDx12ToDx11();
}

ID3D11Resource* NeuralRendering::BeforeUpscaleDx11(ID3D11Device* device, ID3D11DeviceContext* context,
                                                   NVSDK_NGX_Parameter* parameters, IFeature* feature)
{
    if (!Enabled() || !PreUpscale() || !PrepareDx11(device, context))
        return nullptr;

    auto color = Dx11Parameter(parameters, NVSDK_NGX_Parameter_Color);
    if (color == nullptr)
        return nullptr;

    Dx11Slot slot { &_dx11PreInput, &_dx11PreOutput };
    if (!ProcessDx11(parameters, feature, color, slot, true))
        return nullptr;

    return _dx11PreOutput.SharedTexture;
}

ID3D11Resource* NeuralRendering::AfterUpscaleBufferDx11(ID3D11Device* device, ID3D11Resource* like)
{
    if (like == nullptr || device == nullptr)
        return nullptr;

    ID3D11Texture2D* texture = nullptr;
    if (like->QueryInterface(IID_PPV_ARGS(&texture)) != S_OK)
        return nullptr;

    D3D11_TEXTURE2D_DESC wanted {};
    texture->GetDesc(&wanted);
    texture->Release();

    // The upscaler writes it, and the bridge reads it.
    wanted.MipLevels = 1;
    wanted.ArraySize = 1;
    wanted.Usage = D3D11_USAGE_DEFAULT;
    wanted.CPUAccessFlags = 0;
    wanted.MiscFlags = 0;
    wanted.BindFlags |= D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;

    if (_dx11PostBuffer != nullptr)
    {
        D3D11_TEXTURE2D_DESC current {};
        _dx11PostBuffer->GetDesc(&current);
        if (current.Width == wanted.Width && current.Height == wanted.Height && current.Format == wanted.Format &&
            current.BindFlags == wanted.BindFlags)
            return _dx11PostBuffer;

        _dx11PostBuffer->Release();
        _dx11PostBuffer = nullptr;
    }

    if (device->CreateTexture2D(&wanted, nullptr, &_dx11PostBuffer) != S_OK)
    {
        LOG_ERROR("Can't create the neural rendering buffer for a Direct3D 11 upscaler ({}x{}, format {})",
                  wanted.Width, wanted.Height, (int) wanted.Format);
        _dx11PostBuffer = nullptr;
    }

    return _dx11PostBuffer;
}

bool NeuralRendering::AfterUpscaleDx11(ID3D11Device* device, ID3D11DeviceContext* context,
                                       NVSDK_NGX_Parameter* parameters, IFeature* feature, ID3D11Resource* input,
                                       ID3D11Resource* output)
{
    if (PrepareDx11(device, context))
    {
        Dx11Slot slot { &_dx11PostInput, &_dx11PostOutput };
        if (ProcessDx11(parameters, feature, input, slot, false))
        {
            Dx11WithDx12::GetD3D11DeviceContext()->CopyResource(output, _dx11PostOutput.SharedTexture);
            return true;
        }
    }

    // No runtime, or no frame to give it: the stage still has to fill its output.
    context->CopyResource(output, input);
    return true;
}

// The finished image through the network, between two resources in the common state (a swap
// chain's back buffer is in it as well when it waits to be presented): source is read, the
// result is written to dest, which may be source itself. Submits to queue. Whether the network's
// answer was composed in; when it was not, dest has not been touched.
bool NeuralRendering::ComposeFinished(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12Resource* source,
                                      ID3D12Resource* dest, const dlss5nr_frame* prefilled)
{
    // The list, its allocators and the fence that says when one is free again.
    if (_aloneList == nullptr)
    {
        bool made = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_aloneFence)) == S_OK;

        for (UINT i = 0; made && i < kAloneAllocators; ++i)
            made = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&_aloneAllocator[i])) == S_OK;

        made = made && device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _aloneAllocator[0], nullptr,
                                                 IID_PPV_ARGS(&_aloneList)) == S_OK;

        if (!made)
        {
            LOG_ERROR("Neural rendering without an upscaler: can't create its command list");
            _aloneFailed = true;
            return false;
        }

        _aloneList->Close();
        _aloneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    const UINT slot = _aloneNext++ % kAloneAllocators;
    if (_aloneFence->GetCompletedValue() < _aloneAllocatorFence[slot])
    {
        // The GPU is more than a few frames behind: wait for the frame that
        // last used this allocator rather than reset it under its feet.
        _aloneFence->SetEventOnCompletion(_aloneAllocatorFence[slot], _aloneEvent);
        WaitForSingleObject(_aloneEvent, 1000);
    }

    auto description = source->GetDesc();
    const bool resized = description.Width != _aloneWidth || description.Height != _aloneHeight;
    _aloneWidth = description.Width;
    _aloneHeight = description.Height;

    const auto shown = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto written = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!EnsureBuffer(device, _aloneInput, source, shown) || !EnsureBuffer(device, _aloneOutput, source, written))
        return false;

    PushSettings();

    auto cmd = _aloneList;
    _aloneAllocator[slot]->Reset();
    cmd->Reset(_aloneAllocator[slot], nullptr);

    // A back buffer is neither readable by a shader nor writable as an
    // unordered access view in general: it is copied out, and the result
    // copied back.
    Barrier(cmd, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, _aloneInput, shown, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(_aloneInput, source);
    Barrier(cmd, _aloneInput, D3D12_RESOURCE_STATE_COPY_DEST, shown);

    // With an upscaler the caller has described the frame (depth, motion, exposure, size, HDR);
    // on a finished image there is only the image.
    dlss5nr_frame frame {};
    if (prefilled != nullptr)
        frame = *prefilled;
    frame.struct_size = sizeof(frame);
    frame.command_list = cmd;
    frame.input = _aloneInput;
    frame.input_state = shown;
    frame.output = _aloneOutput;
    frame.output_state = written;
    if (prefilled == nullptr)
    {
        frame.width = (uint32_t) description.Width;
        frame.height = description.Height;
        frame.pre_exposure = 1.0f;
        // A floating point back buffer is scRGB: linear light, and beyond 1.
        frame.hdr = description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : 0;
        frame.reset = resized ? 1 : 0;
    }

    const bool composed = _api.process(_context, &frame) == 0;

    if (composed)
    {
        Barrier(cmd, _aloneOutput, written, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (dest == source)
        {
            Barrier(cmd, dest, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        else
        {
            Barrier(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(cmd, dest, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        cmd->CopyResource(dest, _aloneOutput);
        Barrier(cmd, dest, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        Barrier(cmd, _aloneOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, written);
    }
    else
    {
        Barrier(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    }

    cmd->Close();
    ID3D12CommandList* lists[] = { cmd };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(_aloneFence, ++_aloneFenceValue);
    _aloneAllocatorFence[slot] = _aloneFenceValue;

    return composed;
}

void NeuralRendering::Present(ID3D12CommandQueue* queue)
{
    for (size_t i = 0; i < _retired.size();)
    {
        if (--_retired[i].frames <= 0)
        {
            _retired[i].resource->Release();
            _retired[i] = _retired.back();
            _retired.pop_back();
        }
        else
        {
            ++i;
        }
    }

    if (_context != nullptr && queue != nullptr)
        _api.present(_context, queue);
}

void NeuralRendering::GetStatus(dlss5nr_status& status)
{
    status = {};
    status.struct_size = sizeof(status);

    if (_context == nullptr)
    {
        status.state = _tried ? DLSS5NR_STATE_FAILED : DLSS5NR_STATE_STARTING;
        snprintf(status.message, sizeof(status.message), "%s",
                 _tried ? "the runtime could not be loaded (see the log)" : "not started");
        return;
    }

    _api.getStatus(_context, &status);
}
