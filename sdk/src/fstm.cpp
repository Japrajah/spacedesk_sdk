#include "fstm.h"
#include <stdexcept>

namespace sd::native {
namespace {
using A = SAFstmAction;
using S = SA_FSTM_STATE;
// Exact next states and callback positions from UWP SAFstm::SAFstm (0x1400255C0).
// False FLIRT names at 0x1400272E0 and 0x140027750 are respectively Start and reconnect actions.
constexpr SAFstmTransition table[7][17] = {
    { // DISCONNECTED
        {A::Connect, S::UNCONNECTED_CONNECTING_SERVER}, // event 0; sub_1400270A0
        {A::AutoConnect, S::UNCONNECTED_RECONNECTING_SERVER}, // event 1; sub_1400271D0
        {A::None, S::DISCONNECTED}, // event 2; 0
        {A::None, S::DISCONNECTED}, // event 3; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 4; 0
        {A::None, S::DISCONNECTED}, // event 5; 0
        {A::None, S::DISCONNECTED}, // event 6; 0
        {A::None, S::DISCONNECTED}, // event 7; 0
        {A::None, S::DISCONNECTED}, // event 8; 0
        {A::None, S::DISCONNECTED}, // event 9; 0
        {A::None, S::DISCONNECTED}, // event 10; 0
        {A::None, S::DISCONNECTED}, // event 11; 0
        {A::None, S::DISCONNECTED}, // event 12; 0
        {A::None, S::DISCONNECTED}, // event 13; 0
        {A::None, S::DISCONNECTED}, // event 14; 0
        {A::None, S::DISCONNECTED}, // event 15; 0
        {A::None, S::DISCONNECTED}, // event 16; 0
    },
    { // UNCONNECTED_CONNECTING_SERVER
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 0; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 1; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 2; 0
        {A::ServerConnected, S::CONNECTED_DISPLAY_OFF}, // event 3; std::numpunct<unsigned short>::_Init
        {A::None, S::DISCONNECTED}, // event 4; 0
        {A::None, S::DISCONNECTED}, // event 5; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 6; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 7; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 8; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 9; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 10; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 11; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 12; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 13; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 14; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 15; 0
        {A::None, S::UNCONNECTED_CONNECTING_SERVER}, // event 16; 0
    },
    { // UNCONNECTED_RECONNECTING_SERVER
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 0; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 1; 0
        {A::Disconnect, S::DISCONNECTING_SERVER}, // event 2; sub_1400275C0
        {A::ServerConnected, S::CONNECTED_DISPLAY_OFF}, // event 3; std::numpunct<unsigned short>::_Init
        {A::Reconnect, S::UNCONNECTED_RECONNECTING_SERVER}, // event 4; &UnDecorator::getZName
        {A::None, S::DISCONNECTED}, // event 5; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 6; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 7; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 8; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 9; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 10; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 11; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 12; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 13; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 14; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 15; 0
        {A::None, S::UNCONNECTED_RECONNECTING_SERVER}, // event 16; 0
    },
    { // CONNECTED_DISPLAY_OFF
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 0; 0
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 1; 0
        {A::Disconnect, S::DISCONNECTING_SERVER}, // event 2; sub_1400275C0
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 3; 0
        {A::ReconnectConnected, S::UNCONNECTED_RECONNECTING_SERVER}, // event 4; sub_140027AE0
        {A::DisposeContext, S::DISCONNECTED}, // event 5; sub_140027B50
        {A::DisplayOn, S::CONNECTED_DISPLAY_ON}, // event 6; sub_140027A70
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 7; 0
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 8; 0
        {A::InputMouse, S::CONNECTED_DISPLAY_OFF}, // event 9; sub_140027B90
        {A::InputKeyboard, S::CONNECTED_DISPLAY_OFF}, // event 10; sub_140027D60
        {A::InputTouch, S::CONNECTED_DISPLAY_OFF}, // event 11; sub_140027EC0
        {A::InputPen, S::CONNECTED_DISPLAY_OFF}, // event 12; sub_140027FD0
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 13; 0
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 14; 0
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 15; 0
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 16; 0
    },
    { // CONNECTED_DISPLAY_ON
        {A::None, S::CONNECTED_DISPLAY_ON}, // event 0; 0
        {A::None, S::CONNECTED_DISPLAY_ON}, // event 1; 0
        {A::Disconnect, S::DISCONNECTING_SERVER}, // event 2; sub_1400275C0
        {A::None, S::CONNECTED_DISPLAY_ON}, // event 3; 0
        {A::ReconnectConnected, S::UNCONNECTED_RECONNECTING_SERVER}, // event 4; sub_140027AE0
        {A::DisposeContext, S::DISCONNECTED}, // event 5; sub_140027B50
        {A::None, S::CONNECTED_DISPLAY_ON}, // event 6; 0
        {A::None, S::CONNECTED_DISPLAY_OFF}, // event 7; 0
        {A::None, S::CONNECTED_DISPLAY_ON}, // event 8; 0
        {A::InputMouse, S::CONNECTED_DISPLAY_ON}, // event 9; sub_140027B90
        {A::InputKeyboard, S::CONNECTED_DISPLAY_ON}, // event 10; sub_140027D60
        {A::InputTouch, S::CONNECTED_DISPLAY_ON}, // event 11; sub_140027EC0
        {A::InputPen, S::CONNECTED_DISPLAY_ON}, // event 12; sub_140027FD0
        {A::DisposeContext, S::CONNECTED_DISPLAY_ON}, // event 13; sub_140027B50
        {A::None, S::CONNECTED_FULLSCREEN}, // event 14; 0
        {A::None, S::CONNECTED_DISPLAY_ON}, // event 15; 0
        {A::None, S::CONNECTED_DISPLAY_ON}, // event 16; 0
    },
    { // DISCONNECTING_SERVER
        {A::None, S::DISCONNECTING_SERVER}, // event 0; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 1; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 2; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 3; 0
        {A::None, S::DISCONNECTED}, // event 4; 0
        {A::None, S::DISCONNECTED}, // event 5; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 6; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 7; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 8; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 9; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 10; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 11; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 12; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 13; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 14; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 15; 0
        {A::None, S::DISCONNECTING_SERVER}, // event 16; 0
    },
    { // CONNECTED_FULLSCREEN
        {A::None, S::CONNECTED_FULLSCREEN}, // event 0; 0
        {A::None, S::CONNECTED_FULLSCREEN}, // event 1; 0
        {A::FullscreenDisconnect, S::DISCONNECTING_SERVER}, // event 2; sub_140028310
        {A::None, S::CONNECTED_FULLSCREEN}, // event 3; 0
        {A::FullscreenReconnect, S::UNCONNECTED_RECONNECTING_SERVER}, // event 4; sub_1400283C0
        {A::LeaveFullscreen, S::DISCONNECTED}, // event 5; sub_140028470
        {A::None, S::CONNECTED_FULLSCREEN}, // event 6; 0
        {A::LeaveFullscreen, S::CONNECTED_DISPLAY_OFF}, // event 7; sub_140028470
        {A::None, S::CONNECTED_FULLSCREEN}, // event 8; 0
        {A::InputMouse, S::CONNECTED_FULLSCREEN}, // event 9; sub_140027B90
        {A::InputKeyboard, S::CONNECTED_FULLSCREEN}, // event 10; sub_140027D60
        {A::InputTouch, S::CONNECTED_FULLSCREEN}, // event 11; sub_140027EC0
        {A::InputPen, S::CONNECTED_FULLSCREEN}, // event 12; sub_140027FD0
        {A::DisposeContext, S::CONNECTED_FULLSCREEN}, // event 13; sub_140027B50
        {A::None, S::CONNECTED_FULLSCREEN}, // event 14; 0
        {A::DisposeContext, S::CONNECTED_DISPLAY_ON}, // event 15; sub_140027B50
        {A::None, S::CONNECTED_FULLSCREEN}, // event 16; 0
    },
};
}
const SAFstmTransition& SAFstm::Transition(SA_FSTM_STATE state, SA_FSTM_EVENT event) {
    const auto s = uint32_t(state), e = uint32_t(event);
    if (s >= 7 || e >= 17) throw std::out_of_range("Invalid SAFstm state/event");
    return table[s][e];
}
bool SAFstm::ProcessEvent(SA_FSTM_EVENT event, void* context) {
    const auto previous = state_;
    const auto transition = Transition(previous, event);
    if (transition.action != SAFstmAction::None) observer_.Invoke(transition.action, context);
    state_ = transition.next;
    if (previous != state_) observer_.OnEntry(state_, previous, event, context);
    return transition.action != SAFstmAction::None || previous != state_;
}
}
