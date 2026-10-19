// main.cpp - MCHOSE keyboard battery indicator for the Windows taskbar
//
// The tray icon shows the current percentage as a rounded badge, the menu
// carries the device state, refresh, hub launcher, autostart, install and exit.
//
// Usage:
//   MchoseTray.exe                 run (offers to install on first launch)
//   MchoseTray.exe --tray          run directly, no prompts
//   MchoseTray.exe --install       install for the current user and start
//   MchoseTray.exe --uninstall     remove everything
//   MchoseTray.exe --probe         dump every HID collection (bug reports)
//   MchoseTray.exe --once          print one reading to stdout

#include "common.h"
#include "hid.h"
#include "icon.h"
#include "install.h"
#include "protocol.h"

#include <gdiplus.h>
#include <shellapi.h>
#include <atomic>
#include <thread>
#include <memory>
#include <cstring>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

using namespace mchose;

namespace {

constexpr UINT WM_APP_BATTERY = WM_APP + 1;
constexpr UINT WM_APP_REFRESH = WM_APP + 2;
constexpr UINT_PTR ID_TRAY = 1;
constexpr UINT_PTR TIMER_IDLE = 11;
constexpr UINT_PTR TIMER_TRAY_RETRY = 12;
constexpr UINT_PTR TIMER_DIAG = 13;

enum MenuId : UINT
{
    ID_MENU_HEADER = 100,
    ID_MENU_BATTERY,
    ID_MENU_REFRESH,
    ID_MENU_HUB,
    ID_MENU_AUTOSTART,
    ID_MENU_INSTALL,
    ID_MENU_UNINSTALL,
    ID_MENU_HELP,
    ID_MENU_LOG,
    ID_MENU_EXIT,
};

HWND g_hwnd = nullptr;
HICON g_icon = nullptr;
Reading g_reading;
ULONGLONG g_lastQueryMs = 0;
ULONGLONG g_lastOkMs = 0;
HANDLE g_refreshEvent = nullptr;
HANDLE g_stopEvent = nullptr;
std::thread g_worker;
std::atomic<DWORD> g_lastIdleSec{ 0 };
bool g_idlePrimed = false;
UINT g_taskbarCreatedMsg = 0;
DWORD g_nimError = ERROR_SUCCESS;
int g_nimFailures = 0;
bool g_diag = false;

// ---------------------------------------------------------------- tray icon
void UpdateTrayIcon()
{
    const bool known = g_reading.ok;
    HICON fresh = RenderBadgeIcon(known ? g_reading.percent : -1, TrayIconSize(), g_reading.charging);
    if (!fresh)
        return;

    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_APP_REFRESH;
    nid.hIcon = fresh;

    std::wstring tip;
    if (known)
    {
        tip = Format(L"MCHOSE 键盘 %d%%", g_reading.percent);
        if (g_reading.charging)
            tip += L" (充电中)";
    }
    else
    {
        tip = L"MCHOSE 键盘电量：未连接";
    }
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

    if (g_icon)
    {
        nid.uFlags = NIF_ICON | NIF_TIP;   // modify path
        SetLastError(0);
        if (!Shell_NotifyIconW(NIM_MODIFY, &nid))
        {
            g_nimError = GetLastError();
            Log(L"tray: NIM_MODIFY failed (err=%lu)", g_nimError);
        }
        DestroyIcon(g_icon);
    }
    else
    {
        SetLastError(0);
        const BOOL added = Shell_NotifyIconW(NIM_ADD, &nid);
        g_nimError = added ? ERROR_SUCCESS : GetLastError();
        Log(L"tray: NIM_ADD %ls (err=%lu, icon=%dpx, hwnd=%p)", added ? L"ok" : L"FAILED",
            g_nimError, TrayIconSize(), (void*)g_hwnd);
        if (!added)
        {
            // explorer may not be ready yet during logon: retry a few times
            DestroyIcon(fresh);
            if (++g_nimFailures <= 5 && g_hwnd)
                SetTimer(g_hwnd, TIMER_TRAY_RETRY, 2000, nullptr);
            return;
        }
        g_nimFailures = 0;

        // Where did the shell actually put the icon? (useful for diagnostics:
        // a rect is returned only when the icon is visible in the tray area)
        NOTIFYICONIDENTIFIER ident = {};
        ident.cbSize = sizeof(ident);
        ident.hWnd = g_hwnd;
        ident.uID = ID_TRAY;
        RECT rc = {};
        HRESULT hr = Shell_NotifyIconGetRect(&ident, &rc);
        if (SUCCEEDED(hr))
            Log(L"tray: rect l=%ld t=%ld r=%ld b=%ld", rc.left, rc.top, rc.right, rc.bottom);
        else
            Log(L"tray: rect unavailable hr=0x%08lX (icon folded into the overflow flyout)",
                (unsigned long)hr);
    }
    g_icon = fresh;
}

void RemoveTrayIcon()
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = ID_TRAY;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    if (g_icon)
    {
        DestroyIcon(g_icon);
        g_icon = nullptr;
    }
}

// ---------------------------------------------------------------- worker
void RequestRefresh()
{
    if (g_refreshEvent)
        SetEvent(g_refreshEvent);
}

void WorkerLoop()
{
    HANDLE waits[2] = { g_refreshEvent, g_stopEvent };

    // First attempt right away, then adapt.
    ULONGLONG nextDue = GetTickCount64();
    while (true)
    {
        ULONGLONG now = GetTickCount64();
        DWORD waitMs = 1000;
        if (nextDue > now)
        {
            ULONGLONG delta = nextDue - now;
            waitMs = delta > 30000 ? 30000 : (DWORD)delta;
        }
        else
        {
            waitMs = 0;
        }

        DWORD w = WaitForMultipleObjects(2, waits, FALSE, waitMs);
        if (w == WAIT_OBJECT_0 + 1)
            break;   // stop

        now = GetTickCount64();
        const bool forced = (w == WAIT_OBJECT_0);
        if (!forced && now < nextDue)
            continue;   // spurious wake, keep waiting

        Reading r;
        const ULONGLONG started = GetTickCount64();
        bool ok = QueryBattery(r);
        const ULONGLONG spent = GetTickCount64() - started;
        if (spent > 4000)
            Log(L"worker: query took %llu ms", spent);

        if (ok && g_refreshEvent)
        {
            auto* payload = new Reading(r);
            PostMessageW(g_hwnd, WM_APP_BATTERY, 1, (LPARAM)payload);
            g_lastOkMs = GetTickCount64();
            nextDue = g_lastOkMs + 300000;   // 5 min while everything works
        }
        else
        {
            PostMessageW(g_hwnd, WM_APP_BATTERY, 0, 0);
            nextDue = GetTickCount64() + 15000;   // retry every 15 s
        }
        g_lastQueryMs = GetTickCount64();
    }
}

// ---------------------------------------------------------------- MCHOSE HUB
// Looks for the HUB launcher inside one directory.
std::wstring HubExeIn(const std::wstring& dir)
{
    WIN32_FIND_DATAW inner = {};
    HANDLE h2 = FindFirstFileW((dir + L"\\*.exe").c_str(), &inner);
    if (h2 == INVALID_HANDLE_VALUE)
        return L"";

    std::wstring found, best;
    do
    {
        const std::wstring name = inner.cFileName;
        if (name.find(L"HUB") == std::wstring::npos)
            continue;
        if (name.find(L"Uninstall") != std::wstring::npos ||
            name.find(L"Watcher") != std::wstring::npos ||
            name.find(L"Maintenance") != std::wstring::npos)
            continue;
        const std::wstring candidate = dir + L"\\" + name;
        if (found.empty())
            found = candidate;
        // "MCHOSE HUB.exe" beats "... Launcher.exe" and friends.
        if (name.find(L"Launcher") == std::wstring::npos)
        {
            best = candidate;
            break;
        }
    } while (FindNextFileW(h2, &inner));
    FindClose(h2);
    return best.empty() ? found : best;
}

void CollectInstallLocations(HKEY root, std::vector<std::wstring>& out)
{
    for (const wchar_t* sub :
         { L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
           L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" })
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_ENUMERATE_SUB_KEYS, &key) != ERROR_SUCCESS)
            continue;
        DWORD index = 0;
        wchar_t name[512];
        while (RegEnumKeyW(key, index++, name, 512) == ERROR_SUCCESS)
        {
            HKEY item = nullptr;
            if (RegOpenKeyExW(key, name, 0, KEY_QUERY_VALUE, &item) != ERROR_SUCCESS)
                continue;
            auto readValue = [item](const wchar_t* value) -> std::wstring
            {
                wchar_t buf[1024] = {};
                DWORD bytes = sizeof(buf);
                DWORD type = 0;
                if (RegQueryValueExW(item, value, nullptr, &type, (BYTE*)buf, &bytes) != ERROR_SUCCESS)
                    return L"";
                if (type != REG_SZ && type != REG_EXPAND_SZ)
                    return L"";
                return buf;
            };
            const std::wstring display = readValue(L"DisplayName");
            if (display.find(L"MCHOSE") != std::wstring::npos ||
                display.find(L"Mchose") != std::wstring::npos ||
                (display.find(L"迈从") != std::wstring::npos && display.find(L"HUB") != std::wstring::npos))
            {
                std::wstring location = readValue(L"InstallLocation");
                if (!location.empty())
                    out.push_back(location);
            }
            RegCloseKey(item);
        }
        RegCloseKey(key);
    }
}

std::wstring FindHubExe()
{
    std::vector<std::wstring> roots;
    for (const wchar_t* env : { L"ProgramFiles", L"ProgramFiles(x86)", L"LOCALAPPDATA" })
    {
        wchar_t buf[MAX_PATH] = {};
        if (GetEnvironmentVariableW(env, buf, MAX_PATH))
            roots.push_back(buf);
    }
    roots.push_back(L"C:\\Program Files");
    roots.push_back(L"C:\\Program Files (x86)");

    for (const std::wstring& root : roots)
    {
        WIN32_FIND_DATAW fd = {};
        std::wstring pattern = root + L"\\*MCHOSE*";
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                continue;
            const std::wstring exe = HubExeIn(root + L"\\" + fd.cFileName);
            if (!exe.empty())
            {
                FindClose(h);
                return exe;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Installed somewhere unusual (another drive): ask the uninstall entries.
    std::vector<std::wstring> locations;
    CollectInstallLocations(HKEY_CURRENT_USER, locations);
    CollectInstallLocations(HKEY_LOCAL_MACHINE, locations);
    for (const std::wstring& dir : locations)
    {
        const std::wstring exe = HubExeIn(dir);
        if (!exe.empty())
            return exe;
    }
    return L"";
}

// ---------------------------------------------------------------- menu
std::wstring RelTime()
{
    if (!g_lastOkMs)
        return L"尚未读取";
    ULONGLONG sec = (GetTickCount64() - g_lastOkMs) / 1000;
    if (sec < 60)
        return Format(L"%llu 秒前更新", sec);
    if (sec < 3600)
        return Format(L"%llu 分钟前更新", sec / 60);
    return Format(L"%llu 小时前更新", sec / 3600);
}

void ShowMenu()
{
    HMENU menu = CreatePopupMenu();

    std::wstring header = g_reading.ok ? g_reading.source : L"MCHOSE 键盘";
    if (header.empty())
        header = L"MCHOSE 键盘";

    std::wstring battery;
    if (g_reading.ok)
    {
        battery = Format(L"%d%%", g_reading.percent);
        if (g_reading.charging)
            battery += L"  充电中";
    }
    else
    {
        battery = L"未连接";
    }

    // The first two rows are informational; they stay enabled so they render in
    // normal text colour like the official tray menu, and clicks are ignored.
    AppendMenuW(menu, MF_STRING, ID_MENU_HEADER, header.c_str());
    AppendMenuW(menu, MF_STRING, ID_MENU_BATTERY, battery.c_str());
    AppendMenuW(menu, MF_STRING | MF_DISABLED, ID_MENU_BATTERY, RelTime().c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_REFRESH, L"立即刷新");

    const std::wstring hub = FindHubExe();
    if (!hub.empty())
        AppendMenuW(menu, MF_STRING, ID_MENU_HUB, L"打开 MCHOSE HUB");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const UINT autostartFlags = MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0);
    AppendMenuW(menu, autostartFlags, ID_MENU_AUTOSTART, L"开机自启");

    if (IsInstalled())
        AppendMenuW(menu, MF_STRING, ID_MENU_UNINSTALL, L"卸载...");
    else
        AppendMenuW(menu, MF_STRING, ID_MENU_INSTALL, L"安装到本机...");

    AppendMenuW(menu, MF_STRING, ID_MENU_HELP, L"图标不见了？");
    AppendMenuW(menu, MF_STRING, ID_MENU_LOG, L"打开日志文件夹");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    const UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, g_hwnd, nullptr);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd)
    {
    case ID_MENU_REFRESH:
        RequestRefresh();
        break;
    case ID_MENU_HUB:
        ShellExecuteW(nullptr, L"open", hub.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case ID_MENU_AUTOSTART:
    {
        const bool enable = !IsAutostartEnabled();
        if (!SetAutostart(enable))
            MessageBoxW(g_hwnd, L"无法写入开机启动项（注册表被拒绝访问）。", kAppTitle, MB_ICONWARNING);
        break;
    }
    case ID_MENU_INSTALL:
    {
        std::wstring installed, error;
        if (!InstallSelf(installed, error))
        {
            MessageBoxW(g_hwnd, error.c_str(), kAppTitle, MB_ICONERROR);
            break;
        }
        MessageBoxW(g_hwnd,
                    L"已安装到本机用户目录并已加入开机启动。\n\n"
                    L"提示：Windows 11 常把新图标收进任务栏的 ^ 里，\n"
                    L"点开 ^ 把电量胶囊拖到任务栏上就能一直看见。",
                    kAppTitle, MB_ICONINFORMATION);
        break;
    }
    case ID_MENU_UNINSTALL:
    {
        if (MessageBoxW(g_hwnd, L"确定要卸载并删除开机启动项吗？", kAppTitle,
                        MB_ICONQUESTION | MB_YESNO) != IDYES)
            break;
        std::wstring error;
        if (!UninstallSelf(error))
        {
            MessageBoxW(g_hwnd, error.c_str(), kAppTitle, MB_ICONERROR);
            break;
        }
        MessageBoxW(g_hwnd, L"已卸载。程序目录将在几秒内自动删除。", kAppTitle, MB_ICONINFORMATION);
        DestroyWindow(g_hwnd);
        break;
    }
    case ID_MENU_HELP:
        MessageBoxW(g_hwnd,
                    L"Windows 11 默认会把新的托盘图标收进 ^ 溢出区。\n\n"
                    L"· 点任务栏上的 ^，把绿色电量图标拖到任务栏上即可固定；\n"
                    L"· 或者在「设置 → 个性化 → 任务栏 → 其他系统托盘图标」里把它打开。\n\n"
                    L"鼠标悬停在图标上可以看到设备名和电量。",
                    kAppTitle, MB_ICONINFORMATION);
        break;
    case ID_MENU_LOG:
        ShellExecuteW(nullptr, L"open", LogPath().substr(0, LogPath().find_last_of(L'\\')).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case ID_MENU_EXIT:
        DestroyWindow(g_hwnd);
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- window
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_APP_REFRESH:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP ||
            LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == NIN_SELECT ||
            LOWORD(lp) == NIN_KEYSELECT)
            ShowMenu();
        return 0;

    case WM_APP_BATTERY:
    {
        if (wp && lp)
        {
            std::unique_ptr<Reading> r(reinterpret_cast<Reading*>(lp));
            g_reading = *r;
        }
        else
        {
            // Query failed: keep the last good value but mark staleness by age.
            g_reading.ok = g_reading.ok && (GetTickCount64() - g_lastOkMs < 3600000);
        }
        UpdateTrayIcon();
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_TRAY_RETRY)
        {
            KillTimer(hwnd, TIMER_TRAY_RETRY);
            UpdateTrayIcon();
            return 0;
        }
        if (wp == TIMER_DIAG)
        {
            KillTimer(hwnd, TIMER_DIAG);
            std::wstring text = Format(
                L"MCHOSE Battery Tray %ls\n\n"
                L"托盘图标：%ls（错误码 %lu，已重试 %d 次）\n"
                L"窗口句柄：%p\n"
                L"图标尺寸：%d px\n"
                L"电量：%ls\n"
                L"日志：%ls\n\n"
                L"把这段信息发到 issue 里就能定位问题。",
                kAppVersion, g_icon ? L"已注册" : L"注册失败", g_nimError, g_nimFailures,
                (void*)hwnd, TrayIconSize(),
                g_reading.ok ? Format(L"%d%%", g_reading.percent).c_str() : L"未读到",
                LogPath().c_str());
            MessageBoxW(nullptr, text.c_str(), L"MCHOSE 自检", MB_ICONINFORMATION);
            return 0;
        }
        if (wp == TIMER_IDLE)
        {
            LASTINPUTINFO lii = {};
            lii.cbSize = sizeof(lii);
            if (GetLastInputInfo(&lii))
            {
                const DWORD idle = (GetTickCount() - lii.dwTime) / 1000;
                if (g_idlePrimed && g_lastIdleSec.load() >= 60 && idle <= 1)
                {
                    // The user just touched the machine: the keyboard almost
                    // certainly woke up, so this is the cheapest moment to ask.
                    if (GetTickCount64() - g_lastQueryMs > 10000)
                    {
                        Log(L"idle: wake detected, refreshing");
                        RequestRefresh();
                    }
                }
                g_lastIdleSec = idle;
                g_idlePrimed = true;
            }
        }
        return 0;

    case WM_DEVICECHANGE:
        Log(L"device change: %llu", (unsigned long long)wp);
        RequestRefresh();
        return TRUE;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_IDLE);
        RemoveTrayIcon();
        if (g_stopEvent)
            SetEvent(g_stopEvent);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    if (g_taskbarCreatedMsg && msg == g_taskbarCreatedMsg)
    {
        // Explorer restarted: the tray icon must be re-added.
        Log(L"explorer restart detected, re-adding icon");
        g_icon = nullptr;   // old icon handle died with explorer
        UpdateTrayIcon();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- modes
int RunTray()
{
    Log(L"run: enter");

    if (HANDLE quit = CreateEventW(nullptr, TRUE, FALSE, InstanceName(kQuitEventName).c_str()))
    {
        // Another instance may have signalled us; clear it for this run.
        ResetEvent(quit);
        CloseHandle(quit);
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"MCHOSEBatteryTrayWindow";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, wc.lpszClassName, kAppTitle, 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, wc.hInstance, nullptr);
    Log(L"run: window=%p err=%lu", (void*)g_hwnd, GetLastError());
    if (!g_hwnd)
    {
        Log(L"fatal: CreateWindow failed (%lu)", GetLastError());
        return 1;
    }

    g_taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_refreshEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    Log(L"run: rendering icon");
    UpdateTrayIcon();
    Log(L"run: icon pass done (nimErr=%lu, failures=%d)", g_nimError, g_nimFailures);

    g_worker = std::thread(WorkerLoop);
    SetTimer(g_hwnd, TIMER_IDLE, 1000, nullptr);
    if (g_diag)
        SetTimer(g_hwnd, TIMER_DIAG, 4000, nullptr);

    // Clean shutdown requested from another process.
    std::thread quitWatcher([]
    {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, InstanceName(kQuitEventName).c_str());
        while (WaitForSingleObject(ev, 1000) == WAIT_TIMEOUT) {}
        CloseHandle(ev);
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    });
    quitWatcher.detach();

    Log(L"tray started (icon=%dpx)", TrayIconSize());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_worker.joinable())
        g_worker.join();
    if (g_refreshEvent)
        CloseHandle(g_refreshEvent);
    if (g_stopEvent)
        CloseHandle(g_stopEvent);
    Log(L"tray stopped");
    return 0;
}

std::wstring g_outPath;

void Emit(const std::wstring& text)
{
    Log(L"%ls", text.c_str());

    if (!g_outPath.empty())
    {
        HANDLE f = CreateFileW(g_outPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                              nullptr, nullptr);
            std::string utf8((size_t)n, '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), n, nullptr,
                                nullptr);
            DWORD written = 0;
            WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
            CloseHandle(f);
        }
        return;
    }

    // GUI subsystem: borrow the parent console when there is one.
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        AllocConsole();
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE)
    {
        const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0,
                                          nullptr, nullptr);
        std::string utf8((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), utf8.data(), n, nullptr,
                            nullptr);
        DWORD written = 0;
        WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    }
}

int ProbeMode()
{
    std::wstring out;
    const std::vector<HidInterface> devices = EnumerateHid();
    out += Format(L"HID collections: %llu\n", (unsigned long long)devices.size());
    for (const HidInterface& d : devices)
    {
        out += Format(L"  vid=%04X pid=%04X up=%04X u=%04X in=%u out=%u feat=%u\n", d.vid, d.pid,
                      d.usagePage, d.usage, d.inputReportLen, d.outputReportLen, d.featureReportLen);
        out += L"    product=" + d.product + L"\n";
        out += L"    path=" + d.path + L"\n";
    }
    Reading r;
    const wchar_t* driver = nullptr;
    if (QueryBattery(r, &driver))
        out += Format(L"\nreading: %d%%%s via %s (%s)\n", r.percent,
                      r.charging ? L" charging" : L"", driver, r.source.c_str());
    else
        out += L"\nreading: none\n";
    Emit(out);
    return 0;
}

int OnceMode()
{
    Reading r;
    std::wstring out = QueryBattery(r) ? Format(L"%d\n", r.percent) : L"-1\n";
    Emit(out);
    return 0;
}

// Saves an HICON as a PNG (optionally upscaled with nearest neighbour so the
// 1:1 pixels are easy to inspect).
static int SaveIconPng(HICON icon, int px, int zoom)
{
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP dib = CreateCompatibleBitmap(screen, px, px);
    HGDIOBJ old = SelectObject(dc, dib);
    PatBlt(dc, 0, 0, px, px, BLACKNESS);
    DrawIconEx(dc, 0, 0, icon, px, px, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    ReleaseDC(nullptr, screen);

    int rc = 4;
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromHBITMAP(dib, nullptr);
    if (bmp)
    {
        Gdiplus::Bitmap* outBmp = bmp;
        Gdiplus::Bitmap* zoomed = nullptr;
        if (zoom > 1)
        {
            zoomed = new Gdiplus::Bitmap(px * zoom, px * zoom, PixelFormat32bppARGB);
            Gdiplus::Graphics zg(zoomed);
            zg.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
            zg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            zg.DrawImage(bmp, 0, 0, px * zoom, px * zoom);
            outBmp = zoomed;
        }
        CLSID png = {};
        CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);   // image/png
        if (outBmp->Save(g_outPath.c_str(), &png, nullptr) == Gdiplus::Ok)
            rc = 0;
        delete zoomed;
        delete bmp;
    }

    DeleteObject(dib);
    DeleteDC(dc);
    return rc;
}

// Writes the badge as a PNG so the look can be reviewed without a tray.
// Usage: --icon=<percent|unknown> --out=<file.png> [--size=<px>] [--scale=<n>]
int IconMode(int percent, int size, int scale, int charging)
{
    if (g_outPath.empty())
        return 2;
    const int px = size > 0 ? size : TrayIconSize();
    HICON icon = RenderBadgeIcon(percent, px, charging != 0);
    if (!icon)
        return 3;
    const int rc = SaveIconPng(icon, px, scale > 0 ? scale : 1);
    DestroyIcon(icon);
    return rc;
}

// --appicon --out=x.png --size=<px> : renders the exe icon (used to build app.ico)
int AppIconMode(int size, int scale)
{
    if (g_outPath.empty())
        return 2;
    const int px = size > 0 ? size : 64;
    HICON icon = RenderAppIcon(px);
    if (!icon)
        return 3;
    const int rc = SaveIconPng(icon, px, scale > 0 ? scale : 1);
    DestroyIcon(icon);
    return rc;
}

int InstallMode(bool quiet)
{
    std::wstring installed, error;
    if (!InstallSelf(installed, error))
    {
        if (!quiet)
            MessageBoxW(nullptr, error.c_str(), kAppTitle, MB_ICONERROR);
        return 1;
    }
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = installed.c_str();
    sei.lpParameters = L"--tray";
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
    if (!quiet)
        MessageBoxW(nullptr, (L"已安装到：\n" + installed + L"\n\n已加入开机启动。").c_str(),
                    kAppTitle, MB_ICONINFORMATION);
    return 0;
}

int UninstallMode()
{
    std::wstring error;
    const bool ok = UninstallSelf(error);
    // Never block here: a modal dialog keeps the exe image locked and stops the
    // cleanup script from deleting the program directory.
    Log(L"uninstall: %ls (err=%ls)", ok ? L"ok" : L"failed", error.c_str());
    return ok ? 0 : 1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    EnableDpiAwareness();

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartup(&token, &gdiplusInput, nullptr);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode;
    int iconPercent = -2, iconSize = 0, iconScale = 1, iconCharging = 0;
    for (int i = 1; i < argc; ++i)
    {
        std::wstring a = argv[i];
        if (a.rfind(L"--out=", 0) == 0)
            g_outPath = a.substr(6);
        else if (a.rfind(L"--icon=", 0) == 0)
        {
            const std::wstring v = a.substr(7);
            mode = L"--icon";
            iconPercent = (v == L"unknown") ? -1 : _wtoi(v.c_str());
        }
        else if (a.rfind(L"--size=", 0) == 0)
            iconSize = _wtoi(a.c_str() + 7);
        else if (a.rfind(L"--scale=", 0) == 0)
            iconScale = _wtoi(a.c_str() + 8);
        else if (a.rfind(L"--charging=", 0) == 0)
            iconCharging = _wtoi(a.c_str() + 11);
        else if (a == L"--appicon")
            mode = L"--appicon";
        else if (a == L"--install" || a == L"--uninstall" || a == L"--probe" || a == L"--once" ||
                 a == L"--tray" || a == L"--silent")
            mode = a;
        else if (a == L"--diag")
            g_diag = true;
        else if (a == L"--findhub")
            mode = L"--findhub";
    }
    LocalFree(argv);

    Log(L"boot: exe=%ls mode='%ls' diag=%d", ExePath().c_str(), mode.c_str(), g_diag ? 1 : 0);

    int rc = 0;

    if (mode == L"--icon")
    {
        rc = IconMode(iconPercent, iconSize, iconScale, iconCharging);
    }
    else if (mode == L"--appicon")
    {
        rc = AppIconMode(iconSize, iconScale);
    }
    else if (mode == L"--probe")
    {
        rc = ProbeMode();
    }
    else if (mode == L"--findhub")
    {
        const std::wstring hub = FindHubExe();
        Emit(L"hub: " + (hub.empty() ? std::wstring(L"(not found)") : hub));
        rc = 0;
    }
    else if (mode == L"--once")
    {
        rc = OnceMode();
    }
    else if (mode == L"--uninstall")
    {
        rc = UninstallMode();
    }
    else if (mode == L"--install")
    {
        rc = InstallMode(false);
    }
    else if (mode == L"--silent")
    {
        rc = InstallMode(true);
    }
    else
    {
        // Single instance: a second launch simply pokes the running one.
        // MCHOSE_INSTANCE lets a portable/dev copy run beside the installed one.
        const std::wstring instanceName = InstanceName(kMutexName);
        HANDLE mutex = CreateMutexW(nullptr, TRUE, instanceName.c_str());
        const DWORD mutexError = GetLastError();
        if (mutexError == ERROR_ALREADY_EXISTS)
        {
            if (mode != L"--tray")
                MessageBoxW(nullptr, L"MCHOSE 键盘电量已在运行（见任务栏通知区域）。", kAppTitle,
                            MB_ICONINFORMATION);
            if (mutex)
                CloseHandle(mutex);
            Gdiplus::GdiplusShutdown(token);
            return 0;
        }

        if (mode != L"--tray" && !IsInstalled())
        {
            const int answer = MessageBoxW(
                nullptr,
                L"是否将「MCHOSE 键盘电量」安装到本机？\n\n"
                L"• 安装到当前用户目录，不需要管理员权限\n"
                L"• 自动创建开机启动项与开始菜单快捷方式\n"
                L"• 可随时在托盘菜单里卸载\n\n"
                L"选「否」则只在当前目录便携运行，关掉即走（之后仍可右键安装）。",
                kAppTitle, MB_ICONQUESTION | MB_YESNO);
            if (answer == IDYES)
            {
                if (mode != L"--tray")
                    mode = L"--install";
            }
        }

        if (mode == L"--install")
        {
            rc = InstallMode(false);
        }
        else
        {
            rc = RunTray();
        }

        if (mutex)
            CloseHandle(mutex);
    }

    Gdiplus::GdiplusShutdown(token);
    return rc;
}



