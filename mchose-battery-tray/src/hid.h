// hid.h - minimal Win32 HID layer (enumeration + framed IO)
#pragma once

#include "common.h"

namespace mchose {

struct HidInterface
{
    std::wstring path;          // device interface path (\?\hid#vid_41e4&pid_2001...)
    std::wstring product;       // product string, if readable
    std::wstring manufacturer;
    USHORT vid = 0;
    USHORT pid = 0;
    USHORT version = 0;
    USHORT usagePage = 0;
    USHORT usage = 0;
    USHORT inputReportLen = 0;  // including the report-id byte
    USHORT outputReportLen = 0;
    USHORT featureReportLen = 0;
};

// Enumerate every HID collection currently present on the machine.
// Pass vid=0 to enumerate all vendors.
std::vector<HidInterface> EnumerateHid(USHORT vid = 0, USHORT pid = 0);

// A short-lived, exclusive-free handle to one HID collection.
class HidHandle
{
public:
    HidHandle() = default;
    ~HidHandle() { Close(); }
    HidHandle(const HidHandle&) = delete;
    HidHandle& operator=(const HidHandle&) = delete;

    bool Open(const std::wstring& path);
    void Close();
    bool valid() const { return device_ != INVALID_HANDLE_VALUE; }

    // Writes exactly `len` bytes (the buffer must include the report id).
    bool Write(const BYTE* data, DWORD len, DWORD timeoutMs = 500);

    // Reads one input report; returns bytes read (0 on timeout).
    DWORD Read(BYTE* data, DWORD len, DWORD timeoutMs = 500);

private:
    HANDLE device_ = INVALID_HANDLE_VALUE;
    HANDLE event_ = nullptr;
};

} // namespace mchose
