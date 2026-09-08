#pragma once
#include "spacedesk_sdk.h"
#include "spacedesk_protocol.hpp"
#include <cstdint>
#include <map>

namespace sd::native {
// SDK boundary replacing global SAFstm lookup; not a recovered native base.
struct SAPointerOutputSink {
    virtual sd_result Emit(const protocol::MouseHeader&) = 0;
    virtual sd_result Emit(const protocol::PenHeader&) = 0;
    virtual sd_result Emit(const protocol::TouchHeader&) = 0;
protected:
    ~SAPointerOutputSink() = default;
};
class SAPointerOutput {
public:
    SAPointerOutput() = default;
    virtual ~SAPointerOutput() = default;
    // Names are reviewed event-role aliases; exact original method spelling is unknown.
    virtual sd_result OnPointerMoved(const sd_pointer_sample&, uint64_t now) = 0;
    virtual sd_result OnPointerPressed(const sd_pointer_sample&, uint64_t now) = 0;
    virtual sd_result OnPointerReleased(const sd_pointer_sample&, uint64_t now) = 0;
    virtual sd_result OnPointerCaptureLost(const sd_pointer_sample&, uint64_t now) = 0;
    virtual sd_result OnPointerWheelChanged(const sd_pointer_sample&, uint64_t now) = 0;
};
class SAMouseOutput final : public SAPointerOutput {
    SAPointerOutputSink& sink_;
    int32_t delta_x_ = 0, delta_y_ = 0, wheel_ = 0;
    uint32_t changes_ = 0;
    sd_result Emit();
public:
    explicit SAMouseOutput(SAPointerOutputSink& sink) : sink_(sink) {}
    sd_result OnPointerMoved(const sd_pointer_sample&, uint64_t) override;
    sd_result OnPointerPressed(const sd_pointer_sample&, uint64_t) override;
    sd_result OnPointerReleased(const sd_pointer_sample&, uint64_t) override;
    sd_result OnPointerCaptureLost(const sd_pointer_sample& s, uint64_t now) override { return OnPointerReleased(s, now); }
    sd_result OnPointerWheelChanged(const sd_pointer_sample&, uint64_t) override;
    sd_result OnRawMouseMoved(const sd_pointer_sample&);
    void Reset() noexcept { delta_x_ = delta_y_ = wheel_ = 0; changes_ = 0; }
};
class SAPenOutput final : public SAPointerOutput {
    SAPointerOutputSink& sink_;
    // Original 32-byte event context; x/y and pressure become the typed wire packet.
    uint32_t width_ = 0, height_ = 0;
    float x_ = 0, y_ = 0, x_tilt_ = 0, y_tilt_ = 0, pressure_ = 0;
    bool contact_ = false, in_range_ = false;
    void Update(const sd_pointer_sample&);
    sd_result Emit();
public:
    explicit SAPenOutput(SAPointerOutputSink& sink) : sink_(sink) {}
    sd_result OnPointerMoved(const sd_pointer_sample&, uint64_t) override;
    sd_result OnPointerPressed(const sd_pointer_sample&, uint64_t) override;
    sd_result OnPointerReleased(const sd_pointer_sample&, uint64_t) override;
    sd_result OnPointerCaptureLost(const sd_pointer_sample& s, uint64_t now) override { return OnPointerReleased(s, now); }
    sd_result OnPointerWheelChanged(const sd_pointer_sample&, uint64_t) override { return SD_IGNORED; }
    void Reset() noexcept;
};
class SATouchOutput final : public SAPointerOutput {
    SAPointerOutputSink& sink_;
    struct Point { float x, y; };
    // Native ordered map of live float positions and integer snapshot map.
    std::map<uint32_t, Point> contacts_;
    std::map<uint32_t, protocol::TouchContact> snapshot_;
    uint32_t width_ = 0, height_ = 0;
    bool active_ = false, mode_ = false;
    uint64_t next_tick_ = 0;
    void Store(const sd_pointer_sample&);
    sd_result Emit(uint32_t flags);
    sd_result Start(uint64_t now);
    sd_result Stop();
    sd_result RemoveOutside(uint32_t id);
public:
    static constexpr uint32_t timer_interval_ms = 25; // DispatcherTimer interval 250000 x 100 ns
    explicit SATouchOutput(SAPointerOutputSink& sink, bool mode = false) : sink_(sink), mode_(mode) {}
    ~SATouchOutput() override;
    sd_result OnPointerMoved(const sd_pointer_sample&, uint64_t) override;
    sd_result OnPointerPressed(const sd_pointer_sample&, uint64_t) override;
    sd_result OnPointerReleased(const sd_pointer_sample&, uint64_t) override;
    sd_result OnPointerCaptureLost(const sd_pointer_sample& s, uint64_t now) override { return OnPointerReleased(s, now); }
    sd_result OnPointerWheelChanged(const sd_pointer_sample&, uint64_t) override { return SD_IGNORED; }
    bool Active() const noexcept { return active_; }
    sd_result SetMode(bool mode);
    sd_result Reset(bool release = true);
    void ProcessTick(uint64_t now);
    uint32_t UntilTick(uint64_t now) const noexcept;
};
}
