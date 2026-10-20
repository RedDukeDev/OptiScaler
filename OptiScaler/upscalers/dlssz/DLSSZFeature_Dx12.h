#pragma once

#include <upscalers/IFeature_Dx12.h>

#include <zluda/DlssZ.h>

#include <atomic>
#include <memory>
#include <mutex>

// NVIDIA's DLSS super resolution and ray reconstruction on an AMD GPU, through the dlss5nr runtime
// (zluda/DlssZ.h). It stands where DLSS stands for an NVIDIA GPU: a game that asks for DLSS gets it,
// its inputs and its output are the ones the game gives, and OptiScaler's other stages (RCAS, output
// scaling, the neural rendering, frame generation) work around it as they do around any upscaler.
//
// The network runs on the queue between what the game recorded before the upscale and what it records
// after; the command list is split there (see DlssZ.h). So the evaluation is not made in EvaluateInternal
// but at that boundary, with a copy of everything the game's parameter table said: the table is the
// game's, and may be gone by then.
class DLSSZFeatureDx12 : public IFeature_Dx12
{
  public:
    DLSSZFeatureDx12(unsigned int handleId, NVSDK_NGX_Parameter* parameters, bool rayReconstruction);
    ~DLSSZFeatureDx12();

    feature_version Version() override { return _rayReconstruction ? feature_version(310, 9, 1) : feature_version(310, 9, 16); }
    Upscaler GetUpscalerType() const override { return _rayReconstruction ? Upscaler::DLSSD : Upscaler::DLSS; }
    API Api() const override { return IFeature_Dx12::Api(); }
    bool IsWithDx12() override { return false; }
    bool CallsUpscalerEndByItself() override { return IFeature_Dx12::CallsUpscalerEndByItself(); }
    std::optional<double> ReadUpscalerTime(void* commandQueue) override
    {
        return IFeature_Dx12::ReadUpscalerTime(commandQueue);
    }
    void ReadDetailedGpuTimes(void* commandQueue, std::vector<DetailedGpuTime>& detailedGpuTimes) override
    {
        return IFeature_Dx12::ReadDetailedGpuTimes(commandQueue, detailedGpuTimes);
    }

  protected:
    bool InitInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters) override;

  private:
    // What the evaluation at the boundary shares with the feature, which may be gone by then.
    struct Link
    {
        std::mutex lock;
        dlss5nr_upscaler* upscaler = nullptr;
        std::atomic<int> failures = 0;
    };

    // One frame, as the boundary will see it.
    struct Frame
    {
        dlss5nr_upscaler_frame frame {};
        float worldToView[16] = {};
        float viewToClip[16] = {};
    };

    bool _rayReconstruction;
    std::shared_ptr<Link> _link = std::make_shared<Link>();

    // The sizes the network was made for. The feature's own _renderWidth follows each frame's sub-rect.
    unsigned int _createdRenderWidth = 0;
    unsigned int _createdRenderHeight = 0;
    unsigned int _createdOutputWidth = 0;
    unsigned int _createdOutputHeight = 0;

    bool _recreating = false;
    long _lastSkippedFrames = 0;

    // Everything from the game's table that the boundary needs. False when a required input is missing.
    bool Gather(ID3D12GraphicsCommandList* commandList, NVSDK_NGX_Parameter* parameters, Frame& out);
    void FallBack(const char* reason);
};
