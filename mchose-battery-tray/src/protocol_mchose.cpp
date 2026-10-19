// protocol_mchose.cpp
//
// Battery reading for MCHOSE 2.4 GHz dongles that speak the 0x13-frame protocol.
//
// Frame layout (20 bytes, report id 0x13 included in the first byte):
//     [0] 0x13            report id
//     [1] command
//     [2] parameter
//     [3..18] payload
//     [19] checksum = (sum of bytes 0..18) & 0xFF
//
// Session open : 13 05 01 ...            (brings the link up)
// Battery query: 13 4A 01 ...            (same command the official hub sends)
// Answer       : 13 4A 01 00 02 <pct> <flags>
//   flags bit 0x10 = charging, 0x01 = on battery
// Passive push : 13 0A 01 00 04 05 <pct> <flags>
//   payload[1] = 0x05 marks the "battery" event, pct at byte 6, flags at byte 7

#include "protocol.h"

namespace mchose {
namespace {

constexpr BYTE kReportId = 0x13;
constexpr size_t kFrameLen = 20;
constexpr BYTE kCmdSession = 0x05;
constexpr BYTE kCmdBattery = 0x4A;
constexpr BYTE kCmdPush = 0x0A;

void BuildFrame(BYTE* frame, BYTE cmd, BYTE param)
{
    memset(frame, 0, kFrameLen);
    frame[0] = kReportId;
    frame[1] = cmd;
    frame[2] = param;
    BYTE sum = 0;
    for (size_t i = 0; i + 1 < kFrameLen; ++i)
        sum = (BYTE)(sum + frame[i]);
    frame[kFrameLen - 1] = sum;
}

bool ChecksumOk(const BYTE* frame, size_t len)
{
    if (len < 3)
        return false;
    BYTE sum = 0;
    for (size_t i = 0; i + 1 < len; ++i)
        sum = (BYTE)(sum + frame[i]);
    return sum == frame[len - 1];
}

void DumpFrame(const wchar_t* tag, const BYTE* f, size_t n)
{
    wchar_t line[3 * 32 + 32] = {};
    size_t pos = 0;
    for (size_t i = 0; i < n && i < 24; ++i)
        pos += swprintf_s(line + pos, 8, L"%02X ", f[i]);
    Log(L"frame[%s] len=%zu: %s", tag, n, line);
}

class MchoseDriver1Impl final : public Driver
{
public:
    const wchar_t* id() const override { return L"mchose-v1"; }
    const wchar_t* displayName() const override { return L"MCHOSE 2.4G (0x13)"; }

    bool matches(const HidInterface& dev) const override
    {
        // Vendor channel: usage page 0xFF02, report id 0x13, 20-byte reports.
        return dev.vid == 0x41E4 && dev.usagePage == 0xFF02 &&
               dev.inputReportLen >= kFrameLen && dev.outputReportLen >= kFrameLen;
    }

    bool read(const HidInterface& dev, Reading& out) const override
    {
        HidHandle h;
        if (!h.Open(dev.path))
            return false;

        BYTE frame[kFrameLen];
        BuildFrame(frame, kCmdSession, 0x01);
        if (!h.Write(frame, kFrameLen))
            Log(L"mchose: session write failed");

        // Drain whatever the session command produced, then ask for the battery.
        BYTE in[kFrameLen * 4] = {};
        h.Read(in, kFrameLen, 300);

        BuildFrame(frame, kCmdBattery, 0x01);
        if (!h.Write(frame, kFrameLen))
        {
            Log(L"mchose: battery write failed");
            return false;
        }

        const ULONGLONG deadline = GetTickCount64() + 2500;
        BYTE buf[kFrameLen] = {};
        while (GetTickCount64() < deadline)
        {
            DWORD n = h.Read(buf, kFrameLen, 400);
            if (n == 0)
                continue;

            DumpFrame(L"rx", buf, n);
            if (!ChecksumOk(buf, kFrameLen))
                continue;

            const BYTE cmd = buf[1];
            if (cmd == kCmdBattery && buf[2] == 0x01)
            {
                int pct = buf[5];
                if (pct >= 1 && pct <= 100)
                {
                    out.ok = true;
                    out.percent = pct;
                    out.charging = (buf[6] & 0x10) != 0;
                    out.source = dev.product.empty() ? L"MCHOSE 2.4G" : dev.product;
                    return true;
                }
            }
            if (cmd == kCmdPush && buf[2] == 0x01 && buf[5] == 0x05)
            {
                int pct = buf[6];
                if (pct >= 1 && pct <= 100)
                {
                    out.ok = true;
                    out.percent = pct;
                    out.charging = (buf[7] & 0x10) != 0;
                    out.source = dev.product.empty() ? L"MCHOSE 2.4G" : dev.product;
                    return true;
                }
            }
        }
        return false;
    }
};

MchoseDriver1Impl g_mchose1;

} // namespace

const Driver* MchoseDriver1() { return &g_mchose1; }

const std::vector<const Driver*>& Drivers()
{
    static const std::vector<const Driver*> list = { MchoseDriver1() };
    return list;
}

bool QueryBattery(Reading& out, const wchar_t** usedDriver)
{
    // Enumerate once per query: dongles come and go, and a cached path can
    // point at a collection that no longer exists.
    const std::vector<HidInterface> devices = EnumerateHid();
    if (devices.empty())
    {
        Log(L"query: no HID devices visible");
        return false;
    }

    for (const Driver* driver : Drivers())
    {
        for (const HidInterface& dev : devices)
        {
            if (!driver->matches(dev))
                continue;
            Log(L"query: trying driver %s on %s (vid=%04X pid=%04X up=%04X)",
                driver->id(), dev.path.c_str(), dev.vid, dev.pid, dev.usagePage);
            Reading r;
            if (driver->read(dev, r) && r.ok)
            {
                out = r;
                if (usedDriver)
                    *usedDriver = driver->id();
                Log(L"query: ok %d%%%s via %s", r.percent, r.charging ? L" (charging)" : L"",
                    driver->id());
                return true;
            }
        }
    }

    Log(L"query: no driver produced a reading");
    return false;
}

} // namespace mchose
