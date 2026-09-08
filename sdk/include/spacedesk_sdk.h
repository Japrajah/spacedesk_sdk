#ifndef SPACEDESK_SDK_H
#define SPACEDESK_SDK_H
#include <stdint.h>
#if defined(SD_SDK_BUILD)
#define SD_API __declspec(dllexport)
#else
#define SD_API __declspec(dllimport)
#endif
#define SD_CALL __cdecl
#ifdef __cplusplus
extern "C" {
#endif

/* Unofficial experimental SDK. ABI 1; Windows x64. No C++ types cross the DLL.
 * Each handle and its frame views belong to the thread that created it.
 * All calls for that handle, including destruction, must run on that thread.
 * Network, framebuffer processing and decoding run on native task workers.
 * The owner drives FSM notifications and receives immutable snapshots via poll.
 */
typedef struct sd_client sd_client;
typedef struct sd_gpu_texture sd_gpu_texture;
typedef struct sd_settings sd_settings;
typedef struct sd_sonar_session sd_sonar_session;
typedef int32_t sd_result;
/* Already connected byte-stream boundary for the APK USB peripheral protocol.
 * The application supplies the actual peripheral-role bridge/device driver.
 * This is not a WinUSB host enumerator. Callbacks run on SDK workers, may overlap
 * one read with one write, and must not call back into client APIs or throw.
 * OK transfers 1..capacity bytes; TIMEOUT transfers zero; DISCONNECTED is EOF.
 * Each callback must honor timeout_ms (at most 100 ms per SDK call). cancel may
 * overlap either callback and must wake blocked I/O. release is optional and
 * runs exactly once, on the owner thread, after accepted I/O is fully joined.
 * Rejected start arguments do not transfer ownership or invoke callbacks.
 */
typedef sd_result (SD_CALL *sd_stream_read_fn)(void* context, uint8_t* bytes, uint32_t capacity,
    uint32_t timeout_ms, uint32_t* transferred);
typedef sd_result (SD_CALL *sd_stream_write_fn)(void* context, const uint8_t* bytes, uint32_t count,
    uint32_t timeout_ms, uint32_t* transferred);
typedef void (SD_CALL *sd_stream_cancel_fn)(void* context);
typedef void (SD_CALL *sd_stream_release_fn)(void* context);
typedef struct sd_usb_stream {
    uint32_t struct_size, reserved;
    void* context;
    sd_stream_read_fn read;
    sd_stream_write_fn write;
    sd_stream_cancel_fn cancel;
    sd_stream_release_fn release;
} sd_usb_stream;
enum {
    SD_OK = 0, SD_TIMEOUT = 1, SD_DISCONNECTED = 2, SD_NO_FRAME = 3, SD_PENDING = 4, SD_IGNORED = 5,
    SD_INVALID_ARGUMENT = -1, SD_IO_ERROR = -2, SD_PROTOCOL_ERROR = -3,
    SD_UNSUPPORTED = -4, SD_DECODE_ERROR = -5, SD_OUT_OF_MEMORY = -6,
    SD_WRONG_THREAD = -7, SD_INTERNAL_ERROR = -8, SD_GPU_ERROR = -9, SD_TLS_ERROR = -10
};
enum { SD_EVENT_NONE = 0, SD_EVENT_FRAME = 1, SD_EVENT_CONTROL = 2,
       SD_EVENT_DISCONNECTED = 3, SD_EVENT_CONNECTION_STATE = 4, SD_EVENT_RENDERER_ERROR = 5,
       SD_EVENT_CONNECTION_WARNING = 6, SD_EVENT_CLIENT_DISCONNECTED = 7 };
typedef struct sd_connection_watch_state {
    uint32_t struct_size, enabled, running, timed_out;
    uint64_t last_activity_unix_ms;
    int32_t elapsed_ms;
    uint32_t warnings;
    uint64_t last_check_unix_ms;
} sd_connection_watch_state;
/* Original SA_FSTM_STATE values. */
enum {
    SD_STATE_DISCONNECTED = 0, SD_STATE_CONNECTING = 1, SD_STATE_RECONNECTING = 2,
    SD_STATE_DISPLAY_OFF = 3, SD_STATE_DISPLAY_ON = 4, SD_STATE_DISCONNECTING = 5,
    SD_STATE_FULLSCREEN = 6
};
enum { SD_PIXEL_BGRA8 = 1 };
enum { SD_TRANSPORT_TCP = 0, SD_TRANSPORT_WEBSOCKET = 1, SD_TRANSPORT_USB = 2 };
enum { SD_SERVER_FEATURE_AUDIO = 0x08, SD_SERVER_FEATURE_ENCRYPTION = 0x10 };
enum { SD_TLS_NONE = 0, SD_TLS_1_2 = 1, SD_TLS_1_3 = 2 };
typedef struct sd_tls_state {
    uint32_t struct_size, configured, connected, protocol_version;
    uint32_t cipher_suite, last_alert, session_reused, peer_close_notify;
    char cipher_name[96];
} sd_tls_state;
enum { SD_CLIENT_DISPLAY_MONITOR = 0, SD_CLIENT_WEB_BROWSER = 1, SD_CLIENT_RESERVED = 2, SD_CLIENT_AUDIO = 3 };
enum { SD_SETTING_INTEGER = 0, SD_SETTING_BOOLEAN = 1, SD_SETTING_TEXT = 2 };
enum { SD_SETTING_ENGINE = 1, SD_SETTING_FRONTEND = 2, SD_SETTING_PENDING = 4, SD_SETTING_SECRET = 8 };
enum { SD_SETTINGS_INCLUDE_SECRETS = 1 };
typedef struct sd_setting_info {
    uint32_t struct_size, type, application, reserved;
    int64_t minimum, maximum; /* UTF-8 byte lengths for text. */
    const char *key, *default_value, *allowed_values, *apk_key;
} sd_setting_info;
/* Ordered Android-style motion batch used by the APK touch/touchpad path.
 * action is raw MotionEvent.getAction(): 0 DOWN, 1 UP, 2 MOVE, 3 CANCEL;
 * pointer-index actions (5/6/261/...) are retained and ignored as in W1.
 * x/y are mapped display pixels. mapped=0 retains the original fallback rules.
 */
typedef struct sd_touch_input_contact {
    uint32_t pointer_id;
    int32_t x, y;
    uint32_t mapped;
} sd_touch_input_contact;
typedef struct sd_touch_input {
    uint32_t struct_size, action, surface_width, surface_height, timestamp_ms, contact_count;
    sd_touch_input_contact contacts[7];
} sd_touch_input;
/* Portable inputs to APK SAMotionEvent's coordinate mapping. Viewport is the
 * actual input view size (positive signed32 dimensions, no pixel allocation).
 * orientation=-1 (disabled) or 1/2/3/4; aspect<=0 means
 * server aspect is unknown. The mapper accounts for portrait dimension swap. */
typedef struct sd_motion_geometry {
    uint32_t struct_size;
    int32_t orientation;
    uint32_t viewport_width, viewport_height;
    float server_aspect_ratio;
} sd_motion_geometry;
/* Caller-owned state for the original APK accelerometer/configuration policy.
 * Not an Android object layout. No OS sensor, window or network is activated. */
typedef struct sd_orientation_state {
    uint32_t struct_size,natural_portrait;
    int32_t last_angle,candidate_orientation,applied_orientation;
} sd_orientation_state;
/* Android-style configuration: 1 portrait, 2 landscape; display rotation 0..3.
 * Original unusual-value fallback is retained. Starts last_angle=0 and both
 * orientations=-1, then computes the source natural-portrait flag. */
SD_API sd_result SD_CALL sd_orientation_init(sd_orientation_state* state,int32_t configuration_orientation,int32_t display_rotation);
/* Samples use Android accelerometer axes. enabled represents the OS
 * accelerometer_rotation gate. Disabled, flat, NaN and duplicate-angle cases
 * preserve source state as specified; valid calls return SD_OK even unchanged. */
SD_API sd_result SD_CALL sd_orientation_sensor_update(sd_orientation_state* state,float x,float y,float z,uint32_t enabled);
/* Applies source Activity.e1 decision. enabled is the app autorotation setting.
 * SD_OK means the original would request layout/orientation dispatch (including
 * repeated values); SD_IGNORED means no dispatch and no output write. The caller
 * may then use view_set_orientation/set_rotation. Invalid calls are atomic. */
SD_API sd_result SD_CALL sd_orientation_configuration_update(sd_orientation_state* state,int32_t configuration_orientation,uint32_t enabled,uint32_t* orientation);
/* SonarPen 3.11 compute kernel from APK W0.a/b/c and V0.g. PCM values retain
 * their original order (stereo input is not downmixed); analysis rate is 44100.
 * These functions do not open a microphone, change volume, play audio or
 * implement the device calibration/contact/palm-rejection controller. */
typedef struct sd_sonar_signal {
    uint32_t struct_size, sample_count, fft_size, fft_windows;
    int32_t peak_amplitude;
    float steady_amplitude; /* V0.g.d: sqrt(sum(sample^2)/(count*2.0f)). */
    double frequency_hz; /* W0.a.i: interpolated spectrum peak, truncated to 0.1 Hz. */
    double level; /* Original W0.a.h custom score; not a standard dB measurement. */
} sd_sonar_signal;
enum { SD_SONAR_AUDIO_CAPTURE=1, SD_SONAR_AUDIO_RENDER=2 };
typedef struct sd_sonar_audio_device {
    uint32_t struct_size,flow,is_default,form_factor;
    uint32_t source_type_hint,reserved; /* Label/form-factor hint: 0, or APK 3/4/22; bus/compatibility is not resolved. */
    char endpoint_id[1024];
    char name[512];
} sd_sonar_audio_device;
/* Read active Windows audio endpoints without opening a stream or changing
 * volume. Source type is only a selection hint, not proof of a connected pen.
 * Null devices/capacity0 queries count. An undersized non-null array returns
 * PENDING and the required count, leaving the array untouched. Each provided
 * entry must have its struct_size initialized. Endpoint IDs are UTF-8. */
SD_API sd_result SD_CALL sd_sonar_enumerate_audio_devices(sd_sonar_audio_device* devices,uint32_t capacity,uint32_t* count);
enum { SD_SONAR_DEVICE_ANALOG=0, SD_SONAR_DEVICE_SP1=1 };
typedef struct sd_sonar_options {
    uint32_t struct_size,device_kind,read_timeout_ms,reserved;
    char capture_endpoint_id[1024],render_endpoint_id[1024];
} sd_sonar_options;
typedef struct sd_sonar_state {
    uint32_t struct_size,running,device_connected,calibrating,core_ready,profile_enabled,media_keys_requested,events_dropped;
    int32_t raw_status,initialization_result,record_error,last_result;
    uint64_t generation,sequence;
    float pressure,amplitude,frequency_hz,threshold,minimum,maximum;
    float quiet_initial,left_amplitude,quiet_between,right_amplitude;
    uint32_t low_frequency_saved,reserved;
} sd_sonar_state;
enum { SD_SONAR_EVENT_STATUS=1,SD_SONAR_EVENT_BUTTON=2,SD_SONAR_EVENT_MEDIA_KEYS=3,SD_SONAR_EVENT_ERROR=4,SD_SONAR_EVENT_DEVICE=5,SD_SONAR_EVENT_PROFILE=6 };
typedef struct sd_sonar_event {
    uint32_t struct_size,type;
    int32_t value,result;
    uint64_t sequence;
    char message[192];
} sd_sonar_event;
/* Explicit SonarPen calibration/measurement session. The application selects
 * both endpoint IDs and the actual analog/SP1 connection type. Creation reads
 * metadata and starts the owner thread, without audio streams. Start can open the microphone, play the original Sonar tone
 * and change the selected render endpoint's volume; Stop releases streams and
 * restores owned volume changes. No mute state is changed.
 * Handles use the same caller-thread rule as sd_client. Core and COM work run
 * on one private MTA owner. Start/Stop/profile commands are synchronous; state
 * reads are nonblocking snapshots. SD_OK from Start accepts the native start;
 * device activation/calibration can still fail asynchronously. Inspect events
 * and state. core_ready is the original r bit and includes bypassed calibration.
 * A second Start requires Stop first. Each endpoint can be owned by only one
 * active Sonar session in this SDK process.
 * sd_sonar_process_motion runs touch/palm classification; its result can be
 * passed to sd_client_send_pen_viewport. Viewer Sonar binding remains unfinished. */
SD_API sd_result SD_CALL sd_sonar_options_init(sd_sonar_options* options);
SD_API sd_result SD_CALL sd_sonar_create(const sd_sonar_options* options,sd_sonar_session** out_session);
typedef struct sd_sonar_profile_options {
    uint32_t struct_size,reserved;
    char application_id[128];
    char store_path[1024],calibration_provider_path[1024],compatibility_provider_path[1024];
} sd_sonar_profile_options;
enum { SD_SONAR_PROFILE_LOADED=1,SD_SONAR_PROFILE_PRESENT=2,SD_SONAR_PROFILE_BYPASS=4,
    SD_SONAR_PROFILE_LOW_FREQUENCY=8,SD_SONAR_PROFILE_RECEIVER=16,SD_SONAR_PROFILE_PERSISTENT=32 };
typedef struct sd_sonar_profile_state {
    uint32_t struct_size,flags,profile_bytes;
    sd_result store_result;
} sd_sonar_profile_state;
/* Optional Windows adapters for original V/J/SharedPreferences. Paths are
 * absolute UTF-8; empty store_path uses memory. Provider files supply the first
 * string column: missing file means no row, empty file means an empty value.
 * Bootstrap order is calibration, compatibility, receiver registration.
 * Files are read at creation; later messages enter via profile_receive.
 * No Android IPC, directory watcher or network request is installed here. */
SD_API sd_result SD_CALL sd_sonar_profile_options_init(sd_sonar_profile_options* options);
SD_API sd_result SD_CALL sd_sonar_create_with_profiles(const sd_sonar_options* options,
    const sd_sonar_profile_options* profiles,sd_sonar_session** out_session);
/* Original receiver filtering: case-insensitive "all" accepts other senders;
 * exact application_id accepts even self. Other messages return SD_IGNORED.
 * profile_bytes<=65536. Accepted messages persist loaded=true and apply the
 * original partial-update rules even while stopped. Empty text removes profile. */
SD_API sd_result SD_CALL sd_sonar_profile_receive(sd_sonar_session* session,
    const char* to_name,const char* sender_name,const char* profile,uint32_t profile_bytes);
/* profile=null,capacity=0 queries state/size. Otherwise capacity must include a
 * trailing NUL; profile_bytes excludes it. Outputs stay unchanged on error.
 * store_result reports the latest persistence attempt; failed disk writes keep
 * the in-memory preference update, as SharedPreferences.commit(false) does. */
SD_API sd_result SD_CALL sd_sonar_get_profile(sd_sonar_session* session,
    sd_sonar_profile_state* state,char* profile,uint32_t capacity);
SD_API sd_result SD_CALL sd_sonar_start(sd_sonar_session* session);
SD_API sd_result SD_CALL sd_sonar_stop(sd_sonar_session* session);
SD_API sd_result SD_CALL sd_sonar_destroy(sd_sonar_session* session);
SD_API sd_result SD_CALL sd_sonar_get_state(sd_sonar_session* session,sd_sonar_state* state);
typedef struct sd_sonar_pointer_state {
    uint32_t struct_size,pen_active;
    int32_t selected_pointer_id,candidate_pointer_id;
} sd_sonar_pointer_state;
/* Original g.O is raw status R==3 && selected contact D!=-1. Snapshot only. */
SD_API sd_result SD_CALL sd_sonar_get_pointer_state(sd_sonar_session* session,sd_sonar_pointer_state* state);
/* Creates from unified sonar.* settings and applies the palm preference;
 * does not Start audio. Disabled input.sonar_pen returns SD_IGNORED/null. */
SD_API sd_result SD_CALL sd_sonar_create_from_settings(const sd_settings* settings,sd_sonar_session** out_session);
SD_API sd_result SD_CALL sd_sonar_poll(sd_sonar_session* session,uint32_t timeout_ms,sd_sonar_event* event);
/* Stores UTF-8 profile text (up to 65536 bytes); while running also applies the
 * original non-atomic g.u update. Invalid JSON can leave partial native profile
 * state, as documented in SONARPEN.md. Before Start it is a stored preference.
 * Selected endpoint state/removal/addition/format notifications are handled
 * automatically. refresh_devices also requests an explicit rescan. Default
 * endpoint changes never switch the session's explicitly selected IDs. */
SD_API sd_result SD_CALL sd_sonar_set_profile(sd_sonar_session* session,const char* text,uint32_t bytes);
SD_API sd_result SD_CALL sd_sonar_refresh_devices(sd_sonar_session* session);
/* Forward original Android headset/media key IDs 79/85; action 0 DOWN,
 * 1 UP, 2 MULTIPLE. Other key IDs or a not-ready core return SD_IGNORED.
 * A frontend maps its own OS key messages and decides whether to consume them. */
SD_API sd_result SD_CALL sd_sonar_key(sd_sonar_session* session,uint32_t keycode,uint32_t action);
enum { SD_MOTION_MAX_POINTERS=16,SD_MOTION_AXIS_X=0,SD_MOTION_AXIS_Y=1,SD_MOTION_AXIS_PRESSURE=2,SD_MOTION_AXIS_SIZE=3 };
enum { SD_SONAR_MAX_POINTERS=SD_MOTION_MAX_POINTERS,SD_SONAR_AXIS_X=SD_MOTION_AXIS_X,
    SD_SONAR_AXIS_Y=SD_MOTION_AXIS_Y,SD_SONAR_AXIS_PRESSURE=SD_MOTION_AXIS_PRESSURE,SD_SONAR_AXIS_SIZE=SD_MOTION_AXIS_SIZE };
typedef struct sd_sonar_motion_point {
    int32_t pointer_id,tool_type; /* Android-style stable IDs 0..31; finger=1, stylus=2. */
    float axes[64]; /* Axis-ID indexed values; unused slots are zero. */
} sd_sonar_motion_point;
typedef struct sd_sonar_motion {
    uint32_t struct_size,action,pointer_count,meta_state,button_state;
    int32_t device_id;
    uint32_t edge_flags,source,flags,reserved;
    int64_t down_time_ms,event_time_ms;
    float x_precision,y_precision;
    sd_sonar_motion_point pointers[SD_SONAR_MAX_POINTERS];
} sd_sonar_motion;
/* Shared MotionEvent value adapter. Legacy Sonar type names retain their ABI. */
typedef sd_sonar_motion_point sd_motion_point;
typedef sd_sonar_motion sd_motion_event;
typedef struct sd_sonar_motion_result {
    uint32_t struct_size,rebuilt,input_action_after,reserved;
    sd_sonar_motion event;
} sd_sonar_motion_result;
/* Runs the original Sonar touch/palm transformation on the session owner.
 * Input remains unchanged; result reports the original input-action mutation.
 * Output includes all copied axes/metadata, converted tool/pressure and native
 * pointer filtering. A DOWN may wait up to the original 100 ms while complete
 * PCM blocks continue to update the core. No network packet is sent here.
 * Output is written only on SD_OK. The caller maps returned view coordinates
 * using the existing SDK geometry/pen APIs. */
SD_API sd_result SD_CALL sd_sonar_process_motion(sd_sonar_session* session,const sd_sonar_motion* input,sd_sonar_motion_result* output);
/* Original g.j0 boolean (k0), default true; retains learned size/pressure mode. */
SD_API sd_result SD_CALL sd_sonar_set_palm_rejection(sd_sonar_session* session,uint32_t enabled);
SD_API const char* SD_CALL sd_sonar_last_error(sd_sonar_session* session);
/* count is 2..65536 int16 values. Result is written only on success. */
SD_API sd_result SD_CALL sd_sonar_analyze_pcm16(const int16_t* samples,uint32_t count,sd_sonar_signal* result);
/* curve0 is the original constructor default; curve1 is the alternate branch
 * of V0.g.b (not an advertised device/profile option). The polynomial retains
 * its original upper clamp and low-end behavior. Requires finite inputs and
 * maximum>minimum; does not perform calibration or send a pen event. */
SD_API sd_result SD_CALL sd_sonar_pressure_curve(uint32_t curve,float amplitude,float minimum,float maximum,float* pressure);
/* Generate the exact V0.g.n stereo buffer, 44100 frames / 88200 int16 values.
 * channel=0 left, 1 right. Original loop region is the first 22050 frames.
 * Null output with capacity0 queries the required sample count. Capacity is
 * int16 values, not bytes. No playback occurs. Frequency is the original
 * generator parameter; the phase advances by stereo sample index as in APK. */
SD_API sd_result SD_CALL sd_sonar_generate_tone(float frequency,uint32_t channel,
    int16_t* output,uint32_t capacity,uint32_t* sample_count);
enum { SD_COMPRESSION_OFF = 0, SD_COMPRESSION_MJPEG = 3, SD_COMPRESSION_H264 = 4, SD_COMPRESSION_CHOOSE_BEST = 5 };
enum { SD_CHROMA_444 = 0, SD_CHROMA_422 = 1, SD_CHROMA_420 = 2, SD_CHROMA_GRAY = 3, SD_CHROMA_440 = 4, SD_CHROMA_411 = 5 };
enum {
    SD_MOUSE_MOVE = 0x0001, SD_MOUSE_LEFT_DOWN = 0x0002, SD_MOUSE_LEFT_UP = 0x0004,
    SD_MOUSE_RIGHT_DOWN = 0x0008, SD_MOUSE_RIGHT_UP = 0x0010,
    SD_MOUSE_MIDDLE_DOWN = 0x0020, SD_MOUSE_MIDDLE_UP = 0x0040,
    SD_MOUSE_WHEEL = 0x0800, SD_MOUSE_HWHEEL = 0x1000,
    SD_MOUSE_ABSOLUTE = 0x8000 /* Deprecated: does not enable absolute movement in button_flags. */
};
/* Movement flags belong in MouseHeader.flags (+24), not button_flags (+20). */
enum { SD_MOUSE_COORD_ABSOLUTE=1, SD_MOUSE_COORD_VIRTUAL_DESKTOP=2 };
typedef struct sd_mouse_target {
    uint32_t struct_size, rotation; /* 1 upright, 2 clockwise90, 3 180, 4 clockwise270 */
    int32_t desktop_left, desktop_top;
    uint32_t desktop_width, desktop_height;
    int32_t display_left, display_top;
    uint32_t display_width, display_height;
} sd_mouse_target;
enum {
    SD_KEY_EXTENDED = 1, SD_KEY_UP = 2, SD_KEY_UNICODE = 4,
    SD_KEY_HARDWARE = SD_KEY_UNICODE, /* Deprecated misnomer; retained source compatibility. */
    SD_KEY_PRESS = 0x8000, SD_KEY_OSK = 0x10000 /* APK key actions; not Windows KEYEVENTF values. */
};
enum { SD_ANDROID_KEY_DOWN=0, SD_ANDROID_KEY_UP=1, SD_ANDROID_KEY_MULTIPLE=2 };
typedef struct sd_android_key_event {
    uint32_t struct_size, action, key_code, caps_lock_on;
} sd_android_key_event;
enum { SD_TOUCH_DOWN = 2, SD_TOUCH_UP = 4 };
enum { SD_DISCONNECT_REGULAR = 0, SD_DISCONNECT_H264_INIT_FAILED = 1, SD_DISCONNECT_H264_DECODE_FAILED = 2 };
/* Server reasons use a separate domain from the outgoing client codes above. */
enum { SD_SERVER_DISCONNECT_REGULAR=0,SD_SERVER_DISCONNECT_NO_MEMORY=1,SD_SERVER_DISCONNECT_UNSPECIFIED=2,
    SD_SERVER_DISCONNECT_H264_INIT_FAILED=3,SD_SERVER_DISCONNECT_H264_ENCODE_FAILED=4 };
/* Original APK enum names, without allocation; NULL for unknown/sentinel codes.
 * No handle is used, so these helpers may be called from any thread. */
SD_API const char* SD_CALL sd_client_disconnect_reason_name(uint32_t reason);
SD_API const char* SD_CALL sd_server_disconnect_reason_name(uint32_t reason);
/* Windows.Devices.Input.PointerDeviceType values, also used to select output. */
enum { SD_POINTER_TOUCH = 0, SD_POINTER_PEN = 1, SD_POINTER_MOUSE = 2 };
enum { SD_INPUT_TOUCH = 0, SD_INPUT_PEN = 1, SD_INPUT_MOUSE = 2, SD_INPUT_KEYBOARD = 3 };
enum { SD_STREAM_UNDEFINED = 0, SD_STREAM_PAUSE = 1, SD_STREAM_PLAY = 2 };
typedef struct sd_stream_control_state {
    uint32_t struct_size, supported; /* Server announced the APK command; does not confirm its effect. */
    uint32_t requested_state; /* Last successfully queued command, not a server acknowledgement. */
    uint32_t reserved;
} sd_stream_control_state;
/* SDK dispatch values; native virtual order is moved/pressed/released/capture-lost/wheel. */
enum { SD_POINTER_MOVED = 0, SD_POINTER_PRESSED = 1, SD_POINTER_RELEASED = 2,
       SD_POINTER_CAPTURE_LOST = 3, SD_POINTER_WHEEL = 4, SD_POINTER_RAW_MOUSE_MOVE = 5 };
/* Windows.UI.Input.PointerUpdateKind values used by the recovered handlers. */
enum { SD_POINTER_UPDATE_OTHER = 0, SD_POINTER_LEFT_PRESSED = 1, SD_POINTER_LEFT_RELEASED = 2,
       SD_POINTER_RIGHT_PRESSED = 3, SD_POINTER_RIGHT_RELEASED = 4,
       SD_POINTER_MIDDLE_PRESSED = 5, SD_POINTER_MIDDLE_RELEASED = 6 };
enum { SD_POINTER_BUTTON_LEFT = 1, SD_POINTER_BUTTON_RIGHT = 2, SD_POINTER_BUTTON_MIDDLE = 4 };

/* Use sd_config_init before overriding fields. Strings are NUL-terminated UTF-8.
 * Defaults request MJPEG; display_options can select raw/H264 preferences.
 * license_type is a protocol declaration
 * (0 = none by default); it does not grant any server entitlement.
 * Empty device_id generates a per-handle {GUID}; set your own to persist identity.
 * Legacy 32-hex/36-character GUIDs are normalized without changing their value.
 * Other explicit IDs remain opaque; the server's HID touch path needs a nonzero GUID.
 */
typedef struct sd_config {
    uint32_t struct_size;
    uint32_t width, height, refresh_rate, quality;
    uint32_t connect_timeout_ms, io_timeout_ms;
    uint32_t protocol_minor, license_type;
    char device_id[128];
    char device_name[256];
} sd_config;

typedef struct sd_resolution { uint32_t width, height; } sd_resolution;
/* Identification options, applied while disconnected and retained on reconnect.
 * The wire carries up to nine modes. Legacy refresh values 0/1 mean 30/60 Hz.
 * compression accepts OFF/MJPEG/H264; video_codec also accepts CHOOSE_BEST.
 * System decoder availability is checked when the selected codec initializes.
 */
typedef struct sd_display_options {
    uint32_t struct_size;
    uint32_t compression, video_codec, chroma, quality;
    uint32_t refresh_rate, session_type, resolution_count;
    sd_resolution resolutions[9];
} sd_display_options;

typedef struct sd_event {
    uint32_t struct_size;
    uint32_t type;
    uint32_t wire_type; /* packet type, or 0 for CONNECTION_STATE */
    uint32_t value;     /* wire +8, or SA_FSTM_STATE for CONNECTION_STATE */
} sd_event;
/* DISCONNECTED + wire_type 8 reports a server packet: value is the raw server
 * reason (including unknown future values). wire_type 0 is a local/transport
 * termination, not a server reason. CLIENT_DISCONNECTED + wire_type 0 accompanies
 * a negative poll result when a local failure has an outgoing client reason;
 * value uses the client domain. Explicit reason-bearing disconnects do not
 * trigger automatic reconnect; transport-loss policy is separate. */

/* Borrowed, top-down, tightly packed BGRA image; alpha is 255.
 * pixels remains valid until poll delivers another frame, (re)connect, or destroy.
 * Background presentations cannot invalidate an already-delivered snapshot.
 * Copy it before passing to another thread. Dimensions may change at runtime.
 */
typedef struct sd_frame {
    uint32_t struct_size;
    uint32_t width, height, stride, pixel_format;
    uint64_t sequence;
    const uint8_t* pixels;
} sd_frame;

/* Actual source format of the most recently delivered frame, not requested
 * identification preferences. Pixels returned by get_frame are still BGRA. */
typedef struct sd_source_format {
    uint32_t struct_size;
    uint32_t compression, chroma, quality, width, height;
    uint64_t sequence;
} sd_source_format;

typedef struct sd_host {
    uint32_t struct_size;
    char ipv4[16];
    char hostname[512]; /* UTF-8 */
    uint32_t port, server_os, protocol_major, protocol_minor;
    union { uint32_t feature_flags; uint32_t preferred_compression; }; /* deprecated old name, same ABI */
} sd_host;

/* One primary contact and at most six additional contacts. Fields correspond
 * to APK W1/C0686e and UWP's touch packet builder at 0x14002D5A0.
 * flags preserves the original 16-bit value; DOWN=2, UP=4, MOVE=0.
 * Bit 0 is a client mode flag, retained without assigning unproven semantics.
 * timestamp_ms is a caller-supplied wrapping 32-bit millisecond timestamp.
 */
typedef struct sd_touch_contact {
    int32_t x, y;
    uint16_t flags, contact_id;
} sd_touch_contact;
typedef struct sd_touch_event {
    uint32_t struct_size;
    uint32_t surface_width, surface_height, timestamp_ms, contact_count;
    sd_touch_contact contacts[7];
} sd_touch_event;
/* Confirmed pen fields. UWP logs X/Y, Contact, InRange, Pressure, Resolution.
 * Pressure: 0..1, contact and in_range: 0/1. Coordinates are in surface pixels.
 */
typedef struct sd_pen_event {
    uint32_t struct_size;
    uint32_t surface_width, surface_height;
    float x, y, pressure;
    uint32_t contact, in_range;
} sd_pen_event;

/* Frontend adapter for UWP PointerPoint/Properties and renderer mapping.
 * x/y are already mapped framebuffer pixels; mapped=0 means mapping failed
 * (e.g. outside the displayed image). The frontend owns coordinate transforms,
 * pointer capture and cursor visibility. capture_succeeded is the actual result
 * of its capture attempt on PRESSED. Mouse relative motion uses delta_x/y.
 * device_type describes the physical source, independently of selected output.
 * Defaults from init: touch, mapped=1, capture_succeeded=0, zero properties.
 */
typedef struct sd_pointer_sample {
    uint32_t struct_size, device_type, pointer_id, update_kind, buttons;
    uint32_t mapped, capture_succeeded, surface_width, surface_height;
    float x, y, pressure, x_tilt, y_tilt;
    int32_t delta_x, delta_y, wheel_delta;
    uint32_t in_range;
} sd_pointer_sample;

/* Logical window dimensions (DIPs), DisplayInformation DPI and composition scale.
 * Native high-DPI policy halves effective DPI/scales only when dpi>192 and the
 * rounded physical dimensions have max>1920 and min>1080. Zero logical size is
 * accepted; native physical size is at least 1x1. */
typedef struct sd_viewport {
    uint32_t struct_size;
    float logical_width, logical_height, dpi, composition_scale_x, composition_scale_y;
} sd_viewport;
/* Exact geometry values for the current delivered frame; matrices are row-major
 * DirectX world/view/projection values. This API does not create GPU resources. */
typedef struct sd_view_state {
    uint32_t struct_size, surface_width, surface_height;
    float viewport_width, viewport_height, effective_dpi, composition_scale_x, composition_scale_y;
    float zoom, pan_x, pan_y, rotation_x, rotation_y, rotation_z;
    float model[16], view[16], projection[16];
} sd_view_state;

enum { SD_RENDERER_OFFSCREEN = 0, SD_RENDERER_HWND = 1, SD_RENDERER_COMPOSITION = 2 };
enum { SD_RENDERER_HARDWARE = 0, SD_RENDERER_WARP = 1 };
typedef struct sd_renderer_config {
    uint32_t struct_size, target, driver, present_interval;
    void* target_object; /* HWND or IUnknown supporting ISwapChainPanelNative; NULL for offscreen */
} sd_renderer_config;
typedef struct sd_renderer_state {
    uint32_t struct_size, active, target, feature_level, driver_type;
    uint32_t width, height, source_width, source_height, fps;
    int32_t last_hresult;
    uint64_t source_sequence, present_sequence;
} sd_renderer_state;

enum { SD_AUDIO_DISCONNECTED = 0, SD_AUDIO_CONNECTING = 1, SD_AUDIO_STREAMING = 2, SD_AUDIO_FAILED = 3 };
enum { SD_AUDIO_S16_LE = 1 };
typedef struct sd_audio_config {
    uint32_t struct_size, playback, capture_queue_ms, probe_server, activation_timeout_ms;
    char endpoint_id[512]; /* UTF-8 device interface ID; empty selects default render endpoint */
} sd_audio_config;
typedef struct sd_audio_state {
    uint32_t struct_size, state, playback_started, output_channels;
    int32_t last_result, last_hresult;
    uint32_t server_features, reserved;
    uint64_t packets_received, frames_received, buffered_frames;
} sd_audio_state;
typedef struct sd_audio_disconnect_state {
    uint32_t struct_size,received,server_reason,reserved;
} sd_audio_disconnect_state;
typedef struct sd_audio_buffer {
    uint32_t struct_size, sample_rate, channels, sample_format, frame_count;
    uint64_t sequence, frame_index;
    const int16_t* samples;
} sd_audio_buffer;

/* Bounded synchronous UDP discovery. NULL target broadcasts to configured IPv4
 * subnets; a literal target performs a unicast query. port 0 selects 28252.
 * Caller initializes struct_size for every entry in hosts[capacity].
 * capacity: 1..256, timeout_ms: 1..60000. Results deduplicate by address/port.
 * SD_OK with out_count=0 means no reply. If capacity is reached, returns early.
 * Independent of client handles; may run on a worker thread. */
SD_API sd_result SD_CALL sd_discover(const char* target_ipv4, uint16_t port,
    uint32_t timeout_ms, sd_host* hosts, uint32_t capacity, uint32_t* out_count);

SD_API uint32_t SD_CALL sd_abi_version(void);
/* Unified SASettings. Defaults follow normalized APK preferences (quality=60,
 * codec=5, FPS=30, audio playback enabled); native resolution is supplied by the
 * frontend (SDK fallback 1280x720). A new persistent GUID is generated per model.
 * Handles belong to their creating thread. Metadata strings belong to the DLL.
 * ENGINE settings are applied while disconnected; FRONTEND values are retained
 * for the consumer to implement. Nondefault PENDING values make apply return
 * SD_UNSUPPORTED with a diagnostic, without changing the client's configuration.
 */
SD_API sd_result SD_CALL sd_settings_create(sd_settings** settings);
SD_API sd_result SD_CALL sd_settings_destroy(sd_settings* settings);
SD_API uint32_t SD_CALL sd_settings_count(void);
SD_API sd_result SD_CALL sd_settings_describe(uint32_t index, sd_setting_info* info);
SD_API sd_result SD_CALL sd_settings_get_integer(const sd_settings* settings, const char* key, int64_t* value);
SD_API sd_result SD_CALL sd_settings_set_integer(sd_settings* settings, const char* key, int64_t value);
/* Text/output buffers: NULL/0 queries required bytes including NUL. Short output
 * returns INVALID_ARGUMENT, sets required, and leaves the buffer unchanged. */
SD_API sd_result SD_CALL sd_settings_get_text(const sd_settings* settings, const char* key, char* buffer,
    uint32_t capacity, uint32_t* required);
SD_API sd_result SD_CALL sd_settings_set_text(sd_settings* settings, const char* key, const char* value);
/* Versioned UTF-8 SASettings document (escaped tab-separated records). Secrets
 * are omitted unless INCLUDE_SECRETS is specified. Deserialize takes bytes
 * excluding the NUL, validates all records, and updates atomically; omitted
 * keys retain their values. Persistence I/O belongs to the consumer. */
SD_API sd_result SD_CALL sd_settings_serialize(const sd_settings* settings, uint32_t flags, char* buffer,
    uint32_t capacity, uint32_t* required);
SD_API sd_result SD_CALL sd_settings_deserialize(sd_settings* settings, const char* data, uint32_t bytes);
/* Canonical ui.menu_state is APK menu minimization: 0=compact, 1=expanded.
 * Legacy ui.extended_keys is accepted as an alias; metadata/serialization use
 * ui.menu_state. A document containing both spellings is rejected atomically. */
SD_API sd_result SD_CALL sd_client_create_with_settings(const sd_settings* settings, sd_client** client);
SD_API sd_result SD_CALL sd_client_apply_settings(sd_client* client, const sd_settings* settings);
SD_API sd_result SD_CALL sd_client_get_settings(sd_client* client, sd_settings* settings);
/* TLS uses SASettings security.*. password is exactly eight hexadecimal digits
 * (spacedesk PIN), decoded then suffixed with sixteen 0x1A bytes. Alternatively
 * psk_key is the complete hex PSK; do not set both. Empty identity selects
 * Client_identity. TLS1.2 uses ECDHE-PSK-CHACHA20-POLY1305; TLS1.3 uses
 * TLS_CHACHA20_POLY1305_SHA256 with DHE. No plaintext/cipher fallback.
 * The same configuration applies independently to display and audio sockets.
 * State survives stop as diagnostic metadata; connected becomes 0. Protocol
 * version is 0x0303/0x0304. session_reused may be 1 for an external TLS1.3 PSK;
 * no session cache/resumption ticket is shared between connections.
 */
SD_API sd_result SD_CALL sd_client_get_tls_state(sd_client* client, sd_tls_state* state);
SD_API sd_result SD_CALL sd_client_audio_get_tls_state(sd_client* client, sd_tls_state* state);
/* APK connection watcher: complete frame/visibility/ping packets reset activity.
 * After strictly half the timeout it emits CONNECTION_WARNING (value=elapsed ms),
 * after strictly the full timeout it reports IO_ERROR and closes the transport.
 * Existing auto_reconnect policy applies. It sends no network ping of its own.
 * SASettings watcher_poll_ms=0 disables it (legacy constructor default); the
 * APK profile defaults to 10000 ms polling / 20000 ms timeout. Runs without poll.
 */
SD_API sd_result SD_CALL sd_client_get_connection_watch_state(sd_client* client, sd_connection_watch_state* state);
SD_API sd_result SD_CALL sd_config_init(sd_config* config);
SD_API sd_result SD_CALL sd_display_options_init(sd_display_options* options);
SD_API sd_result SD_CALL sd_client_create(const sd_config* config, sd_client** out_client);
SD_API sd_result SD_CALL sd_client_destroy(sd_client* client);
/* Select while disconnected; retained across reconnect. WebSocket uses the
 * service's GET / HTTP upgrade on the same port and browser client type 1.
 * This legacy setter accepts TCP/WEBSOCKET only. Select USB through SASettings
 * connection.transport=1; get_transport then returns SD_TRANSPORT_USB (2).
 * All transports share frame/input APIs. TLS is configured through SASettings. */
SD_API sd_result SD_CALL sd_client_set_transport(sd_client* client, uint32_t transport);
SD_API sd_result SD_CALL sd_client_get_transport(sd_client* client, uint32_t* transport);
SD_API sd_result SD_CALL sd_client_get_display_options(sd_client* client, sd_display_options* options);
/* Identification preferences, accepted only in DISCONNECTED state. An active
 * or pending connection returns SD_INVALID_ARGUMENT with a diagnostic and is
 * preserved. No settings packet, disconnect or implicit reconnect is sent.
 * After explicit disconnect, changes are sent by the next connect/start. */
SD_API sd_result SD_CALL sd_client_set_display_options(sd_client* client, const sd_display_options* options);
/* IPv4 literal; port 0 selects 28252. Synchronous convenience wrapper around
 * native Connect/Start tasks. OK means TCP established and identification task
 * completed; asynchronous worker errors are reported by poll. It does not mean
 * the server has accepted the requested display yet. */
SD_API sd_result SD_CALL sd_client_connect(sd_client* client, const char* ipv4, uint16_t port);
/* Queues the original native Connect task; drive completion/reconnect using poll.
 * auto_reconnect=1 uses the native AUTO_CONNECT/reconnect state transitions.
 * Returns SD_PENDING when queued. No background callback enters application code. */
SD_API sd_result SD_CALL sd_client_start(sd_client* client, const char* ipv4, uint16_t port, uint32_t auto_reconnect);
/* USB protocol startup on a caller-supplied, already connected peripheral stream.
 * Requires SASettings connection.transport=1, WebSocket/TLS disabled, display
 * client type0, base compression3, codec3/4/5. session_type is derived as4/6 from
 * audio.enabled. No automatic reuse after detach: pass a fresh stream to restart.
 * All usual frame/GPU/input APIs are shared. Audio uses the same stream and its
 * configuration at startup; audio_start/stop cannot renegotiate it while active.
 * Returns PENDING after accepting callbacks. Owner poll reports startup/events.
 */
SD_API sd_result SD_CALL sd_client_start_usb(sd_client* client, const sd_usb_stream* stream);
/* APK type6 stream command, 1=PAUSE/2=PLAY, for TCP/WebSocket/TLS/USB. Supported
 * only after poll delivers the server's DISPLAY_SETTINGS (type4) announcement.
 * Without it returns SD_UNSUPPORTED without sending or closing the connection;
 * while disconnected/reconnecting returns SD_IGNORED. The application owns
 * pause/resume lifecycle timing; frame ACK, ping and input remain independent.
 * Announcement survives automatic reconnect to the same session, as in the APK
 * Activity. A new start/connect or terminal disconnect resets it. */
SD_API sd_result SD_CALL sd_client_set_stream_state(sd_client* client, uint32_t state);
SD_API sd_result SD_CALL sd_client_get_stream_control_state(sd_client* client, sd_stream_control_state* state);
SD_API sd_result SD_CALL sd_client_get_state(sd_client* client, uint32_t* state);
/* Change transport-error reconnect policy without changing the current FSM
 * state. A manual first attempt can start with auto_reconnect=0, then enable
 * this policy: initial failure stays terminal; loss after connection retries.
 * A later start/connect call selects its own initial policy again. */
SD_API sd_result SD_CALL sd_client_set_auto_reconnect(sd_client* client, uint32_t enabled);
SD_API sd_result SD_CALL sd_client_set_fullscreen(sd_client* client, uint32_t enabled);
SD_API sd_result SD_CALL sd_client_disconnect(sd_client* client);
SD_API sd_result SD_CALL sd_client_disconnect_with_reason(sd_client* client, uint32_t reason);
/* Processes at most one state/control/frame event; timeout 0 is nonblocking.
 * Worker threads retain partial TCP reads independently of polling.
 * One frame event per presentation (1..4 JPEG rectangles). Flow control retires
 * pending groups, including incomplete groups interrupted by ping.
 * Decode/present/ACK and ping echo do not require polling. Unread frame events
 * coalesce to the newest snapshot; sequence gaps are normal. Control packets
 * may be delivered before an earlier frame finishes decoding. USB keeps a
 * separate JPEG counter: ping does not retire/reset a partial USB frame group.
 * CONNECTION_STATE uses event.value for the recovered SA_FSTM_STATE.
 * Auto-reconnect reports recoverable connection loss as CONNECTION_STATE;
 * fatal protocol/decoder errors still return a negative result.
 * Initialize output.struct_size to sizeof(output) before poll/get_frame. */
SD_API sd_result SD_CALL sd_client_poll(sd_client* client, uint32_t timeout_ms, sd_event* output);
SD_API sd_result SD_CALL sd_client_get_frame(sd_client* client, sd_frame* output);
SD_API sd_result SD_CALL sd_client_get_source_format(sd_client* client, sd_source_format* output);
/* Native mouse input uses relative device deltas x/y, SD_MOUSE_MOVE and flags=0.
 * Use zero deltas for button-only/wheel events. Raw flags are forwarded as given;
 * framebuffer pixel coordinates must not be passed as relative motion.
 * button_data is the signed wheel delta (normally +/-120).
 * Input is routed through native FSM states 3/4/6. SD_OK means queued;
 * other states return SD_IGNORED. Worker send errors are reported by poll. */
SD_API sd_result SD_CALL sd_client_send_mouse(sd_client* client, int32_t x, int32_t y,
    int32_t button_data, uint32_t button_flags, uint32_t flags);
/* Default target: whole primary desktop, for a stream mirroring it. For an
 * extended monitor supply its bounds and the full virtual desktop bounds.
 * Automatic discovery of remote desktop/monitor bounds is not implemented.
 * Finish active drags before changing targets. */
SD_API sd_result SD_CALL sd_mouse_target_init(sd_mouse_target* target);
SD_API sd_result SD_CALL sd_client_set_mouse_target(sd_client* client, const sd_mouse_target* target);
SD_API sd_result SD_CALL sd_client_send_mouse_absolute(sd_client* client, float x, float y,
    uint32_t surface_width, uint32_t surface_height, int32_t button_data, uint32_t button_flags);
SD_API sd_result SD_CALL sd_client_send_keyboard(sd_client* client, uint32_t virtual_key,
    uint32_t scan_code, uint32_t flags);
/* Live packet capability gates. Release held raw keys/contacts in the caller
 * before disabling them; the SDK retires its own stateful touch timer/map. */
SD_API sd_result SD_CALL sd_client_set_input_enabled(sd_client* client, uint32_t input_type, uint32_t enabled);
/* Live APK touch-packet interpretation: 0=touchscreen, 1=touchpad. Finish the
 * current physical gesture before switching. Does not change mouse emulation. */
SD_API sd_result SD_CALL sd_client_set_touch_input_type(sd_client* client, uint32_t type);
/* Stateful APK hardware-key path. Requires client.os=3 and Android keycodes.
 * Ordinary DOWN first emits CapsLock PRESS if the source CapsLock bit changed;
 * CapsLock itself emits one PRESS on UP. MULTIPLE/CapsLock DOWN emit nothing.
 * UI/popup/suspend/SonarPen interception belongs to the caller. Software text
 * edits use send_text_utf16; do not feed their synthesized events here as well.
 * Retained source state survives transport reconnect, as in the APK Activity.
 * This does not query or report the remote desktop's actual CapsLock state. */
SD_API sd_result SD_CALL sd_client_send_android_key(sd_client* client, const sd_android_key_event* event);
/* Reset when attaching a new input source (APK S1 constructor state). No packet
 * is sent; ordinary transport reconnect does not require this reset. */
SD_API sd_result SD_CALL sd_client_reset_android_keyboard(sd_client* client);
/* APK KeyEventUnicodeSend: one UTF-16 code unit in +12, virtual_key=0, flags=4.
 * NUL is ignored as in the APK. Surrogate units are sent separately, in order. */
SD_API sd_result SD_CALL sd_client_send_unicode(sd_client* client, uint32_t utf16_unit);
/* APK text edit: delete a trailing suffix, then append UTF-16 text. Both counts
 * are bounded to 4096. Embedded NUL is ignored; lengths count UTF-16 units, not
 * graphemes. Deletion assumes the remote caret is at the end of the retained text.
 * Android OS3 uses original OSK keycodes/flags for Backspace/space/LF. Windows
 * OS1/2 uses corresponding native key down/up packets; other units use Unicode.
 * This API does not retain an editable/IME history or manipulate application UI.
 * SD_OK means queued, not acknowledged/transactional insertion on the server.
 */
SD_API sd_result SD_CALL sd_client_send_text_utf16(sd_client* client, const uint16_t* text,
    uint32_t length, uint32_t backspaces_before);
SD_API sd_result SD_CALL sd_client_send_touch(sd_client* client, const sd_touch_event* event);
/* APK W1 / SAMotionEvent path. input.touch_type=0 sends touchscreen flags and
 * up to seven ordered contacts; 1 sends primary-only touchpad flags. Both use
 * protocol type12 (gesture interpretation belongs to the server). No SDK timer.
 * timestamp_ms is the low32 bits of Unix wall-clock milliseconds; wire uses low16.
 * Do not mix with raw send_touch or UWP send_pointer TOUCH for one gesture.
 * Invalid arguments do not modify the session; ignored actions/mapping return IGNORED.
 */
SD_API sd_result SD_CALL sd_client_send_touch_input(sd_client* client, const sd_touch_input* input);
/* Original APK SAMotionEvent mapping, independent of HWND/DPI/GPU state.
 * map_point receives coordinates after MotionEvent's float-to-int truncation.
 * On IGNORED (outside the letterbox band), output coordinates stay unchanged.
 * Original inclusive edges, integer half sizes and no general viewport clamp
 * are retained. This is separate from UWP renderer's map_pointer_sample. */
SD_API sd_result SD_CALL sd_motion_geometry_init(sd_motion_geometry* geometry);
SD_API sd_result SD_CALL sd_motion_map_point(const sd_motion_geometry* geometry,
    int32_t x,int32_t y,int32_t* mapped_x,int32_t* mapped_y);
/* Raw viewport contacts -> APK mapping -> primary/list fallback -> W1 packet.
 * Input dimensions must equal geometry viewport dimensions. mapped=0 marks an
 * unavailable source contact. Do not pre-map coordinates with another route.
 * Orientation only affects mapping here; it does not send a rotation command. */
SD_API sd_result SD_CALL sd_client_send_touch_viewport(sd_client* client,
    const sd_motion_geometry* geometry,const sd_touch_input* input);
SD_API sd_result SD_CALL sd_client_send_pen(sd_client* client, const sd_pen_event* event);
enum { SD_PEN_MOTION_CONTACT=0, SD_PEN_MOTION_HOVER=1 };
/* Original APK V1 contact/hover builders after SAMotionEvent viewport mapping.
 * Accepts the same event value returned by sd_sonar_process_motion; no Sonar
 * session is required. Caller selects pen routing and contact vs hover.
 * Preserves pointer-zero pressure (without clipping), raw action and Java
 * float-to-int coordinate conversion, including fallback to another contact's
 * mapped coordinates. No action-index decoding is needed on this route.
 * Contact is zero only for raw action 1; hover always sets
 * in_range=1 with zero pressure/contact. No mapped point returns SD_IGNORED. */
SD_API sd_result SD_CALL sd_client_send_pen_viewport(sd_client* client,
    const sd_motion_geometry* geometry,const sd_motion_event* input,uint32_t kind);
/* APK V1.c + SAActivityDisplay.o1: released=0/1 builds mouse right DOWN/UP,
 * dispatched through the original keyboard FSM event. The SDK applies its
 * existing input.keyboard gate to that event. The caller decides whether to
 * consume the headset key. */
SD_API sd_result SD_CALL sd_client_send_pen_button(sd_client* client,uint32_t released);
enum { SD_MOTION_OUTPUT_TOUCH=0,SD_MOTION_OUTPUT_PEN=1,SD_MOTION_OUTPUT_PEN_HOVER=2 };
typedef struct sd_motion_surface {
    uint32_t struct_size,width,height,mapped_mask,timestamp_ms;
} sd_motion_surface;
/* Motion coordinates already mapped by the caller/SDK renderer. Mask bits use
 * pointer-index order; zero bits retain SAMotionEvent's mapping-failure rules.
 * Selects original W1/V1 builders, including pointer-zero pressure after fallback.
 * Up to 16 input points; W1 still serializes at most seven contacts. No geometry
 * transform is repeated. timestamp_ms is the wrapping W1 wallclock timestamp. */
SD_API sd_result SD_CALL sd_client_send_mapped_motion(sd_client* client,uint32_t output,
    const sd_motion_surface* surface,const sd_motion_event* input);
SD_API sd_result SD_CALL sd_pointer_sample_init(sd_pointer_sample* sample);
/* Optional recovered SAMouseOutput/SAPenOutput/SATouchOutput stateful layer.
 * Do not mix raw send_* and this layer for one physical pointer stream.
 * Touch stores up to seven contacts sorted by id (0..65535), truncates mapped
 * float coordinates toward zero, and sends native timer updates every 25 ms.
 * Drive poll while touch is active; timer updates run on the owner thread.
 * Unsupported events/states return SD_IGNORED. SD_OK may mean state was updated
 * without immediate transmission (e.g. touch waits for the next timer tick).
 */
SD_API sd_result SD_CALL sd_client_send_pointer(sd_client* client, uint32_t output_type,
    uint32_t event_type, const sd_pointer_sample* sample);
/* Native touch constructor flag; change only with no active/stored contacts.
 * 0: timer flags 2. 1: timer flags 1; mode bit also marks primary DOWN/UP.
 * No unproven UI meaning is assigned to the flag. */
SD_API sd_result SD_CALL sd_client_set_touch_mode(sd_client* client, uint32_t mode);
/* Recreate logical pointer state: active touch sends final UP; mouse/pen state
 * is cleared as in their constructors. The frontend owns mouse/key releases. */
SD_API sd_result SD_CALL sd_client_reset_pointer_output(sd_client* client);
SD_API sd_result SD_CALL sd_viewport_init(sd_viewport* viewport);
SD_API sd_result SD_CALL sd_client_set_viewport(sd_client* client, const sd_viewport* viewport);
SD_API sd_result SD_CALL sd_client_get_view_state(sd_client* client, sd_view_state* state);
/* Native deltas: zoom changes only if the result is >0.05; each pan axis changes
 * only inside (-1,1); rotate adds X/Y and subtracts Z, in radians. */
SD_API sd_result SD_CALL sd_client_view_zoom(sd_client* client, float delta);
SD_API sd_result SD_CALL sd_client_view_pan(sd_client* client, float x, float y);
SD_API sd_result SD_CALL sd_client_view_rotate(sd_client* client, float x, float y, float z);
/* APK Q2 texture orientation: 1 identity, 2 clockwise90, 3 180, 4 clockwise270.
 * Changes aspect-fit and source-coordinate mapping together. Independent of
 * incremental 3D view_rotate and remote set_rotation; does not send input.
 * Retained across framebuffer size changes, view_reset and renderer reattach. */
SD_API sd_result SD_CALL sd_client_view_set_orientation(sd_client* client, uint32_t orientation);
SD_API sd_result SD_CALL sd_client_view_get_orientation(sd_client* client, uint32_t* orientation);
SD_API sd_result SD_CALL sd_client_view_reset(sd_client* client);
/* Map sample.x/y from logical viewport coordinates using the original 3D path.
 * Preserves all other input properties; writes surface size and mapped flag.
 * SD_OK also covers points outside the image (mapped=0, x/y retained). Such
 * events must still reach send_pointer so release/fallback branches can run.
 * SD_NO_FRAME before an image is delivered. Frame-size changes reset transforms;
 * ordinary window/DPI changes retain them. */
SD_API sd_result SD_CALL sd_client_map_pointer_sample(sd_client* client, sd_pointer_sample* sample);
/* Optional native D3D11 renderer. Default config: offscreen, hardware, sync=1.
 * Draw/present runs in the framebuffer worker before ACK, without owner poll.
 * View APIs use the actual GPU view while attached; CPU frame views still keep
 * their existing immutable owner-poll contract. HWND/panel lifetime belongs to
 * the caller: detach before destroying it. Attach/detach run on the owner thread.
 * A GPU failure emits RENDERER_ERROR (event.value is HRESULT) and stops GPU work;
 * CPU frames/network continue. Reattach to recreate the device/resources.
 */
SD_API sd_result SD_CALL sd_renderer_config_init(sd_renderer_config* config);
SD_API sd_result SD_CALL sd_client_renderer_attach(sd_client* client, const sd_renderer_config* config);
SD_API sd_result SD_CALL sd_client_renderer_detach(sd_client* client);
SD_API sd_result SD_CALL sd_client_renderer_get_state(sd_client* client, sd_renderer_state* state);
SD_API sd_result SD_CALL sd_client_renderer_present(sd_client* client);
/* GPU output readback: buffer=NULL/capacity=0 queries dimensions. Otherwise
 * capacity must hold stride*height. Frame pixels points to caller's buffer and
 * sequence is the GPU presentation sequence. Returns SD_NO_FRAME before an image.
 */
SD_API sd_result SD_CALL sd_client_renderer_read_pixels(sd_client* client, uint8_t* buffer,
    uint64_t capacity, sd_frame* frame);
/* Immutable GPU snapshot, independent of client lifetime. Contains a shared NT
 * texture with keyed mutex. Native pointers/handle returned below are borrowed
 * until destroy. External readers acquire key 1 and release key 1. No renderer
 * or client context should be mutated through these resources.
 */
SD_API sd_result SD_CALL sd_client_renderer_copy_texture(sd_client* client, sd_gpu_texture** texture);
SD_API sd_result SD_CALL sd_gpu_texture_get_native(sd_gpu_texture* texture, void** d3d11_texture, void** shared_handle);
SD_API sd_result SD_CALL sd_gpu_texture_read_pixels(sd_gpu_texture* texture, uint8_t* buffer,
    uint64_t capacity, sd_frame* frame);
SD_API sd_result SD_CALL sd_gpu_texture_destroy(sd_gpu_texture* texture);
/* Separate native TCP audio session using the current display identity/address.
 * Defaults capture PCM in a bounded 2-second queue; playback is opt-in.
 * Wire format is the UWP profile: 48000 Hz, stereo S16_LE. No audio ACK.
 * probe_server checks discovery feature bit 8; no UDP reply is treated as unknown.
 * Capture queue 0 disables capture (requires playback=1), otherwise 10..10000 ms.
 * A full capture queue applies backpressure, never silently drops audio.
 * Start requires an established display session and returns SD_PENDING. Audio
 * errors do not stop display. Enabled audio restarts after display reconnect;
 * stop disables that preference. Calls belong to the client's owner thread.
 */
SD_API sd_result SD_CALL sd_audio_config_init(sd_audio_config* config);
SD_API sd_result SD_CALL sd_client_audio_start(sd_client* client, const sd_audio_config* config);
SD_API sd_result SD_CALL sd_client_audio_stop(sd_client* client);
SD_API sd_result SD_CALL sd_client_audio_get_state(sd_client* client, sd_audio_state* state);
/* Snapshot of an explicit type8 packet on the separate audio connection.
 * received distinguishes server reason zero from no packet; unknown values are
 * retained. Stop preserves it; a new audio or display start clears it. EOF,
 * local Stop and I/O errors do not invent server reasons. USB multiplexed
 * disconnect belongs to the display event and does not set this snapshot. */
SD_API sd_result SD_CALL sd_client_audio_get_disconnect_state(sd_client* client,sd_audio_disconnect_state* state);
/* Audio data is immutable until the next successful audio_poll, audio stop,
 * display reconnect or client destruction. Large packets may span buffers with
 * the same sequence; frame_index is the stream offset. Copy before another thread.
 * Playback-only sessions expose state/errors here but no PCM buffers.
 */
SD_API sd_result SD_CALL sd_client_audio_poll(sd_client* client, uint32_t timeout_ms, sd_audio_buffer* buffer);
/* rotation is the unmodified protocol orientation value; no degrees conversion. */
SD_API sd_result SD_CALL sd_client_set_rotation(sd_client* client, uint32_t rotation);
/* Last diagnostic for this handle, UTF-8, never free. Read on the owner thread.
 * A wrong-thread call returns SD_WRONG_THREAD without changing this string. */
SD_API const char* SD_CALL sd_client_last_error(const sd_client* client);

#ifdef __cplusplus
}
#endif
#endif
