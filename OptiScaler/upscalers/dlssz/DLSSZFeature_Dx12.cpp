#include <pch.h>
#include <Config.h>

#include <imgui/ImGuiNotify.hpp>

#include <algorithm>
#include <optional>

#include "DLSSZFeature_Dx12.h"

#include <zluda/d4r_commands/d3d12_command_hooks.h>

namespace
{

template <typename T> T GetParameter(NVSDK_NGX_Parameter* parameters, const char* name)
{
    T value {};
    if (parameters->Get(name, &value) != NVSDK_NGX_Result_Success)
        parameters->Get(name, (void**) &value);

    return value;
}

// The first of the names under which a game may hand over a texture.
ID3D12Resource* FirstResource(NVSDK_NGX_Parameter* parameters, std::initializer_list<const char*> names)
{
    for (const char* name : names)
        if (auto resource = GetParameter<ID3D12Resource*>(parameters, name))
            return resource;

    return nullptr;
}

dlss5nr_point Origin(NVSDK_NGX_Parameter* parameters, const char* x, const char* y)
{
    unsigned int px = 0, py = 0;
    parameters->Get(x, &px);
    parameters->Get(y, &py);
    return { px, py };
}

// The state a texture is in at this point of the list: what the list's own recording says, else what the
// game is expected to leave it in (the configured barrier state, or the one NGX asks for).
template <typename Option>
std::optional<D3D12_RESOURCE_STATES> Override(const Option& configured)
{
    if (!configured.has_value())
        return std::nullopt;

    return (D3D12_RESOURCE_STATES) configured.value();
}

uint32_t StateAt(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                 std::optional<D3D12_RESOURCE_STATES> configured, D3D12_RESOURCE_STATES contract)
{
    const auto fallback = configured.value_or(contract);

    if (resource == nullptr)
        return fallback;

    try
    {
        return d4r::win::commands::resource_state(list, resource, fallback);
    }
    catch (...)
    {
        return fallback;
    }
}

dlss5nr_resource Resource(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                          std::optional<D3D12_RESOURCE_STATES> configured, D3D12_RESOURCE_STATES contract)
{
    return { resource, resource ? StateAt(list, resource, configured, contract) : 0u };
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

// What a game gets when it names no preset is what NVIDIA's own DLSS gives it: K (the first transformer) for DLAA,
// Quality and Balanced, M (DLSS 4.5's second one) for Performance, L for Ultra Performance. Measured here at each
// mode's own resolution to 2560x1440 (ms a frame): K 6.2 to 5.7 and M 26.8 to 5.4 at DLAA down to Ultra Performance,
// L 91 to 10: M costs as many pixels as it is given, so it is for the modes that render few.
constexpr int kPresetK = 11, kPresetL = 12, kPresetM = 13;
// What "Latest" means in the menu: the newest model there is.
constexpr int kDefaultPreset = kPresetM;

int DefaultPresetFor(int quality)
{
    switch (quality)
    {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:
        return kPresetM;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
        return kPresetL;
    default:
        return kPresetK;
    }
}

// The preset a game asked for at this quality (NVIDIA's perf/quality value), 0 when it did not.
int RequestedPreset(NVSDK_NGX_Parameter* parameters, int quality)
{
    const char* name = nullptr;
    switch (quality)
    {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:
        name = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance;
        break;
    case NVSDK_NGX_PerfQuality_Value_Balanced:
        name = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced;
        break;
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:
        name = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality;
        break;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
        name = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance;
        break;
    case NVSDK_NGX_PerfQuality_Value_UltraQuality:
        name = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality;
        break;
    case NVSDK_NGX_PerfQuality_Value_DLAA:
        name = NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA;
        break;
    default:
        return 0;
    }

    unsigned int preset = 0;
    parameters->Get(name, &preset);
    return (int) preset;
}

// What the menu's "Render Presets Override" asks for at this quality: the one for all, else the one of this quality.
std::optional<uint32_t> MenuPreset(int quality)
{
    auto& config = *Config::Instance();
    if (!config.RenderPresetOverride.value_or_default())
        return std::nullopt;

    if (config.RenderPresetForAll.has_value())
        return config.RenderPresetForAll.value();

    const CustomOptional<uint32_t>* perQuality = nullptr;
    switch (quality)
    {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:
        perQuality = &config.RenderPresetPerformance;
        break;
    case NVSDK_NGX_PerfQuality_Value_Balanced:
        perQuality = &config.RenderPresetBalanced;
        break;
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:
        perQuality = &config.RenderPresetQuality;
        break;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
        perQuality = &config.RenderPresetUltraPerformance;
        break;
    case NVSDK_NGX_PerfQuality_Value_UltraQuality:
        perQuality = &config.RenderPresetUltraQuality;
        break;
    case NVSDK_NGX_PerfQuality_Value_DLAA:
        perQuality = &config.RenderPresetDLAA;
        break;
    default:
        return std::nullopt;
    }

    if (perQuality->has_value())
        return perQuality->value();

    return std::nullopt;
}

// A preset number the library can be asked for: 1 (A) to 15 (O). The menu's "Latest" is M, the newest model here;
// 0 (the game's own, "Default") and anything else mean no choice.
int Valid(uint32_t preset)
{
    if (preset == NV_PRESET_LATEST)
        return kDefaultPreset;

    return preset >= 1 && preset <= 15 ? (int) preset : 0;
}

// The render preset for this quality, in this order: the menu's, the configuration file's, the game's hint, NVIDIA's default.
int PresetFor(NVSDK_NGX_Parameter* parameters, int quality)
{
    if (const auto chosen = MenuPreset(quality); chosen.has_value())
        if (const int preset = Valid(*chosen); preset != 0)
            return preset;

    if (Config::Instance()->ZludaDlssPreset.has_value())
        return Config::Instance()->ZludaDlssPreset.value();

    const int requested = Valid((unsigned int) RequestedPreset(parameters, quality));
    return requested == 0 ? DefaultPresetFor(quality) : requested;
}

// Ray reconstruction has two presets: D and E. Left alone, the library uses a default of its own that is neither (the
// image comes out grainy), where NVIDIA's driver sets D.
constexpr int kRrPresetD = 4;
constexpr int kRrPresetE = 5;

int ValidRr(uint32_t preset)
{
    if (preset == NV_PRESET_LATEST)
        return kRrPresetE;

    return preset == kRrPresetD || preset == kRrPresetE ? (int) preset : 0;
}

// The render preset of ray reconstruction at this quality: the menu's, the game's hint, D.
int RrPresetFor(NVSDK_NGX_Parameter* parameters, int quality)
{
    auto& config = *Config::Instance();

    if (config.DLSSDRenderPresetOverride.value_or_default())
    {
        const CustomOptional<uint32_t>* perQuality = nullptr;
        const char* hint = nullptr;
        switch (quality)
        {
        case NVSDK_NGX_PerfQuality_Value_MaxPerf:
            perQuality = &config.DLSSDRenderPresetPerformance;
            break;
        case NVSDK_NGX_PerfQuality_Value_Balanced:
            perQuality = &config.DLSSDRenderPresetBalanced;
            break;
        case NVSDK_NGX_PerfQuality_Value_MaxQuality:
            perQuality = &config.DLSSDRenderPresetQuality;
            break;
        case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
            perQuality = &config.DLSSDRenderPresetUltraPerformance;
            break;
        case NVSDK_NGX_PerfQuality_Value_UltraQuality:
            perQuality = &config.DLSSDRenderPresetUltraQuality;
            break;
        case NVSDK_NGX_PerfQuality_Value_DLAA:
            perQuality = &config.DLSSDRenderPresetDLAA;
            break;
        default:
            break;
        }

        if (config.DLSSDRenderPresetForAll.has_value())
            if (const int preset = ValidRr(config.DLSSDRenderPresetForAll.value()); preset != 0)
                return preset;

        if (perQuality != nullptr && perQuality->has_value())
            if (const int preset = ValidRr(perQuality->value()); preset != 0)
                return preset;

        (void) hint;
    }

    const char* name = nullptr;
    switch (quality)
    {
    case NVSDK_NGX_PerfQuality_Value_MaxPerf:
        name = "RayReconstruction.Hint.Render.Preset.Performance";
        break;
    case NVSDK_NGX_PerfQuality_Value_Balanced:
        name = "RayReconstruction.Hint.Render.Preset.Balanced";
        break;
    case NVSDK_NGX_PerfQuality_Value_MaxQuality:
        name = "RayReconstruction.Hint.Render.Preset.Quality";
        break;
    case NVSDK_NGX_PerfQuality_Value_UltraPerformance:
        name = "RayReconstruction.Hint.Render.Preset.UltraPerformance";
        break;
    case NVSDK_NGX_PerfQuality_Value_UltraQuality:
        name = "RayReconstruction.Hint.Render.Preset.UltraQuality";
        break;
    case NVSDK_NGX_PerfQuality_Value_DLAA:
        name = "RayReconstruction.Hint.Render.Preset.DLAA";
        break;
    default:
        break;
    }

    if (name != nullptr)
    {
        unsigned int asked = 0;
        parameters->Get(name, &asked);
        if (const int preset = ValidRr(asked); preset != 0)
            return preset;
    }

    return kRrPresetD;
}

} // namespace

DLSSZFeatureDx12::DLSSZFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters, bool rayReconstruction)
    : IFeature(handleId, parameters), IFeature_Dx12(handleId, parameters), _rayReconstruction(rayReconstruction)
{
    _initParameters = SetInitParameters(parameters);
    _moduleLoaded = DlssZ::Available(rayReconstruction) && DlssZ::Load();

    if (!_moduleLoaded)
        LOG_ERROR("DLSS on AMD is not available: the dlss5nr runtime or {} is missing",
                  wstring_to_string(DlssZ::SnippetPath(rayReconstruction).wstring()));
}

DLSSZFeatureDx12::~DLSSZFeatureDx12()
{
    if (State::Instance().isShuttingDown)
        return;

    std::lock_guard<std::mutex> guard(_link->lock);
    if (_link->upscaler != nullptr)
    {
        DlssZ::Functions().destroy(_link->upscaler);
        _link->upscaler = nullptr;
    }
}

bool DLSSZFeatureDx12::InitInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters)
{
    if (IsInited())
        return true;

    if (!_moduleLoaded || Device == nullptr)
        return false;

    // Tracking the game's command lists is what lets the network run in the middle of one. A device that
    // was not hooked as it was created can still be, but the lists the game began before are not tracked.
    if (!DlssZ::InstallCommandHooks(Device, true))
        return false;

    if (!_initParameters)
        _initParameters = SetInitParameters(parameters);

    _createdRenderWidth = RenderWidth();
    _createdRenderHeight = RenderHeight();
    _createdOutputWidth = TargetWidth();
    _createdOutputHeight = TargetHeight();

    if (_createdRenderWidth == 0 || _createdRenderHeight == 0 || _createdOutputWidth == 0 || _createdOutputHeight == 0)
    {
        LOG_ERROR("DLSS on AMD: no resolution to create the upscaler for");
        return false;
    }

    const auto snippet = DlssZ::SnippetPath(_rayReconstruction);
    const auto directory = DlssZ::RuntimeDirectory();

    dlss5nr_upscaler_create_info info {};
    info.struct_size = sizeof(info);
    info.device = Device;
    info.kind = _rayReconstruction ? DLSS5NR_UPSCALER_RAY_RECONSTRUCTION : DLSS5NR_UPSCALER_SUPER_RESOLUTION;
    info.runtime_dir = directory.c_str();
    info.snippet_path = snippet.c_str();
    info.log = &RuntimeLog;
    info.render_width = _createdRenderWidth;
    info.render_height = _createdRenderHeight;
    info.output_width = _createdOutputWidth;
    info.output_height = _createdOutputHeight;
    info.flags = (IsHdr() ? DLSS5NR_UPSCALER_HDR : 0) | (LowResMV() ? DLSS5NR_UPSCALER_MOTION_LOW_RES : 0) |
                 (JitteredMV() ? DLSS5NR_UPSCALER_MOTION_JITTERED : 0) |
                 (DepthInverted() ? DLSS5NR_UPSCALER_DEPTH_INVERTED : 0) |
                 (AutoExposure() ? DLSS5NR_UPSCALER_AUTO_EXPOSURE : 0);
    // The library refuses Ultra Quality (0xBAD00010, an unsupported parameter) where it takes every other mode. The
    // sizes are the game's, so the nearest mode it has gives the same picture; the game's own mode still picks the preset.
    const int asked = (int) PerfQualityValue();
    info.quality = asked == NVSDK_NGX_PerfQuality_Value_UltraQuality ? (int) NVSDK_NGX_PerfQuality_Value_MaxQuality : asked;
    // Ray reconstruction has presets of its own, which the game names differently: left to the library.
    info.preset = _rayReconstruction ? RrPresetFor(parameters, asked) : PresetFor(parameters, asked);

    LOG_INFO("DLSS on AMD: creating {} for {}x{} to {}x{}, quality {}, preset {}",
             _rayReconstruction ? "ray reconstruction" : "super resolution", info.render_width, info.render_height,
             info.output_width, info.output_height, info.quality, info.preset);

    const double began = Util::MillisecondsNow();
    dlss5nr_upscaler* upscaler = nullptr;
    int result;

    {
        std::lock_guard<std::mutex> ownerGuard(_ownerMutex);
        if (auto previous = _owner.lock(); previous && previous != _link)
        {
            std::lock_guard<std::mutex> guard(previous->lock);
            if (previous->upscaler != nullptr)
            {
                LOG_INFO("DLSS on AMD: the upscaler of the previous feature is released for this one");
                DlssZ::Functions().destroy(previous->upscaler);
                previous->upscaler = nullptr;
            }
        }
    }

    {
        ScopedSkipHeapCapture skipHeapCapture {};
        ScopedSkipSpoofingGlobal skipSpoofing {};
        result = DlssZ::Functions().create(&info, &upscaler);
    }

    if (result != 0 || upscaler == nullptr)
    {
        LOG_ERROR("DLSS on AMD: the upscaler could not be created: {}", DlssZ::Functions().lastError());
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(_link->lock);
        _link->upscaler = upscaler;
    }

    {
        std::lock_guard<std::mutex> ownerGuard(_ownerMutex);
        _owner = _link;
    }

    dlss5nr_upscaler_status status {};
    status.struct_size = sizeof(status);
    DlssZ::Functions().getStatus(upscaler, &status);
    LOG_INFO("DLSS on AMD: created in {:.0f} ms, {} native kernels, {} synchronisation", Util::MillisecondsNow() - began,
             status.native_kernels, status.gpu_synchronised ? "GPU" : "CPU");

    SetInit(true);
    return true;
}

void DLSSZFeatureDx12::FallBack(const char* reason)
{
    LOG_ERROR("DLSS on AMD: {}; falling back to FSR 2.1.2", reason);
    ImGui::InsertNotification({ ImGuiToastType::Warning, 10000, "DLSS on AMD failed, using FSR 2.1.2" });

    State::Instance().newBackend = Upscaler::FSR21;
    State::Instance().changeBackend[Handle()->Id] = true;
}

// Zeroes, as every new committed resource is: one plane at the same distance everywhere. The network works without
// depth detail, the picture is not as good as with the game's.
ID3D12Resource* DLSSZFeatureDx12::FlatDepth(ID3D12Resource* like)
{
    const auto size = like->GetDesc();

    if (_flatDepth != nullptr)
    {
        const auto have = _flatDepth->GetDesc();
        if (have.Width == size.Width && have.Height == size.Height)
            return _flatDepth.Get();

        _flatDepth.Reset();
    }

    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(like->GetDevice(IID_PPV_ARGS(&device))))
        return nullptr;

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = size.Width;
    desc.Height = size.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                               IID_PPV_ARGS(&_flatDepth))))
        return nullptr;

    LOG_INFO("DLSS on AMD: the game gives no depth, a flat one of {}x{} stands in", (unsigned) size.Width, size.Height);
    return _flatDepth.Get();
}

bool DLSSZFeatureDx12::Gather(ID3D12GraphicsCommandList* list, NVSDK_NGX_Parameter* parameters, Frame& out)
{
    auto& config = *Config::Instance();
    auto& state = State::Instance();
    auto& f = out.frame;
    f.struct_size = sizeof(f);

    const bool unreal = state.NVNGX_Engine == NVSDK_NGX_ENGINE_TYPE_UNREAL || state.gameEngine == GameEngineType::Unreal ||
                        (state.gameQuirks & GameQuirk::ForceUnrealEngine);

    auto color = GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_Color);
    auto motion = GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_MotionVectors);
    auto depth = GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_Depth);
    auto output = GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_Output);

    if (depth == nullptr && color != nullptr)
        depth = FlatDepth(color);

    if (color == nullptr || motion == nullptr || depth == nullptr || output == nullptr)
    {
        LOG_ERROR("DLSS on AMD: an input is missing (color {}, motion {}, depth {}, output {})", (void*) color,
                  (void*) motion, (void*) depth, (void*) output);
        return false;
    }

    // Unreal leaves its inputs in other states than the NGX contract names; the configuration remembers.
    if (unreal && !config.ColorResourceBarrier.has_value())
        config.ColorResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_RENDER_TARGET);

    if (unreal && !config.MVResourceBarrier.has_value())
        config.MVResourceBarrier.set_volatile_value(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const auto read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    f.color = Resource(list, color, Override(config.ColorResourceBarrier), read);
    f.motion = Resource(list, motion, Override(config.MVResourceBarrier), read);
    f.depth = Resource(list, depth, Override(config.DepthResourceBarrier), read);
    f.output = Resource(list, output, Override(config.OutputResourceBarrier), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // The exposure the game computed, unless the network is told to find its own.
    if (!AutoExposure())
    {
        auto exposure = GetParameter<ID3D12Resource*>(parameters, NVSDK_NGX_Parameter_ExposureTexture);
        if (exposure == nullptr)
        {
            LOG_WARN("DLSS on AMD: no exposure texture although the game did not ask for auto exposure; "
                     "recreating with auto exposure");
            state.autoExposure = true;
            state.changeBackend[Handle()->Id] = true;
            _recreating = true;
            return false;
        }

        f.exposure = Resource(list, exposure, Override(config.ExposureResourceBarrier), read);
    }

    // Where the network's history is not to be trusted. The game's DLSS masks, or FSR's when the game
    // is one of those and OptiScaler is turning its inputs into DLSS's.
    if (!config.DisableReactiveMask.value_or(false))
    {
        auto reactive = FirstResource(parameters, { NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, "FSR.reactive" });
        auto transparency =
            FirstResource(parameters, { NVSDK_NGX_Parameter_TransparencyMask, "FSR.transparencyAndComposition" });

        f.reactive_mask = Resource(list, reactive, Override(config.MaskResourceBarrier), read);
        f.transparency_mask = Resource(list, transparency, Override(config.MaskResourceBarrier), read);
        _accessToReactiveMask = reactive != nullptr;
        _hasTM = transparency != nullptr;
    }

    // The region the game rendered, and where each texture's region starts.
    unsigned int width = 0, height = 0;
    GetRenderResolution(parameters, &width, &height);
    // Dynamic resolution renders from half the output to the whole of it, whatever size the feature was
    // created for.
    f.render_width = std::min(width, std::max(_createdRenderWidth, _createdOutputWidth));
    f.render_height = std::min(height, std::max(_createdRenderHeight, _createdOutputHeight));

    f.color_origin = Origin(parameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y);
    f.depth_origin = Origin(parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X,
                            NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y);
    f.motion_origin = Origin(parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X,
                             NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y);
    f.mask_origin = Origin(parameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_X,
                           NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_SubrectBase_Y);
    if (f.mask_origin.x == 0 && f.mask_origin.y == 0)
        f.mask_origin = Origin(parameters, NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_X,
                               NVSDK_NGX_Parameter_DLSS_Input_Translucency_SubrectBase_Y);
    f.output_origin = Origin(parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X,
                             NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y);

    parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_X, &f.jitter_x);
    parameters->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y, &f.jitter_y);

    f.motion_scale_x = 1.0f;
    f.motion_scale_y = 1.0f;
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &f.motion_scale_x);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &f.motion_scale_y);

    f.pre_exposure = 1.0f;
    f.exposure_scale = 1.0f;
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &f.pre_exposure);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Exposure_Scale, &f.exposure_scale);

    if (_frameCount < 3)
        LOG_DEBUG("DLSS on AMD: frame {}: jitter {} {}, motion scale {} {}, pre-exposure {}, exposure scale {}", _frameCount,
                  f.jitter_x, f.jitter_y, f.motion_scale_x, f.motion_scale_y, f.pre_exposure, f.exposure_scale);

    // OptiScaler's own sharpening stage (RCAS) comes after the network: DLSS 3.1 and later has none.
    f.sharpness = 0.0f;

    int reset = 0;
    parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    f.reset = reset;

    _hasColor = true;
    _hasDepth = true;
    _hasMV = true;
    _hasExposure = f.exposure.resource != nullptr;
    _hasOutput = true;

    if (!_rayReconstruction)
        return true;

    // Ray reconstruction also reads the G-buffer, and the camera.
    f.diffuse_albedo = Resource(list, FirstResource(parameters, { "GBuffer.DiffuseAlbedo", "DLSS.Input.DiffuseAlbedo" }),
                                std::nullopt, read);
    f.specular_albedo = Resource(
        list, FirstResource(parameters, { "GBuffer.SpecularAlbedo", "DLSS.Input.SpecularAlbedo" }), std::nullopt, read);
    f.normals = Resource(list, FirstResource(parameters, { NVSDK_NGX_Parameter_GBuffer_Normals }), std::nullopt, read);
    f.roughness = Resource(list, FirstResource(parameters, { NVSDK_NGX_Parameter_GBuffer_Roughness }), std::nullopt, read);
    f.hit_distance = Resource(list, FirstResource(parameters, { "DLSSD.SpecularHitDistance" }), std::nullopt, read);

    if (f.diffuse_albedo.resource == nullptr || f.specular_albedo.resource == nullptr || f.normals.resource == nullptr)
    {
        LOG_ERROR("DLSS on AMD: ray reconstruction is missing a G-buffer input (diffuse {}, specular {}, normals {})",
                  (void*) f.diffuse_albedo.resource, (void*) f.specular_albedo.resource, (void*) f.normals.resource);
        return false;
    }

    f.guide_origin = Origin(parameters, "DLSS.Input.DiffuseAlbedo.Subrect.Base.X", "DLSS.Input.DiffuseAlbedo.Subrect.Base.Y");

    float* worldToView = nullptr;
    float* viewToClip = nullptr;
    parameters->Get("WorldToViewMatrix", (void**) &worldToView);
    parameters->Get("ViewToClipMatrix", (void**) &viewToClip);

    // The game owns the matrices and may overwrite them before the boundary: a copy goes with the frame.
    if (worldToView != nullptr)
    {
        memcpy(out.worldToView, worldToView, sizeof(out.worldToView));
        f.world_to_view = out.worldToView;
    }

    if (viewToClip != nullptr)
    {
        memcpy(out.viewToClip, viewToClip, sizeof(out.viewToClip));
        f.view_to_clip = out.viewToClip;
    }

    return true;
}

bool DLSSZFeatureDx12::EvaluateInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters)
{
    if (!IsInited() || _link->upscaler == nullptr)
        return false;

    // A boundary that failed has been seen only here: three in a row and the upscaler is not coming back.
    if (_link->failures >= 3)
    {
        FallBack("the upscaler failed repeatedly");
        return false;
    }

    // Lists that began before the hooks were installed cannot be split.
    if (!DlssZ::HooksAreSettled())
    {
        if (_lastSkippedFrames++ == 0)
            LOG_WARN("DLSS on AMD: waiting for the command list hooks to settle");
        return true;
    }

    auto frame = std::make_shared<Frame>();
    _recreating = false;
    if (!Gather(commandList, parameters, *frame))
        return _recreating;

    auto link = _link;
    try
    {
        d4r::win::commands::record_boundary(commandList,
                                            [link, frame](ID3D12CommandQueue* queue)
                                            {
                                                std::lock_guard<std::mutex> guard(link->lock);
                                                if (link->upscaler == nullptr)
                                                    return;

                                                if (DlssZ::Functions().evaluate(link->upscaler, queue, &frame->frame) != 0)
                                                {
                                                    link->failures++;
                                                    LOG_ERROR("DLSS on AMD: evaluation failed: {}",
                                                              DlssZ::Functions().lastError());
                                                }
                                                else
                                                {
                                                    link->failures = 0;
                                                }
                                            });
    }
    catch (const std::exception& failure)
    {
        LOG_ERROR("DLSS on AMD: the command list could not be split: {}", failure.what());
        _link->failures++;
        return false;
    }

    _frameCount++;
    return true;
}
