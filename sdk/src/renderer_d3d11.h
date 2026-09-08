#pragma once
#include "renderer_geometry.h"
#include "frame_decoder.h"
#include <d3d11_3.h>
#include <dxgi1_3.h>
#include <wrl/client.h>
#include <windows.ui.xaml.media.dxinterop.h>
#include <array>
#include <mutex>
#include <memory>

namespace sd::native {
struct GpuFailure : Failure {
    HRESULT hresult;
    GpuFailure(HRESULT result, const char* operation);
};
struct D3D11Context {
    HMODULE module = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device3> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context;
    std::recursive_mutex mutex;
    D3D_FEATURE_LEVEL feature_level = D3D_FEATURE_LEVEL_9_1;
    D3D_DRIVER_TYPE driver = D3D_DRIVER_TYPE_HARDWARE;
    ~D3D11Context();
};
// Native class name. Extra retained VB/IB refs let the SDK rebind after ClearState.
class PSDX11BufferManager {
    uint32_t orientation_=1;
public:
    Microsoft::WRL::ComPtr<ID3D11Buffer> vertices, indices, constants;
    virtual ~PSDX11BufferManager() = default;
    void Initialize(ID3D11Device3* device);
    void Bind(ID3D11DeviceContext1* context, const sd_view_state& state,uint32_t orientation);
};
// All 15 native slots, in their observed order. There is no virtual destructor
// slot in the native base; SDK ownership always deletes the concrete renderer.
class PSRendererD3DBase {
protected:
    uint32_t width_ = 0, height_ = 0, fps_ = 0;
    PSRendererD3DBase() = default;
    ~PSRendererD3DBase() = default;
public:
    virtual void Initialize(const sd_renderer_config&, const PSRendererGeometry&) = 0;
    virtual void ClearState() = 0;
    virtual void WindowSizeSet(float, float) = 0;
    virtual void CompositionScaleSet(float, float) = 0;
    virtual void FrameBufferSizeSet(uint32_t, uint32_t) = 0;
    virtual void Zoom(float) = 0;
    virtual void Pan(float, float) = 0;
    virtual void Rotate(float, float, float) = 0;
    virtual void TransformReset() = 0;
    virtual void FrameBufferPartialUpdate(const uint8_t*, size_t, const protocol::Rectangle&) = 0;
    virtual void Reserved10() = 0;
    virtual void Reserved11() = 0;
    virtual void Present(bool count_frame) = 0;
    virtual DirectX::XMFLOAT2 CenterGet() = 0;
    virtual bool InputTransform(float, float, sd_pointer_sample&) = 0;
};
class PSRendererD3D11 final : public PSRendererD3DBase {
    mutable std::recursive_mutex mutex_;
    std::shared_ptr<D3D11Context> gpu_;
    std::unique_ptr<PSRendererGeometry> geometry_;
    sd_viewport viewport_{};
    sd_renderer_config config_{};
    bool enabled_ = false;
    HRESULT last_hresult_ = S_OK;
    std::shared_ptr<const FrameSnapshot> latest_;
    uint64_t uploaded_sequence_ = 0, present_sequence_ = 0;
    std::array<uint32_t,50> times_{};
    size_t time_index_ = 0;
    PSDX11BufferManager buffers_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_;
    Microsoft::WRL::ComPtr<ISwapChainPanelNative> panel_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> source_, output_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_view_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView1> output_view_;
    uint32_t output_width_ = 0, output_height_ = 0;
    void PrivateDeviceCreate();
    void PrivateShadersCreate();
    void PrivateResourcesWindowCreate();
    void Draw();
    void UpdateView();
    void Upload(const FrameSnapshot&, const std::vector<protocol::Rectangle>&);
    void ReleaseResources() noexcept;
public:
    PSRendererD3D11();
    ~PSRendererD3D11();
    void Initialize(const sd_renderer_config&, const PSRendererGeometry&) override;
    void ClearState() override;
    void WindowSizeSet(float, float) override;
    void CompositionScaleSet(float, float) override;
    void FrameBufferSizeSet(uint32_t, uint32_t) override;
    void Zoom(float) override;
    void Pan(float,float) override;
    void Rotate(float,float,float) override;
    void TransformReset() override;
    void FrameBufferPartialUpdate(const uint8_t*,size_t,const protocol::Rectangle&) override;
    void Reserved10() override {}
    void Reserved11() override {}
    void Present(bool) override;
    DirectX::XMFLOAT2 CenterGet() override;
    bool InputTransform(float,float,sd_pointer_sample&) override;
    void Detach() noexcept;
    void ResetStream() noexcept;
    void ViewportSet(const sd_viewport&);
    void ViewStateGet(sd_view_state&) const;
    void OrientationSet(uint32_t orientation);
    void StateGet(sd_renderer_state&) const;
    bool Enabled() const noexcept;
    void OnFrame(std::shared_ptr<const FrameSnapshot>, const std::vector<protocol::Rectangle>&);
    void MarkFailed(HRESULT) noexcept;
    sd_result ReadPixels(uint8_t*,uint64_t,sd_frame&);
    std::unique_ptr<::sd_gpu_texture> CopyTexture();
};
}
struct sd_gpu_texture {
    std::shared_ptr<sd::native::D3D11Context> gpu;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> keyed;
    HANDLE shared = nullptr;
    uint32_t width = 0,height = 0;
    uint64_t sequence = 0;
    ~sd_gpu_texture();
    sd_result ReadPixels(uint8_t*,uint64_t,sd_frame&);
};
