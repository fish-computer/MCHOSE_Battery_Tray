// install.h - autostart, per-user install and clean uninstall
#pragma once

#include "common.h"

namespace mchose {

bool IsInstalled();
bool IsAutostartEnabled();
bool SetAutostart(bool enable);

// Points the autostart entry at an explicit executable. Used by install so the
// Run value always names the installed copy, never the folder we were started
// from.
bool SetAutostartFor(const std::wstring& exePath);

// Copies this exe into %LOCALAPPDATA%\Programs\..., registers the uninstall
// entry, adds a Start Menu shortcut and enables autostart.
// Returns the installed exe path on success.
bool InstallSelf(std::wstring& installedPath, std::wstring& error);

// Removes every trace (autostart, shortcut, uninstall entry, files).
// Returns false when the directory could not be scheduled for deletion.
bool UninstallSelf(std::wstring& error);

std::wstring StartMenuShortcutPath();
std::wstring VersionString();

} // namespace mchose
