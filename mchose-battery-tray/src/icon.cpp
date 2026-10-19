// icon.cpp - the tray badge: rounded square, big white digits
#include "icon.h"

#include <objidl.h>
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace mchose {

static const Color kGreen(255, 16, 185, 129);
static const Color kAmber(255, 245, 158, 11);
static const Color kRed(255, 239, 68, 68);
static const Color kGrey(255, 148, 155, 164);

// Rounded square + hairline outline, used by both the tray badge and the app icon.
static void DrawBadgeBackground(Graphics& g, float S, Color bg, bool outline)
{
    SolidBrush brush(bg);
    GraphicsPath path;
    const float r = S * 0.32f;
    path.AddArc(0.5f, 0.5f, r * 2, r * 2, 180, 90);
    path.AddArc(S - r * 2 - 0.5f, 0.5f, r * 2, r * 2, 270, 90);
    path.AddArc(S - r * 2 - 0.5f, S - r * 2 - 0.5f, r * 2, r * 2, 0, 90);
    path.AddArc(0.5f, S - r * 2 - 0.5f, r * 2, r * 2, 90, 90);
    path.CloseFigure();
    g.FillPath(&brush, &path);

    if (outline)
    {
        Pen edge(Color(70, 255, 255, 255), S * 0.055f);
        g.DrawPath(&edge, &path);
    }
}

static void PaintAppIcon(Graphics& g, float S)
{
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));
    DrawBadgeBackground(g, S, kGreen, true);

    // A bolt reads far better than any glyph at 16 px.
    const PointF unit[6] = {
        PointF(0.60f, 0.08f), PointF(0.27f, 0.56f), PointF(0.47f, 0.56f),
        PointF(0.41f, 0.92f), PointF(0.74f, 0.44f), PointF(0.54f, 0.44f),
    };
    PointF bolt[6];
    for (int i = 0; i < 6; ++i)
        bolt[i] = PointF(unit[i].X * S, unit[i].Y * S);
    SolidBrush white(Color(255, 255, 255, 255));
    g.FillPolygon(&white, bolt, 6);
}

// Renders `paint` at 8x and downsamples, which keeps the rounded corners clean.
template <typename PaintFn>
static HICON Supersample(int size, PaintFn paint)
{
    const int S = size * 8;
    Bitmap big(S, S, PixelFormat32bppARGB);
    {
        Graphics g(&big);
        paint(g, (float)S);
    }

    Bitmap thumb(size, size, PixelFormat32bppARGB);
    Graphics g2(&thumb);
    g2.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g2.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g2.SetCompositingQuality(CompositingQualityHighQuality);
    g2.Clear(Color(0, 0, 0, 0));
    g2.DrawImage(&big, 0, 0, size, size);
    g2.Flush();

    HICON icon = nullptr;
    thumb.GetHICON(&icon);
    return icon;
}

static void PaintBadge(Graphics& g, int percent, bool charging, float S)
{
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.Clear(Color(0, 0, 0, 0));

    Color bg;
    if (percent < 0)
        bg = kGrey;
    else if (percent > 50)
        bg = kGreen;
    else if (percent > 20)
        bg = kAmber;
    else
        bg = kRed;

    DrawBadgeBackground(g, S, bg, percent >= 0);

    wchar_t text[8] = {};
    if (percent < 0)
        wcscpy_s(text, L"?");
    else
        swprintf_s(text, L"%d", percent);

    const wchar_t* family = L"Segoe UI";
    FontFamily probe(family);
    if (!probe.IsAvailable())
        family = L"Arial";
    FontFamily ff(family);

    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    sf.SetFormatFlags(StringFormatFlagsNoWrap);

    // Fill the badge height first; if the digits are still too wide (e.g. "100")
    // squeeze them horizontally instead of shrinking them into unreadable stubs.
    const float availW = S * 0.84f;
    const float availH = S * 0.66f;
    float fontSize = S * 0.80f;
    auto* font = new Font(&ff, fontSize, FontStyleBold, UnitPixel);
    RectF box;
    for (int i = 0; i < 10; ++i)
    {
        g.MeasureString(text, -1, font, RectF(0, 0, 4000, 4000), &sf, &box);
        if (box.Height <= availH || i == 9)
            break;
        delete font;
        fontSize *= 0.94f;
        font = new Font(&ff, fontSize, FontStyleBold, UnitPixel);
    }
    g.MeasureString(text, -1, font, RectF(0, 0, 4000, 4000), &sf, &box);

    float squeeze = 1.0f;
    if (box.Width > availW && box.Width > 0.0f)
        squeeze = availW / box.Width;

    SolidBrush white(Color(255, 255, 255, 255));
    const GraphicsState state = g.Save();
    if (squeeze < 1.0f)
    {
        g.TranslateTransform(S * 0.5f, S * 0.5f);
        g.ScaleTransform(squeeze, 1.0f);
        g.TranslateTransform(-S * 0.5f, -S * 0.5f);
    }
    g.DrawString(text, -1, font, RectF(0, 0, S, S), &sf, &white);
    g.Restore(state);
    delete font;

    // At tray sizes (16-24 px) a charging bolt on top of the digits just turns
    // into noise, so charging is conveyed by the tooltip and the tray menu.
    (void)charging;
}

HICON RenderBadgeIcon(int percent, int size, bool charging)
{
    if (size <= 0)
        size = 16;
    return Supersample(size, [percent, charging](Graphics& g, float S)
                       { PaintBadge(g, percent, charging, S); });
}

HICON RenderAppIcon(int size)
{
    if (size <= 0)
        size = 64;
    return Supersample(size, [](Graphics& g, float S) { PaintAppIcon(g, S); });
}

} // namespace mchose
