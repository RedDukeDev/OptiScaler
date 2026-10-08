#include <pch.h>

#include "NeuralRendering.h"

#include <Config.h>
#include <State.h>
#include <upscalers/IFeature.h>

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
    settings.keep_local_tone = config->NrKeepLocalTone.value_or_default() ? 1 : 0;
    settings.blend_frames = config->NrBlendFrames.value_or_default();
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
    LOG_INFO("Neural rendering runtime started from {}", wstring_to_string(directory.wstring()));
    return true;
}

bool NeuralRendering::FillFrame(ID3D12GraphicsCommandList* cmd, NVSDK_NGX_Parameter* parameters, IFeature* feature,
                                dlss5nr_frame& frame, bool beforeUpscale)
{
    auto config = Config::Instance();

    frame.struct_size = sizeof(frame);
    frame.command_list = cmd;

    frame.depth = GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_Depth);
    frame.motion = GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_MotionVectors);
    frame.depth_state = StateFromConfig(config->DepthResourceBarrier);
    frame.motion_state = StateFromConfig(config->MVResourceBarrier);
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

    frame.exposure = feature->AutoExposure()
                         ? nullptr
                         : GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_ExposureTexture);
    frame.exposure_state = StateFromConfig(config->ExposureResourceBarrier);
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

bool NeuralRendering::WithoutUpscaler(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue)
{
    if (_presentsSinceUpscaler < kPresentsWithoutUpscaler)
        ++_presentsSinceUpscaler;

    if (!RunsWithoutUpscaler() || !RuntimePresent() || swapChain == nullptr || queue == nullptr)
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
            backBuffer->Release();
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

    auto description = backBuffer->GetDesc();
    const bool resized = description.Width != _aloneWidth || description.Height != _aloneHeight;
    _aloneWidth = description.Width;
    _aloneHeight = description.Height;

    const auto shown = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto written = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!EnsureBuffer(device, _aloneInput, backBuffer, shown) || !EnsureBuffer(device, _aloneOutput, backBuffer, written))
    {
        backBuffer->Release();
        return false;
    }

    PushSettings();

    auto cmd = _aloneList;
    _aloneAllocator[slot]->Reset();
    cmd->Reset(_aloneAllocator[slot], nullptr);

    // A back buffer is neither readable by a shader nor writable as an
    // unordered access view in general: it is copied out, and the result
    // copied back.
    Barrier(cmd, backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd, _aloneInput, shown, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(_aloneInput, backBuffer);
    Barrier(cmd, _aloneInput, D3D12_RESOURCE_STATE_COPY_DEST, shown);

    dlss5nr_frame frame {};
    frame.struct_size = sizeof(frame);
    frame.command_list = cmd;
    frame.input = _aloneInput;
    frame.input_state = shown;
    frame.output = _aloneOutput;
    frame.output_state = written;
    frame.width = (uint32_t) description.Width;
    frame.height = description.Height;
    frame.pre_exposure = 1.0f;
    // A floating point back buffer is scRGB: linear light, and beyond 1.
    frame.hdr = description.Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 1 : 0;
    frame.reset = resized ? 1 : 0;

    const bool composed = _api.process(_context, &frame) == 0;

    if (composed)
    {
        Barrier(cmd, _aloneOutput, written, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(backBuffer, _aloneOutput);
        Barrier(cmd, backBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        Barrier(cmd, _aloneOutput, D3D12_RESOURCE_STATE_COPY_SOURCE, written);
    }
    else
    {
        Barrier(cmd, backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
    }

    cmd->Close();
    ID3D12CommandList* lists[] = { cmd };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(_aloneFence, ++_aloneFenceValue);
    _aloneAllocatorFence[slot] = _aloneFenceValue;

    backBuffer->Release();
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
