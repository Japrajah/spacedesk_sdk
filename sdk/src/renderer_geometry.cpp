#include "renderer_geometry.h"
#include "frame_decoder.h"
#include "display_orientation.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace sd::native {
using namespace DirectX;
void PSDX11Transforms::CameraSet(float aspect, float half_height, float front_z) {
    const float fov = 2.0f * std::atan(half_height / camera_distance);
    // Native inlines this construction, with no DirectXMath debug assertion on
    // narrow aspect ratios. Keep its scalar sin/cos and right-handed constants.
    float sine, cosine;
    XMScalarSinCos(&sine, &cosine, fov * 0.5f);
    const float cotangent = cosine / sine;
    const float depth = 100.0f / (0.01f - 100.0f);
    XMStoreFloat4x4(&projection, XMMatrixSet(cotangent / aspect, 0, 0, 0,
        0, cotangent, 0, 0, 0, 0, depth, -1, 0, 0, depth * 0.01f, 0));
    const auto eye = XMVectorSet(0, 0, front_z + camera_distance, 0);
    const auto target = XMVectorSubtract(eye, XMVectorSet(0, 0, 0.00001f, 0));
    XMStoreFloat4x4(&view, XMMatrixLookAtRH(eye, target, XMVectorSet(0, 1, 0, 0)));
}
void PSDX11Transforms::ModelSet(float rx, float ry, float rz, float zoom, float px, float py,
                               float width, float height, float viewport_aspect) {
    float aspect = width / height;
    if (aspect == 0) aspect = 1;
    float scale = zoom;
    if (aspect > viewport_aspect) scale = zoom * viewport_aspect / aspect;
    auto matrix = XMMatrixScaling(aspect * scale, scale, 1);
    matrix = XMMatrixMultiply(matrix, XMMatrixRotationZ(rz));
    matrix = XMMatrixMultiply(matrix, XMMatrixRotationY(ry));
    matrix = XMMatrixMultiply(matrix, XMMatrixRotationX(rx));
    matrix = XMMatrixMultiply(matrix, XMMatrixTranslation(px, py, 0));
    XMStoreFloat4x4(&model, matrix);
}
bool PSDX11Scene::InputTransform(float width, float height, float x, float y, XMFLOAT2& result) const {
    const auto model = XMLoadFloat4x4(&transforms.model);
    const auto view = XMLoadFloat4x4(&transforms.view);
    const auto projection = XMLoadFloat4x4(&transforms.projection);
    const auto near_point = XMVector3Unproject(XMVectorSet(x, y, 0, 0), 0, 0, width, height, 0, 1, projection, view, model);
    const auto far_point = XMVector3Unproject(XMVectorSet(x, y, 1, 0), 0, 0, width, height, 0, 1, projection, view, model);
    const auto ray = XMVector3Normalize(XMVectorSubtract(far_point, near_point));
    const auto origin = XMVectorSet(-half_extent.x, half_extent.y, half_extent.z, 0);
    const auto edge_u = XMVectorSet(2 * half_extent.x, 0, 0, 0);
    const auto edge_v = XMVectorSet(0, -2 * half_extent.y, 0, 0);
    const auto cross = XMVector3Cross(ray, edge_v);
    const float determinant = XMVectorGetX(XMVector3Dot(cross, edge_u));
    if (!std::isfinite(determinant) || (determinant > -0.000001f && determinant < 0.000001f)) return false;
    const float inverse = 1 / determinant;
    const auto offset = XMVectorSubtract(near_point, origin);
    const float u = XMVectorGetX(XMVector3Dot(offset, cross)) * inverse;
    if (!std::isfinite(u) || u < 0 || u > 1) return false;
    const float v = XMVectorGetX(XMVector3Dot(XMVector3Cross(offset, edge_u), ray)) * inverse;
    if (!std::isfinite(v) || v < 0 || v > 1) return false;
    // Native checks u and v independently: this is the full rectangle, not u+v<=1.
    result = {u, v}; return true;
}
PSRendererGeometry::PSRendererGeometry() {
    viewport_.struct_size = sizeof(viewport_);
    viewport_.logical_width = 1280; viewport_.logical_height = 720;
    viewport_.dpi = 96; viewport_.composition_scale_x = viewport_.composition_scale_y = 1;
    Update();
}
void PSRendererGeometry::Update() {
    const float dpi = viewport_.dpi;
    effective_dpi_ = dpi;
    scale_x_ = viewport_.composition_scale_x; scale_y_ = viewport_.composition_scale_y;
    auto pixels = [](float logical, float value) { return std::floor(value * logical / 96.0f + 0.5f); };
    if (dpi > 192) {
        const float w = pixels(viewport_.logical_width, dpi), h = pixels(viewport_.logical_height, dpi);
        if (std::max(w, h) > 1920 && std::min(w, h) > 1080) {
            effective_dpi_ *= 0.5f; scale_x_ *= 0.5f; scale_y_ *= 0.5f;
        }
    }
    physical_width_ = std::max(pixels(viewport_.logical_width, effective_dpi_), 1.0f);
    physical_height_ = std::max(pixels(viewport_.logical_height, effective_dpi_), 1.0f);
    if (!HasFrame()) return;
    const float aspect = physical_width_ / physical_height_;
    scene_.transforms.CameraSet(aspect, scene_.half_extent.y, scene_.half_extent.z);
    scene_.transforms.ModelSet(std::fmod(rx_, XM_2PI), std::fmod(ry_, XM_2PI), std::fmod(rz_, XM_2PI),
                              zoom_, pan_x_, pan_y_, float(DisplaySideways(orientation_)?height_:width_),
                              float(DisplaySideways(orientation_)?width_:height_), aspect);
}
void PSRendererGeometry::WindowSizeSet(const sd_viewport& viewport) { viewport_ = viewport; Update(); }
void PSRendererGeometry::OrientationSet(uint32_t orientation) { orientation_=orientation;Update(); }
void PSRendererGeometry::FrameBufferSizeSet(uint32_t width, uint32_t height) {
    if (width_ != width || height_ != height) { width_ = width; height_ = height; Reset(); }
}
void PSRendererGeometry::Zoom(float delta) {
    const float candidate = zoom_ + delta;
    if (!std::isfinite(candidate)) throw Failure(SD_INVALID_ARGUMENT, "View zoom overflow");
    if (candidate > 0.05f) zoom_ = candidate;
    Update();
}
void PSRendererGeometry::Pan(float x, float y) {
    const float px = pan_x_ + x, py = pan_y_ + y;
    if (x > 0 ? px < 1 : px > -1) pan_x_ = px;
    if (y > 0 ? py < 1 : py > -1) pan_y_ = py;
    Update();
}
void PSRendererGeometry::Rotate(float x, float y, float z) {
    const float rx = rx_ + x, ry = ry_ + y, rz = rz_ - z;
    if (!std::isfinite(rx) || !std::isfinite(ry) || !std::isfinite(rz))
        throw Failure(SD_INVALID_ARGUMENT, "View rotation overflow");
    rx_ = rx; ry_ = ry; rz_ = rz;
    Update();
}
void PSRendererGeometry::Reset() { zoom_ = 1; pan_x_ = pan_y_ = rx_ = ry_ = rz_ = 0; Update(); }
void PSRendererGeometry::StateGet(sd_view_state& state) const {
    state = {}; state.struct_size = sizeof(state);
    state.surface_width = width_; state.surface_height = height_;
    state.viewport_width = physical_width_; state.viewport_height = physical_height_;
    state.effective_dpi = effective_dpi_; state.composition_scale_x = scale_x_; state.composition_scale_y = scale_y_;
    state.zoom = zoom_; state.pan_x = pan_x_; state.pan_y = pan_y_;
    state.rotation_x = rx_; state.rotation_y = ry_; state.rotation_z = rz_;
    if (HasFrame()) {
        std::memcpy(state.model, &scene_.transforms.model, sizeof(state.model));
        std::memcpy(state.view, &scene_.transforms.view, sizeof(state.view));
        std::memcpy(state.projection, &scene_.transforms.projection, sizeof(state.projection));
    }
}
bool PSRendererGeometry::InputTransform(float x, float y, XMFLOAT2& output) const {
    if (!HasFrame()) return false;
    XMFLOAT2 uv{};
    if (!scene_.InputTransform(physical_width_, physical_height_, x * scale_x_, y * scale_y_, uv)) return false;
    const auto source=DisplaySourceUv(orientation_,uv.x,uv.y);
    output = {source[0] * float(width_), source[1] * float(height_)};
    return true;
}
}
