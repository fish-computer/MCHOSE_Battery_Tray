// protocol.h - battery-reading drivers
#pragma once

#include "common.h"
#include "hid.h"

namespace mchose {

struct Reading
{
    bool ok = false;
    int percent = -1;
    bool charging = false;
    std::wstring source;   // human readable, e.g. "MCHOSE 2.4G"
};

// A driver knows how to turn one HID collection into a battery percentage.
class Driver
{
public:
    virtual ~Driver() = default;
    virtual const wchar_t* id() const = 0;
    virtual const wchar_t* displayName() const = 0;
    // Cheap structural check (vendor/product/usage page/report sizes).
    virtual bool matches(const HidInterface& dev) const = 0;
    // Perform a query. Returns true when `out` was filled.
    virtual bool read(const HidInterface& dev, Reading& out) const = 0;
};

// Driver registry, most specific first.
const std::vector<const Driver*>& Drivers();

// Find the first device a driver claims and query it.
// `usedDriver` receives the driver that answered (may be null).
bool QueryBattery(Reading& out, const wchar_t** usedDriver = nullptr);

// Implemented by protocol_mchose.cpp
const Driver* MchoseDriver1();

} // namespace mchose
