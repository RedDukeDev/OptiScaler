#include "pch.h"
#include "NvInterpolator.h"

#include "Nvngx_FG.h"

#include <NVNGX_Parameter.h>

#include <array>
#include <atomic>
#include <mutex>
#include <fstream>
#include <iomanip>
#include <Util.h>
#include <cmath>

namespace
{

ID3D12Device* device = nullptr;
NVSDK_NGX_Handle* handle = nullptr;
NVNGX_Parameters* parameters = nullptr;

uint32_t createdWidth = 0, createdHeight = 0;
bool createdHdr = false, createdDepthInverted = false, createdJitteredMotion = false;
DXGI_FORMAT createdFormat = DXGI_FORMAT_UNKNOWN;

constexpr unsigned long long kApplicationId = 0x4F707469; // 'Opti'

// What the library is given pointers to: the matrices, a queue to be told of, and the odd ones Streamline hands over
float viewToClip[16], clipToView[16], clipToLensClip[16], clipToPrevClip[16], prevClipToClip[16];
ID3D12CommandAllocator* allocator = nullptr;
ID3D12Resource* emptyUi = nullptr;
uint32_t emptyUiWidth = 0, emptyUiHeight = 0;
HANDLE fenceEvent = nullptr;
char debugText[8] = {};

void SetResource(const char* key, ID3D12Resource* resource)
{
    if (resource != nullptr)
        parameters->Set(key, resource);
}

void SetRect(const char* name, uint32_t width, uint32_t height)
{
    parameters->Set(std::format("DLSSG.{}SubrectBaseX", name).c_str(), 0u);
    parameters->Set(std::format("DLSSG.{}SubrectBaseY", name).c_str(), 0u);
    parameters->Set(std::format("DLSSG.{}SubrectWidth", name).c_str(), width);
    parameters->Set(std::format("DLSSG.{}SubrectHeight", name).c_str(), height);
}

// What does not change from one frame to the next
void SetFeatureParameters(const NvInterpolator::Frame& frame)
{
    parameters->Set("Width", frame.displayWidth);
    parameters->Set("Height", frame.displayHeight);
    parameters->Set("DLSSG.Width", frame.displayWidth);
    parameters->Set("DLSSG.Height", frame.displayHeight);
    parameters->Set("DLSSG.BackbufferFormat", (unsigned int) frame.backbufferFormat);
    // Streamline says "HDR" with the SDR backbuffers of this game too, and the library works as intended with it
    parameters->Set("DLSSG.ColorBuffersHDR", 1u);
    parameters->Set("DLSSG.DepthInverted", frame.depthInverted ? 1u : 0u);
    parameters->Set("DLSSG.MvecJittered", frame.jitteredMotion ? 1u : 0u);
    parameters->Set("DLSSG.MvecDilated", 0u);
    parameters->Set("DLSSG.OrthoProjection", 0u);
    parameters->Set("DLSSG.MultiFrameCountMax", 1u);
    parameters->Set("DLSSG.DynamicResolution", 0u);
    parameters->Set("DLSSG.EnableInterp", 1u);
    parameters->Set("DLSSG.InternalWidth", 0u);
    parameters->Set("DLSSG.InternalHeight", 0u);
    parameters->Set("DLSSG.UseReflexMatrices", 0u);
}

// Matrices in double precision, row-major, for row vectors: the game's world is in the hundreds of thousands of units,
// where a float cannot tell one frame's camera from the next one's
using Mat = std::array<double, 16>;

Mat Identity()
{
    Mat m {};
    m[0] = m[5] = m[10] = m[15] = 1.0;
    return m;
}

Mat Multiply(const Mat& a, const Mat& b)
{
    Mat r {};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k)
                r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}

bool Invert(const Mat& source, Mat& result)
{
    Mat m = source;
    result = Identity();

    for (int column = 0; column < 4; ++column)
    {
        int pivot = column;
        for (int row = column + 1; row < 4; ++row)
            if (std::fabs(m[row * 4 + column]) > std::fabs(m[pivot * 4 + column]))
                pivot = row;

        if (std::fabs(m[pivot * 4 + column]) < 1e-300)
            return false;

        if (pivot != column)
            for (int k = 0; k < 4; ++k)
            {
                std::swap(m[pivot * 4 + k], m[column * 4 + k]);
                std::swap(result[pivot * 4 + k], result[column * 4 + k]);
            }

        const double scale = 1.0 / m[column * 4 + column];
        for (int k = 0; k < 4; ++k)
        {
            m[column * 4 + k] *= scale;
            result[column * 4 + k] *= scale;
        }

        for (int row = 0; row < 4; ++row)
        {
            if (row == column)
                continue;

            const double factor = m[row * 4 + column];
            for (int k = 0; k < 4; ++k)
            {
                m[row * 4 + k] -= factor * m[column * 4 + k];
                result[row * 4 + k] -= factor * result[column * 4 + k];
            }
        }
    }

    return true;
}

bool HasCamera(const float* up, const float* right, const float* forward)
{
    for (int i = 0; i < 3; ++i)
        if (up[i] != 0.0f || right[i] != 0.0f || forward[i] != 0.0f)
            return true;

    return false;
}

// World to view of a camera: view space with x right, y up, z forward (what Streamline's matrices are)
Mat WorldToView(const float* position, const float* up, const float* right, const float* forward)
{
    Mat translation = Identity();
    translation[12] = -(double) position[0];
    translation[13] = -(double) position[1];
    translation[14] = -(double) position[2];

    Mat rotation = Identity();
    for (int i = 0; i < 3; ++i)
    {
        rotation[i * 4 + 0] = right[i];
        rotation[i * 4 + 1] = up[i];
        rotation[i * 4 + 2] = forward[i];
    }

    return Multiply(translation, rotation);
}

void Store(float* destination, const Mat& matrix)
{
    for (int i = 0; i < 16; ++i)
        destination[i] = (float) matrix[i];
}

bool Finite(const Mat& matrix)
{
    for (double v : matrix)
        if (!std::isfinite(v) || std::fabs(v) > 1e30)
            return false;

    return true;
}

// The projection, and what the camera does from one frame to the next, the way the game would have told it
void SetMatrices(const NvInterpolator::Frame& frame)
{
    const double near_ = std::max((double) frame.cameraNear, 1e-4);
    const double far_ = std::max((double) frame.cameraFar, near_ * 2.0);
    const double fov = frame.cameraFov > 0.0f ? frame.cameraFov : 1.0;
    const double aspect = frame.cameraAspect > 0.0f ? frame.cameraAspect
                                                     : (double) frame.displayWidth / std::max(frame.displayHeight, 1u);
    const double f = 1.0 / std::tan(fov * 0.5);

    // Depth is 1 at the near plane and 0 at the far one where it is inverted
    const double a = frame.depthInverted ? near_ / (near_ - far_) : far_ / (far_ - near_);
    const double b = frame.depthInverted ? near_ * far_ / (far_ - near_) : -near_ * far_ / (far_ - near_);

    Mat projection {};
    projection[0] = f / aspect;
    projection[5] = f;
    projection[10] = a;
    projection[11] = 1.0;
    projection[14] = b;

    Mat inverse = Identity();
    if (!Invert(projection, inverse))
        inverse = Identity();

    Mat toPrevious = Identity();
    Mat toCurrent = Identity();

    if (HasCamera(frame.cameraUp, frame.cameraRight, frame.cameraForward) &&
        HasCamera(frame.previousUp, frame.previousRight, frame.previousForward))
    {
        const Mat now = WorldToView(frame.cameraPosition, frame.cameraUp, frame.cameraRight, frame.cameraForward);
        const Mat before =
            WorldToView(frame.previousPosition, frame.previousUp, frame.previousRight, frame.previousForward);

        // A point in this frame's clip space, in the clip space of the frame before: through the camera, in the world
        // and back, which is a small step between the two cameras
        Mat nowInverse;
        if (Invert(now, nowInverse))
        {
            const Mat step = Multiply(nowInverse, before);
            const Mat candidate = Multiply(Multiply(inverse, step), projection);
            Mat candidateInverse;

            if (Finite(candidate) && Invert(candidate, candidateInverse) && Finite(candidateInverse))
            {
                toPrevious = candidate;
                toCurrent = candidateInverse;
            }
        }
    }

    Store(viewToClip, projection);
    Store(clipToView, inverse);
    Store(clipToLensClip, Identity());
    Store(clipToPrevClip, toPrevious);
    Store(prevClipToClip, toCurrent);

    static int logged = 0;
    if (logged++ % 240 == 0)
        LOG_DEBUG("NVIDIA frame generation camera: ClipToPrevClip translation {} {} {}, scale {} {} {}", toPrevious[12],
                  toPrevious[13], toPrevious[14], toPrevious[0], toPrevious[5], toPrevious[10]);

    parameters->Set("DLSSG.CameraViewToClip", (void*) viewToClip);
    parameters->Set("DLSSG.ClipToCameraView", (void*) clipToView);
    parameters->Set("DLSSG.ClipToLensClip", (void*) clipToLensClip);
    parameters->Set("DLSSG.ClipToPrevClip", (void*) clipToPrevClip);
    parameters->Set("DLSSG.PrevClipToClip", (void*) prevClipToClip);
}

// An interface of nothing, transparent: what the library composes onto the frame where the game has none to give. Left
// readable for good, as the library reads it.
ID3D12Resource* EmptyUi(ID3D12Device* device, uint32_t width, uint32_t height)
{
    if (emptyUi != nullptr && emptyUiWidth == width && emptyUiHeight == height)
        return emptyUi;

    if (emptyUi != nullptr)
    {
        emptyUi->Release();
        emptyUi = nullptr;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                               IID_PPV_ARGS(&emptyUi))))
        return nullptr;

    emptyUiWidth = width;
    emptyUiHeight = height;
    return emptyUi;
}

bool EnsureFeature(const NvInterpolator::Frame& frame)
{
    if (device != frame.device)
    {
        NvInterpolator::Release();
        device = frame.device;
    }

    if (handle != nullptr && createdWidth == frame.displayWidth && createdHeight == frame.displayHeight &&
        createdFormat == frame.backbufferFormat && createdHdr == frame.hdr &&
        createdDepthInverted == frame.depthInverted && createdJitteredMotion == frame.jitteredMotion)
        return true;

    if (handle != nullptr)
    {
        Nvngx_FG::D3D12_ReleaseFeature(handle);
        handle = nullptr;
    }

    // The game's own NGX initialisation brings the provider up where there is one
    if (!State::Instance().nvngxDx12Inited)
    {
        NVSDK_NGX_FeatureCommonInfo info = {};
        const auto result = Nvngx_FG::D3D12_Init_Ext(kApplicationId, L".", frame.device, NVSDK_NGX_Version_API, &info);

        if (NVSDK_NGX_FAILED(result))
        {
            LOG_ERROR("NVIDIA frame generation could not be initialised: {:X}", (UINT) result);
            return false;
        }
    }

    if (parameters == nullptr)
        parameters = GetNGXParameters(API::DX12, true);

    SetFeatureParameters(frame);

    const auto result =
        Nvngx_FG::D3D12_CreateFeature(frame.commandList, NVSDK_NGX_Feature_FrameGeneration, parameters, &handle);

    if (NVSDK_NGX_FAILED(result))
    {
        LOG_ERROR("NVIDIA frame generation could not create its feature ({}x{}, format {}): {:X}", frame.displayWidth,
                  frame.displayHeight, (int) frame.backbufferFormat, (UINT) result);
        handle = nullptr;
        return false;
    }

    createdWidth = frame.displayWidth;
    createdHeight = frame.displayHeight;
    createdFormat = frame.backbufferFormat;
    createdHdr = frame.hdr;
    createdDepthInverted = frame.depthInverted;
    createdJitteredMotion = frame.jitteredMotion;

    LOG_INFO("NVIDIA frame generation feature created ({}x{}, format {})", frame.displayWidth, frame.displayHeight,
             (int) frame.backbufferFormat);
    return true;
}

} // namespace

// What FSR's frame generation is told of a frame is not what Streamline tells the library: FSR swaps the near and far
// planes where depth is inverted (and gives an infinite one as the largest float), takes the vertical field of view
// where the library is given the horizontal one, and scales the motion vectors to pixels where the library wants them
// in the unit of the screen.
static NvInterpolator::Frame ForLibrary(const NvInterpolator::Frame& game)
{
    NvInterpolator::Frame frame = game;

    if (frame.depthInverted && frame.cameraNear > frame.cameraFar)
        std::swap(frame.cameraNear, frame.cameraFar);

    constexpr float kLargest = 1.0e6f;
    frame.cameraFar = std::min(frame.cameraFar, kLargest);

    if (frame.cameraNear <= 0.0f)
        frame.cameraNear = 0.01f;

    if (frame.renderWidth != 0 && frame.renderHeight != 0)
    {
        frame.motionScaleX = game.motionScaleX / (float) frame.renderWidth;
        frame.motionScaleY = game.motionScaleY / (float) frame.renderHeight;
    }

    return frame;
}

bool NvInterpolator::Evaluate(const Frame& gameFrame)
{
    // One evaluation at a time: the parameters are shared
    static std::mutex evaluating;
    std::lock_guard<std::mutex> lock(evaluating);

    const Frame frame = ForLibrary(gameFrame);

    if (frame.device == nullptr || frame.commandList == nullptr || frame.backbuffer == nullptr ||
        frame.depth == nullptr || frame.motion == nullptr || frame.output == nullptr)
        return false;

    if (!Nvngx_FG::isDx12Available() || !EnsureFeature(frame))
        return false;

    SetFeatureParameters(frame);

    SetResource("DLSSG.Backbuffer", frame.backbuffer);
    SetResource("DLSSG.HUDLess", frame.hudless);

    // The library composes the interface onto the frame it makes, from this: without one it has nothing to compose
    {
        ID3D12Resource* ui = frame.ui != nullptr ? frame.ui : EmptyUi(frame.device, frame.displayWidth, frame.displayHeight);
        SetResource("DLSSG.UI", ui);
        SetRect("UI", frame.displayWidth, frame.displayHeight);
    }
    SetResource("DLSSG.Depth", frame.depth);
    SetResource("DLSSG.MVecs", frame.motion);
    SetResource("DLSSG.OutputInterpolated", frame.output);

    SetRect("Backbuffer", frame.displayWidth, frame.displayHeight);
    SetRect("InputBackbuffer", frame.displayWidth, frame.displayHeight);
    SetRect("HUDLess", frame.displayWidth, frame.displayHeight);
    SetRect("OutputInterpolated", frame.displayWidth, frame.displayHeight);
    SetRect("Depth", frame.renderWidth, frame.renderHeight);
    SetRect("MVecs", frame.renderWidth, frame.renderHeight);

    parameters->Set("DLSSG.JitterOffsetX", frame.jitterX);
    parameters->Set("DLSSG.JitterOffsetY", frame.jitterY);
    parameters->Set("DLSSG.MvecScaleX", frame.motionScaleX);
    parameters->Set("DLSSG.MvecScaleY", frame.motionScaleY);
    {
        // Where the far plane is infinite the game's Streamline says 0.01 and 75000 here, and the matrices carry the real
        // projection: the library worked with those
        const bool infinite = gameFrame.cameraNear >= 1.0e30f || gameFrame.cameraFar >= 1.0e30f;
        parameters->Set("DLSSG.CameraNear", infinite ? 0.01f : frame.cameraNear);
        parameters->Set("DLSSG.CameraFar", infinite ? 75000.0f : frame.cameraFar);
    }
    {
        // The horizontal one, as the game's own Streamline gave it
        const float aspect = frame.cameraAspect > 0.0f ? frame.cameraAspect
                                                        : (float) frame.displayWidth / std::max(frame.displayHeight, 1u);
        parameters->Set("DLSSG.CameraFOV", 2.0f * std::atan(std::tan(frame.cameraFov * 0.5f) * aspect));
    }
    parameters->Set("DLSSG.CameraAspectRatio", frame.cameraAspect);

    parameters->Set("DLSSG.CameraPosX", frame.cameraPosition[0]);
    parameters->Set("DLSSG.CameraPosY", frame.cameraPosition[1]);
    parameters->Set("DLSSG.CameraPosZ", frame.cameraPosition[2]);
    parameters->Set("DLSSG.CameraUpX", frame.cameraUp[0]);
    parameters->Set("DLSSG.CameraUpY", frame.cameraUp[1]);
    parameters->Set("DLSSG.CameraUpZ", frame.cameraUp[2]);
    parameters->Set("DLSSG.CameraRightX", frame.cameraRight[0]);
    parameters->Set("DLSSG.CameraRightY", frame.cameraRight[1]);
    parameters->Set("DLSSG.CameraRightZ", frame.cameraRight[2]);
    parameters->Set("DLSSG.CameraFwdX", frame.cameraForward[0]);
    parameters->Set("DLSSG.CameraFwdY", frame.cameraForward[1]);
    parameters->Set("DLSSG.CameraFwdZ", frame.cameraForward[2]);

    SetMatrices(frame);

    // The rest of what Streamline hands over with a frame
    if (allocator == nullptr)
        frame.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));

    if (fenceEvent == nullptr)
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    parameters->Set("DLSSG.CmdQueue", (void*) frame.queue);
    parameters->Set("DLSSG.CmdAlloc", (void*) allocator);
    parameters->Set("DLSSG.FenceEvent", (void*) fenceEvent);
    parameters->Set("DLSSG.UserDebugText", (void*) debugText);
    parameters->Set("DLSSG.DispatchFlags", 0x10000000u);
    parameters->Set("DLSSG.IsRecording", 1u);
    parameters->Set("DLSSG.run_lowres_mvec_pass", 1u);
    parameters->Set("DLSSG.ShowDebug", 0u);
    parameters->Set("DLSSG.MenuDetectionEnabled", 0u);
    parameters->Set("DLSSG.MinRelativeLinearDepthObjectSeparation", 40.0f);
    parameters->Set("DLSSG.MvecInvalidValue", 3.4028234663852886e+38f);
    parameters->Set("DLSSG.CameraMotionIncluded", 1u);
    parameters->Set("DLSSG.CameraPinholeOffsetX", 0.0f);
    parameters->Set("DLSSG.CameraPinholeOffsetY", 0.0f);
    parameters->Set("DLSSG.EvalFlags", 0u);
    parameters->Set("DLSSG.NoWarp", 0u);
    parameters->Set("DLSSG.NotRenderingGameFrames", 0u);

    parameters->Set("DLSSG.Reset", frame.reset ? 1u : 0u);
    parameters->Set("DLSSG.MultiFrameCount", 1u);
    parameters->Set("DLSSG.MultiFrameIndex", 1u);
    parameters->Set("DLSSG.BackbufferFrameID", (unsigned int) frame.frameId);

    {
        static int evaluated = 0;
        if (evaluated++ % 240 < 2)
            LOG_DEBUG("NVIDIA frame generation frame {}: reset {}, render {}x{}, near {}, far {}, fov {}, jitter {} {}, "
                      "motion scale {} {}, depth inverted {}, hdr {}, camera at {} {} {}",
                      frame.frameId, frame.reset, frame.renderWidth, frame.renderHeight, frame.cameraNear,
                      frame.cameraFar, frame.cameraFov, frame.jitterX, frame.jitterY, frame.motionScaleX,
                      frame.motionScaleY, frame.depthInverted, frame.hdr, frame.cameraPosition[0],
                      frame.cameraPosition[1], frame.cameraPosition[2]);
    }

    // A note of every parameter as the library gets it, for the offline harness to give it the same (fg_params.txt, once,
    // when nv_dump_params.txt is made beside the DLL)
    {
        static const auto marker = Util::DllPath().parent_path() / L"nv_dump_params.txt";
        static const auto output = Util::DllPath().parent_path() / L"fg_params.txt";
        std::error_code error;
        if (std::filesystem::exists(marker, error))
        {
            std::filesystem::remove(marker, error);

            std::ofstream file(output);
            auto keys = static_cast<const NVNGX_Parameters*>(parameters)->enumerate();
            std::sort(keys.begin(), keys.end());

            for (const auto& key : keys)
            {
                if (!key.starts_with("DLSSG."))
                    continue;

                void* pointer = nullptr;
                double number = 0;
                const bool matrix = key == "DLSSG.CameraViewToClip" || key == "DLSSG.ClipToCameraView" ||
                                    key == "DLSSG.ClipToLensClip" || key == "DLSSG.ClipToPrevClip" ||
                                    key == "DLSSG.PrevClipToClip";

                if (parameters->Get(key.c_str(), &pointer) == NVSDK_NGX_Result_Success && pointer != nullptr)
                {
                    if (matrix)
                    {
                        file << key << " matrix";
                        for (int i = 0; i < 16; ++i)
                            file << " " << std::setprecision(10) << static_cast<const float*>(pointer)[i];
                        file << std::string(1, (char) 10);
                    }
                    else
                        file << key << " pointer" << std::string(1, (char) 10);
                }
                else if (parameters->Get(key.c_str(), &number) == NVSDK_NGX_Result_Success)
                    file << key << " number " << std::setprecision(17) << number << std::string(1, (char) 10);
            }

            LOG_INFO("NVIDIA frame generation parameters written to fg_params.txt");
        }
    }

    const auto result = Nvngx_FG::D3D12_EvaluateFeature(frame.commandList, handle, parameters, nullptr);

    if (NVSDK_NGX_FAILED(result))
    {
        static int reported = 0;
        if (reported++ < 5)
            LOG_ERROR("NVIDIA frame generation failed: {:X}", (UINT) result);
        return false;
    }

    return true;
}

void NvInterpolator::Release()
{
    if (handle != nullptr)
    {
        Nvngx_FG::D3D12_ReleaseFeature(handle);
        handle = nullptr;
    }

    createdWidth = createdHeight = 0;
    device = nullptr;
}
