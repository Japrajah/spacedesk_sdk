#include "renderer_d3d11.h"
#include "display_orientation.h"
#include "sdk_shaders.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <limits>

namespace sd::native {
using Microsoft::WRL::ComPtr;
namespace {
std::string error_text(HRESULT hr, const char* operation) {
    char text[256]{};
    std::snprintf(text, sizeof(text), "%s failed: HRESULT 0x%08X", operation, unsigned(hr));
    return text;
}
void check(HRESULT hr, const char* operation) { if (FAILED(hr)) throw GpuFailure(hr, operation); }
struct Vertex { DirectX::XMFLOAT3 position; DirectX::XMFLOAT2 uv; };
static_assert(sizeof(Vertex) == 20);
const Vertex vertices[] = {
    {{-.5f,-.5f,-.0125f},{0,1}}, {{-.5f,-.5f,.0125f},{0,1}},
    {{-.5f,.5f,-.0125f},{0,0}}, {{-.5f,.5f,.0125f},{0,0}},
    {{.5f,-.5f,-.0125f},{1,1}}, {{.5f,-.5f,.0125f},{1,1}},
    {{.5f,.5f,-.0125f},{1,0}}, {{.5f,.5f,.0125f},{1,0}}
};
const uint16_t indices[] = {0,2,1,1,2,3,4,5,6,5,7,6,0,1,5,0,5,4,2,6,7,2,7,3,0,4,6,0,6,2,1,3,7,1,7,5};
void metadata(sd_frame& frame, uint32_t width, uint32_t height, uint64_t sequence, uint8_t* pixels) {
    frame = {}; frame.struct_size = sizeof(frame); frame.width = width; frame.height = height;
    frame.stride = width * 4; frame.pixel_format = SD_PIXEL_BGRA8; frame.sequence = sequence; frame.pixels = pixels;
}
sd_result read_texture(D3D11Context& gpu, ID3D11Texture2D* texture, uint32_t width, uint32_t height,
                      uint64_t sequence, uint8_t* buffer, uint64_t capacity, sd_frame& frame) {
    metadata(frame, width, height, sequence, nullptr);
    if (!buffer) return capacity ? SD_INVALID_ARGUMENT : SD_OK;
    if (capacity < uint64_t(frame.stride) * height) return SD_INVALID_ARGUMENT;
    D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    check(gpu.device->CreateTexture2D(&desc, nullptr, &staging), "GPU readback CreateTexture2D");
    gpu.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    check(gpu.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "GPU readback Map");
    for (uint32_t y = 0; y < height; ++y)
        std::memcpy(buffer + size_t(y) * frame.stride, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, frame.stride);
    gpu.context->Unmap(staging.Get(), 0); frame.pixels = buffer;
    return SD_OK;
}
}
GpuFailure::GpuFailure(HRESULT hr, const char* operation)
    : Failure(SD_GPU_ERROR, error_text(hr, operation).c_str()), hresult(hr) {}
D3D11Context::~D3D11Context() {
    if (context) { context->ClearState(); context->Flush(); }
    context.Reset(); device.Reset();
    if (module) FreeLibrary(module);
}
void PSDX11BufferManager::Initialize(ID3D11Device3* device) {
    orientation_=1;
    D3D11_BUFFER_DESC desc{}; desc.ByteWidth = sizeof(::sd::native::vertices); desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA data{}; data.pSysMem = ::sd::native::vertices;
    check(device->CreateBuffer(&desc, &data, &vertices), "PSDX11BufferManager::VertexBufferInitialize");
    desc.ByteWidth = sizeof(::sd::native::indices); desc.BindFlags = D3D11_BIND_INDEX_BUFFER; data.pSysMem = ::sd::native::indices;
    check(device->CreateBuffer(&desc, &data, &indices), "PSDX11BufferManager::IndexBufferInitialize");
    desc.ByteWidth = 192; desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    check(device->CreateBuffer(&desc, nullptr, &constants), "PSDX11BufferManager::ConstantBufferInitialize");
}
void PSDX11BufferManager::Bind(ID3D11DeviceContext1* context, const sd_view_state& state,uint32_t orientation) {
    if(orientation_!=orientation) {
        Vertex changed[8];std::copy(std::begin(::sd::native::vertices),std::end(::sd::native::vertices),changed);
        for(auto& vertex:changed) {
            const auto uv=DisplaySourceUv(orientation,vertex.uv.x,vertex.uv.y);
            vertex.uv={uv[0],uv[1]};
        }
        context->UpdateSubresource(vertices.Get(),0,nullptr,changed,0,0);orientation_=orientation;
    }
    UINT stride = 20, offset = 0;
    auto vb = vertices.Get(); auto cb = constants.Get();
    context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R16_UINT, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->UpdateSubresource1(cb, 0, nullptr, state.model, 0, 0, 0);
    context->VSSetConstantBuffers1(0, 1, &cb, nullptr, nullptr);
}
PSRendererD3D11::PSRendererD3D11() : geometry_(std::make_unique<PSRendererGeometry>()) {
    viewport_ = geometry_->ViewportGet(); config_.struct_size = sizeof(config_); config_.present_interval = 1;
}
PSRendererD3D11::~PSRendererD3D11() { Detach(); }
void PSRendererD3D11::PrivateDeviceCreate() {
    gpu_ = std::make_shared<D3D11Context>();
    gpu_->module = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!gpu_->module) throw GpuFailure(HRESULT_FROM_WIN32(GetLastError()), "LoadLibrary d3d11.dll");
    const auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(gpu_->module, "D3D11CreateDevice"));
    if (!create) throw GpuFailure(E_NOINTERFACE, "D3D11CreateDevice export");
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,D3D_FEATURE_LEVEL_9_3,D3D_FEATURE_LEVEL_9_2,D3D_FEATURE_LEVEL_9_1};
    gpu_->driver = config_.driver == SD_RENDERER_WARP ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    check(create(nullptr, gpu_->driver, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 7, D3D11_SDK_VERSION,
                 &device, &gpu_->feature_level, &context), "PSRendererD3D11::PrivateDeviceCreate");
    check(device.As(&gpu_->device), "ID3D11Device3");
    check(context.As(&gpu_->context), "ID3D11DeviceContext1");
}
void PSRendererD3D11::PrivateShadersCreate() {
    check(gpu_->device->CreateVertexShader(shaders::vertex, sizeof(shaders::vertex), nullptr, &vertex_), "PSDX11Scene::CreateVertexShader");
    check(gpu_->device->CreatePixelShader(shaders::pixel, sizeof(shaders::pixel), nullptr, &pixel_), "PSDX11Scene::CreatePixelShader");
    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0}};
    check(gpu_->device->CreateInputLayout(elements, 2, shaders::vertex, sizeof(shaders::vertex), &layout_), "PSDX11Scene::CreateInputLayout");
    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_ANISOTROPIC;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxAnisotropy = 16; sampler.ComparisonFunc = D3D11_COMPARISON_ALWAYS; sampler.MaxLOD = D3D11_FLOAT32_MAX;
    check(gpu_->device->CreateSamplerState(&sampler, &sampler_), "PSDX11Scene::CreateSamplerState");
}
void PSRendererD3D11::PrivateResourcesWindowCreate() {
    sd_view_state state{}; geometry_->StateGet(state);
    if (state.viewport_width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || state.viewport_height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
        throw GpuFailure(E_INVALIDARG, "Render target exceeds Direct3D dimensions");
    const auto width = uint32_t(state.viewport_width), height = uint32_t(state.viewport_height);
    if (output_ && width == output_width_ && height == output_height_) {
        if (config_.target == SD_RENDERER_COMPOSITION) {
            ComPtr<IDXGISwapChain2> swap2; check(swap_chain_.As(&swap2), "IDXGISwapChain2");
            const DXGI_MATRIX_3X2_F transform{1/state.composition_scale_x,0,0,1/state.composition_scale_y,0,0};
            check(swap2->SetMatrixTransform(&transform), "SetMatrixTransform");
        }
        return;
    }
    gpu_->context->OMSetRenderTargets(0, nullptr, nullptr); output_view_.Reset(); output_.Reset();
    if (config_.target == SD_RENDERER_OFFSCREEN) {
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(gpu_->device->CreateTexture2D(&desc, nullptr, &output_), "Offscreen CreateTexture2D");
    } else {
        if (!swap_chain_) {
            ComPtr<IDXGIDevice1> dxgi; ComPtr<IDXGIAdapter> adapter; ComPtr<IDXGIFactory2> factory;
            check(gpu_->device.As(&dxgi), "IDXGIDevice1"); check(dxgi->GetAdapter(&adapter), "DXGI GetAdapter");
            check(adapter->GetParent(IID_PPV_ARGS(&factory)), "DXGI GetParent");
            DXGI_SWAP_CHAIN_DESC1 desc{}; desc.Width = width; desc.Height = height;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            if (config_.target == SD_RENDERER_HWND) {
                check(factory->CreateSwapChainForHwnd(gpu_->device.Get(), static_cast<HWND>(config_.target_object), &desc, nullptr, nullptr, &swap_chain_), "CreateSwapChainForHwnd");
                check(factory->MakeWindowAssociation(static_cast<HWND>(config_.target_object), DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation");
            } else {
                check(factory->CreateSwapChainForComposition(gpu_->device.Get(), &desc, nullptr, &swap_chain_), "CreateSwapChainForComposition");
                check(static_cast<IUnknown*>(config_.target_object)->QueryInterface(IID_PPV_ARGS(&panel_)), "ISwapChainPanelNative");
                check(panel_->SetSwapChain(swap_chain_.Get()), "ISwapChainPanelNative::SetSwapChain");
            }
            check(dxgi->SetMaximumFrameLatency(1), "SetMaximumFrameLatency");
        } else check(swap_chain_->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0), "ResizeBuffers");
        check(swap_chain_->SetRotation(DXGI_MODE_ROTATION_IDENTITY), "SetRotation");
        if (config_.target == SD_RENDERER_COMPOSITION) {
            ComPtr<IDXGISwapChain2> swap2; check(swap_chain_.As(&swap2), "IDXGISwapChain2");
            const DXGI_MATRIX_3X2_F transform{1 / state.composition_scale_x,0,0,1 / state.composition_scale_y,0,0};
            check(swap2->SetMatrixTransform(&transform), "SetMatrixTransform");
        }
        check(swap_chain_->GetBuffer(0, IID_PPV_ARGS(&output_)), "SwapChain GetBuffer");
    }
    check(gpu_->device->CreateRenderTargetView1(output_.Get(), nullptr, &output_view_), "PSDX11Scene::CreateRenderTargetView");
    output_width_ = width; output_height_ = height;
}
void PSRendererD3D11::ReleaseResources() noexcept {
    if (panel_) panel_->SetSwapChain(nullptr);
    if (gpu_ && gpu_->context) { std::lock_guard<std::recursive_mutex> lock(gpu_->mutex); gpu_->context->ClearState(); gpu_->context->Flush(); }
    panel_.Reset(); swap_chain_.Reset(); source_view_.Reset(); source_.Reset(); output_view_.Reset(); output_.Reset();
    sampler_.Reset(); layout_.Reset(); vertex_.Reset(); pixel_.Reset();
    buffers_.vertices.Reset(); buffers_.indices.Reset(); buffers_.constants.Reset(); gpu_.reset();
    width_ = height_ = output_width_ = output_height_ = fps_ = 0; uploaded_sequence_ = 0;
}
void PSRendererD3D11::Initialize(const sd_renderer_config& config, const PSRendererGeometry& geometry) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    enabled_ = false; ReleaseResources(); config_ = config; last_hresult_ = S_OK;
    geometry_ = std::make_unique<PSRendererGeometry>(geometry); viewport_ = geometry_->ViewportGet();
    times_ = {}; time_index_ = 0; present_sequence_ = 0;
    try {
        PrivateDeviceCreate(); buffers_.Initialize(gpu_->device.Get()); PrivateShadersCreate();
        PrivateResourcesWindowCreate(); enabled_ = true;
        if (latest_) { Upload(*latest_, {}); Present(false); }
    } catch (const GpuFailure& e) { last_hresult_ = e.hresult; enabled_ = false; ReleaseResources(); throw; }
}
void PSRendererD3D11::Detach() noexcept { std::lock_guard<std::recursive_mutex> lock(mutex_); enabled_ = false; ReleaseResources(); }
void PSRendererD3D11::ResetStream() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex_); latest_.reset(); uploaded_sequence_ = 0;
    geometry_->FrameBufferSizeSet(0, 0); width_ = height_ = fps_ = 0; times_ = {}; time_index_ = 0;
    source_view_.Reset(); source_.Reset();
}
void PSRendererD3D11::ClearState() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (gpu_) { std::lock_guard<std::recursive_mutex> context_lock(gpu_->mutex); gpu_->context->ClearState(); }
}
void PSRendererD3D11::FrameBufferSizeSet(uint32_t width, uint32_t height) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (width_ == width && height_ == height && source_) return;
    geometry_->FrameBufferSizeSet(width, height); width_ = width; height_ = height;
    source_view_.Reset(); source_.Reset(); uploaded_sequence_ = 0;
    if (!gpu_ || !width || !height) return;
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    check(gpu_->device->CreateTexture2D(&desc, nullptr, &source_), "PSDX11Scene::CreateTexture2D");
    check(gpu_->device->CreateShaderResourceView(source_.Get(), nullptr, &source_view_), "PSDX11Scene::CreateShaderResourceView");
}
void PSRendererD3D11::FrameBufferPartialUpdate(const uint8_t* bytes, size_t size, const protocol::Rectangle& rect) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!source_ || !bytes || rect.left < 0 || rect.top < 0 || rect.right <= rect.left || rect.bottom <= rect.top ||
        uint32_t(rect.right) > width_ || uint32_t(rect.bottom) > height_ || size < uint64_t(rect.right-rect.left)*(rect.bottom-rect.top)*4)
        throw GpuFailure(E_INVALIDARG, "PSRendererD3D11::FrameBufferPartialUpdate");
    const D3D11_BOX box{UINT(rect.left),UINT(rect.top),0,UINT(rect.right),UINT(rect.bottom),1};
    std::lock_guard<std::recursive_mutex> context_lock(gpu_->mutex);
    gpu_->context->UpdateSubresource(source_.Get(), 0, &box, bytes, UINT(rect.right-rect.left)*4, 1);
}
void PSRendererD3D11::Upload(const FrameSnapshot& frame, const std::vector<protocol::Rectangle>& dirty) {
    FrameBufferSizeSet(frame.width, frame.height);
    if (uploaded_sequence_ + 1 != frame.sequence || !uploaded_sequence_ || dirty.empty()) {
        FrameBufferPartialUpdate(frame.pixels.data(), frame.pixels.size(), {0,0,int32_t(frame.width),int32_t(frame.height)});
    } else {
        std::vector<uint8_t> tile;
        for (const auto& rect : dirty) {
            const auto stride = size_t(rect.right - rect.left) * 4;
            tile.resize(stride * (rect.bottom - rect.top));
            for (int32_t y = rect.top; y < rect.bottom; ++y)
                std::memcpy(tile.data() + size_t(y-rect.top)*stride, frame.pixels.data() + size_t(y)*frame.stride + size_t(rect.left)*4, stride);
            FrameBufferPartialUpdate(tile.data(), tile.size(), rect);
        }
    }
    uploaded_sequence_ = frame.sequence;
}
void PSRendererD3D11::Draw() {
    if (!gpu_ || !output_view_) throw GpuFailure(E_UNEXPECTED, "Renderer is not initialized");
    auto context = gpu_->context.Get(); ID3D11RenderTargetView* rtv = output_view_.Get();
    context->OMSetRenderTargets(1, &rtv, nullptr);
    const float black[] = {0,0,0,1}; context->ClearRenderTargetView(rtv, black);
    if (!source_view_) return;
    const D3D11_VIEWPORT viewport{0,0,float(output_width_),float(output_height_),0,1}; context->RSSetViewports(1, &viewport);
    sd_view_state state{}; geometry_->StateGet(state); buffers_.Bind(context, state,geometry_->OrientationGet());
    context->IASetInputLayout(layout_.Get()); context->VSSetShader(vertex_.Get(), nullptr, 0); context->PSSetShader(pixel_.Get(), nullptr, 0);
    auto srv = source_view_.Get(); auto sampler = sampler_.Get(); context->PSSetShaderResources(0, 1, &srv); context->PSSetSamplers(0, 1, &sampler);
    context->DrawIndexed(36, 0, 0);
}
void PSRendererD3D11::Present(bool count_frame) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!enabled_) throw GpuFailure(FAILED(last_hresult_) ? last_hresult_ : E_UNEXPECTED, "Renderer is not active");
    std::lock_guard<std::recursive_mutex> context_lock(gpu_->mutex);
    Draw();
    HRESULT result = swap_chain_ ? swap_chain_->Present(config_.present_interval, 0) : S_OK;
    check(result, "PSRendererD3D11::Present"); ++present_sequence_;
    if (result == S_OK && count_frame) {
        times_[time_index_] = uint32_t(std::clock()); time_index_ = (time_index_ + 1) % times_.size();
        const auto last = time_index_ ? time_index_ - 1 : times_.size() - 1;
        const auto average = (times_[last] - times_[time_index_]) / 49;
        fps_ = average ? 1000 / average : 0;
    }
}
void PSRendererD3D11::OnFrame(std::shared_ptr<const FrameSnapshot> frame, const std::vector<protocol::Rectangle>& dirty) {
    std::lock_guard<std::recursive_mutex> lock(mutex_); latest_ = std::move(frame);
    if (enabled_) { Upload(*latest_, dirty); Present(true); }
}
void PSRendererD3D11::UpdateView() {
    if (enabled_) { std::lock_guard<std::recursive_mutex> context_lock(gpu_->mutex); PrivateResourcesWindowCreate(); Present(false); }
}
void PSRendererD3D11::ViewportSet(const sd_viewport& viewport) {
    std::lock_guard<std::recursive_mutex> lock(mutex_); viewport_ = viewport; geometry_->WindowSizeSet(viewport_); UpdateView();
}
void PSRendererD3D11::WindowSizeSet(float w,float h) { std::lock_guard<std::recursive_mutex> lock(mutex_); auto v = viewport_; v.logical_width = w; v.logical_height = h; ViewportSet(v); }
void PSRendererD3D11::CompositionScaleSet(float x,float y) { std::lock_guard<std::recursive_mutex> lock(mutex_); auto v = viewport_; v.composition_scale_x = x; v.composition_scale_y = y; ViewportSet(v); }
void PSRendererD3D11::Zoom(float delta) { std::lock_guard<std::recursive_mutex> lock(mutex_); geometry_->Zoom(delta); UpdateView(); }
void PSRendererD3D11::Pan(float x,float y) { std::lock_guard<std::recursive_mutex> lock(mutex_); geometry_->Pan(x,y); UpdateView(); }
void PSRendererD3D11::Rotate(float x,float y,float z) { std::lock_guard<std::recursive_mutex> lock(mutex_); geometry_->Rotate(x,y,z); UpdateView(); }
void PSRendererD3D11::TransformReset() { std::lock_guard<std::recursive_mutex> lock(mutex_); geometry_->Reset(); UpdateView(); }
void PSRendererD3D11::OrientationSet(uint32_t orientation) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);geometry_->OrientationSet(orientation);UpdateView();
}
DirectX::XMFLOAT2 PSRendererD3D11::CenterGet() {
    std::lock_guard<std::recursive_mutex> lock(mutex_); sd_view_state s{}; geometry_->StateGet(s);
    using namespace DirectX;
    const auto p = XMVector3Project(XMVectorSet(0,0,.0125f,0),0,0,s.viewport_width,s.viewport_height,0,1,
        XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(s.projection)), XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(s.view)),
        XMLoadFloat4x4(reinterpret_cast<const XMFLOAT4X4*>(s.model)));
    return {XMVectorGetX(p)/s.composition_scale_x,XMVectorGetY(p)/s.composition_scale_y};
}
bool PSRendererD3D11::InputTransform(float x,float y,sd_pointer_sample& sample) {
    std::lock_guard<std::recursive_mutex> lock(mutex_); sample.surface_width = width_; sample.surface_height = height_; sample.mapped = 0;
    DirectX::XMFLOAT2 point{};
    if (geometry_->InputTransform(x,y,point)) { sample.x = point.x; sample.y = point.y; sample.mapped = 1; }
    return geometry_->HasFrame();
}
void PSRendererD3D11::ViewStateGet(sd_view_state& state) const { std::lock_guard<std::recursive_mutex> lock(mutex_); geometry_->StateGet(state); }
void PSRendererD3D11::StateGet(sd_renderer_state& state) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_); state = {}; state.struct_size = sizeof(state); state.active = enabled_;
    state.target = config_.target; state.last_hresult = last_hresult_; state.width = output_width_; state.height = output_height_;
    state.source_width = latest_ ? latest_->width : 0; state.source_height = latest_ ? latest_->height : 0;
    state.fps = fps_; state.source_sequence = uploaded_sequence_; state.present_sequence = present_sequence_;
    if (gpu_) { state.feature_level = gpu_->feature_level; state.driver_type = gpu_->driver; }
}
bool PSRendererD3D11::Enabled() const noexcept { std::lock_guard<std::recursive_mutex> lock(mutex_); return enabled_; }
void PSRendererD3D11::MarkFailed(HRESULT hr) noexcept { std::lock_guard<std::recursive_mutex> lock(mutex_); last_hresult_ = hr; enabled_ = false; }
sd_result PSRendererD3D11::ReadPixels(uint8_t* buffer,uint64_t capacity,sd_frame& frame) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!enabled_) throw GpuFailure(FAILED(last_hresult_) ? last_hresult_ : E_UNEXPECTED, "Renderer is not active");
    if (!latest_) return SD_NO_FRAME;
    std::lock_guard<std::recursive_mutex> context_lock(gpu_->mutex);
    if (buffer) Draw(); // Refresh the current flip-chain backbuffer before capture.
    return read_texture(*gpu_, output_.Get(), output_width_, output_height_, present_sequence_, buffer, capacity, frame);
}
std::unique_ptr<::sd_gpu_texture> PSRendererD3D11::CopyTexture() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!enabled_) throw GpuFailure(FAILED(last_hresult_) ? last_hresult_ : E_UNEXPECTED, "Renderer is not active");
    if (!latest_) return {};
    std::lock_guard<std::recursive_mutex> context_lock(gpu_->mutex); Draw();
    auto result = std::make_unique<::sd_gpu_texture>(); result->gpu = gpu_; result->width = output_width_; result->height = output_height_; result->sequence = present_sequence_;
    D3D11_TEXTURE2D_DESC desc{}; output_->GetDesc(&desc); desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    check(gpu_->device->CreateTexture2D(&desc, nullptr, &result->texture), "GPU snapshot CreateTexture2D");
    check(result->texture.As(&result->keyed), "GPU snapshot IDXGIKeyedMutex");
    const auto acquired = result->keyed->AcquireSync(0, 1000);
    if (acquired != S_OK) throw GpuFailure(FAILED(acquired) ? acquired : E_FAIL, "GPU snapshot AcquireSync");
    gpu_->context->CopyResource(result->texture.Get(), output_.Get()); gpu_->context->Flush();
    check(result->keyed->ReleaseSync(1), "GPU snapshot ReleaseSync");
    ComPtr<IDXGIResource1> resource; check(result->texture.As(&resource), "GPU snapshot IDXGIResource1");
    check(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &result->shared), "GPU snapshot CreateSharedHandle");
    return result;
}
}
sd_gpu_texture::~sd_gpu_texture() {
    if (shared) CloseHandle(shared);
    keyed.Reset(); texture.Reset(); gpu.reset();
}
sd_result sd_gpu_texture::ReadPixels(uint8_t* buffer,uint64_t capacity,sd_frame& frame) {
    std::lock_guard<std::recursive_mutex> lock(gpu->mutex);
    HRESULT status = keyed->AcquireSync(1, 1000);
    if (status != S_OK) throw sd::native::GpuFailure(FAILED(status) ? status : E_FAIL, "GPU snapshot AcquireSync");
    try {
        auto result = sd::native::read_texture(*gpu, texture.Get(), width, height, sequence, buffer, capacity, frame);
        keyed->ReleaseSync(1); return result;
    } catch (...) { keyed->ReleaseSync(1); throw; }
}
