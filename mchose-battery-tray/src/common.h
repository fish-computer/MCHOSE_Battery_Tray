// common.h - shared helpers (logging, paths, strings, DPI)
#pragma once

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <objidl.h>   // IStream: required by <gdiplus.h>
#include <string>
#include <cwchar>
#include <cstdarg>
#include <cstdio>
#include <vector>

namespace mchose {

// ---------------------------------------------------------------- constants
inline constexpr const wchar_t* kAppTitle = L"MCHOSE 键盘电量";
inline constexpr const wchar_t* kAppId = L"MCHOSEBatteryTray";
inline constexpr const wchar_t* kAppVersion = L"1.0.0";
inline constexpr const wchar_t* kMutexName = L"Local\\MCHOSE_BatteryTray_SingleInstance";
inline constexpr const wchar_t* kQuitEventName = L"Local\\MCHOSE_BatteryTray_Quit";
inline constexpr const wchar_t* kRunValue = L"MCHOSEBatteryTray";
inline constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MCHOSE_BatteryTray";

// ---------------------------------------------------------------- logging
inline std::wstring LogPath()
{
    // MCHOSE_LOG overrides the location (handy when testing or when the
    // default folder is read-only).
    wchar_t override_[MAX_PATH * 2] = {};
    DWORD on = GetEnvironmentVariableW(L"MCHOSE_LOG", override_, MAX_PATH * 2);
    if (on > 0 && on < MAX_PATH * 2)
        return override_;

    wchar_t dir[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        GetTempPathW(MAX_PATH, dir);
    std::wstring p(dir);
    p += L"\\MCHOSE Battery Tray";
    CreateDirectoryW(p.c_str(), nullptr);
    p += L"\\log.txt";
    return p;
}

inline void Log(const wchar_t* fmt, ...)
{
    wchar_t buf[2048] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);   // truncate, never abort
    va_end(ap);

    // keep the log from growing forever
    std::wstring path = LogPath();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
    {
        ULARGE_INTEGER sz;
        sz.LowPart = fad.nFileSizeLow;
        sz.HighPart = fad.nFileSizeHigh;
        if (sz.QuadPart > 512 * 1024)
            DeleteFileW(path.c_str());
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"a, ccs=UTF-8") == 0 && f)
    {
        fwprintf(f, L"[%04d-%02d-%02d %02d:%02d:%02d] %ls\n", st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, buf);
        fclose(f);
    }
}

// ---------------------------------------------------------------- paths
inline std::wstring ExePath()
{
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

inline std::wstring ExeDir()
{
    std::wstring p = ExePath();
    size_t pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

inline bool FileExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Install locations, in order of preference. The first one that exists (or can
// be created) wins; the others are only used when Windows refuses the usual
// %LOCALAPPDATA%\Programs (locked-down machines, odd ACLs, roaming profiles).
inline std::vector<std::wstring> InstallDirCandidates()
{
    std::vector<std::wstring> dirs;
    wchar_t local[MAX_PATH] = {};
    wchar_t roaming[MAX_PATH] = {};
    const DWORD nLocal = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    const DWORD nRoaming = GetEnvironmentVariableW(L"APPDATA", roaming, MAX_PATH);

    if (nLocal && nLocal < MAX_PATH)
    {
        dirs.push_back(std::wstring(local) + L"\\Programs\\MCHOSEBatteryTray");
        dirs.push_back(std::wstring(local) + L"\\MCHOSEBatteryTray");
    }
    if (nRoaming && nRoaming < MAX_PATH)
        dirs.push_back(std::wstring(roaming) + L"\\MCHOSEBatteryTray");
    dirs.push_back(ExeDir() + L"\\MCHOSEBatteryTray");
    return dirs;
}

inline bool DirExists(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// %LOCALAPPDATA%\Programs\MCHOSEBatteryTray (or whichever candidate is in use)
inline std::wstring InstallDir()
{
    const std::vector<std::wstring> dirs = InstallDirCandidates();
    for (const std::wstring& d : dirs)
    {
        if (DirExists(d) && FileExists(d + L"\\MCHOSE Battery Tray.exe"))
            return d;
    }
    for (const std::wstring& d : dirs)
    {
        if (DirExists(d))
            return d;
    }
    return dirs.empty() ? ExeDir() : dirs.front();
}

inline std::wstring InstalledExe()
{
    return InstallDir() + L"\\MCHOSE Battery Tray.exe";
}

// ---------------------------------------------------------------- misc
inline void EnableDpiAwareness()
{
    using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (auto fn = (Fn)GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
        fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

inline int TrayIconSize()
{
    int size = GetSystemMetrics(SM_CXSMICON);
    return size > 0 ? size : 16;
}

inline std::wstring Format(const wchar_t* fmt, ...)
{
    wchar_t buf[2048] = {};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

// Lets a portable copy (MCHOSE_INSTANCE=dev) live beside the installed one
// without tripping the single-instance guard or stealing its quit event.
inline std::wstring InstanceName(const wchar_t* base)
{
    wchar_t over[128] = {};
    if (GetEnvironmentVariableW(L"MCHOSE_INSTANCE", over, 128) > 0)
        return std::wstring(base) + L"_" + over;
    return base;
}

} // namespace mchose
