#ifndef AI_DRAW_UTILS_H
#define AI_DRAW_UTILS_H

#include "detection_types.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace AI {

/* ── Pixel-level drawing ──────────────────────────────────────────── */

inline void draw_pixel(uint8_t* bgr, int w, int h, int x, int y,
                        uint8_t r, uint8_t g, uint8_t b)
{
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    bgr[(y * w + x) * 3 + 0] = b;
    bgr[(y * w + x) * 3 + 1] = g;
    bgr[(y * w + x) * 3 + 2] = r;
}

inline void draw_rect(uint8_t* bgr, int w, int h,
                       int x1, int y1, int x2, int y2,
                       uint8_t r, uint8_t g, uint8_t b, int thickness = 2)
{
    for (int t = 0; t < thickness; ++t) {
        for (int x = x1 - t; x <= x2 + t; ++x) {
            draw_pixel(bgr, w, h, x, y1 - t, r, g, b);
            draw_pixel(bgr, w, h, x, y2 + t, r, g, b);
        }
        for (int y = y1 - t; y <= y2 + t; ++y) {
            draw_pixel(bgr, w, h, x1 - t, y, r, g, b);
            draw_pixel(bgr, w, h, x2 + t, y, r, g, b);
        }
    }
}

/* ── Minimal 5x7 bitmap font (digits 0-9, A-Z) ───────────────────── */

inline const uint8_t (*mini_font())[5]
{
    static const uint8_t FONT[][5] = {
        /* 0 */ {0x7C,0xA2,0x92,0x8A,0x7C}, /* 1 */ {0x00,0x84,0xFE,0x80,0x00},
        /* 2 */ {0xC4,0xA2,0x92,0x92,0x8C}, /* 3 */ {0x44,0x82,0x92,0x92,0x6C},
        /* 4 */ {0x30,0x28,0x24,0xFE,0x20}, /* 5 */ {0x4E,0x8A,0x8A,0x8A,0x72},
        /* 6 */ {0x78,0x94,0x92,0x92,0x60}, /* 7 */ {0x02,0xE2,0x12,0x0A,0x06},
        /* 8 */ {0x6C,0x92,0x92,0x92,0x6C}, /* 9 */ {0x0C,0x92,0x92,0x52,0x3C},
        /* A */ {0xFC,0x12,0x12,0x12,0xFC}, /* B */ {0xFE,0x92,0x92,0x92,0x6C},
        /* C */ {0x7C,0x82,0x82,0x82,0x44}, /* D */ {0xFE,0x82,0x82,0x82,0x7C},
        /* E */ {0xFE,0x92,0x92,0x92,0x82}, /* F */ {0xFE,0x12,0x12,0x12,0x02},
        /* G */ {0x7C,0x82,0x92,0x92,0x74}, /* H */ {0xFE,0x10,0x10,0x10,0xFE},
        /* I */ {0x00,0x82,0xFE,0x82,0x00}, /* J */ {0x40,0x80,0x82,0x7E,0x02},
        /* K */ {0xFE,0x10,0x28,0x44,0x82}, /* L */ {0xFE,0x80,0x80,0x80,0x80},
        /* M */ {0xFE,0x04,0x18,0x04,0xFE}, /* N */ {0xFE,0x08,0x10,0x20,0xFE},
        /* O */ {0x7C,0x82,0x82,0x82,0x7C}, /* P */ {0xFE,0x12,0x12,0x12,0x0C},
        /* Q */ {0x7C,0x82,0xA2,0x42,0xBC}, /* R */ {0xFE,0x12,0x32,0x52,0x8C},
        /* S */ {0x4C,0x92,0x92,0x92,0x64}, /* T */ {0x02,0x02,0xFE,0x02,0x02},
        /* U */ {0x7E,0x80,0x80,0x80,0x7E}, /* V */ {0x3E,0x40,0x80,0x40,0x3E},
        /* W */ {0x7E,0x80,0x70,0x80,0x7E}, /* X */ {0xC6,0x28,0x10,0x28,0xC6},
        /* Y */ {0x06,0x08,0xF0,0x08,0x06}, /* Z */ {0xC2,0xA2,0x92,0x8A,0x86},
    };
    return FONT;
}

inline void draw_char(uint8_t* bgr, int w, int h, int cx, int cy,
                       char ch, uint8_t r, uint8_t g, uint8_t b)
{
    int idx = -1;
    if (ch >= '0' && ch <= '9') idx = ch - '0';
    else if (ch >= 'A' && ch <= 'Z') idx = ch - 'A' + 10;
    else if (ch >= 'a' && ch <= 'z') idx = ch - 'a' + 10;
    else return;

    if (idx < 0 || idx >= 36) return;

    const auto* font = mini_font();
    for (int col = 0; col < 5; ++col) {
        uint8_t bits = font[idx][col];
        for (int row = 0; row < 7; ++row) {
            if (bits & (1 << (row + 1))) {
                draw_pixel(bgr, w, h, cx + col, cy + row, r, g, b);
            }
        }
    }
}

inline void draw_text(uint8_t* bgr, int w, int h, int x, int y,
                       const char* text, uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; text[i]; ++i) {
        draw_char(bgr, w, h, x + i * 6, y, text[i], r, g, b);
    }
}

/* ── Per-class colors (RGB) ───────────────────────────────────────── */

inline const uint8_t (*class_colors())[3]
{
    static const uint8_t COLORS[][3] = {
        {255,0,0}, {0,255,0}, {0,0,255}, {255,255,0},
        {255,0,255}, {0,255,255}, {128,255,0}, {255,128,0},
        {0,128,255}, {128,0,255}, {255,0,128}, {0,255,128},
    };
    return COLORS;
}

/**
 * Draw detections with letterbox-aware coordinate mapping.
 *
 * Detections are in model input space (model_size x model_size, e.g. 640x640).
 * Camera frame is cam_w x cam_h (e.g. 640x480).
 * Letterbox pads top/bottom: pad_top = (model_size - cam_h) / 2
 */
inline void draw_detections_mapped(uint8_t* bgr, int cam_w, int cam_h,
                                     const std::vector<Detection>& dets,
                                     int model_size)
{
    const int pad_top = (model_size - cam_h) / 2;
    const auto* colors = class_colors();

    for (const auto& d : dets) {
        int x1 = (int)d.bbox.x1;
        int y1 = (int)d.bbox.y1 - pad_top;
        int x2 = (int)d.bbox.x2;
        int y2 = (int)d.bbox.y2 - pad_top;

        if (x1 < 0) x1 = 0;
        if (y1 < 0) y1 = 0;
        if (x2 >= cam_w) x2 = cam_w - 1;
        if (y2 >= cam_h) y2 = cam_h - 1;
        if (x1 >= x2 || y1 >= y2) continue;

        int ci = d.class_id % 12;
        uint8_t r = colors[ci][0], g = colors[ci][1], b = colors[ci][2];

        draw_rect(bgr, cam_w, cam_h, x1, y1, x2, y2, r, g, b, 2);

        char label[64];
        snprintf(label, sizeof(label), "%s %d",
                 coco_label(d.class_id), (int)(d.confidence * 100));

        int lw = (int)strlen(label) * 6 + 2;
        int label_y = y1 - 9;
        if (label_y < 0) label_y = y2 + 2;

        for (int ly = label_y; ly < label_y + 8; ++ly)
            for (int lx = x1; lx < x1 + lw && lx < cam_w; ++lx)
                draw_pixel(bgr, cam_w, cam_h, lx, ly, r, g, b);

        draw_text(bgr, cam_w, cam_h, x1 + 1, label_y + 1, label, 255, 255, 255);
    }
}

} // namespace AI

#endif // AI_DRAW_UTILS_H
