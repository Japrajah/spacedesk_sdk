#pragma once
#include <array>
#include <cstdint>

namespace sd::native {
enum class SA_FSTM_STATE : uint32_t {
    DISCONNECTED = 0, UNCONNECTED_CONNECTING_SERVER = 1, UNCONNECTED_RECONNECTING_SERVER = 2,
    CONNECTED_DISPLAY_OFF = 3, CONNECTED_DISPLAY_ON = 4, DISCONNECTING_SERVER = 5, CONNECTED_FULLSCREEN = 6
};
enum class SA_FSTM_EVENT : uint32_t {
    MENU_CONNECT_SERVER = 0, MENU_AUTO_CONNECT_SERVER, MENU_DISCONNECT_SERVER, SERVER_CONNECTED,
    SERVER_DISCONNECTED_RECONNECT_ON, SERVER_DISCONNECTED_RECONNECT_OFF,
    SERVER_DISPLAY_VISIBILITY_ON, SERVER_DISPLAY_VISIBILITY_OFF, SERVER_POWER_SUSPEND,
    CLIENT_INPUT_MOUSE, CLIENT_INPUT_KEYBOARD, CLIENT_INPUT_TOUCH, CLIENT_INPUT_PEN,
    CLIENT_DISPLAY_ORIENTATION_CHANGED, CLIENT_FULLSCREEN_ON, CLIENT_FULLSCREEN_OFF, TIMER
};
// Descriptive IDs for recovered callbacks; the original addresses are retained
// in fstm.cpp and docs/reverse/uwp-fstm-transitions.json.
enum class SAFstmAction : uint32_t {
    None, Connect, AutoConnect, ServerConnected, Disconnect, Reconnect, ReconnectConnected,
    DisplayOn, DisposeContext, InputMouse, InputKeyboard, InputTouch, InputPen,
    FullscreenDisconnect, FullscreenReconnect, LeaveFullscreen
};
struct SAFstmTransition { SAFstmAction action; SA_FSTM_STATE next; };
// SDK adapter boundary: original UI actions become state notifications, while
// transport actions retain the native ordering. Not an inferred native base.
struct SAFstmObserver {
    virtual void Invoke(SAFstmAction, void* context) = 0;
    virtual void OnEntry(SA_FSTM_STATE, SA_FSTM_STATE previous, SA_FSTM_EVENT, void* context) = 0;
protected:
    ~SAFstmObserver() = default;
};
class SAFstm {
    SAFstmObserver& observer_;
    SA_FSTM_STATE state_ = SA_FSTM_STATE::DISCONNECTED;
public:
    explicit SAFstm(SAFstmObserver& observer) : observer_(observer) {}
    SA_FSTM_STATE State() const noexcept { return state_; }
    // This is owner-thread code, matching the native UI/FSM serialization.
    bool ProcessEvent(SA_FSTM_EVENT event, void* context = nullptr);
    static const SAFstmTransition& Transition(SA_FSTM_STATE, SA_FSTM_EVENT);
};
}
