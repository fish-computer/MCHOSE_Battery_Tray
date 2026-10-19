// hid.cpp
#include "hid.h"

#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")

namespace mchose {

static std::wstring WideFromBuffer(const wchar_t* buf, size_t chars)
{
    size_t n = 0;
    while (n < chars && buf[n])
        ++n;
    return std::wstring(buf, n);
}

std::vector<HidInterface> EnumerateHid(USHORT vid, USHORT pid)
{
    std::vector<HidInterface> out;

    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return out;

    for (DWORD i = 0;; ++i)
    {
        SP_DEVICE_INTERFACE_DATA did = {};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &did))
            break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &did, nullptr, 0, &need, nullptr);
        if (need == 0)
            continue;

        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &did, detail, need, nullptr, nullptr))
            continue;

        HidInterface dev;
        dev.path = detail->DevicePath;

        HANDLE h = CreateFileW(dev.path.c_str(), 0,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            continue;

        HIDD_ATTRIBUTES attrs = {};
        attrs.Size = sizeof(attrs);
        if (!HidD_GetAttributes(h, &attrs))
        {
            CloseHandle(h);
            continue;
        }
        dev.vid = attrs.VendorID;
        dev.pid = attrs.ProductID;
        dev.version = attrs.VersionNumber;

        wchar_t str[256] = {};
        if (HidD_GetProductString(h, str, sizeof(str)))
            dev.product = WideFromBuffer(str, 255);
        wmemset(str, 0, 256);
        if (HidD_GetManufacturerString(h, str, sizeof(str)))
            dev.manufacturer = WideFromBuffer(str, 255);

        PHIDP_PREPARSED_DATA preparsed = nullptr;
        if (HidD_GetPreparsedData(h, &preparsed))
        {
            HIDP_CAPS caps = {};
            if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS)
            {
                dev.usagePage = caps.UsagePage;
                dev.usage = caps.Usage;
                dev.inputReportLen = caps.InputReportByteLength;
                dev.outputReportLen = caps.OutputReportByteLength;
                dev.featureReportLen = caps.FeatureReportByteLength;
            }
            HidD_FreePreparsedData(preparsed);
        }
        CloseHandle(h);

        if (vid && dev.vid != vid)
            continue;
        if (pid && dev.pid != pid)
            continue;

        out.push_back(dev);
    }

    SetupDiDestroyDeviceInfoList(set);
    return out;
}

bool HidHandle::Open(const std::wstring& path)
{
    Close();
    device_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_OVERLAPPED, nullptr);
    if (device_ == INVALID_HANDLE_VALUE)
    {
        Log(L"hid: CreateFile failed (%s) err=%lu", path.c_str(), GetLastError());
        return false;
    }
    event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event_)
    {
        Close();
        return false;
    }
    return true;
}

void HidHandle::Close()
{
    if (device_ != INVALID_HANDLE_VALUE)
    {
        CancelIoEx(device_, nullptr);
        CloseHandle(device_);
        device_ = INVALID_HANDLE_VALUE;
    }
    if (event_)
    {
        CloseHandle(event_);
        event_ = nullptr;
    }
}

bool HidHandle::Write(const BYTE* data, DWORD len, DWORD timeoutMs)
{
    if (!valid())
        return false;

    OVERLAPPED ov = {};
    ov.hEvent = event_;
    ResetEvent(event_);
    DWORD written = 0;
    BOOL ok = WriteFile(device_, data, len, &written, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING)
    {
        Log(L"hid: WriteFile immediate fail err=%lu", GetLastError());
        return false;
    }
    DWORD w = WaitForSingleObject(event_, timeoutMs);
    if (w != WAIT_OBJECT_0)
    {
        CancelIoEx(device_, &ov);
        WaitForSingleObject(event_, 100);
        Log(L"hid: write timeout (%lu ms)", timeoutMs);
        return false;
    }
    if (!GetOverlappedResult(device_, &ov, &written, FALSE))
    {
        Log(L"hid: write result err=%lu", GetLastError());
        return false;
    }
    return written == len;
}

DWORD HidHandle::Read(BYTE* data, DWORD len, DWORD timeoutMs)
{
    if (!valid())
        return 0;

    OVERLAPPED ov = {};
    ov.hEvent = event_;
    ResetEvent(event_);
    DWORD got = 0;
    BOOL ok = ReadFile(device_, data, len, &got, &ov);
    if (!ok && GetLastError() != ERROR_IO_PENDING)
        return 0;

    DWORD w = WaitForSingleObject(event_, timeoutMs);
    if (w != WAIT_OBJECT_0)
    {
        CancelIoEx(device_, &ov);
        WaitForSingleObject(event_, 100);
        return 0;
    }
    if (!GetOverlappedResult(device_, &ov, &got, FALSE))
        return 0;
    return got;
}

} // namespace mchose
