// icon.h - GDI+ tray badge rendering
#pragma once

#include "common.h"

namespace mchose {

// Draws the rounded "NN" badge into a new HICON of `size` pixels.
// percent < 0 renders a neutral "?" badge.
HICON RenderBadgeIcon(int percent, int size, bool charging);

// The application/exe icon (green badge with a bolt), used for the .ico resource.
HICON RenderAppIcon(int size);

} // namespace mchose
