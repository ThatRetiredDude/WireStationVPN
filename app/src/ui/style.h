// The look the app's pages share: colours, the controller's button glyphs,
// labels and the spinner. From TokenStation5's ps5/ui/style.h, in
// WireStationVPN's colours.
#pragma once
#include <cmath>
#include <string>

#include "canvas.h"

namespace ui {

// WireStationVPN's red on near-black, from the logo.
constexpr Color kBgTop = rgb(0x120809), kBgBottom = rgb(0x050505);
constexpr Color kPanel = rgb(0x16100F), kPanelLine = rgb(0x3A1C1C);
constexpr Color kRow = rgb(0x1C1413), kRowSel = rgb(0x3A1618);
constexpr Color kText = rgb(0xF4EEEE), kText2 = rgb(0xB9A9A9), kMuted = rgb(0x7A6A6A);
constexpr Color kAccent = rgb(0xFF3B3B), kGreen = rgb(0x3DDC97), kRed = rgb(0xFF5C70), kAmber = rgb(0xFFB547);
constexpr Color kTrack = rgb(0x3A1C1C);
constexpr Color kCrossColor = rgb(0x8FB0FF), kCircleColor = rgb(0xFF6F7F), kSquareColor = rgb(0xFF8BD0),
                kTriangleColor = rgb(0x4BE0A8);

enum PadGlyph { kGlyphCross, kGlyphCircle, kGlyphSquare, kGlyphTriangle };

inline void glyph(Canvas &c, float cx, float cy, PadGlyph g, bool enabled = true) {
    c.circle(cx, cy, 16, rgb(0x2A1E1E));
    Color col = g == kGlyphCross    ? kCrossColor
                : g == kGlyphCircle ? kCircleColor
                : g == kGlyphSquare ? kSquareColor
                                    : kTriangleColor;
    if (!enabled) col = kMuted;
    switch (g) {
        case kGlyphCross:
            c.segment(cx - 6, cy - 6, cx + 6, cy + 6, 3, col);
            c.segment(cx - 6, cy + 6, cx + 6, cy - 6, 3, col);
            break;
        case kGlyphCircle: c.ring(cx, cy, 7, 3, col); break;
        case kGlyphSquare: c.round_rect_line(cx - 7, cy - 7, 14, 14, 2, 3, col); break;
        case kGlyphTriangle:
            c.segment(cx, cy - 7.5f, cx - 8, cy + 6, 3, col);
            c.segment(cx - 8, cy + 6, cx + 8, cy + 6, 3, col);
            c.segment(cx + 8, cy + 6, cx, cy - 7.5f, 3, col);
            break;
    }
}

// A shoulder button's name (L1, R1) in a rounded key at x; returns its width.
inline int key_cap(Canvas &c, const Font &f, int x, int y, const std::string &name, bool enabled = true) {
    const int w = Canvas::measure(f, name) + 24;
    c.round_rect((float)x, (float)y - 25, (float)w, 34, 10, rgb(0x2A1E1E));
    c.text(f, x + 12, y - 1, name, enabled ? kText2 : kMuted);
    return w;
}

// A rounded label right-aligned at `right`; returns its left edge.
inline int pill(Canvas &c, const Font &f, int right, int top, const std::string &text, Color col) {
    const int w = Canvas::measure(f, text) + 32, h = 34;
    c.round_rect((float)(right - w), (float)top, (float)w, (float)h, h * 0.5f, alpha(col, 46));
    c.text(f, right - w + 16, top + 24, text, col);
    return right - w;
}

inline void spinner(Canvas &c, float cx, float cy, double t, Color col) {
    const int phase = (int)(t * 12) % 8;
    for (int i = 0; i < 8; i++) {
        const float a = (float)(i * M_PI / 4);
        const int age = (phase - i + 8) % 8;
        c.circle(cx + 11 * std::cos(a), cy + 11 * std::sin(a), 3.2f, alpha(col, 255 - age * 28));
    }
}

// Text with a controller button drawn in it: before, the button, after.
inline void text_glyph(Canvas &c, const Font &f, int x, int y, const std::string &before, PadGlyph g,
                       const std::string &after, Color col) {
    x += c.text(f, x, y, before, col) + 6;
    glyph(c, (float)x + 16, (float)y - f.px * 0.33f, g);
    c.text(f, x + 38, y, after, col);
}

}  // namespace ui
