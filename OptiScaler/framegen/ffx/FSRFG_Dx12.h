#pragma once
#include "SysUtils.h"
#include <framegen/IFGFeature_Dx12.h>
#include <proxies/FfxApi_Proxy.h>
#include <shaders/format_transfer/FT_Dx12.h>
#include <shaders/hud_copy/HudCopy_Dx12.h>
#include <shaders/hudless_compare_compute/HCC_Dx12.h>
#include <shaders/reproject/Reproject_Dx12.h>

#include <ffx_framegeneration.h>
#include "Rdna2FgApi.h"

class FSRFG_Dx12 : public virtual IFGFeature_Dx12
{
  private:
    ffxContext _swapChainContext = nullptr;
    ffxContext _fgContext = nullptr;
    FfxApiSurfaceFormat _lastHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
    FfxApiSurfaceFormat _usingHudlessFormat = FFX_API_SURFACE_FORMAT_UNKNOWN;
    feature_version _version { 0, 0, 0 };

    bool _linkedHudlesDesc = false;

    uint32_t _maxRenderWidth = 0;
    uint32_t _maxRenderHeight = 0;

    // Callbacks run on FFX's thread, guards what they use against ReleaseResourceCopies
    std::mutex _callbackResourceMutex;

    // Dispatched in order on the same queue, one buffer is enough
    std::unique_ptr<HudCopy_Dx12> _hudCopy;
    std::unique_ptr<HCC_Dx12> _hudlessCompareCompute;

    std::unique_ptr<FT_Dx12> _hudlessTransfer[BUFFER_COUNT];
    ID3D12Resource* _hudlessCopyResource[BUFFER_COUNT] {};
    std::unique_ptr<FT_Dx12> _uiTransfer[BUFFER_COUNT];

    std::pair<InputDelta, uint64_t> _deltaSimToFake[BUFFER_COUNT];

    ID3D12GraphicsCommandList* _fgCommandList[BUFFER_COUNT] {};
    ID3D12CommandAllocator* _fgCommandAllocator[BUFFER_COUNT] {};

    // RDNA2 DLSS-FG (Rdna2FgRuntime.dll, [FSRFG] Rdna2FG): replaces only FFX's interpolation dispatch,
    // FFX's swapchain keeps presenting and pacing (fgport\INTEGRATION.md)
    HMODULE _rdna2Dll = nullptr;
    void* _rdna2Ctx = nullptr;
    bool _rdna2Failed = false;
    PFN_Rdna2FgCreate _rdna2Create = nullptr;
    PFN_Rdna2FgRecordInputs _rdna2RecordInputs = nullptr;
    PFN_Rdna2FgRecordOutput _rdna2RecordOutput = nullptr;
    PFN_Rdna2FgGetStatus _rdna2GetStatus = nullptr;
    PFN_Rdna2FgDestroy _rdna2Destroy = nullptr;
    PFN_Rdna2FgLastError _rdna2LastError = nullptr;
    bool Rdna2Ready();
    void Rdna2Release();
    bool Rdna2RecordInputs(int fIndex, UINT64 frameId);

    static FfxApiResourceState GetFfxApiState(D3D12_RESOURCE_STATES state)
    {
        switch (state)
        {
        case D3D12_RESOURCE_STATE_COMMON:
            return FFX_API_RESOURCE_STATE_COMMON;
        case D3D12_RESOURCE_STATE_UNORDERED_ACCESS:
            return FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        case D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:
            return FFX_API_RESOURCE_STATE_COMPUTE_READ;
        case D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE:
            return FFX_API_RESOURCE_STATE_PIXEL_READ;
        case (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE):
            return FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        case D3D12_RESOURCE_STATE_COPY_SOURCE:
            return FFX_API_RESOURCE_STATE_COPY_SRC;
        case D3D12_RESOURCE_STATE_COPY_DEST:
            return FFX_API_RESOURCE_STATE_COPY_DEST;
        case D3D12_RESOURCE_STATE_GENERIC_READ:
            return FFX_API_RESOURCE_STATE_GENERIC_READ;
        case D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT:
            return FFX_API_RESOURCE_STATE_INDIRECT_ARGUMENT;
        case D3D12_RESOURCE_STATE_RENDER_TARGET:
            return FFX_API_RESOURCE_STATE_RENDER_TARGET;
        default:
            return FFX_API_RESOURCE_STATE_COMMON;
        }
    }

    bool ExecuteCommandList(int index);
    bool Dispatch();
    void ConfigureFramePaceTuning();
    bool HudlessFormatTransfer(int index, ID3D12Device* device, DXGI_FORMAT targetFormat, Dx12Resource* resource);
    bool UIFormatTransfer(int index, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, DXGI_FORMAT targetFormat,
                          Dx12Resource* resource);

  protected:
    void ReleaseObjects() override final;
    void ReleaseResourceCopies() override final;
    void CreateObjects(ID3D12Device* InDevice) override final;

  public:
    // IFGFeature
    const char* Name() override final;
    feature_version Version() override final;
    HWND Hwnd() override final;

    void* FrameGenerationContext() override final;
    void* SwapchainContext() override final;

    bool CreateSwapchainInternal(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                 IDXGISwapChain** swapChain) override final;
    bool CreateSwapchain1Internal(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                  DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                  IDXGISwapChain1** swapChain) override final;
    bool ReleaseSwapchain(HWND hwnd) override final;

    void CreateContext(ID3D12Device* device, FG_Constants& fgConstants) override final;
    void Activate() override final;
    void Deactivate() override final;
    void DestroyFGContext() override final;
    bool Shutdown() override final;

    void EvaluateState(ID3D12Device* device, FG_Constants& fgConstants) override final;

    bool Present() override final;

    bool SetResource(Dx12Resource* inputResource) override final;
    void SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) override final;

    ffxReturnCode_t PresentCallback(ffxCallbackDescFrameGenerationPresent* params);
    ffxReturnCode_t DispatchCallback(ffxDispatchDescFrameGeneration* params);
    std::string Rdna2Status(); // empty when RDNA2 DLSS-FG is not running

    FSRFG_Dx12() : IFGFeature_Dx12(), IFGFeature()
    {
        //
    }

    ~FSRFG_Dx12();

    // Inherited via IFGFeature_Dx12
    bool SetInterpolatedFrameCount(UINT interpolatedFrameCount) override;
    // bool HasReprojection() override final { return true; };
};
