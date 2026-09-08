#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <shellapi.h>
#include "spacedesk_sdk.h"
#include "spacedesk_settings.hpp"
#include "viewer_pointer.h"
#include "settings_dialog.h"
#include "apk_touch_adapter.h"
#include "viewer_connection.h"
#include "connection_timers.h"
#include "remote_control.h"
#include "text_input.h"
#include "android_keyboard.h"
#include "relative_mouse.h"
#include "local_touch.h"
#include "sonar_viewer.h"
#include "disconnect_message.h"
#include "display_orientation.h"
#include "gdi_frame.h"
#include <algorithm>
#include <array>
#include <string>
#include <fstream>
#include <filesystem>
#include <iterator>
#include <chrono>
#include <future>
#include <optional>
#include <vector>

namespace {
sd_client* client = nullptr;
sd_frame frame{};
bool connected = false, input_enabled = true;
bool mouse_enabled = true, keyboard_enabled = true, pen_enabled = true, touch_enabled = true;
bool android_keyboard_profile = false;
bool touch_packets_enabled = true;
uint32_t mouse_output = 0, physical_touch_output = 0;
uint32_t mouse_movement = 0;
sd_pointer_sample last_absolute_position{};
bool gpu_requested = false, gpu_enabled = false;
bool auto_orientation=false;
uint32_t view_orientation=1,last_remote_orientation=0;
HMONITOR orientation_monitor=nullptr;
sd::viewer::GdiFrameSurface gdi_surface;
bool session_started = false, ever_connected = false, discovery_waiting = false;
bool discovery_automatic = false;
std::string active_address;
uint16_t active_port = 28252;
bool menu_enabled = true, show_fps = false, fullscreen = false;
uint32_t menu_state = 1;
HWND compact_menu_button = nullptr;
uint32_t connection_state = SD_STATE_DISCONNECTED;
uint16_t requested_port = 28252;
std::string explicit_address;
std::wstring status_text = L"disconnected";
bool encoding_attention=false;
HMENU session_menu = nullptr;
WINDOWPLACEMENT normal_placement{sizeof(WINDOWPLACEMENT)};
LONG_PTR normal_style = WS_OVERLAPPEDWINDOW;
uint64_t fps_since = 0, fps_frames = 0, fps_sequence = 0;
double fps_value = 0;
sd::viewer::GenerationGate discovery_generation;
sd::viewer::APKConnectionTimeouts retry_timers;
uint32_t active_auto_timeout = 0, active_reconnect_timeout = 0;
std::optional<sd::viewer::ConnectionTarget> discovery_target;
uint64_t discovery_next = 0, job_generation = 0, desired_generation = 0;
struct DiscoveryReply { sd_result result; std::vector<sd_host> hosts; };
std::future<DiscoveryReply> discovery_job;
enum : UINT { MenuConnect = 2001, MenuDisconnect, MenuSettings, MenuInput, MenuFullscreen, MenuExit, MenuTextInput,
              MenuPresentation, MenuCompactPopup, MenuMouseLocal, MenuMouseRemote, MenuMouseDirect,
              MenuTouchscreen, MenuTouchpad, MenuTouchOff, MenuKeyboard, MenuStreamPause };
bool stream_manual_pause=false,stream_minimized=false;
bool application_active=false;
std::unique_ptr<sd::SASettings> viewer_settings;
sd::viewer::PointerAdapter pointers;
sd::viewer::APKTouchAdapter touch_batches;
sd::viewer::MouseToTouchAdapter mouse_touch;
sd::viewer::ClassicTouchOutput classic_touch;
sd::viewer::LocalTouchCapture local_touch_capture;
sd::viewer::SonarViewer sonar;
sd::viewer::RemoteExitChord remote_exit;
sd::viewer::RelativeMouseCapture relative_mouse;
uint32_t activation_mouse_up=0;
sd_result send_pointer(uint32_t type, uint32_t event, const sd_pointer_sample& sample);
sd_result send_mouse_touch(uint32_t type,uint32_t event,const sd_pointer_sample& sample) {
    if(type!=SD_POINTER_TOUCH) return SD_INVALID_ARGUMENT;
    return classic_touch.Process(event,sample,timeGetTime(),[](const sd_touch_event& touch) {
        return connected?sd_client_send_touch(client,&touch):SD_IGNORED;
    });
}
void map_pointer(HWND window, sd_pointer_sample& sample);
void refresh_orientation(HWND window,bool force=false);
void gpu_viewport(HWND window);
uint64_t touch_timestamp() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
sd_result send_touch(const sd_touch_input& input) {
    return connected ? sd_client_send_touch_input(client, &input) : SD_IGNORED;
}
LONG last_pointer_x = 0, last_pointer_y = 0;
bool have_pointer_position = false;
std::array<bool, 256> pressed{};
std::array<uint32_t, 256> scans{}, key_flags{};
uint32_t mouse_down = 0;
constexpr uint32_t mouse_touch_buttons=SD_MOUSE_LEFT_DOWN|SD_MOUSE_RIGHT_DOWN;
RECT image_rect(HWND window) {
    RECT area{}; GetClientRect(window, &area);
    if (!frame.pixels || !area.right || !area.bottom) return area;
    const bool sideways=view_orientation==2 || view_orientation==4;
    const auto source_width=sideways?frame.height:frame.width,source_height=sideways?frame.width:frame.height;
    const double scale = std::min(double(area.right) / source_width, double(area.bottom) / source_height);
    const LONG w = LONG(source_width * scale), h = LONG(source_height * scale);
    return {(area.right - w) / 2, (area.bottom - h) / 2, (area.right + w) / 2, (area.bottom + h) / 2};
}
void title(HWND window, const wchar_t* state) {
    status_text = state;
    sd_stream_control_state stream{sizeof(stream)};
    if(client) sd_client_get_stream_control_state(client,&stream);
    std::wstring text = L"spacedesk SDK 0.1 - "; text += status_text;
    if(connected && stream.requested_state==SD_STREAM_PAUSE) text+=L" | stream pause requested";
    if(!active_address.empty()) text+=L" | "+std::wstring(active_address.begin(),active_address.end())+L":"+std::to_wstring(active_port);
    if (gpu_requested) text += gpu_enabled ? L" | D3D11" : L" | GDI fallback";
    if (show_fps) { wchar_t rate[48]{}; swprintf_s(rate, L" | %.1f FPS", fps_value); text += rate; }
    const auto sonar_status=sonar.Status();if(!sonar_status.empty()) text+=L" | "+sonar_status;
    if(connected && input_enabled && mouse_enabled && mouse_output==0 && mouse_movement==1)
        text+=relative_mouse.Active()?L" | mouse captured (exit shortcut or F8 releases)":L" | click image to capture mouse";
    if(connected && input_enabled && mouse_enabled && mouse_output==1)
        text+=local_touch_capture.Active()?L" | local touch: cross cursor (exit shortcut or F8 releases)":L" | click image to activate local touch";
    text += L" | F5: connect | F8: input | F9: settings | F11: fullscreen"; SetWindowTextW(window, text.c_str());
    if (session_menu) {
        CheckMenuItem(session_menu, MenuInput, MF_BYCOMMAND | (input_enabled ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(session_menu, MenuFullscreen, MF_BYCOMMAND | (fullscreen ? MF_CHECKED : MF_UNCHECKED));
        EnableMenuItem(session_menu, MenuDisconnect, MF_BYCOMMAND | (session_started || discovery_waiting ? MF_ENABLED : MF_GRAYED));
        EnableMenuItem(session_menu, MenuTextInput, MF_BYCOMMAND | (connected && input_enabled && keyboard_enabled ? MF_ENABLED : MF_GRAYED));
        CheckMenuRadioItem(session_menu,MenuMouseLocal,MenuMouseDirect,
            mouse_enabled && mouse_output==1?MenuMouseLocal:mouse_enabled && mouse_output==0?
                (mouse_movement==1?MenuMouseRemote:MenuMouseDirect):0,MF_BYCOMMAND);
        CheckMenuRadioItem(session_menu,MenuTouchscreen,MenuTouchOff,
            touch_enabled?(physical_touch_output==1?MenuTouchpad:MenuTouchscreen):MenuTouchOff,MF_BYCOMMAND);
        CheckMenuItem(session_menu,MenuKeyboard,MF_BYCOMMAND|(keyboard_enabled?MF_CHECKED:MF_UNCHECKED));
        EnableMenuItem(session_menu,MenuStreamPause,MF_BYCOMMAND|(connected && stream.supported?MF_ENABLED:MF_GRAYED));
        CheckMenuItem(session_menu,MenuStreamPause,MF_BYCOMMAND|(stream_manual_pause?MF_CHECKED:MF_UNCHECKED));
    }
    if (!frame.pixels) InvalidateRect(window, nullptr, TRUE);
}
void update_menu(HWND window) {
    SetMenu(window, menu_enabled && !fullscreen && menu_state==1 ? session_menu : nullptr); DrawMenuBar(window);
    const bool compact=menu_enabled && (fullscreen || menu_state==0);
    if(compact && !compact_menu_button) {
        compact_menu_button=CreateWindowW(L"BUTTON",L"Menu",WS_CHILD|WS_TABSTOP|BS_PUSHBUTTON,
            0,0,0,0,window,HMENU(MenuCompactPopup),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(compact_menu_button,WM_SETFONT,WPARAM(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    }
    if(compact_menu_button) {
        const UINT dpi=GetDpiForWindow(window);const int unit=dpi?dpi:96;
        SetWindowPos(compact_menu_button,HWND_TOP,MulDiv(10,unit,96),MulDiv(10,unit,96),
            MulDiv(72,unit,96),MulDiv(28,unit,96),SWP_NOACTIVATE|(compact?SWP_SHOWWINDOW:SWP_HIDEWINDOW));
    }
    if(session_menu) ModifyMenuW(session_menu,MenuPresentation,MF_BYCOMMAND|MF_STRING,MenuPresentation,
        menu_state==0?L"Expand menu":L"Compact menu");
    if(session_menu) EnableMenuItem(session_menu,MenuPresentation,MF_BYCOMMAND|(fullscreen?MF_GRAYED:MF_ENABLED));
}
void read_input_settings() {
    auto_orientation=viewer_settings->GetInteger("display.autorotation")!=0;
    const auto shortcut_error = sd::viewer::ViewerShortcutError(*viewer_settings);
    if (!shortcut_error.empty()) throw std::runtime_error(shortcut_error);
    mouse_enabled = viewer_settings->GetInteger("input.mouse") != 0;
    keyboard_enabled = viewer_settings->GetInteger("input.keyboard") != 0;
    const bool next_android=viewer_settings->GetInteger("client.os")==3;
    if(next_android && !android_keyboard_profile) sd_client_reset_android_keyboard(client);
    android_keyboard_profile=next_android;
    pen_enabled = viewer_settings->GetInteger("input.pen") != 0;
    touch_packets_enabled = viewer_settings->GetInteger("input.touch") != 0;
    mouse_output = uint32_t(viewer_settings->GetInteger("input.mouse_output"));
    mouse_movement = uint32_t(viewer_settings->GetInteger("input.mouse_movement"));
    physical_touch_output = uint32_t(viewer_settings->GetInteger("input.touch_output"));
    touch_enabled = touch_packets_enabled && physical_touch_output != 2;
    menu_enabled = viewer_settings->GetInteger("ui.display_menu") != 0;
    menu_state = uint32_t(viewer_settings->GetInteger("ui.menu_state"));
    show_fps = viewer_settings->GetInteger("render.show_fps") != 0;
    active_auto_timeout = uint32_t(viewer_settings->GetInteger("connection.auto_connect_timeout_ms"));
    active_reconnect_timeout = uint32_t(viewer_settings->GetInteger("connection.reconnect_timeout_ms"));
    const auto letter = sd::viewer::SettingsWide(viewer_settings->GetText("ui.remote_exit_letter"));
    if (!remote_exit.Configure(viewer_settings->GetInteger("ui.remote_exit_ctrl") != 0,
        viewer_settings->GetInteger("ui.remote_exit_alt") != 0, viewer_settings->GetInteger("ui.remote_exit_shift") != 0,
        letter.empty() ? 0 : letter.front())) throw std::runtime_error("The remote input release shortcut requires an alphabetic letter or an empty value.");
}
void edit_profile(HWND window) {
    if (!viewer_settings) return;
    try {
        if (!sd::viewer::EditSettings(window, *viewer_settings, session_started || discovery_waiting,encoding_attention?"display.codec":nullptr)) return;
        encoding_attention=false;
        title(window, L"profile updated - press F5 to connect with it");
    } catch (const std::exception& e) { MessageBoxA(window, e.what(), "Settings error", MB_OK | MB_ICONERROR); }
}
void release_mouse() {
    if (client && mouse_down && connected && mouse_output == 0) {
        if(mouse_movement==0 && last_absolute_position.mapped)
            sd_client_send_mouse_absolute(client,last_absolute_position.x,last_absolute_position.y,
                last_absolute_position.surface_width,last_absolute_position.surface_height,0,mouse_down<<1);
        else sd_client_send_mouse(client,0,0,0,mouse_down<<1,0);
    }
    mouse_touch.CaptureLost(send_mouse_touch);
    mouse_down = 0;
}
sd_result send_key(uint32_t key,uint32_t scan,uint32_t flags) {
    if(!android_keyboard_profile) return sd_client_send_keyboard(client,key,scan,flags);
    const auto mapped=sd::viewer::AndroidKey(key,scan,(flags&SD_KEY_EXTENDED)!=0);
    if(!mapped) return SD_IGNORED;
    sd_android_key_event event{sizeof(event),uint32_t((flags&SD_KEY_UP)?SD_ANDROID_KEY_UP:SD_ANDROID_KEY_DOWN),
        *mapped,uint32_t((GetKeyState(VK_CAPITAL)&1)!=0)};
    return sd_client_send_android_key(client,&event);
}
void release_input() {
    if (!client) return;
    remote_exit.Reset(); mouse_touch.Reset(send_mouse_touch);
    sonar.ResetInput();
    local_touch_capture.End();relative_mouse.End();activation_mouse_up=0;
    touch_batches.Reset(touch_timestamp(), send_touch);
    pointers.Reset([](uint32_t type, uint32_t event, const sd_pointer_sample& sample) {
        return connected ? sd_client_send_pointer(client, type, event, &sample) : SD_IGNORED;
    });
    // Includes the touch timer/map after an outside move or a failed capture.
    sd_client_reset_pointer_output(client);
    for (uint32_t i = 0; i < pressed.size(); ++i) {
        if (pressed[i] && connected) send_key(i,scans[i],key_flags[i]|SD_KEY_UP);
        pressed[i] = false;
    }
    release_mouse();
}
void clear_frame(HWND window) {
    gdi_surface.Reset();last_remote_orientation=0;
    frame = {}; have_pointer_position = false;
    last_absolute_position={};
    fps_since = GetTickCount64(); fps_frames = fps_sequence = 0; fps_value = 0;
    InvalidateRect(window, nullptr, TRUE);
}
void disconnect_session(HWND window, const wchar_t* status = L"disconnected") {
    encoding_attention=false;
    stream_manual_pause=false;
    sd::viewer::CloseTextInput();
    discovery_generation.Cancel(); discovery_target.reset(); discovery_waiting = false;
    active_address.clear();
    retry_timers.Clear();
    release_input(); ReleaseCapture(); connected = false; session_started = false;
    sonar.Close();
    clear_frame(window);
    const auto result = sd_client_disconnect(client);
    connection_state = SD_STATE_DISCONNECTED;
    title(window, result < 0 ? L"disconnect failed" : status);
}
void handle_result(HWND window, sd_result result,const sd_event* event=nullptr) {
    if (result >= 0 && result != SD_DISCONNECTED) return;
    sd::viewer::CloseTextInput();
    connected = false; session_started = false;
    retry_timers.Clear();
    clear_frame(window); release_input(); ReleaseCapture();
    sonar.Deactivate();
    connection_state = SD_STATE_DISCONNECTED;
    const auto message=sd::viewer::IsServerDisconnect(event)?sd::viewer::ServerDisconnectMessage(event->value):
        sd::viewer::IsClientDisconnect(event)?sd::viewer::ClientDisconnectMessage(event->value):
        std::wstring(result==SD_DISCONNECTED?L"disconnected":L"stream error");
    encoding_attention=sd::viewer::DisconnectSuggestsEncoding(event);
    title(window,message.c_str());
    if (result < 0 && !sd::viewer::IsServerDisconnect(event) && !sd::viewer::IsClientDisconnect(event))
        MessageBoxA(window, sd_client_last_error(client), "SDK error", MB_OK | MB_ICONERROR);
}
void handle_gpu_result(HWND window, sd_result result) {
    if (result == SD_GPU_ERROR) {
        gpu_enabled = false; sd_client_renderer_detach(client);
        const auto current = status_text; title(window, current.c_str()); InvalidateRect(window,nullptr,FALSE);
    } else handle_result(window,result);
}
void sync_stream_state(HWND window,bool lifecycle_event=false) {
    if(!client || !connected) return;
    sd_stream_control_state state{sizeof(state)};
    if(sd_client_get_stream_control_state(client,&state)!=SD_OK || !state.supported) return;
    const uint32_t desired=stream_manual_pause||stream_minimized?SD_STREAM_PAUSE:SD_STREAM_PLAY;
    // A newly announced stream already plays. Do not invent an initial PLAY;
    // explicit restore/resume follows the APK Activity.onResume command.
    if(state.requested_state!=desired && (desired==SD_STREAM_PAUSE || state.requested_state!=SD_STREAM_UNDEFINED || lifecycle_event))
        handle_result(window,sd_client_set_stream_state(client,desired));
    const auto current=status_text;title(window,current.c_str());
}
void refresh_orientation(HWND window,bool force) {
    if(!client) return;
    uint32_t desired=1;
    if(auto_orientation) {
        const auto monitor=MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST);
        if(force || monitor!=orientation_monitor) {
            const auto queried=sd::viewer::DisplayOrientation(window);
            if(!queried) { orientation_monitor=nullptr;return; }
            desired=*queried;orientation_monitor=monitor;
        } else desired=view_orientation;
    } else orientation_monitor=nullptr;
    if(desired!=view_orientation) {
        release_input();ReleaseCapture();view_orientation=desired;
        handle_gpu_result(window,sd_client_view_set_orientation(client,desired));gpu_viewport(window);
        InvalidateRect(window,nullptr,FALSE);
    }
    if(connected && auto_orientation && (connection_state==SD_STATE_DISPLAY_ON || connection_state==SD_STATE_FULLSCREEN) &&
       last_remote_orientation!=desired) {
        const auto result=sd_client_set_rotation(client,desired);
        if(result==SD_OK) last_remote_orientation=desired;else handle_result(window,result);
    }
}
void select_mouse_mode(HWND window,uint32_t mode) {
    release_input();ReleaseCapture();
    if(sd_client_set_input_enabled(client,SD_INPUT_MOUSE,1)!=SD_OK) return;
    if(mode==1) {
        // Touch Off applies to physical input. Mouse-as-touch still needs the
        // SDK's touch packet capability, and always uses absolute touch mode.
        if(sd_client_set_input_enabled(client,SD_INPUT_TOUCH,1)!=SD_OK || sd_client_set_touch_mode(client,1)!=SD_OK) return;
        if(!touch_packets_enabled) viewer_settings->SetInteger("input.touch_output",2);
        viewer_settings->SetInteger("input.touch",1);
    }
    viewer_settings->SetInteger("input.mouse",1);viewer_settings->SetInteger("input.mouse_output",mode==1?1:0);
    if(mode!=1) viewer_settings->SetInteger("input.mouse_movement",mode==0?1:0);
    read_input_settings();input_enabled=true;
    if(mode==0 && connected && frame.pixels) { SetFocus(window);relative_mouse.Begin(window); }
    if(mode==1 && connected && frame.pixels) { SetFocus(window);local_touch_capture.Begin(window); }
    const auto current=status_text;title(window,current.c_str());
}
void select_touch_mode(HWND window,uint32_t mode) {
    release_input();ReleaseCapture();
    if(mode<2) {
        if(sd_client_set_input_enabled(client,SD_INPUT_TOUCH,1)!=SD_OK || sd_client_set_touch_input_type(client,mode)!=SD_OK) return;
        viewer_settings->SetInteger("input.touch",1);viewer_settings->SetInteger("input.touch_type",mode);
    }
    viewer_settings->SetInteger("input.touch_output",mode);read_input_settings();
    const auto current=status_text;title(window,current.c_str());
}
void apply_connection_state(HWND window, uint32_t state) {
    const bool changed = state != connection_state;
    if (sd::viewer::ConnectionHasTransport(connection_state) && state == SD_STATE_RECONNECTING)
        retry_timers.Schedule(active_reconnect_timeout, sd::viewer::ConnectionUptime());
    connection_state = state;
    const bool transport = sd::viewer::ConnectionHasTransport(state);
    if (!transport) {
        sd::viewer::CloseTextInput();
        connected = false; clear_frame(window); release_input(); ReleaseCapture();
        sonar.Deactivate();
        if (state == SD_STATE_DISCONNECTED) { session_started = false; retry_timers.Clear(); title(window, L"disconnected - F5 retries, F9 edits settings"); }
        else if (state == SD_STATE_CONNECTING || state == SD_STATE_RECONNECTING)
            title(window, ever_connected ? L"reconnecting - Shift+F5 cancels" : L"connecting - Shift+F5 cancels");
        else title(window, L"disconnecting");
    } else {
        connected = true; ever_connected = true;
        if(changed) {
            if(state==SD_STATE_DISPLAY_ON) last_remote_orientation=0;
            refresh_orientation(window,true);
        }
        if(changed) sync_stream_state(window);
        if (changed) title(window, state == SD_STATE_DISPLAY_OFF ? L"connected - waiting for display" : (input_enabled ? L"display + input" : L"display only"));
        if (fullscreen && state == SD_STATE_DISPLAY_ON) sd_client_set_fullscreen(client, 1);
        else if (!fullscreen && state == SD_STATE_FULLSCREEN) sd_client_set_fullscreen(client, 0);
    }
}
void start_target(HWND window, const std::string& address, uint16_t port, bool automatic) {
    active_address=address;active_port=port;
    clear_frame(window); connected = false; ever_connected = false;
    const auto result = sd_client_start(client, address.c_str(), port, automatic ? 1 : 0);
    if (result < 0) { handle_result(window, result); return; }
    const auto retries = sd_client_set_auto_reconnect(client, 1);
    if (retries != SD_OK) { handle_result(window, retries); return; }
    session_started = true;
    if (automatic) retry_timers.Schedule(active_auto_timeout, sd::viewer::ConnectionUptime());
    uint32_t state = SD_STATE_CONNECTING;
    const auto queried = sd_client_get_state(client, &state);
    if (queried < 0) handle_result(window, queried); else apply_connection_state(window, state);
}
void connect_profile(HWND window, bool automatic = false) {
    disconnect_session(window, L"preparing connection");
    try {
        const auto result = viewer_settings->Apply(client);
        if (result != SD_OK) throw std::runtime_error(sd_client_last_error(client));
        read_input_settings(); update_menu(window);
        sonar.Configure(*viewer_settings,client);
        refresh_orientation(window,true);
        const auto touch_mode = sd_client_set_touch_mode(client, 1);
        if (touch_mode != SD_OK) throw std::runtime_error(sd_client_last_error(client));
        const auto target = sd::viewer::MakeConnectionTarget(*viewer_settings, explicit_address, requested_port);
        if (target.mode == sd::viewer::ConnectionTargetMode::Direct) { start_target(window, target.target, target.port, automatic); return; }
        desired_generation = discovery_generation.Begin(); discovery_target = target; discovery_waiting = true;
        discovery_automatic = automatic;
        discovery_next = GetTickCount64();
        title(window, L"discovering server - Shift+F5 cancels");
    } catch (const std::exception& e) {
        title(window, L"connection profile needs attention - F9 opens settings");
        MessageBoxA(window, e.what(), "Connection settings", MB_OK | MB_ICONWARNING);
    }
}
void poll_discovery(HWND window) {
    if (discovery_job.valid() && discovery_job.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        const uint64_t completed = job_generation;
        try {
            auto reply = discovery_job.get();
            if (discovery_waiting && discovery_target && discovery_generation.Current(completed)) {
                if (reply.result < 0) throw std::runtime_error("Local discovery failed. Check the network, or disable discovery to connect directly by IP.");
                const auto match = sd::viewer::MatchDiscovery(*discovery_target, reply.hosts.data(), reply.hosts.size());
                if (match) {
                    discovery_waiting = false; discovery_target.reset(); discovery_generation.Cancel();
                    start_target(window, match->ipv4, match->port, discovery_automatic);
                }
            }
        } catch (const std::exception& e) {
            if (discovery_generation.Current(completed)) {
                discovery_waiting = false; discovery_target.reset(); discovery_generation.Cancel();
                title(window, L"discovery needs attention - F9 opens settings");
                MessageBoxA(window, e.what(), "Server discovery", MB_OK | MB_ICONWARNING);
            }
        }
    }
    if (!discovery_waiting || !discovery_target) return;
    const uint64_t now = GetTickCount64();
    if (discovery_job.valid() || now < discovery_next) return;
    const auto target = *discovery_target;
    // Begin is used only when a new target is requested. A job retains that
    // generation so cancellation cannot connect a stale discovery reply.
    job_generation = desired_generation;
    discovery_next = now + 250;
    try { discovery_job = std::async(std::launch::async, [target] {
        DiscoveryReply reply{SD_OK, std::vector<sd_host>(256)};
        for (auto& host : reply.hosts) host.struct_size = sizeof(host);
        uint32_t count = 0;
        reply.result = sd_discover(target.mode == sd::viewer::ConnectionTargetMode::DiscoverIp ? target.target.c_str() : nullptr,
            target.port, 250, reply.hosts.data(), uint32_t(reply.hosts.size()), &count);
        reply.hosts.resize(count); return reply;
    }); } catch (const std::exception& e) {
        discovery_generation.Cancel(); discovery_waiting = false; discovery_target.reset();
        title(window, L"cannot start discovery"); MessageBoxA(window, e.what(), "Server discovery", MB_OK | MB_ICONWARNING);
    }
}
void set_fullscreen(HWND window, bool enabled) {
    if (enabled == fullscreen) return;
    release_input(); ReleaseCapture();
    if (enabled) {
        normal_placement.length = sizeof(normal_placement); GetWindowPlacement(window, &normal_placement);
        normal_style = GetWindowLongPtrW(window, GWL_STYLE);
        MONITORINFO monitor{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
        fullscreen = true; SetWindowLongPtrW(window, GWL_STYLE, normal_style & ~WS_OVERLAPPEDWINDOW);
        update_menu(window);
        SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
            monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    } else {
        fullscreen = false; SetWindowLongPtrW(window, GWL_STYLE, normal_style); update_menu(window);
        SetWindowPlacement(window, &normal_placement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    }
    sd_client_set_fullscreen(client, enabled ? 1 : 0);
    const auto current = status_text; title(window, current.c_str());
}
void gpu_viewport(HWND window) {
    if ((!gpu_requested && view_orientation==1) || !client) return;
    RECT area{}; GetClientRect(window,&area);
    const UINT dpi = GetDpiForWindow(window) ? GetDpiForWindow(window) : 96;
    sd_viewport viewport{}; sd_viewport_init(&viewport);
    viewport.dpi = float(dpi); viewport.composition_scale_x = viewport.composition_scale_y = float(dpi)/96;
    viewport.logical_width = float(area.right)*96/dpi; viewport.logical_height = float(area.bottom)*96/dpi;
    handle_gpu_result(window,sd_client_set_viewport(client,&viewport));
}
bool pointer_position(HWND window, LPARAM lp, bool clamp, LONG& x, LONG& y) {
    if (gpu_enabled || view_orientation!=1) {
        const UINT dpi = GetDpiForWindow(window) ? GetDpiForWindow(window) : 96;
        sd_pointer_sample sample{}; sd_pointer_sample_init(&sample);
        sample.x = float(GET_X_LPARAM(lp))*96/dpi; sample.y = float(GET_Y_LPARAM(lp))*96/dpi;
        if (sd_client_map_pointer_sample(client,&sample) != SD_OK || !sample.mapped) {
            if (!clamp || !have_pointer_position) return false;
            x=last_pointer_x; y=last_pointer_y; return true;
        }
        x=std::clamp<LONG>(LONG(sample.x),0,LONG(sample.surface_width-1));
        y=std::clamp<LONG>(LONG(sample.y),0,LONG(sample.surface_height-1));
    } else {
        const auto rect=image_rect(window); POINT point{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};
        if (rect.right<=rect.left || rect.bottom<=rect.top || (!clamp && !PtInRect(&rect,point))) return false;
        x=std::clamp<LONG>(LONG((int64_t(point.x-rect.left)*frame.width)/(rect.right-rect.left)),0,LONG(frame.width-1));
        y=std::clamp<LONG>(LONG((int64_t(point.y-rect.top)*frame.height)/(rect.bottom-rect.top)),0,LONG(frame.height-1));
    }
    last_pointer_x=x; last_pointer_y=y; have_pointer_position=true; return true;
}
void mouse(HWND window, LPARAM lp, uint32_t buttons, int32_t data = 0, bool clamp = false) {
    if (!connected || !input_enabled || !mouse_enabled || mouse_output != 0 || mouse_movement!=0 || !frame.pixels) return;
    if(GetFocus()!=window && GetCapture()!=window) return;
    sd_pointer_sample sample{};sd_pointer_sample_init(&sample);
    sample.x=float(GET_X_LPARAM(lp));sample.y=float(GET_Y_LPARAM(lp));map_pointer(window,sample);
    if(!sample.mapped) {
        if(!clamp || !last_absolute_position.mapped) return;
        sample=last_absolute_position;
    }
    last_absolute_position=sample;
    handle_result(window,sd_client_send_mouse_absolute(client,sample.x,sample.y,sample.surface_width,sample.surface_height,data,buttons));
}
void sync_direct_mouse(HWND window) {
    if(mouse_movement!=0 || mouse_output!=0 || !connected || !frame.pixels) return;
    POINT point{};if(!GetCursorPos(&point)) return;
    if(compact_menu_button && (GetWindowLongPtrW(compact_menu_button,GWL_STYLE)&WS_VISIBLE)) {
        RECT button{};GetWindowRect(compact_menu_button,&button);if(PtInRect(&button,point)) return;
    }
    if(ScreenToClient(window,&point)) mouse(window,MAKELPARAM(point.x,point.y),SD_MOUSE_MOVE);
}
void raw_mouse(HWND window, HRAWINPUT handle) {
    if (!connected || !input_enabled || !mouse_enabled || mouse_output != 0 ||
        mouse_movement!=1 || !frame.pixels || GetFocus() != window || !relative_mouse.Active()) return;
    RAWINPUT input{}; UINT bytes = sizeof(input);
    const UINT read = GetRawInputData(handle, RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER));
    if (read == UINT(-1) || read < offsetof(RAWINPUT, data) + sizeof(RAWMOUSE) ||
        input.header.dwType != RIM_TYPEMOUSE) return;
    const auto& motion = input.data.mouse;
    // Absolute HID samples are not relative deltas. Touch/pen promotion must
    // not duplicate the dedicated pointer route. Captured raw input owns buttons.
    if ((motion.usFlags & MOUSE_MOVE_ABSOLUTE) || sd::viewer::PromotedMouse(motion.ulExtraInformation)) return;
    uint32_t buttons=(motion.usButtonFlags&0x3f)<<1;
    // A legacy activation UP can be the last event from the old registration.
    // If no raw UP followed it, the next real press starts a fresh pair.
    for(const uint32_t down:{SD_MOUSE_LEFT_DOWN,SD_MOUSE_RIGHT_DOWN,SD_MOUSE_MIDDLE_DOWN})
        if(buttons&down) activation_mouse_up&=~(down<<1);
    const auto suppressed=buttons&activation_mouse_up;
    buttons&=~suppressed;activation_mouse_up&=~suppressed;
    if(motion.lLastX || motion.lLastY) buttons|=SD_MOUSE_MOVE;
    int32_t wheel=0;
    if(motion.usButtonFlags&RI_MOUSE_WHEEL) { buttons|=SD_MOUSE_WHEEL;wheel=int16_t(motion.usButtonData); }
    if(motion.usButtonFlags&RI_MOUSE_HWHEEL) { buttons|=SD_MOUSE_HWHEEL;wheel=int16_t(motion.usButtonData); }
    if(!buttons) return;
    for(const uint32_t down:{SD_MOUSE_LEFT_DOWN,SD_MOUSE_RIGHT_DOWN,SD_MOUSE_MIDDLE_DOWN}) {
        if(buttons&down) mouse_down|=down;
        if(buttons&(down<<1)) mouse_down&=~down;
    }
    handle_result(window,sd_client_send_mouse(client,motion.lLastX,motion.lLastY,wheel,buttons,0));
}
void text_input(HWND window) {
    if(!connected || !input_enabled || !keyboard_enabled) return;
    release_input();ReleaseCapture();
    try {
        sd::viewer::ShowTextInput(window,[](const std::u16string& text,uint32_t erase) {
            if(!connected || !input_enabled || !keyboard_enabled) return sd_result(SD_DISCONNECTED);
            const std::vector<uint16_t> units(text.begin(),text.end());
            return sd_client_send_text_utf16(client,units.data(),uint32_t(units.size()),erase);
        },!viewer_settings || viewer_settings->GetInteger("ui.softinput_message")!=0);
    } catch(const std::exception&) { MessageBoxW(window,L"Cannot open text input",L"Viewer",MB_OK|MB_ICONERROR); }
}
sd_result send_pointer(uint32_t type, uint32_t event, const sd_pointer_sample& sample) {
    return connected ? sd_client_send_pointer(client, type, event, &sample) : SD_IGNORED;
}
void map_pointer(HWND window, sd_pointer_sample& sample) {
    sample.mapped = 0;
    if (!frame.pixels) return;
    if (gpu_enabled || view_orientation!=1) {
        sd::viewer::ToDips(sample, GetDpiForWindow(window));
        if (sd_client_map_pointer_sample(client, &sample) != SD_OK) sample.mapped = 0;
    } else sd::viewer::MapGdi(sample, image_rect(window), frame.width, frame.height);
}
void emulate_mouse(HWND window, LPARAM lp, uint32_t event) {
    sd_pointer_sample sample{}; sd_pointer_sample_init(&sample);
    sample.device_type = SD_POINTER_MOUSE; sample.pointer_id = 0;
    sample.x = float(GET_X_LPARAM(lp)); sample.y = float(GET_Y_LPARAM(lp));
    // Either physical button holds one finger; the native adapter receives
    // its original primary-button events after this frontend normalization.
    sample.buttons = mouse_down & mouse_touch_buttons ? SD_POINTER_BUTTON_LEFT : 0;
    sample.update_kind = event == SD_POINTER_PRESSED ? SD_POINTER_LEFT_PRESSED : event == SD_POINTER_RELEASED ? SD_POINTER_LEFT_RELEASED : SD_POINTER_UPDATE_OTHER;
    sample.capture_succeeded = GetCapture() == window; sample.in_range = 1;
    map_pointer(window, sample);
    const bool enabled = connected && input_enabled && mouse_enabled && touch_packets_enabled && mouse_output == 1 && frame.pixels;
    handle_result(window, mouse_touch.Process(event, sample, enabled, send_mouse_touch));
}
bool pointer(HWND window, UINT message, WPARAM wp) {
    const UINT32 id = GET_POINTERID_WPARAM(wp);
    if (message == WM_POINTERCAPTURECHANGED) {
        // No new GetPointerInfo payload is attached to capture loss.
        handle_result(window, touch_batches.CaptureLost(id, touch_timestamp(), send_touch));
        if(sonar.Tracked(id)) handle_result(window,sonar.ResetInput());
        handle_result(window, pointers.CaptureLost(id, send_pointer));
        return true;
    }
    POINTER_INFO info{};
    if (!GetPointerInfo(id, &info)) {
        if(sonar.Tracked(id)) handle_result(window,sonar.ResetInput());
        if (touch_batches.Tracked(id)) handle_result(window, touch_batches.CaptureLost(id, touch_timestamp(), send_touch));
        if (pointers.Tracked(id)) handle_result(window, pointers.CaptureLost(id, send_pointer));
        POINTER_INPUT_TYPE type{};
        // Consume a failed query too: allowing only one event from this
        // sequence through DefWindowProc can generate an extra mouse gesture.
        return !GetPointerType(id, &type) || type == PT_TOUCH || type == PT_PEN;
    }
    if (info.pointerType != PT_TOUCH && info.pointerType != PT_PEN) return false;
    POINTER_PEN_INFO pen{};
    const bool has_pen = info.pointerType == PT_PEN && GetPointerPenInfo(id, &pen);
    POINT position = info.ptPixelLocation;
    if (!ScreenToClient(window, &position)) {
        if(sonar.Tracked(id)) handle_result(window,sonar.ResetInput());
        handle_result(window, touch_batches.CaptureLost(id, touch_timestamp(), send_touch));
        handle_result(window, pointers.CaptureLost(id, send_pointer)); return true;
    }
    auto sample = sd::viewer::Sample(info, position, has_pen ? &pen : nullptr);
    const bool in_contact = (info.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
    const bool canceled = (info.pointerFlags & POINTER_FLAG_CANCELED) != 0;
    // Win32 captures contact pointers implicitly to WM_POINTERDOWN's target.
    // SetCapture/GetCapture belong to the independent mouse stream.
    sample.capture_succeeded = message == WM_POINTERDOWN && info.hwndTarget == window && in_contact && !canceled;
    map_pointer(window, sample);
    const bool enabled = connected && input_enabled && application_active && frame.pixels &&
        (info.pointerType == PT_PEN ? pen_enabled : (touch_enabled || sonar.Available()));
    if (message == WM_POINTERDOWN && enabled && sample.mapped) SetFocus(window);
    if(info.pointerType==PT_TOUCH && sonar.Available()) {
        if(!enabled) { handle_result(window,sonar.ResetInput());return true; }
        POINTER_TOUCH_INFO touch{};const auto detail=GetPointerTouchInfo(id,&touch)?&touch:nullptr;
        sd::viewer::SonarTouchSample input;
        input.windows_id=id;input.point=sd::viewer::SonarTouchMetrics(float(position.x),float(position.y),detail,sonar.MaximumContactDiameter());
        input.mapped=sample.mapped!=0;input.captured=sample.capture_succeeded!=0;input.contact=in_contact;input.canceled=canceled;
        handle_result(window,sonar.Touch(message,input,pen_enabled,touch_enabled,touch_timestamp(),frame.width,frame.height,[&](sd_motion_point& point) {
            sd_pointer_sample mapped{};sd_pointer_sample_init(&mapped);mapped.x=point.axes[0];mapped.y=point.axes[1];
            map_pointer(window,mapped);point.axes[0]=mapped.x;point.axes[1]=mapped.y;return mapped.mapped!=0;
        }));
    } else if (info.pointerType == PT_TOUCH)
        handle_result(window, touch_batches.Process(message, sample, in_contact, canceled, enabled, touch_timestamp(), send_touch));
    else handle_result(window, pointers.Process(message, sample, in_contact, canceled, enabled, send_pointer));
    return true;
}
LRESULT CALLBACK procedure(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        // Foreground-only registration retains normal window/menu mouse input.
        const RAWINPUTDEVICE device{0x01, 0x02, 0, window};
        return RegisterRawInputDevices(&device, 1, sizeof(device)) ? 0 : -1;
    }
    case WM_INPUT:
        if (GET_RAWINPUT_CODE_WPARAM(wp) == RIM_INPUT) raw_mouse(window, reinterpret_cast<HRAWINPUT>(lp));
        // Windows requires default processing to clean up foreground RAWINPUT.
        return DefWindowProcW(window, msg, wp, lp);
    case WM_TIMER:
        poll_discovery(window);
        for (int i = 0; i < 8 && session_started; ++i) {
            // poll may release the SDK snapshot on a reconnect or error. Never
            // leave the borrowed pointer reachable by a nested paint callback.
            const sd_frame previous = frame; frame = {};
            sd_event event{}; event.struct_size = sizeof(event);
            const auto result = sd_client_poll(client, 0, &event);
            handle_result(window, result,&event);
            if (result < 0 || result == SD_DISCONNECTED) break;
            uint32_t state = SD_STATE_DISCONNECTED;
            const auto queried = sd_client_get_state(client, &state);
            if (queried != SD_OK) { handle_result(window, queried); break; }
            apply_connection_state(window, state);
            if (connected && event.type != SD_EVENT_FRAME) frame = previous;
            if (result != SD_OK) break;
            if (event.type == SD_EVENT_RENDERER_ERROR) handle_gpu_result(window,SD_GPU_ERROR);
            if (event.type == SD_EVENT_CONTROL && event.wire_type==4) sync_stream_state(window);
            if (event.type == SD_EVENT_FRAME) {
                frame.struct_size = sizeof(frame);
                const auto fetched = sd_client_get_frame(client, &frame);
                handle_result(window, fetched);
                if (fetched != SD_OK) { frame = {}; break; }
                if (!previous.pixels || previous.width != frame.width || previous.height != frame.height)
                    sync_direct_mouse(window);
                const auto now = GetTickCount64();
                fps_frames += fps_sequence && frame.sequence >= fps_sequence ? frame.sequence - fps_sequence : 1;
                fps_sequence = frame.sequence;
                if (now - fps_since >= 1000) {
                    fps_value = double(fps_frames) * 1000 / double(now - fps_since); fps_frames = 0; fps_since = now;
                }
                title(window, input_enabled ? L"display + input" : L"display only");
                if (!gpu_enabled) InvalidateRect(window, nullptr, FALSE);
            } else if (event.type == SD_EVENT_CONNECTION_WARNING) {
                wchar_t slow[128]{}; swprintf_s(slow, L"waiting for server data (%u ms) - connection may be slow", event.value);
                title(window, slow);
            }
        }
        {
            const auto previous=sonar.Status();
            if(connected && frame.pixels && !stream_minimized && application_active && !touch_batches.Count()) sonar.Activate();
            sonar.Poll();
            if(previous!=sonar.Status()) { const auto current=status_text;title(window,current.c_str()); }
        }
        // APK Handler timers remain queued after a successful connection. A
        // TIMER has an effect only while retrying; manual disconnect clears all.
        if (retry_timers.ConsumeBefore(sd::viewer::ConnectionUptime()) &&
            (connection_state == SD_STATE_CONNECTING || connection_state == SD_STATE_RECONNECTING))
            disconnect_session(window, L"automatic connection timeout - F5 retries");
        if (show_fps && connected && GetTickCount64() - fps_since >= 1000) {
            const auto now = GetTickCount64(); fps_value = double(fps_frames) * 1000 / double(now - fps_since);
            fps_frames = 0; fps_since = now;
            const auto current = status_text; title(window, current.c_str());
        }
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        if (gpu_enabled && connected && frame.pixels) {
            handle_gpu_result(window,sd_client_renderer_present(client));
            if (gpu_enabled) { EndPaint(window,&paint); return 0; }
        }
        RECT area{}; GetClientRect(window, &area); FillRect(dc, &area, (HBRUSH)GetStockObject(BLACK_BRUSH));
        if (frame.pixels) {
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = LONG(frame.width); info.bmiHeader.biHeight = -LONG(frame.height);
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            const auto r = image_rect(window);
            SetStretchBltMode(dc, COLORONCOLOR);
            if(view_orientation==1)
                StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0,
                    int(frame.width), int(frame.height), frame.pixels, &info, DIB_RGB_COLORS, SRCCOPY);
            else gdi_surface.Draw(dc,r,frame,view_orientation);
        } else {
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(225,225,225));
            std::wstring text = status_text + L"\n\nF5 connects or retries. Shift+F5 disconnects or cancels.\nF9 edits the profile. F11 toggles fullscreen.";
            RECT text_area = area; InflateRect(&text_area, -24, -24);
            DrawTextW(dc, text.c_str(), -1, &text_area, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        }
        EndPaint(window, &paint); return 0;
    }
    case WM_SIZE: {
        refresh_orientation(window);
        const bool minimized=wp==SIZE_MINIMIZED;
        if(minimized!=stream_minimized) {
            stream_minimized=minimized;
            if(minimized) { release_input();ReleaseCapture();sonar.Deactivate(); }
            sync_stream_state(window,true);
        }
        local_touch_capture.UpdateBounds();relative_mouse.UpdateBounds();gpu_viewport(window);sync_direct_mouse(window);InvalidateRect(window, nullptr, FALSE);return 0;
    }
    case WM_MOVE: refresh_orientation(window);local_touch_capture.UpdateBounds();relative_mouse.UpdateBounds();sync_direct_mouse(window);return 0;
    case WM_DISPLAYCHANGE: refresh_orientation(window,true);local_touch_capture.UpdateBounds();relative_mouse.UpdateBounds();gpu_viewport(window);sync_direct_mouse(window);InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP: release_input();ReleaseCapture();return 0;
    case WM_SETCURSOR:
        if(relative_mouse.Active()) { SetCursor(nullptr);return TRUE; }
        if(local_touch_capture.Active() && LOWORD(lp)==HTCLIENT) { SetCursor(LoadCursorW(nullptr,IDC_CROSS));return TRUE; }
        break;
    case WM_DPICHANGED: {
        const RECT* area=reinterpret_cast<const RECT*>(lp);
        SetWindowPos(window,nullptr,area->left,area->top,area->right-area->left,area->bottom-area->top,SWP_NOZORDER|SWP_NOACTIVATE);
        update_menu(window);gpu_viewport(window);sync_direct_mouse(window); return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case MenuMouseLocal:select_mouse_mode(window,1);return 0;
        case MenuMouseRemote:select_mouse_mode(window,0);return 0;
        case MenuMouseDirect:select_mouse_mode(window,3);sync_direct_mouse(window);return 0;
        case MenuStreamPause: {
            sd_stream_control_state state{sizeof(state)};
            if(connected && sd_client_get_stream_control_state(client,&state)==SD_OK && state.supported) {
                release_input();ReleaseCapture();stream_manual_pause=!stream_manual_pause;sync_stream_state(window,true);
            }
            return 0;
        }
        case MenuTouchscreen:select_touch_mode(window,0);return 0;
        case MenuTouchpad:select_touch_mode(window,1);return 0;
        case MenuTouchOff:select_touch_mode(window,2);return 0;
        case MenuKeyboard:
            release_input();ReleaseCapture();
            if(sd_client_set_input_enabled(client,SD_INPUT_KEYBOARD,!keyboard_enabled)==SD_OK) {
                viewer_settings->SetInteger("input.keyboard",!keyboard_enabled);read_input_settings();
                const auto current=status_text;title(window,current.c_str());
            }
            return 0;
        case MenuPresentation:
            if(fullscreen) return 0; // The menu bar is restored on leaving fullscreen.
            release_input();ReleaseCapture();
            menu_state=menu_state==0?1:0;
            if(viewer_settings) viewer_settings->SetInteger("ui.menu_state",menu_state);
            update_menu(window);gpu_viewport(window);return 0;
        case MenuCompactPopup: {
            release_input();ReleaseCapture();
            const auto popup=session_menu?GetSubMenu(session_menu,0):nullptr;
            if(popup && compact_menu_button) {
                RECT button{};GetWindowRect(compact_menu_button,&button);
                const UINT selected=TrackPopupMenuEx(popup,TPM_RETURNCMD|TPM_RIGHTBUTTON,
                    button.left,button.bottom,window,nullptr);
                if(selected) SendMessageW(window,WM_COMMAND,selected,0);
            }
            return 0;
        }
        case MenuConnect: connect_profile(window); return 0;
        case MenuDisconnect: disconnect_session(window); return 0;
        case MenuSettings: release_input(); ReleaseCapture(); edit_profile(window); return 0;
        case MenuTextInput: text_input(window);return 0;
        case MenuInput:
            release_input(); input_enabled = !input_enabled; ReleaseCapture();
            if(!input_enabled) sd::viewer::CloseTextInput();
            if (connected) title(window, input_enabled ? L"display + input" : L"display only");
            else { const auto current = status_text; title(window, current.c_str()); } return 0;
        case MenuFullscreen: set_fullscreen(window, !fullscreen); return 0;
        case MenuExit: DestroyWindow(window); return 0;
        } break;
    case WM_POINTERDOWN: case WM_POINTERUPDATE: case WM_POINTERUP:
    case WM_POINTERENTER: case WM_POINTERLEAVE: case WM_POINTERCAPTURECHANGED:
    case WM_POINTERWHEEL: case WM_POINTERHWHEEL:
        if (pointer(window, msg, wp)) return 0;
        break;
    case WM_MOUSEMOVE:
        if (!sd::viewer::PromotedMouse(ULONG_PTR(GetMessageExtraInfo()))) {
            if (mouse_output == 1) emulate_mouse(window, lp, SD_POINTER_MOVED);
            else if(mouse_movement==0) mouse(window,lp,SD_MOUSE_MOVE,0,mouse_down!=0);
        }
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        if(mouse_output==0 && mouse_movement==1 && relative_mouse.Active()) return 0;
        if (sd::viewer::PromotedMouse(ULONG_PTR(GetMessageExtraInfo())) ||
            !connected || !input_enabled || !mouse_enabled || mouse_output == 2 || !frame.pixels) return 0;
        if (mouse_output == 1 && (msg == WM_MBUTTONDOWN || !touch_packets_enabled)) return 0;
        const bool touch_held=mouse_output==1 && (mouse_down&mouse_touch_buttons)!=0;
        LONG x,y; if (!pointer_position(window,lp,false,x,y) && !(touch_held && GetCapture()==window)) return 0;
        if(mouse_output==1 && !local_touch_capture.Active()) {
            SetFocus(window);if(!local_touch_capture.Begin(window)) return 0;
        }
        if(mouse_output==0 && mouse_movement==1 && !relative_mouse.Active()) {
            SetFocus(window);
            if(relative_mouse.Begin(window)) {
                const uint32_t down=msg==WM_LBUTTONDOWN?SD_MOUSE_LEFT_DOWN:msg==WM_RBUTTONDOWN?SD_MOUSE_RIGHT_DOWN:SD_MOUSE_MIDDLE_DOWN;
                activation_mouse_up=down<<1;
                title(window,L"display + input");
            }
            return 0; // Activating capture must not click an unrelated host location.
        }
        SetFocus(window); if(GetCapture()!=window) SetCapture(window);
        uint32_t button = msg == WM_LBUTTONDOWN ? SD_MOUSE_LEFT_DOWN : msg == WM_RBUTTONDOWN ? SD_MOUSE_RIGHT_DOWN : SD_MOUSE_MIDDLE_DOWN;
        mouse_down |= button;
        if (mouse_output == 1) emulate_mouse(window, lp, touch_held?SD_POINTER_MOVED:SD_POINTER_PRESSED); else mouse(window, lp, button);
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        // RAWINPUT owns buttons while captured, including an activation UP
        // already queued before NOLEGACY was registered. Do not release capture.
        if(mouse_output==0 && mouse_movement==1 && relative_mouse.Active()) return 0;
        if (sd::viewer::PromotedMouse(ULONG_PTR(GetMessageExtraInfo()))) return 0;
        if (mouse_output == 1 && msg == WM_MBUTTONUP) return 0;
        uint32_t button = msg == WM_LBUTTONUP ? SD_MOUSE_LEFT_DOWN : msg == WM_RBUTTONUP ? SD_MOUSE_RIGHT_DOWN : SD_MOUSE_MIDDLE_DOWN;
        if(mouse_output==1) {
            const bool held=(mouse_down&button)!=0;mouse_down&=~button;
            if(held) emulate_mouse(window,lp,(mouse_down&mouse_touch_buttons)?SD_POINTER_MOVED:SD_POINTER_RELEASED);
            if(!(mouse_down&mouse_touch_buttons) && !local_touch_capture.Active()) ReleaseCapture();
            return 0;
        }
        if (mouse_down & button) {
            mouse(window, lp, button << 1, 0, true);
        }
        mouse_down &= ~button; if (!mouse_down) ReleaseCapture(); return 0;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        if (sd::viewer::PromotedMouse(ULONG_PTR(GetMessageExtraInfo()))) return 0;
        POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}; ScreenToClient(window, &p);
        mouse(window, MAKELPARAM(p.x, p.y), msg == WM_MOUSEWHEEL ? SD_MOUSE_WHEEL : SD_MOUSE_HWHEEL,
            GET_WHEEL_DELTA_WPARAM(wp)); return 0;
    }
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        const bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
        uint32_t chord_key = uint32_t(wp);
        if (wp == VK_CONTROL) chord_key = lp & (1LL << 24) ? VK_RCONTROL : VK_LCONTROL;
        else if (wp == VK_MENU) chord_key = lp & (1LL << 24) ? VK_RMENU : VK_LMENU;
        else if (wp == VK_SHIFT) {
            const auto side = MapVirtualKeyW(uint32_t((lp >> 16) & 255), MAPVK_VSC_TO_VK_EX);
            if (side == VK_LSHIFT || side == VK_RSHIFT) chord_key = side;
        }
        if (connected && input_enabled && remote_exit.OnKey(chord_key, !up)) {
            release_input(); ReleaseCapture(); input_enabled = false;
            sd::viewer::CloseTextInput();
            title(window, L"display only - input released by shortcut; F8 enables input"); return 0;
        }
        if (wp == VK_F5) {
            if (!up && !(lp & (1LL << 30))) {
                if (GetKeyState(VK_SHIFT) < 0) disconnect_session(window); else connect_profile(window);
            }
            return 0;
        }
        if (wp == VK_F11) {
            if (!up && !(lp & (1LL << 30))) set_fullscreen(window, !fullscreen);
            return 0;
        }
        if (wp == VK_ESCAPE && !connected && (session_started || discovery_waiting)) {
            if (!up) disconnect_session(window); return 0;
        }
        if (wp == VK_F9) {
            if (!up && !(lp & (1LL << 30))) { release_input(); ReleaseCapture(); edit_profile(window); }
            return 0;
        }
        if (wp == VK_F10) {
            if(!up && !(lp & (1LL<<30))) text_input(window);
            return 0;
        }
        if (wp == VK_F8) {
            if (!up && !(lp & (1LL << 30))) {
                release_input(); input_enabled = !input_enabled; ReleaseCapture();
                if(!input_enabled) sd::viewer::CloseTextInput();
                if (connected) title(window, input_enabled ? L"display + input" : L"display only");
                else { const auto current = status_text; title(window, current.c_str()); }
            }
            return 0;
        }
        sd_result sonar_key=SD_IGNORED;
        if(connected && input_enabled && wp==VK_MEDIA_PLAY_PAUSE && sonar.MediaKey(up,&sonar_key)) {
            handle_result(window,sonar_key);return 0;
        }
        if (connected && input_enabled && keyboard_enabled && wp < pressed.size()) {
            // CapsLock DOWN is ignored by the APK path. Focus cleanup must not
            // invent its toggle-on-UP; the next real DOWN observes source lock state.
            pressed[wp] = !up && !(android_keyboard_profile && wp==VK_CAPITAL);
            uint32_t flags = up ? SD_KEY_UP : 0;
            if (lp & (1LL << 24)) flags |= SD_KEY_EXTENDED;
            scans[wp] = uint32_t((lp >> 16) & 255); key_flags[wp] = flags & SD_KEY_EXTENDED;
            handle_result(window, send_key(uint32_t(wp),uint32_t((lp>>16)&255),flags));
        }
        return 0;
    }
    case WM_APPCOMMAND:
        if(connected && input_enabled && GET_APPCOMMAND_LPARAM(lp)==APPCOMMAND_MEDIA_PLAY_PAUSE) {
            sd_result down=SD_IGNORED,up=SD_IGNORED;
            if(sonar.MediaKey(false,&down)) { sonar.MediaKey(true,&up);handle_result(window,down<0?down:up);return TRUE; }
        }
        break;
    case WM_ACTIVATEAPP:
        application_active=wp!=0;
        if(!application_active) { release_input();ReleaseCapture();sonar.Deactivate(); }
        return 0;
    case WM_KILLFOCUS: release_input(); ReleaseCapture(); return 0;
    case WM_CANCELMODE: release_input(); ReleaseCapture(); return 0;
    case WM_CAPTURECHANGED: local_touch_capture.End();relative_mouse.End();activation_mouse_up=0;release_mouse();return 0;
    case WM_DESTROY:
        compact_menu_button=nullptr; // Owned child is destroyed with this window.
        { const RAWINPUTDEVICE device{0x01, 0x02, RIDEV_REMOVE, nullptr};
          RegisterRawInputDevices(&device, 1, sizeof(device)); }
        disconnect_session(window);
        if (gpu_requested) { sd_client_renderer_detach(client); gpu_enabled=false; }
        if (session_menu) { SetMenu(window, nullptr); DestroyMenu(session_menu); session_menu = nullptr; }
        KillTimer(window, 1); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool websocket=false, edit_settings=false; uint32_t gpu_driver=SD_RENDERER_HARDWARE; int first=1;
    std::wstring settings_path;
    while (argv && first<argc) {
        if (wcscmp(argv[first],L"--websocket")==0) websocket=true;
        else if (wcscmp(argv[first],L"--gpu")==0) gpu_requested=true;
        else if (wcscmp(argv[first],L"--warp")==0) { gpu_requested=true; gpu_driver=SD_RENDERER_WARP; }
        else if (wcscmp(argv[first],L"--settings")==0 && first+1<argc) settings_path=argv[++first];
        else if (wcscmp(argv[first],L"--edit-settings")==0) edit_settings=true;
        else if (wcscmp(argv[first],L"--auto-connect")==0) {} // Compatibility alias; no-address launch is automatic now.
        else break;
        ++first;
    }
    if (!argv) {
        MessageBoxW(nullptr, L"Cannot read the viewer command line.", L"spacedesk SDK viewer", MB_OK | MB_ICONERROR);
        if (argv) LocalFree(argv); return 2;
    }
    const bool has_host = first < argc;
    std::wstring wide_ip = has_host ? argv[first] : L""; const auto port = argc > first + 1 ? wcstoul(argv[first + 1], nullptr, 10) : 28252;
    LocalFree(argv);
    if (!port || port > 65535) return 2;
    requested_port = uint16_t(port);
    // WM_POINTER positions are physical screen pixels. Both GDI and GPU paths
    // must opt out of DPI virtualization before creating any viewer windows.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (edit_settings) {
        try {
            viewer_settings = std::make_unique<sd::SASettings>();
            if (!settings_path.empty()) {
                sd::viewer::SettingsDraft loaded(*viewer_settings); loaded.Load(settings_path);
                viewer_settings->Deserialize(loaded.Value().Serialize(SD_SETTINGS_INCLUDE_SECRETS));
            }
            if (websocket) { viewer_settings->SetInteger("connection.websocket", 1); viewer_settings->SetInteger("client.type", SD_CLIENT_WEB_BROWSER); }
            if (!sd::viewer::EditSettings(nullptr, *viewer_settings)) return 2;
            if (!has_host) return 0;
        } catch (const std::exception& e) { MessageBoxA(nullptr, e.what(), "Settings error", MB_OK | MB_ICONERROR); return 1; }
    }
    std::string ip;
    for (const auto ch : wide_ip) {
        if (ch > 127) return 2;
        ip.push_back(static_cast<char>(ch));
    }
    explicit_address = ip;
    sd_config config{}; sd_config_init(&config);
    auto result = sd_client_create(&config, &client);
    if (result != SD_OK) { MessageBoxW(nullptr, L"Cannot initialize SDK", L"SDK error", MB_OK); return 1; }
    if(viewer_settings || !settings_path.empty()) {
        try {
            if (!viewer_settings) {
                viewer_settings = std::make_unique<sd::SASettings>();
                sd::viewer::SettingsDraft loaded(*viewer_settings); loaded.Load(settings_path);
                viewer_settings->Deserialize(loaded.Value().Serialize(SD_SETTINGS_INCLUDE_SECRETS));
            }
            result=viewer_settings->Apply(client);
            if(result!=SD_OK) throw std::runtime_error(sd_client_last_error(client));
        } catch(const std::exception& e) {
            MessageBoxA(nullptr,e.what(),"Settings error",MB_OK);sd_client_destroy(client);return 1;
        }
    }
    try {
        if (!viewer_settings) { viewer_settings = std::make_unique<sd::SASettings>(); viewer_settings->ReadFrom(client); }
        read_input_settings();
    } catch (const std::exception& e) {
        MessageBoxA(nullptr,e.what(),"Viewer input settings",MB_OK);sd_client_destroy(client);return 1;
    }
    if (websocket && sd_client_set_transport(client, SD_TRANSPORT_WEBSOCKET) != SD_OK) {
        sd_client_destroy(client); return 1;
    }
    viewer_settings->ReadFrom(client);
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = procedure;
    cls.lpszClassName = L"SpacedeskSdkViewer"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    HWND window = CreateWindowExW(0, cls.lpszClassName, L"spacedesk SDK - waiting for first frame", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 760, nullptr, nullptr, instance, nullptr);
    if (!window) { sd_client_destroy(client); return 1; }
    session_menu = CreateMenu(); HMENU actions = CreatePopupMenu();
    AppendMenuW(actions, MF_STRING, MenuConnect, L"Connect / reconnect\tF5");
    AppendMenuW(actions, MF_STRING, MenuDisconnect, L"Disconnect / cancel\tShift+F5");
    AppendMenuW(actions, MF_STRING, MenuSettings, L"Settings...\tF9");
    AppendMenuW(actions, MF_STRING|MF_GRAYED, MenuStreamPause, L"Request stream pause");
    AppendMenuW(actions, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(actions, MF_STRING, MenuKeyboard, L"Keyboard");
    AppendMenuW(actions, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(actions, MF_STRING, MenuMouseLocal, L"Mouse local (use as touch)");
    AppendMenuW(actions, MF_STRING, MenuMouseRemote, L"Mouse remote");
    AppendMenuW(actions, MF_STRING, MenuMouseDirect, L"Mouse direct (absolute)");
    AppendMenuW(actions, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(actions, MF_STRING, MenuTouchscreen, L"Touchscreen (absolute)");
    AppendMenuW(actions, MF_STRING, MenuTouchpad, L"Touchpad / Trackpad (relative)");
    AppendMenuW(actions, MF_STRING, MenuTouchOff, L"Touch Off");
    AppendMenuW(actions, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(actions, MF_STRING, MenuInput, L"Send input\tF8");
    AppendMenuW(actions, MF_STRING, MenuTextInput, L"Text input...\tF10");
    AppendMenuW(actions, MF_STRING, MenuPresentation, L"Compact menu");
    AppendMenuW(actions, MF_STRING, MenuFullscreen, L"Fullscreen\tF11");
    AppendMenuW(actions, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(actions, MF_STRING, MenuExit, L"Exit");
    AppendMenuW(session_menu, MF_POPUP, UINT_PTR(actions), L"Functionality");
    update_menu(window);
    if (gpu_requested) {
        gpu_viewport(window);
        sd_renderer_config render{}; sd_renderer_config_init(&render);
        render.driver=gpu_driver; render.target=SD_RENDERER_HWND; render.target_object=window;
        gpu_enabled=sd_client_renderer_attach(client,&render)==SD_OK;
        if (!gpu_enabled) sd_client_renderer_detach(client);
    }
    ShowWindow(window, show); SetTimer(window, 1, 8, nullptr);
    // No address is the normal discovery launch, including a plain double-click.
    connect_profile(window, explicit_address.empty());
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if(!sd::viewer::TextInputMessage(&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    // Only the bounded discovery worker outlives cancellation. Discard it before
    // unloading the DLL; no stale result is ever passed to start_target.
    if (discovery_job.valid()) discovery_job.wait();
    sd_client_disconnect(client); sd_client_destroy(client); client = nullptr; return 0;
}
