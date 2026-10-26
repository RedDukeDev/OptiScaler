#pragma once
#include "Nvngx_DllProxy.h"

// NVIDIA's own frame generation (nvngx_dlssg.dll) on a GPU that is not NVIDIA's. The library refuses to be driven
// except from a module called nvngx.dll, so it is loaded through the dlss5nr NGX host, which makes the calls; its
// NVAPI requests are answered by the dlss5nr NVAPI.
class Nvngx_Nvidia : public Nvngx_DllProxy
{
  protected:
    void LoadLibraries() override final;

  public:
    Nvngx_Nvidia() { LoadLibraries(); }

    int getMaxFakeFramesCount() override { return 1; }
    FGNvngxReplacement getType() override { return FGNvngxReplacement::Nvidia; }
};
