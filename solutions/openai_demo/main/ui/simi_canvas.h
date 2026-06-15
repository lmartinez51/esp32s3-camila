/**
 * @file simi_canvas.h
 * @brief Lightweight off-screen software rasterizer for the Dr. Simi visual system.
 *
 *        All drawing happens into an in-RAM 16bpp canvas and is later blitted to the
 *        panel in a single transaction (see ui_panel_blit). This avoids per-pixel SPI
 *        traffic and flicker, and makes parameterized animation cheap.
 *
 *        Pixel format helper (SIMI_RGB) was reverse-engineered from the project's
 *        existing, empirically-correct color macros in ui.h. On this ESP-BOX-3 panel
 *        (BGR color space + byte swap handled by the driver) the 16-bit word decodes as:
 *            value = (B5 << 11) | (R6 << 5) | (G5 << 0)
 *        Verified: RED(255,0,0)->0x07E0, GREEN(0,255,0)->0x001F, BLUE(0,0,255)->0xF800,
 *        WHITE->0xFFFF, all matching the known-good macros.
 *
 * @author Lorenzo Martínez
 * @platform ESP32-S3-BOX3
 */
#ifndef MAIN_UI_SIMI_CANVAS_H
#define MAIN_UI_SIMI_CANVAS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief Compose a panel-native 16bpp color from 8-bit R/G/B components.
 *
 *        Empirically verified on this ESP-BOX-3 panel: it byte-swaps the 16-bit word
 *        and then interprets it as standard RGB565 (R[15:11] G[10:5] B[4:0]). So we
 *        build a normal RGB565 value and byte-swap it to the panel's wire order.
 *        (Confirmed against the project's RED/WHITE macros and on-device captures.)
 */
static inline uint16_t simi_color(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t s = (uint16_t)(((uint16_t)(r >> 3) << 11) |
                            ((uint16_t)(g >> 2) << 5) |
                            ((uint16_t)(b >> 3)));
    return (uint16_t)((s << 8) | (s >> 8));
}
#define SIMI_RGB(r, g, b) simi_color((uint8_t)(r), (uint8_t)(g), (uint8_t)(b))

/** @brief A mutable 16bpp drawing surface backed by a caller-owned buffer. */
typedef struct
{
    uint16_t *buf; /**< Pixel buffer, w*h uint16_t, row-major. */
    int w;         /**< Width in pixels. */
    int h;         /**< Height in pixels. */
    int clip_x0;   /**< Clip rect left (inclusive). */
    int clip_y0;   /**< Clip rect top (inclusive). */
    int clip_x1;   /**< Clip rect right (inclusive). */
    int clip_y1;   /**< Clip rect bottom (inclusive). */
} simi_canvas_t;

/* ── Primitivas de relleno ── */
void canvas_clear(simi_canvas_t *cv, uint16_t color);
void canvas_fill_rect(simi_canvas_t *cv, int x, int y, int w, int h, uint16_t color);
void canvas_fill_round_rect(simi_canvas_t *cv, int x, int y, int w, int h, int radius, uint16_t color);
void canvas_fill_circle(simi_canvas_t *cv, int cx, int cy, int r, uint16_t color);
void canvas_fill_ellipse(simi_canvas_t *cv, int cx, int cy, int rx, int ry, uint16_t color);
void canvas_fill_triangle(simi_canvas_t *cv, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color);

/* ── Primitivas de trazo ── */
/** @brief Hollow ellipse ring of given thickness (e.g. eyeglasses). */
void canvas_stroke_ellipse(simi_canvas_t *cv, int cx, int cy, int rx, int ry, int thickness, uint16_t color);
/** @brief Thick line segment drawn as a sweep of small discs. */
void canvas_thick_line(simi_canvas_t *cv, int x0, int y0, int x1, int y1, int thickness, uint16_t color);

/**
 * @brief Draws a quadratic mouth/smile curve centered at (cx, cy).
 * @param half_w   Half-width of the mouth in pixels.
 * @param curve    Vertical sag at center: >0 smile (∪), <0 frown (∩), 0 flat.
 * @param thickness Stroke thickness in pixels.
 */
void canvas_smile(simi_canvas_t *cv, int cx, int cy, int half_w, int curve, int thickness, uint16_t color);

#ifdef __cplusplus
}
#endif
#endif /* MAIN_UI_SIMI_CANVAS_H */
