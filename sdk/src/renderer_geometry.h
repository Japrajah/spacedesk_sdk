#pragma once
#include "spacedesk_sdk.h"
#include <DirectXMath.h>

namespace sd::native {
// Recovered class/constructor state. Methods are descriptive role aliases.
class PSDX11Transforms {
public:
    float camera_distance = 1.5f;
    DirectX::XMFLOAT4X4 model{}, view{}, projection{};
    PSDX11Transforms() = default;
    virtual ~PSDX11Transforms() = default;
    void CameraSet(float aspect, float half_height, float front_z);
    void ModelSet(float rx, float ry, float rz, float zoom, float pan_x, float pan_y,
                  float surface_width, float surface_height, float viewport_aspect);
};
// Geometry portion of the native scene. GPU resources are still a separate port.
class PSDX11Scene {
public:
    const DirectX::XMFLOAT3 half_extent{0.5f, 0.5f, 0.0125f};
    PSDX11Transforms transforms;
    bool InputTransform(float viewport_width, float viewport_height, float x, float y,
                        DirectX::XMFLOAT2& normalized) const;
};
// SDK adapter holding PSRendererD3D11's view state without claiming a GPU backend.
class PSRendererGeometry {
    sd_viewport viewport_{};
    uint32_t width_ = 0, height_ = 0;
    uint32_t orientation_ = 1;
    float physical_width_ = 1, physical_height_ = 1;
    float effective_dpi_ = 96, scale_x_ = 1, scale_y_ = 1;
    float zoom_ = 1, pan_x_ = 0, pan_y_ = 0, rx_ = 0, ry_ = 0, rz_ = 0;
    PSDX11Scene scene_;
    void Update();
public:
    PSRendererGeometry();
    void WindowSizeSet(const sd_viewport& viewport);
    void FrameBufferSizeSet(uint32_t width, uint32_t height);
    void Zoom(float delta);
    void Pan(float x, float y);
    void Rotate(float x, float y, float z);
    void OrientationSet(uint32_t orientation);
    uint32_t OrientationGet() const noexcept { return orientation_; }
    void Reset();
    void StateGet(sd_view_state& state) const;
    sd_viewport ViewportGet() const noexcept { return viewport_; }
    bool InputTransform(float x, float y, DirectX::XMFLOAT2& output) const;
    bool HasFrame() const noexcept { return width_ != 0 && height_ != 0; }
    uint32_t Width() const noexcept { return width_; }
    uint32_t Height() const noexcept { return height_; }
};
}
