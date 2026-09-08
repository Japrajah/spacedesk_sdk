#pragma once
#include <windows.h>
#include <optional>
#include <cstdint>
namespace sd::viewer {
// DEVMODE rotations are counter-clockwise; APK Q2 display rotations are CW.
inline std::optional<uint32_t> DisplayOrientation(HWND window) {
    MONITORINFOEXW monitor{};monitor.cbSize=sizeof(monitor);
    if(!GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),reinterpret_cast<MONITORINFO*>(&monitor))) return std::nullopt;
    DEVMODEW mode{};mode.dmSize=sizeof(mode);
    if(!EnumDisplaySettingsW(monitor.szDevice,ENUM_CURRENT_SETTINGS,&mode)) return std::nullopt;
    const DWORD rotation=(mode.dmFields&DM_DISPLAYORIENTATION)?mode.dmDisplayOrientation:DMDO_DEFAULT;
    if(rotation>DMDO_270) return std::nullopt;
    // APK's canonical orientations are landscape-based, including on devices
    // whose natural panel mode is portrait (Activity configuration + sensor hint).
    const bool sideways=rotation==DMDO_90 || rotation==DMDO_270;
    const DWORD natural_width=sideways?mode.dmPelsHeight:mode.dmPelsWidth;
    const DWORD natural_height=sideways?mode.dmPelsWidth:mode.dmPelsHeight;
    const uint32_t base=natural_width && natural_height && natural_width<natural_height?1:0;
    return ((base+4-rotation)%4)+1;
}
}
