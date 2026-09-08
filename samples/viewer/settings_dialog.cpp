#include "settings_dialog.h"
#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>

namespace sd::viewer {
std::wstring SettingsWide(const std::string& text) {
    if (text.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!count) throw std::runtime_error("Invalid UTF-8 text");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), count);
    return result;
}
std::string SettingsUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) throw std::runtime_error("Invalid Unicode text");
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
namespace {
struct Presentation { const char* key; const wchar_t* label; const wchar_t* choices; };
const Presentation presentation[] = {
    {"client.name", L"Client name", L""}, {"client.id", L"Client identity (GUID)", L""},
    {"client.type", L"Client type", L"0=Display monitor|1=Web browser|2=Reserved (unavailable)|3=Audio only (not a display)"},
    {"client.os", L"Declared operating system", L"0=Unknown|1=Windows native|2=Windows UWP|3=Android|4=iOS"},
    {"client.license_type", L"Declared license type", L"0=None|1=Basic|2=Advanced|3=Android business|4=Windows UWP|5=iOS"},
    {"state.license_information", L"Stored Android billing state", L""},
    {"protocol.minor", L"Protocol minor version", L"7=4.7|8=4.8"},
    {"connection.transport", L"Connection transport", L"0=Network|1=USB (adapter required)|2=Wi-Fi Direct (unavailable)"},
    {"connection.websocket", L"Use WebSocket", L""},
    {"connection.connect_timeout_ms", L"Socket connect timeout (ms)", L""},
    {"connection.io_timeout_ms", L"Socket I/O timeout (ms)", L""},
    {"connection.auto_connect", L"Automatic connection target", L"0=Discover available server|1=IP address|2=Host name"},
    {"connection.ip", L"Server IP address", L""}, {"connection.hostname", L"Server host name", L""},
    {"connection.auto_connect_timeout_ms", L"Automatic connection timeout (ms)", L""},
    {"connection.reconnect_timeout_ms", L"Reconnect timeout (ms)", L""},
    {"connection.watcher_poll_ms", L"Connection watcher interval (ms)", L""},
    {"connection.watcher_timeout_ms", L"Connection watcher timeout (ms)", L""},
    {"connection.discovery", L"Discover servers on local network", L""},
    {"connection.recent_addresses", L"Recent server addresses", L""},
    {"security.encryption", L"Encryption", L"0=None|1=TLS 1.2|2=TLS 1.3"},
    {"security.psk_identity", L"TLS PSK identity", L""},
    {"security.psk_key", L"TLS PSK key (hex)", L""},
    {"security.password", L"Server PIN (8 hex digits)", L""},
    {"display.compression", L"Compression", L"0=Off (raw)|3=Enabled"},
    {"display.codec", L"Preferred video codec", L"0=Uncompressed|3=MJPEG|4=H.264|5=Choose best"},
    {"display.chroma", L"Chroma subsampling", L"0=4:4:4|1=4:2:2|2=4:2:0|3=Grayscale|4=4:4:0|5=4:1:1"},
    {"display.quality", L"Image quality (0-100)", L""},
    {"display.fps", L"Frame rate limit (FPS)", L""},
    {"display.bitrate_kbps", L"Requested bitrate (kbps)", L""},
    {"display.resolutions", L"Resolutions (WIDTHxHEIGHT;...)", L""},
    {"display.session_type", L"Display session type", L""},
    {"display.resolution_selection", L"Resolution selection preference", L""},
    {"display.autorotation", L"Automatic rotation", L""},
    {"render.bitmap_format", L"Android bitmap format preference", L"0=RGB565|1=ARGB8888"},
    {"render.show_fps", L"Show frame rate", L""},
    {"input.mouse", L"Enable mouse input", L""}, {"input.keyboard", L"Enable keyboard input", L""},
    {"input.mouse_output", L"Mouse output", L"0=Mouse|1=Emulated touchscreen|2=Disabled"},
    {"input.mouse_movement", L"Mouse movement", L"0=Direct absolute|1=Remote captured"},
    {"input.mouse_desktop_rect", L"Remote desktop rectangle", L""},
    {"input.mouse_target_rect", L"Remote target monitor rectangle", L""},
    {"input.mouse_target_rotation", L"Remote target rotation", L"1=0 degrees|2=90 degrees|3=180 degrees|4=270 degrees"},
    {"input.touch", L"Enable touch input", L""},
    {"input.touch_type", L"APK touch protocol mode", L"0=Touchscreen|1=Touchpad"},
    {"input.touch_output", L"Physical touch output", L"0=Touchscreen|1=Touchpad|2=Disabled"},
    {"input.pen", L"Enable pen input", L""}, {"input.sonar_pen", L"Enable SonarPen", L""},
    {"input.sonar_palm_rejection", L"SonarPen palm rejection", L""},
    {"sonar.capture_endpoint_id", L"SonarPen capture device", L""},
    {"sonar.render_endpoint_id", L"SonarPen output device", L""},
    {"sonar.device_kind", L"SonarPen connection type", L"0=Analog audio connector|1=SP1 USB adapter"},
    {"sonar.read_timeout_ms", L"SonarPen audio read timeout (ms)", L""},
    {"sonar.application_id", L"Profile receiver name", L""},
    {"sonar.store_path", L"SonarPen preferences file", L""},
    {"sonar.calibration_provider_path", L"Initial calibration profile file", L""},
    {"sonar.compatibility_provider_path", L"Initial compatibility value file", L""},
    {"sonar.contact_size_max_px", L"Maximum contact diameter (pixels)", L""},
    {"audio.enabled", L"Receive server audio", L""}, {"audio.playback", L"Play audio locally", L""},
    {"audio.capture_queue_ms", L"Audio capture queue (ms; 0 disables)", L""},
    {"audio.probe_server", L"Probe server audio capability", L""},
    {"audio.activation_timeout_ms", L"Audio activation timeout (ms)", L""},
    {"audio.endpoint_id", L"Audio endpoint ID (empty = default)", L""},
    {"ui.display_menu", L"Show display menu preference", L""},
    {"ui.menu_state", L"Session menu presentation", L"0=Compact|1=Expanded"},
    {"ui.startup_message", L"Show startup message preference", L""},
    {"ui.softinput_message", L"Show soft keyboard message preference", L""},
    {"ui.remote_exit_ctrl", L"Release remote input: Ctrl", L""},
    {"ui.remote_exit_alt", L"Release remote input: Alt", L""},
    {"ui.remote_exit_shift", L"Release remote input: Shift", L""},
    {"ui.remote_exit_letter", L"Release remote input: optional letter", L""}
};
std::wstring group(const std::string& key) {
    if(key.rfind("sonar.",0)==0 || key.rfind("input.sonar_",0)==0) return L"SonarPen";
    const auto prefix = key.substr(0, key.find('.'));
    if (prefix == "client" || prefix == "protocol" || prefix == "state") return L"Client identity";
    if (prefix == "connection") return L"Connection";
    if (prefix == "security") return L"Security";
    if (prefix == "display" || prefix == "render") return L"Display";
    if (prefix == "input") return L"Input";
    if (prefix == "audio") return L"Audio";
    return L"Interface";
}
std::string read_file(const std::wstring& path) {
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open settings profile");
    const auto size = file.tellg();
    if (size < 0 || size > 1024 * 1024) throw std::runtime_error("Settings profile exceeds 1 MiB");
    file.seekg(0);
    std::string data(size_t(size), '\0');
    if (size && !file.read(data.data(), std::streamsize(size))) throw std::runtime_error("Cannot read settings profile");
    return data;
}
void normalize_viewer_compression(SASettings& settings) {
    // Imported SDK profiles can expose independent wire fields. The viewer
    // presents one compression choice, matching the recovered Windows builder.
    if (!settings.GetInteger("display.codec") || !settings.GetInteger("display.compression")) {
        settings.SetInteger("display.codec", 0); settings.SetInteger("display.compression", 0);
    } else settings.SetInteger("display.compression", 3);
}
std::wstring normalized_exit_letter(const std::wstring& text) {
    if (text.empty()) return {};
    wchar_t first = text.front();
    if (!IsCharAlphaW(first)) throw std::runtime_error("The remote input release shortcut accepts an alphabetic letter, or an empty value.");
    CharUpperBuffW(&first, 1); return std::wstring(1, first);
}
}
std::vector<SettingField> SettingsFields() {
    std::vector<SettingField> fields;
    for (uint32_t i = 0; i < sd_settings_count(); ++i) {
        SettingField field; field.info.struct_size = sizeof(field.info);
        if (sd_settings_describe(i, &field.info) != SD_OK) throw std::runtime_error("Cannot read settings schema");
        field.group = group(field.info.key); field.label = SettingsWide(field.info.key);
        for (const auto& p : presentation) if (std::string(field.info.key) == p.key) {
            field.label = p.label; std::wstring choices = p.choices; size_t start = 0;
            while (start < choices.size()) {
                auto end = choices.find(L'|', start); auto entry = choices.substr(start, end - start);
                auto equal = entry.find(L'=');
                field.choices.push_back({std::stoll(entry.substr(0, equal)), entry.substr(equal + 1)});
                if (end == std::wstring::npos) break;
                start = end + 1;
            }
            break;
        }
        // Unknown future enum fields still use schema values instead of guessing.
        if (field.choices.empty() && *field.info.allowed_values) {
            std::string values = field.info.allowed_values; size_t start = 0;
            while (start < values.size()) {
                const auto end = values.find(',', start);
                const auto value = std::stoll(values.substr(start, end - start));
                field.choices.push_back({value, std::to_wstring(value)});
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }
        fields.push_back(std::move(field));
    }
    return fields;
}
std::vector<AudioEndpointChoice> AudioEndpointChoices(uint32_t flow) {
    uint32_t count=0;if(sd_sonar_enumerate_audio_devices(nullptr,0,&count)!=SD_OK) return {};
    for(unsigned attempt=0;attempt<3;++attempt) {
        if(count>1024) return {};
        std::vector<sd_sonar_audio_device> devices(count);for(auto& device:devices) device.struct_size=sizeof(device);
        const auto result=sd_sonar_enumerate_audio_devices(devices.data(),uint32_t(devices.size()),&count);
        if(result==SD_PENDING || (result==SD_OK && count>devices.size())) continue;if(result!=SD_OK) return {};
        std::vector<AudioEndpointChoice> choices;
        for(uint32_t i=0;i<count;++i) if(devices[i].flow==flow) {
            auto label=SettingsWide(devices[i].name);if(devices[i].is_default) label+=L" (default)";
            choices.push_back({devices[i].endpoint_id,std::move(label)});
        }
        return choices;
    }
    return {};
}
std::wstring SettingSupport(const SettingField& field) {
    const std::string key = field.info.key;
    if(key=="input.sonar_pen") return L"Uses the selected audio devices for SonarPen capture and power tone while this app is active and the display is connected. Minimize, app deactivation or disconnect stops audio. Requires a physical SonarPen and touchscreen.";
    if(key=="input.sonar_palm_rejection") return L"Enables palm classification from pressure and available touch-size measurements. A maximum contact diameter enables size comparisons; zero uses pressure measurements only.";
    if(key=="sonar.capture_endpoint_id") return L"Choose the audio input connected to SonarPen, or enter a saved endpoint ID. Selecting a device here does not start recording.";
    if(key=="sonar.render_endpoint_id") return L"Choose the SonarPen audio output used for the power tone. This is separate from server audio playback.";
    if(key=="sonar.device_kind") return L"Select the actual physical connection: analog audio connector or SP1 USB adapter. The device name alone does not establish compatibility.";
    if(key=="sonar.contact_size_max_px") return L"Maximum detectable contact diameter for this touchscreen, in screen pixels. Zero leaves size unavailable. This Windows calibration is separate from Android's device-specific size scale.";
    if(key=="sonar.store_path") return L"Optional absolute filename for saved Sonar profiles and preferences. Empty uses memory only. Changes apply on the next connection.";
    if(key=="sonar.calibration_provider_path") return L"Optional absolute UTF-8 profile filename, read when Sonar initializes. Missing file preserves the saved profile; an empty file clears it.";
    if(key=="sonar.compatibility_provider_path") return L"Optional absolute UTF-8 compatibility value filename. This value is preserved for profile exchange; it does not bypass calibration in this source version.";
    if(key=="sonar.application_id") return L"Exact recipient name for profile messages. Use the same name when reopening the preferences file.";
    if(key=="sonar.read_timeout_ms") return L"Maximum wait for an audio block. Stop interrupts the wait. The default is 250 ms.";
    if (key == "ui.display_menu") return L"Shows this viewer's Session menu. F9 still opens settings when the menu is hidden. Fullscreen hides the menu until F11 restores the window.";
    if (key == "render.show_fps") return L"Shows received frame rate in this viewer's title bar. This is measured from delivered SDK frame sequences, not the configured FPS limit.";
    if (key == "connection.auto_connect") return L"Without a command-line address, discover the first available server or use the configured IP/name. Host names match discovery replies exactly; DNS is not used.";
    if (key == "connection.discovery") return L"Controls discovery for configured IP/name targets. Disable for direct IPv4. With no target, the viewer always searches for an available server.";
    if (key == "connection.auto_connect_timeout_ms") return L"Budget for automatic connection attempts, starting after discovery has selected a server. Zero is unlimited. Manual F5 does not use this initial budget.";
    if (key == "connection.reconnect_timeout_ms") return L"Budget after an established connection is lost. Zero is unlimited. This does not insert a delay between retries; the viewer owns the timer.";
    if (key == "input.mouse_output") return L"Routes mouse input as mouse packets or touchscreen taps/swipes. Left or right button holds one touch contact; releasing the last held button ends it. Disabled stops this physical route.";
    if (key == "input.mouse_movement") return L"Direct aligns to the visible pointer using absolute coordinates. Remote hides and captures the local cursor for relative movement; the exit shortcut or F8 releases it.";
    if (key == "input.mouse_desktop_rect" || key == "input.mouse_target_rect")
        return L"Leave both empty when mirroring the primary monitor. For an extended monitor enter remote left,top,width,height for the complete desktop and this monitor. Negative positions are allowed.";
    if (key == "input.touch_output") return L"Routes physical touch as touchscreen or server-interpreted touchpad. Disabled stops physical touch only; it does not disable mouse-to-touch emulation.";
    if (key == "input.touch_type") return L"APK touch packet mode. Choosing touchscreen/touchpad synchronizes the physical touch output unless that route is disabled.";
    if (key == "ui.menu_state") return L"Compact shows a small Menu button over the display; Expanded uses the normal menu bar. Fullscreen uses the compact button. Display menu must be enabled.";
    if (key.find("ui.remote_exit_") == 0) return L"Select at least two keys. Hold every selected modifier and optional letter, then release all selected keys to stop forwarding input. The display stays connected. F8 enables input again. The first alphabetic UTF-16 letter is uppercased.";
    if (key == "display.bitrate_kbps")
        return L"No direct bitrate command is established for this protocol profile. Use image quality and FPS. A nonzero bitrate request is stored but cannot be applied.";
    if (key == "display.autorotation")
        return L"Follows the orientation of the local monitor containing this window and requests the matching remote display orientation. Resizing the window keeps the selected angle. Image and pointer mapping rotate together.";
    if (field.info.application & SD_SETTING_PENDING)
        return L"Not implemented. The value is stored in profiles, but the SDK rejects a non-default value when applying settings.";
    if (field.info.application & SD_SETTING_FRONTEND)
        return L"Application preference. Stored by the SDK; its effect requires support in the client application. Saving it does not implement the corresponding APK feature.";
    if (key == "connection.transport") return L"Network is supported by this viewer. USB profiles can be saved, but connecting needs a client with a USB peripheral adapter. Wi-Fi Direct is unavailable in the source APK.";
    if (key == "client.type") return L"Display monitor and web browser are supported. WebSocket requires web browser type. Audio uses a separate connection; it is not a display profile.";
    if (key == "client.license_type") return L"Protocol declaration only. This setting does not purchase, grant or validate a license.";
    if (key == "security.password") return L"Eight hexadecimal PIN digits. Used only with TLS. Leave PSK key empty to derive it from the PIN. Saved credentials are plain text when explicitly included.";
    if (key == "security.psk_key") return L"Full hexadecimal pre-shared key; takes precedence over the PIN. Saved credentials are plain text when explicitly included.";
    if (key == "display.resolutions") return L"One to nine resolutions, separated by semicolons; for example 1920x1080;1280x720. First entry is preferred.";
    if (key == "display.quality") return L"Codec quality preference. A value of zero becomes 1 on the wire, matching the APK.";
    return L"SDK setting. Changes take effect when the profile is applied before connecting.";
}
SettingsDraft::SettingsDraft(const SASettings& source) : value_(std::make_unique<SASettings>()) {
    value_->Deserialize(source.Serialize(SD_SETTINGS_INCLUDE_SECRETS));
    normalize_viewer_compression(*value_);
}
void SettingsDraft::Set(const SettingField& field, const std::wstring& text) {
    try {
        if (field.info.type == SD_SETTING_TEXT) value_->SetText(field.info.key,
            SettingsUtf8(std::string(field.info.key) == "ui.remote_exit_letter" ? normalized_exit_letter(text) : text));
        else {
            const auto ascii = SettingsUtf8(text); int64_t number = 0;
            const auto parsed = std::from_chars(ascii.data(), ascii.data() + ascii.size(), number);
            if (ascii.empty() || parsed.ec != std::errc() || parsed.ptr != ascii.data() + ascii.size())
                throw std::runtime_error("Enter a whole decimal number");
            value_->SetInteger(field.info.key, number);
            // Windows VIEWER's identification builder (0x435450) couples the
            // preference codec to the base compression. This is viewer policy;
            // the general SDK setters keep both wire fields independently usable.
            if (std::string(field.info.key) == "display.codec") value_->SetInteger("display.compression", number ? 3 : 0);
            else if (std::string(field.info.key) == "display.compression") {
                if (!number) value_->SetInteger("display.codec", 0);
                else {
                    value_->SetInteger("display.compression", 3);
                    if (!value_->GetInteger("display.codec")) value_->SetInteger("display.codec", 5);
                }
            }
            else if (std::string(field.info.key) == "input.touch_output" && number < 2)
                value_->SetInteger("input.touch_type", number);
            else if (std::string(field.info.key) == "input.touch_type" && value_->GetInteger("input.touch_output") != 2)
                value_->SetInteger("input.touch_output", number);
        }
    } catch (const SettingsError&) {
        throw std::runtime_error(std::string(field.info.key) + ": value must fit the displayed type, range and allowed values");
    }
}
void SettingsDraft::Defaults() { value_ = std::make_unique<SASettings>(); }
void SettingsDraft::Load(const std::wstring& path) {
    auto next = std::make_unique<SASettings>();
    const auto document = read_file(path); next->Deserialize(document);
    normalize_viewer_compression(*next);
    next->SetText("ui.remote_exit_letter", SettingsUtf8(normalized_exit_letter(SettingsWide(next->GetText("ui.remote_exit_letter")))));
    if (document.find("\ninput.touch_output\t") == std::string::npos) next->SetInteger("input.touch_output", next->GetInteger("input.touch_type"));
    else if (next->GetInteger("input.touch_output") < 2) next->SetInteger("input.touch_type", next->GetInteger("input.touch_output"));
    value_.swap(next); // Omitted credentials never leak in from the previous profile.
}
void SettingsDraft::Save(const std::wstring& path, bool credentials) const {
    const auto shortcut_error = ViewerShortcutError(*value_);
    if (!shortcut_error.empty()) throw std::runtime_error(shortcut_error);
    const auto data = value_->Serialize(credentials ? SD_SETTINGS_INCLUDE_SECRETS : 0);
    const std::filesystem::path target = std::filesystem::absolute(std::filesystem::path(path));
    // CREATE_NEW prevents overwriting an unrelated temporary file. Replace stays
    // in the same directory so a failed save preserves the previous profile.
    std::wstring temporary; HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned retry = 0; retry != 100 && file == INVALID_HANDLE_VALUE; ++retry) {
        temporary = target.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(retry);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) throw std::runtime_error("Cannot create settings profile in this folder");
    }
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create temporary settings profile");
    DWORD written = 0;
    const bool saved = WriteFile(file, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!saved || !MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str()); throw std::runtime_error("Cannot save settings profile; previous file was preserved");
    }
}
std::string ViewerShortcutError(const SASettings& settings) {
    try {
        const auto letter = normalized_exit_letter(SettingsWide(settings.GetText("ui.remote_exit_letter")));
        const auto count = settings.GetInteger("ui.remote_exit_ctrl") + settings.GetInteger("ui.remote_exit_alt") +
            settings.GetInteger("ui.remote_exit_shift") + (letter.empty() ? 0 : 1);
        if (count < 2) return "Select at least two keys for the remote input release shortcut (Ctrl, Alt, Shift and/or a letter).";
        return {};
    }
    catch (const std::exception& error) { return error.what(); }
}
std::string SettingsDraft::Validate() const {
    const auto shortcut_error = ViewerShortcutError(*value_);
    if (!shortcut_error.empty()) return shortcut_error;
    sd_config config{}; sd_config_init(&config); sd_client* temporary = nullptr;
    const auto created = sd_client_create(&config, &temporary);
    if (created != SD_OK) return "Cannot create temporary client for validation";
    const auto applied = value_->Apply(temporary);
    std::string error = applied == SD_OK ? "" : sd_client_last_error(temporary);
    if (applied != SD_OK && error.empty()) error = "The SDK rejected this profile";
    sd_client_destroy(temporary); return error;
}
void SettingsDraft::Commit(SASettings& destination) const {
    const auto error = Validate();
    if (!error.empty()) throw std::runtime_error(error);
    destination.Deserialize(value_->Serialize(SD_SETTINGS_INCLUDE_SECRETS));
}

namespace {
constexpr wchar_t dialog_class[] = L"SpacedeskSdkSettingsEditor";
enum : int { Groups = 101, Fields, TextValue, BoolValue, EnumValue, LoadProfile, SaveProfile, Defaults, Credentials, Status, DeviceValue };
struct Dialog {
    SASettings& destination;
    SettingsDraft draft;
    std::vector<SettingField> fields = SettingsFields();
    std::vector<size_t> visible;
    std::vector<AudioEndpointChoice> endpoints;
    std::string initial_field;
    HWND window = nullptr, groups = nullptr, list = nullptr, heading = nullptr, key = nullptr, support = nullptr;
    HWND range = nullptr, edit = nullptr, check = nullptr, combo = nullptr, device = nullptr, credentials = nullptr, status = nullptr;
    HWND load = nullptr, save = nullptr, defaults = nullptr, apply = nullptr, cancel = nullptr;
    HWND last_focus = nullptr;
    HFONT font = nullptr, bold = nullptr;
    size_t selected = size_t(-1);
    int scale = 96, group_index = 0, scroll_x = 0, scroll_y = 0;
    bool active_session = false, accepted = false, done = false, filling = false, laying_out = false;
    Dialog(SASettings& settings, bool active,const char* initial=nullptr) : destination(settings), draft(settings), initial_field(initial?initial:""), active_session(active) {}
    ~Dialog() { if (font) DeleteObject(font); if (bold) DeleteObject(bold); }
    int P(int value) const { return MulDiv(value, scale, 96); }
    HWND Control(const wchar_t* klass, const wchar_t* text, DWORD style, int id = 0) {
        auto h = CreateWindowExW((style & WS_BORDER) ? WS_EX_CLIENTEDGE : 0, klass, text,
            WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
        if (!h) throw std::runtime_error("Cannot create settings control");
        SendMessageW(h, WM_SETFONT, WPARAM(font), FALSE); return h;
    }
    void SetScale(int dpi) {
        scale = dpi ? dpi : 96; const HFONT previous_font = font, previous_bold = bold;
        font = CreateFontW(-P(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        bold = CreateFontW(-P(20), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, WPARAM(child == heading ? bold : font), TRUE);
        if (previous_font) DeleteObject(previous_font); if (previous_bold) DeleteObject(previous_bold);
        if (list) SendMessageW(list, LB_SETHORIZONTALEXTENT, P(430), 0);
    }
    void CreateControls() {
        SetScale(int(GetDpiForWindow(window)));
        groups = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, Groups);
        const wchar_t* names[] = {L"All settings", L"Client identity", L"Connection", L"Security", L"Display", L"Input", L"Audio", L"Interface", L"SonarPen"};
        for (auto name : names) SendMessageW(groups, CB_ADDSTRING, 0, LPARAM(name));
        SendMessageW(groups, CB_SETCURSEL, 0, 0);
        list = Control(L"LISTBOX", L"", WS_BORDER | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL, Fields);
        SendMessageW(list, LB_SETHORIZONTALEXTENT, P(430), 0);
        heading = Control(L"STATIC", L"", SS_LEFT); SendMessageW(heading, WM_SETFONT, WPARAM(bold), FALSE);
        key = Control(L"EDIT", L"", ES_READONLY | ES_AUTOHSCROLL);
        support = Control(L"STATIC", L"", SS_LEFT);
        range = Control(L"STATIC", L"", SS_LEFT);
        edit = Control(L"EDIT", L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, TextValue);
        SendMessageW(edit, EM_SETLIMITTEXT, 8192, 0);
        check = Control(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX | WS_TABSTOP, BoolValue);
        combo = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, EnumValue);
        device = Control(L"COMBOBOX", L"", CBS_DROPDOWN | WS_TABSTOP | WS_VSCROLL | CBS_AUTOHSCROLL, DeviceValue);
        SendMessageW(device,CB_SETCUEBANNER,TRUE,LPARAM(L"Choose a device or enter an endpoint ID"));
        credentials = Control(L"BUTTON", L"Include credentials when saving (plain text)", BS_AUTOCHECKBOX | WS_TABSTOP, Credentials);
        status = Control(L"STATIC", active_session ? L"Editing a draft for the next connection. The current session keeps its settings." : L"Apply updates the viewer profile. Connecting and profile files are separate actions.", SS_LEFT, Status);
        load = Control(L"BUTTON", L"Load profile...", BS_PUSHBUTTON | WS_TABSTOP, LoadProfile);
        save = Control(L"BUTTON", L"Save profile...", BS_PUSHBUTTON | WS_TABSTOP, SaveProfile);
        defaults = Control(L"BUTTON", L"Restore defaults", BS_PUSHBUTTON | WS_TABSTOP, Defaults);
        apply = Control(L"BUTTON", active_session ? L"Use next connection" : L"Apply", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
        cancel = Control(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);
        Filter();if(!initial_field.empty()) SelectField(initial_field);Layout();
    }
    void Position(HWND h, int x, int y, int w, int height) { MoveWindow(h, P(x - scroll_x), P(y - scroll_y), P(w), P(height), TRUE); }
    void Layout() {
        if (!groups || laying_out) return;
        laying_out = true;
        RECT r{}; GetClientRect(window, &r);
        int view_w = MulDiv(r.right, 96, scale), view_h = MulDiv(r.bottom, 96, scale);
        int w = std::max(800, view_w), h = std::max(490, view_h);
        scroll_x = std::clamp(scroll_x, 0, w - view_w); scroll_y = std::clamp(scroll_y, 0, h - view_h);
        SCROLLINFO horizontal{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, w - 1, UINT(view_w), scroll_x, 0};
        SCROLLINFO vertical{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS, 0, h - 1, UINT(view_h), scroll_y, 0};
        SetScrollInfo(window, SB_HORZ, &horizontal, TRUE); SetScrollInfo(window, SB_VERT, &vertical, TRUE);
        // Scrollbar appearance changes the client area. Recalculate once after
        // Windows adds/removes them, then keep every field reachable by scrolling.
        GetClientRect(window, &r); view_w = MulDiv(r.right, 96, scale); view_h = MulDiv(r.bottom, 96, scale);
        w = std::max(800, view_w); h = std::max(490, view_h);
        horizontal.nMax = w - 1; horizontal.nPage = UINT(view_w);
        vertical.nMax = h - 1; vertical.nPage = UINT(view_h);
        SetScrollInfo(window, SB_HORZ, &horizontal, TRUE); SetScrollInfo(window, SB_VERT, &vertical, TRUE);
        const int left = std::clamp(w / 3, 260, 360), x = left + 36, right = w - x - 22;
        Position(groups, 18, 20, left, 280); Position(list, 18, 58, left, h - 212);
        Position(heading, x, 22, right, 52); Position(key, x, 82, right, 26);
        Position(support, x, 121, right, 100); Position(range, x, 233, right, 48);
        Position(edit, x, 297, right, 33); Position(check, x, 297, right, 33); Position(combo, x, 297, right, 280);
        Position(device,x,297,right,280);
        Position(credentials, 18, h - 143, w - 36, 26); Position(status, 18, h - 109, w - 36, 44);
        Position(load, 18, h - 49, 117, 32); Position(save, 143, h - 49, 117, 32); Position(defaults, 268, h - 49, 135, 32);
        Position(apply, w - 285, h - 49, 166, 32); Position(cancel, w - 111, h - 49, 93, 32);
        laying_out = false;
    }
    void Scroll(int bar, int code) {
        SCROLLINFO info{sizeof(SCROLLINFO), SIF_ALL}; GetScrollInfo(window, bar, &info);
        int next = info.nPos;
        if (code == SB_LINEUP) next -= 24; else if (code == SB_LINEDOWN) next += 24;
        else if (code == SB_PAGEUP) next -= std::max(24, int(info.nPage) - 24);
        else if (code == SB_PAGEDOWN) next += std::max(24, int(info.nPage) - 24);
        else if (code == SB_THUMBTRACK || code == SB_THUMBPOSITION) next = info.nTrackPos;
        else if (code == SB_TOP) next = info.nMin; else if (code == SB_BOTTOM) next = info.nMax;
        next = std::clamp(next, 0, std::max(0, info.nMax + 1 - int(info.nPage)));
        (bar == SB_HORZ ? scroll_x : scroll_y) = next; Layout(); InvalidateRect(window, nullptr, TRUE);
    }
    void Reveal(HWND control) {
        if (!control || !IsChild(window, control)) return;
        RECT rect{}, area{}; GetWindowRect(control, &rect); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2); GetClientRect(window, &area);
        int x = scroll_x, y = scroll_y;
        if (rect.left < 0) x += MulDiv(rect.left, 96, scale);
        else if (rect.right > area.right) x += MulDiv(rect.right - area.right, 96, scale);
        if (rect.top < 0) y += MulDiv(rect.top, 96, scale);
        else if (rect.bottom > area.bottom) y += MulDiv(rect.bottom - area.bottom, 96, scale);
        if (x != scroll_x || y != scroll_y) { scroll_x = x; scroll_y = y; Layout(); InvalidateRect(window, nullptr, TRUE); }
    }
    std::wstring Text(HWND h) const {
        const int count = GetWindowTextLengthW(h); std::wstring value(size_t(count) + 1, L'\0');
        GetWindowTextW(h, value.data(), count + 1); value.resize(count); return value;
    }
    void Error(const std::exception& e) { SetWindowTextW(status, SettingsWide(e.what()).c_str()); MessageBeep(MB_ICONWARNING); }
    bool Store() {
        if (selected == size_t(-1) || filling) return true;
        try {
            const auto& f = fields[selected]; std::wstring value;
            if (f.info.type == SD_SETTING_BOOLEAN) value = SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0";
            else if (!f.choices.empty()) {
                const auto index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
                if (index < 0 || size_t(index) >= f.choices.size()) throw std::runtime_error("Choose a value from the list");
                value = std::to_wstring(f.choices[size_t(index)].value);
            } else if(std::string(f.info.key)=="sonar.capture_endpoint_id" || std::string(f.info.key)=="sonar.render_endpoint_id") {
                const auto choice=SendMessageW(device,CB_GETCURSEL,0,0);
                const auto typed=Text(device);
                value=choice>=0 && size_t(choice)<endpoints.size() && typed==endpoints[size_t(choice)].label?SettingsWide(endpoints[size_t(choice)].id):typed;
            } else value = Text(edit);
            draft.Set(f, value); return true;
        } catch (const std::exception& e) { Error(e); return false; }
    }
    void Show(size_t index) {
        filling = true; selected = index;
        // EM_SETPASSWORDCHAR redraws the existing text immediately. Remove the
        // previous field's contents while its old mask is still in place, before
        // reusing this control for an unmasked field.
        SetWindowTextW(edit, L"");
        const auto& f = fields[index]; SetWindowTextW(heading, f.label.c_str()); SetWindowTextW(key, SettingsWide(f.info.key).c_str());
        SetWindowTextW(support, SettingSupport(f).c_str());
        std::wstring detail = f.info.type == SD_SETTING_TEXT ? L"UTF-8 length: " : L"Allowed range: ";
        detail += std::to_wstring(f.info.minimum) + L" to " + std::to_wstring(f.info.maximum);
        detail += f.info.type == SD_SETTING_TEXT ? L" bytes" : L"";
        detail += L"\r\nDefault: ";
        detail += *f.info.default_value ? SettingsWide(f.info.default_value) :
            std::string(f.info.key)=="client.id"?L"(generated identity)":
            (std::string(f.info.key)=="sonar.capture_endpoint_id" || std::string(f.info.key)=="sonar.render_endpoint_id")?L"(no device selected)":L"(empty)";
        SetWindowTextW(range, detail.c_str());
        const bool boolean = f.info.type == SD_SETTING_BOOLEAN, enumeration = !boolean && !f.choices.empty();
        const bool audio_endpoint=std::string(f.info.key)=="sonar.capture_endpoint_id" || std::string(f.info.key)=="sonar.render_endpoint_id";
        ShowWindow(check, boolean ? SW_SHOW : SW_HIDE); ShowWindow(combo, enumeration ? SW_SHOW : SW_HIDE);
        ShowWindow(device,audio_endpoint?SW_SHOW:SW_HIDE);ShowWindow(edit, !boolean && !enumeration && !audio_endpoint ? SW_SHOW : SW_HIDE);
        if (boolean) SendMessageW(check, BM_SETCHECK, draft.Value().GetInteger(f.info.key) ? BST_CHECKED : BST_UNCHECKED, 0);
        else if (enumeration) {
            SendMessageW(combo, CB_RESETCONTENT, 0, 0);
            const auto value = draft.Value().GetInteger(f.info.key);
            for (size_t i = 0; i < f.choices.size(); ++i) {
                SendMessageW(combo, CB_ADDSTRING, 0, LPARAM(f.choices[i].label.c_str()));
                if (f.choices[i].value == value) SendMessageW(combo, CB_SETCURSEL, WPARAM(i), 0);
            }
        } else if(audio_endpoint) {
            endpoints=AudioEndpointChoices(std::string(f.info.key)=="sonar.capture_endpoint_id"?SD_SONAR_AUDIO_CAPTURE:SD_SONAR_AUDIO_RENDER);
            SendMessageW(device,CB_RESETCONTENT,0,0);const auto value=draft.Value().GetText(f.info.key);int selected_device=-1;
            for(size_t i=0;i<endpoints.size();++i) {
                SendMessageW(device,CB_ADDSTRING,0,LPARAM(endpoints[i].label.c_str()));if(endpoints[i].id==value) selected_device=int(i);
            }
            if(selected_device>=0) SendMessageW(device,CB_SETCURSEL,WPARAM(selected_device),0);
            else SetWindowTextW(device,SettingsWide(value).c_str());
        } else {
            SendMessageW(edit, EM_SETPASSWORDCHAR, (f.info.application & SD_SETTING_SECRET) ? 0x25CF : 0, 0);
            const auto value = f.info.type == SD_SETTING_TEXT ? SettingsWide(draft.Value().GetText(f.info.key)) : std::to_wstring(draft.Value().GetInteger(f.info.key));
            SetWindowTextW(edit, value.c_str()); SendMessageW(edit, EM_SETSEL, 0, 0);
        }
        filling = false;
    }
    void SelectField(const std::string& name) {
        for(size_t i=0;i<visible.size();++i) if(name==fields[visible[i]].info.key) {
            Show(visible[i]);SendMessageW(list,LB_SETCURSEL,WPARAM(i),0);SendMessageW(list,LB_SETTOPINDEX,WPARAM(i),0);return;
        }
    }
    void Filter() {
        filling = true; visible.clear(); selected = size_t(-1); SendMessageW(list, LB_RESETCONTENT, 0, 0);
        const auto chosen = SendMessageW(groups, CB_GETCURSEL, 0, 0); group_index = int(chosen); const auto name = Text(groups);
        for (size_t i = 0; i < fields.size(); ++i) if (!chosen || fields[i].group == name) {
            visible.push_back(i); std::wstring text = fields[i].label;
            if (fields[i].info.application & SD_SETTING_PENDING) text += L" [pending]";
            else if (fields[i].info.application & SD_SETTING_FRONTEND) text += L" [app]";
            SendMessageW(list, LB_ADDSTRING, 0, LPARAM(text.c_str()));
        }
        filling = false; if (!visible.empty()) { SendMessageW(list, LB_SETCURSEL, 0, 0); Show(visible.front()); }
    }
    std::wstring File(bool writing) {
        std::wstring path(32768, L'\0'); if (writing) path.replace(0, 18, L"client.sasettings");
        OPENFILENAMEW info{}; info.lStructSize = sizeof(info); info.hwndOwner = window;
        info.lpstrFilter = L"SASettings profiles\0*.sasettings\0All files\0*.*\0\0"; info.lpstrFile = path.data(); info.nMaxFile = DWORD(path.size());
        info.lpstrDefExt = L"sasettings"; info.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (writing ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
        const bool result = writing ? GetSaveFileNameW(&info) != FALSE : GetOpenFileNameW(&info) != FALSE;
        if (!result) { if (CommDlgExtendedError()) throw std::runtime_error("Cannot open profile file dialog"); return {}; }
        path.resize(wcslen(path.c_str())); return path;
    }
    void Command(int id, int notification) {
        if (filling) return;
        if (id == IDCANCEL) { DestroyWindow(window); return; }
        if (id == Fields && notification == LBN_SELCHANGE) {
            const auto next = SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (!Store()) { auto old = std::find(visible.begin(), visible.end(), selected); SendMessageW(list, LB_SETCURSEL, old - visible.begin(), 0); return; }
            if (next >= 0 && size_t(next) < visible.size()) Show(visible[size_t(next)]); return;
        }
        if (id == Groups && notification == CBN_SELCHANGE) {
            if (Store()) Filter(); else SendMessageW(groups, CB_SETCURSEL, WPARAM(group_index), 0);
            return;
        }
        if (id != IDOK && id != LoadProfile && id != SaveProfile && id != Defaults) return;
        try {
            if (id == Defaults) { draft.Defaults(); Filter(); SetWindowTextW(status, L"Defaults restored in the draft. Cancel preserves the previous viewer profile."); return; }
            // A load replaces the draft, so invalid text in its current control
            // should not trap the user or prevent choosing another profile.
            if (id != LoadProfile && !Store()) return;
            if (id == IDOK) { draft.Commit(destination); accepted = true; DestroyWindow(window); }
            else if (id == LoadProfile) {
                const auto path = File(false); if (path.empty()) return;
                draft.Load(path); Filter(); SendMessageW(credentials, BM_SETCHECK, BST_UNCHECKED, 0);
                SetWindowTextW(status, L"Profile loaded into the draft. Apply validates it before updating the viewer.");
            } else if (id == SaveProfile) {
                const auto path = File(true); if (path.empty()) return;
                const bool include = SendMessageW(credentials, BM_GETCHECK, 0, 0) == BST_CHECKED;
                draft.Save(path, include);
                SetWindowTextW(status, include ? L"Profile saved with plain-text credentials. The active viewer profile is unchanged until Apply." : L"Profile saved without credentials. The active viewer profile is unchanged until Apply.");
            }
        } catch (const std::exception& e) { Error(e); }
    }
    static LRESULT CALLBACK Procedure(HWND h, UINT message, WPARAM wp, LPARAM lp) {
        Dialog* self = reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (message == WM_NCCREATE) { self = static_cast<Dialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); self->window = h; SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(self)); }
        if (!self) return DefWindowProcW(h, message, wp, lp);
        try {
            switch (message) {
            case WM_CREATE: self->CreateControls(); return 0;
            case WM_COMMAND: {
                const HWND control = reinterpret_cast<HWND>(lp), focus = GetFocus();
                if (control && (control == focus || IsChild(control, focus))) self->Reveal(control);
                self->Command(LOWORD(wp), HIWORD(wp)); return 0;
            }
            case WM_ACTIVATE:
                if (LOWORD(wp) == WA_INACTIVE) {
                    const auto focus = GetFocus();
                    if (focus && IsChild(h, focus)) self->last_focus = focus;
                }
                break;
            case WM_SETFOCUS:
                if (self->last_focus && IsWindow(self->last_focus) && IsChild(h, self->last_focus) &&
                    IsWindowVisible(self->last_focus) && IsWindowEnabled(self->last_focus)) SetFocus(self->last_focus);
                else if (self->list) SetFocus(self->list);
                return 0;
            case WM_SIZE: self->Layout(); return 0;
            case WM_VSCROLL: self->Scroll(SB_VERT, LOWORD(wp)); return 0;
            case WM_HSCROLL: self->Scroll(SB_HORZ, LOWORD(wp)); return 0;
            case WM_MOUSEWHEEL: self->Scroll(SB_VERT, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? SB_LINEUP : SB_LINEDOWN); return 0;
            case WM_DPICHANGED: {
                self->SetScale(HIWORD(wp)); const auto* area = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(h, nullptr, area->left, area->top, area->right - area->left, area->bottom - area->top, SWP_NOZORDER | SWP_NOACTIVATE);
                self->Layout(); return 0;
            }
            case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {self->P(320), self->P(240)}; return 0;
            case WM_CLOSE: DestroyWindow(h); return 0;
            case WM_DESTROY: self->done = true; return 0;
            case WM_NCDESTROY: SetWindowLongPtrW(h, GWLP_USERDATA, 0); self->window = nullptr; break;
            }
        } catch (const std::exception& e) {
            if (message == WM_CREATE) return -1;
            if (self->status) self->Error(e);
        }
        return DefWindowProcW(h, message, wp, lp);
    }
    HWND Create(HWND owner) {
        WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = Procedure; cls.lpszClassName = dialog_class;
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = HBRUSH(COLOR_BTNFACE + 1);
        if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("Cannot register settings window");
        const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem(); scale = dpi ? int(dpi) : 96;
        MONITORINFO monitor{sizeof(MONITORINFO)}; GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor);
        const auto& work = monitor.rcWork;
        const int width = std::min<int>(P(1020), work.right - work.left), height = std::min<int>(P(720), work.bottom - work.top);
        const int x = work.left + (work.right - work.left - width) / 2, y = work.top + (work.bottom - work.top - height) / 2;
        window = CreateWindowExW(WS_EX_CONTROLPARENT, dialog_class, L"SASettings - spacedesk SDK viewer", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_HSCROLL | WS_VSCROLL,
            x, y, width, height, owner, nullptr, cls.hInstance, this);
        if (!window) throw std::runtime_error("Cannot create settings editor");
        return window;
    }
};
}
bool EditSettings(HWND owner, SASettings& settings, bool connected,const char* initial_field) {
    Dialog dialog(settings, connected,initial_field); dialog.Create(owner);
    const bool owner_enabled = owner && IsWindowEnabled(owner);
    if (owner_enabled) EnableWindow(owner, FALSE);
    ShowWindow(dialog.window, SW_SHOW); SetFocus(dialog.list);
    MSG message{};
    while (!dialog.done) {
        const auto result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) { if (!result) PostQuitMessage(int(message.wParam)); if (dialog.window) DestroyWindow(dialog.window); break; }
        if (!IsDialogMessageW(dialog.window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (owner_enabled && IsWindow(owner)) { EnableWindow(owner, TRUE); SetActiveWindow(owner); }
    return dialog.accepted;
}
bool SettingsDialogSmokeTest(const wchar_t* bitmap_path,const char* focus_key) {
    SASettings original; const auto before = original.Serialize(SD_SETTINGS_INCLUDE_SECRETS);
    Dialog dialog(original, false,"display.codec"); dialog.Create(nullptr);
    bool valid=dialog.selected<dialog.fields.size() && std::string(dialog.fields[dialog.selected].info.key)=="display.codec";
    struct MaskTransitionProbe {
        WNDPROC original = nullptr;
        bool exposed_previous_value = false;
        static LRESULT CALLBACK Procedure(HWND h, UINT message, WPARAM wp, LPARAM lp) {
            auto* probe = reinterpret_cast<MaskTransitionProbe*>(GetWindowLongPtrW(h, GWLP_USERDATA));
            if (message == EM_SETPASSWORDCHAR && !wp &&
                CallWindowProcW(probe->original, h, EM_GETPASSWORDCHAR, 0, 0) && GetWindowTextLengthW(h))
                probe->exposed_previous_value = true;
            return CallWindowProcW(probe->original, h, message, wp, lp);
        }
    } mask_probe;
    SetWindowLongPtrW(dialog.edit, GWLP_USERDATA, LONG_PTR(&mask_probe));
    mask_probe.original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(dialog.edit, GWLP_WNDPROC, LONG_PTR(MaskTransitionProbe::Procedure)));
    dialog.draft.Value().SetText("security.password", "DEADBEEF");
    for (size_t i = 0; i < dialog.fields.size(); ++i) {
        dialog.Show(i); const auto& field = dialog.fields[i];
        if (GetWindowTextLengthW(dialog.heading) == 0 || GetWindowTextLengthW(dialog.support) == 0) valid = false;
        if ((field.info.application & SD_SETTING_SECRET) && SendMessageW(dialog.edit, EM_GETPASSWORDCHAR, 0, 0) == 0) valid = false;
        if (!dialog.Store()) valid = false;
        if(std::string(field.info.key)=="sonar.capture_endpoint_id" || std::string(field.info.key)=="sonar.render_endpoint_id") {
            const auto original=dialog.draft.Value().GetText(field.info.key);
            if(!dialog.endpoints.empty()) {
                SendMessageW(dialog.device,CB_SETCURSEL,0,0);
                if(!dialog.Store() || dialog.draft.Value().GetText(field.info.key)!=dialog.endpoints[0].id) valid=false;
            }
            SetWindowTextW(dialog.device,L"manually-entered-endpoint");
            if(!dialog.Store() || dialog.draft.Value().GetText(field.info.key)!="manually-entered-endpoint") valid=false;
            dialog.draft.Value().SetText(field.info.key,original);
        }
    }
    for (int group = 1; group < SendMessageW(dialog.groups,CB_GETCOUNT,0,0); ++group) {
        SendMessageW(dialog.groups, CB_SETCURSEL, group, 0); dialog.Command(Groups, CBN_SELCHANGE);
        if (dialog.visible.empty()) valid = false;
        for (auto index : dialog.visible) if (dialog.fields[index].group != dialog.Text(dialog.groups)) valid = false;
    }
    SendMessageW(dialog.groups, CB_SETCURSEL, 0, 0); dialog.Command(Groups, CBN_SELCHANGE);
    if (dialog.visible.size() != dialog.fields.size()) valid = false;
    dialog.Show(0); SendMessageW(dialog.list, LB_SETCURSEL, 0, 0); SendMessageW(dialog.list, LB_SETTOPINDEX, 0, 0);
    if (mask_probe.exposed_previous_value) valid = false;
    if (GetWindowTextLengthW(dialog.edit) == 0 || GetWindowTextLengthW(dialog.groups) == 0) valid = false;
    RECT client{}; GetClientRect(dialog.window, &client);
    for (auto control : {dialog.groups, dialog.list, dialog.heading, dialog.key, dialog.support, dialog.range,
        dialog.edit, dialog.credentials, dialog.status, dialog.load, dialog.save, dialog.defaults, dialog.apply, dialog.cancel}) {
        RECT rect{}; GetWindowRect(control, &rect); MapWindowPoints(nullptr, dialog.window, reinterpret_cast<POINT*>(&rect), 2);
        // Combo drop-down rectangles include their unopened popup height.
        if (rect.left < 0 || rect.top < 0 || rect.right > client.right || rect.bottom > client.bottom) valid = false;
    }
    if (bitmap_path) {
        if(focus_key) for(size_t i=0;i<dialog.fields.size();++i) if(std::string(dialog.fields[i].info.key)==focus_key) {
            dialog.Show(i);SendMessageW(dialog.list,LB_SETCURSEL,WPARAM(i),0);SendMessageW(dialog.list,LB_SETTOPINDEX,WPARAM(i),0);
            if(!dialog.endpoints.empty() && (std::string(focus_key)=="sonar.capture_endpoint_id" || std::string(focus_key)=="sonar.render_endpoint_id")) SendMessageW(dialog.device,CB_SETCURSEL,0,0);
            break;
        }
        HDC dc = GetDC(dialog.window), memory = CreateCompatibleDC(dc);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = client.right;
        info.bmiHeader.biHeight = -client.bottom; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        void* pixels = nullptr; HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bitmap || !memory) valid = false;
        else {
            HGDIOBJ old = SelectObject(memory, bitmap);
            FillRect(memory, &client, HBRUSH(COLOR_BTNFACE + 1));
            // Top-level WM_PRINT includes a nonclient offset even for a client
            // bitmap. Print each actual child at its client-relative position;
            // this also captures EDIT/COMBOBOX content in a hidden parent.
            for (HWND child = GetWindow(dialog.window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                if (!(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
                RECT rect{}; GetWindowRect(child, &rect); MapWindowPoints(nullptr, dialog.window, reinterpret_cast<POINT*>(&rect), 2);
                const int state = SaveDC(memory); SetViewportOrgEx(memory, rect.left, rect.top, nullptr);
                IntersectClipRect(memory, 0, 0, rect.right - rect.left, rect.bottom - rect.top);
                SendMessageW(child, WM_PRINT, WPARAM(memory), PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
                if (child == dialog.edit || child == dialog.key || child == dialog.groups || child == dialog.combo || child == dialog.device)
                {
                    // Some native EDIT/COMBO implementations omit text when
                    // printing a hidden parent. This diagnostic uses the real
                    // control value/font; visible UI painting is unchanged.
                    auto text=dialog.Text(child);
                    if(child==dialog.edit) { const auto mask=SendMessageW(child,EM_GETPASSWORDCHAR,0,0);if(mask) text.assign(text.size(),wchar_t(mask)); }
                    RECT body{3,2,rect.right-rect.left-3,rect.bottom-rect.top-2};
                    if(child==dialog.groups || child==dialog.combo || child==dialog.device) body.right-=dialog.P(19);
                    const bool readonly=(child==dialog.edit || child==dialog.key) && (GetWindowLongPtrW(child,GWL_STYLE)&ES_READONLY);
                    FillRect(memory,&body,GetSysColorBrush(readonly?COLOR_BTNFACE:COLOR_WINDOW));
                    SelectObject(memory,dialog.font);SetBkMode(memory,TRANSPARENT);SetTextColor(memory,GetSysColor(COLOR_WINDOWTEXT));
                    DrawTextW(memory,text.c_str(),int(text.size()),&body,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
                }
                RestoreDC(memory, state);
            }
            GdiFlush(); BITMAPFILEHEADER header{}; header.bfType = 0x4D42; header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
            const auto bytes = uint32_t(client.right * client.bottom * 4); header.bfSize = header.bfOffBits + bytes;
            std::ofstream output(std::filesystem::path(bitmap_path), std::ios::binary);
            output.write(reinterpret_cast<const char*>(&header), sizeof(header)); output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader)); output.write(static_cast<const char*>(pixels), bytes);
            if (!output) valid = false;
            SelectObject(memory, old);
        }
        if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); ReleaseDC(dialog.window, dc);
    }
    // Small/high-DPI work areas must still expose every action, even when a
    // complete two-column form cannot fit at once.
    SetWindowPos(dialog.window, nullptr, 0, 0, dialog.P(320), dialog.P(240), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    for (auto control : {dialog.apply, dialog.cancel, dialog.load, dialog.save, dialog.defaults}) {
        dialog.Reveal(control);
        RECT rect{}, view{}; GetWindowRect(control, &rect); MapWindowPoints(nullptr, dialog.window, reinterpret_cast<POINT*>(&rect), 2); GetClientRect(dialog.window, &view);
        if (rect.left < 0 || rect.top < 0 || rect.right > view.right || rect.bottom > view.bottom) valid = false;
    }
    dialog.Show(0);SetWindowTextW(dialog.edit, L"Discard this edited client name");
    if (!dialog.Store() || dialog.draft.Value().GetText("client.name") != "Discard this edited client name") valid = false;
    dialog.Command(IDCANCEL, 0);
    return valid && !dialog.accepted && before == original.Serialize(SD_SETTINGS_INCLUDE_SECRETS);
}
}
