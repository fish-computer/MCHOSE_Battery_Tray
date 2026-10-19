// install.cpp
#include "install.h"
#include "common.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace mchose {

static bool RegWriteString(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const DWORD bytes = (DWORD)((wcslen(value) + 1) * sizeof(wchar_t));
    LONG rc = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE*)value, bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static bool RegWriteDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

static bool RegReadString(HKEY root, const wchar_t* sub, const wchar_t* name, std::wstring& out)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    wchar_t buf[1024] = {};
    DWORD bytes = sizeof(buf);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(key, name, nullptr, &type, (BYTE*)buf, &bytes);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ))
        return false;
    out = buf;
    return true;
}

std::wstring VersionString() { return kAppVersion; }

bool IsInstalled()
{
    std::wstring exe;
    if (!RegReadString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", exe))
        return false;
    return FileExists(InstalledExe());
}

std::wstring StartMenuShortcutPath()
{
    wchar_t appdata[MAX_PATH] = {};
    GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    return std::wstring(appdata) +
           L"\\Microsoft\\Windows\\Start Menu\\Programs\\MCHOSE 键盘电量.lnk";
}

static bool CreateShortcut(const std::wstring& lnk, const std::wstring& target,
                           const std::wstring& args, const std::wstring& desc)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IShellLinkW, (void**)&link);
    if (FAILED(hr))
        return false;

    link->SetPath(target.c_str());
    if (!args.empty())
        link->SetArguments(args.c_str());
    link->SetDescription(desc.c_str());
    link->SetWorkingDirectory(ExeDir().c_str());

    bool ok = false;
    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)))
    {
        ok = SUCCEEDED(file->Save(lnk.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

bool IsAutostartEnabled()
{
    std::wstring value;
    return RegReadString(HKEY_CURRENT_USER, kRunKey, kRunValue, value) && !value.empty();
}

bool SetAutostartFor(const std::wstring& exePath)
{
    std::wstring cmd = L"\"" + exePath + L"\" --tray";
    Log(L"install: autostart -> %ls", cmd.c_str());
    return RegWriteString(HKEY_CURRENT_USER, kRunKey, kRunValue, cmd.c_str());
}

bool SetAutostart(bool enable)
{
    if (enable)
    {
        // When the tool is installed, autostart must point at the installed
        // copy even if this session was started from a portable folder.
        std::wstring exe = FileExists(InstalledExe()) ? InstalledExe() : ExePath();
        return SetAutostartFor(exe);
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
        return false;
    RegDeleteValueW(key, kRunValue);
    RegCloseKey(key);
    return true;
}

bool InstallSelf(std::wstring& installedPath, std::wstring& error)
{
    Log(L"install: source=%ls", ExePath().c_str());
    {
        wchar_t local[MAX_PATH] = {};
        wchar_t roaming[MAX_PATH] = {};
        GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        GetEnvironmentVariableW(L"APPDATA", roaming, MAX_PATH);
        wchar_t user[256] = {};
        DWORD userLen = 256;
        GetUserNameW(user, &userLen);
        Log(L"install: user=%ls local=%ls roaming=%ls", user, local, roaming);
    }

    // Some machines refuse %LOCALAPPDATA%\Programs (err 5), so walk a list of
    // fallbacks and keep the first directory we can actually create.
    std::wstring dir;
    std::wstring lastError;
    for (const std::wstring& candidate : InstallDirCandidates())
    {
        if (!CreateDirectoryW(candidate.c_str(), nullptr))
        {
            const DWORD err = GetLastError();
            if (err != ERROR_ALREADY_EXISTS)
            {
                Log(L"install: CreateDirectory failed err=%lu dir=%ls", err, candidate.c_str());
                lastError = candidate + L"（错误码 " + std::to_wstring(err) + L"）";
                continue;
            }
        }
        Log(L"install: using dir=%ls", candidate.c_str());
        dir = candidate;
        break;
    }
    if (dir.empty())
    {
        error = L"无法创建安装目录：" + lastError;
        return false;
    }

    const std::wstring target = dir + L"\\MCHOSE Battery Tray.exe";
    if (_wcsicmp(target.c_str(), ExePath().c_str()) != 0)
    {
        if (!CopyFileW(ExePath().c_str(), target.c_str(), FALSE))
        {
            const DWORD err = GetLastError();
            error = L"无法复制程序文件（错误码 " + std::to_wstring(err) + L"）";
            Log(L"install: CopyFile failed err=%lu target=%ls", err, target.c_str());
            return false;
        }
    }

    installedPath = target;

    // Uninstall entry ("Apps & features")
    RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayName", L"MCHOSE 键盘电量托盘");
    RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayIcon", target.c_str());
    RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion", kAppVersion);
    RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"Publisher", L"MCHOSE Battery Tray (open source)");
    RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation", dir.c_str());
    const std::wstring uninstallCmd = L"\"" + target + L"\" --uninstall";
    RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString", uninstallCmd.c_str());
    RegWriteString(HKEY_CURRENT_USER, kUninstallKey, L"QuietUninstallString", uninstallCmd.c_str());
    RegWriteDword(HKEY_CURRENT_USER, kUninstallKey, L"NoModify", 1);
    RegWriteDword(HKEY_CURRENT_USER, kUninstallKey, L"NoRepair", 1);
    RegWriteDword(HKEY_CURRENT_USER, kUninstallKey, L"EstimatedSize", 2048);

    CreateShortcut(StartMenuShortcutPath(), target, L"", L"MCHOSE 键盘电量托盘");
    SetAutostartFor(target);
    Log(L"install: ok -> %s", target.c_str());
    return true;
}

bool UninstallSelf(std::wstring& error)
{
    // Ask a running instance to go away.
    const std::wstring quitEvent = InstanceName(kQuitEventName);
    if (HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, quitEvent.c_str()))
    {
        SetEvent(ev);
        CloseHandle(ev);
        Sleep(700);
    }

    SetAutostart(false);
    DeleteFileW(StartMenuShortcutPath().c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);

    // Logs live outside the install dir; drop them too so nothing is left behind.
    {
        const std::wstring logPath = LogPath();
        const size_t cut = logPath.find_last_of(L'\\');
        DeleteFileW(logPath.c_str());
        if (cut != std::wstring::npos)
            RemoveDirectoryW(logPath.substr(0, cut).c_str());
    }

    // A running exe cannot delete itself, and holding a message box open keeps
    // the image locked, so hand the job to a detached batch file that retries
    // until this process is really gone.
    const std::wstring dir = InstallDir();
    const std::wstring exe = InstalledExe();
    bool selfInside = _wcsicmp(ExePath().c_str(), exe.c_str()) == 0;
    if (!selfInside && !FileExists(exe))
    {
        return true;   // nothing installed
    }

    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring bat = std::wstring(temp) + L"mchose-battery-tray-uninstall.cmd";

    std::wstring script =
        L"@echo off\r\n"
        L"setlocal\r\n"
        L"ping -n 2 127.0.0.1 >nul\r\n"
        L"set tries=0\r\n"
        L":retry\r\n"
        L"del /f /q \"" + exe + L"\" 2>nul\r\n"
        L"if not exist \"" + exe + L"\" goto cleanup\r\n"
        L"set /a tries+=1\r\n"
        L"if %tries% GEQ 30 goto cleanup\r\n"
        L"ping -n 2 127.0.0.1 >nul\r\n"
        L"goto retry\r\n"
        L":cleanup\r\n"
        L"rmdir /s /q \"" + dir + L"\" 2>nul\r\n"
        L"del /f /q \"%~f0\"\r\n";

    bool wrote = false;
    // cmd.exe reads batch files in the ANSI code page, so store the script that
    // way (a UTF-16 batch file is silently ignored).
    std::string ansi;
    {
        const int need = WideCharToMultiByte(CP_ACP, 0, script.c_str(), (int)script.size(),
                                             nullptr, 0, nullptr, nullptr);
        if (need > 0)
        {
            ansi.resize((size_t)need);
            WideCharToMultiByte(CP_ACP, 0, script.c_str(), (int)script.size(),
                                ansi.data(), need, nullptr, nullptr);
        }
    }
    if (!ansi.empty())
    {
        if (FILE* f = nullptr; _wfopen_s(&f, bat.c_str(), L"wb") == 0 && f)
        {
            wrote = fwrite(ansi.data(), 1, ansi.size(), f) == ansi.size();
            fclose(f);
        }
    }
    if (!wrote)
    {
        // Fall back to the old one-liner if the batch file cannot be created.
        wchar_t cmd[2048];
        swprintf_s(cmd,
                   L"cmd.exe /c ping -n 3 127.0.0.1 >nul & del /f /q \"%ls\" & "
                   L"rmdir /s /q \"%ls\"",
                   exe.c_str(), dir.c_str());
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                            nullptr, &si, &pi))
        {
            error = L"无法启动清理进程";
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log(L"uninstall: fallback cleanup scheduled for %s", dir.c_str());
        return true;
    }

    std::wstring cmd = L"cmd.exe /c \"" + bat + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &si, &pi))
    {
        error = L"无法启动清理进程（错误码 " + std::to_wstring(GetLastError()) + L"）";
        Log(L"uninstall: CreateProcess failed err=%lu bat=%ls", GetLastError(), bat.c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Log(L"uninstall: cleanup scheduled for %s (bat=%ls)", dir.c_str(), bat.c_str());
    return true;
}

} // namespace mchose
