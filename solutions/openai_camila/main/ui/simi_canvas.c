/**
 * @file simi_canvas.c
 * @brief Software rasterizer primitives for the Dr. Simi visual system.
 *
 *        Integer-friendly, bounds-clipped fills and strokes over a 16bpp buffer.
 *        sqrt() is used per-row (not per-pixel) so cost stays modest.
 *
 * @author Lorenzo Martínez
 * @platform ESP32-S3-BOX3
 */
#include "simi_canvas.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** @brief Write one pixel with bounds checking. */
static inline void set_px(simi_canvas_t *cv, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= cv->w || y >= cv->h)
    {
        return;
    }
    cv->buf[y * cv->w + x] = color;
}

/** @brief Fill a horizontal run [x0, x1] on row y, clipped to the canvas. */
static inline void hspan(simi_canvas_t *cv, int x0, int x1, int y, uint16_t color)
{
    if (y < cv->clip_y0 || y > cv->clip_y1)
    {
        return;
    }
    if (x0 < cv->clip_x0)
    {
        x0 = cv->clip_x0;
    }
    if (x1 > cv->clip_x1)
    {
        x1 = cv->clip_x1;
    }
    if (x0 > x1)
    {
        return;
    }
    uint16_t *row = &cv->buf[y * cv->w];
    for (int x = x0; x <= x1; x++)
    {
        row[x] = color;
    }
}

void canvas_clear(simi_canvas_t *cv, uint16_t color)
{
    if (!cv || !cv->buf)
    {
        return;
    }
    canvas_fill_rect(cv, cv->clip_x0, cv->clip_y0, cv->clip_x1 - cv->clip_x0 + 1, cv->clip_y1 - cv->clip_y0 + 1, color);
}

void canvas_fill_rect(simi_canvas_t *cv, int x, int y, int w, int h, uint16_t color)
{
    if (!cv || w <= 0 || h <= 0)
    {
        return;
    }
    for (int row = y; row < y + h; row++)
    {
        hspan(cv, x, x + w - 1, row, color);
    }
}

void canvas_fill_ellipse(simi_canvas_t *cv, int cx, int cy, int rx, int ry, uint16_t color)
{
    if (!cv || rx <= 0 || ry <= 0)
    {
        return;
    }
    for (int dy = -ry; dy <= ry; dy++)
    {
        // half-width of the ellipse at this row
        float t = 1.0f - ((float)(dy * dy) / (float)(ry * ry));
        if (t < 0.0f)
        {
            continue;
        }
        int hw = (int)(rx * sqrtf(t) + 0.5f);
        hspan(cv, cx - hw, cx + hw, cy + dy, color);
    }
}

void canvas_fill_circle(simi_canvas_t *cv, int cx, int cy, int r, uint16_t color)
{
    canvas_fill_ellipse(cv, cx, cy, r, r, color);
}

void canvas_fill_round_rect(simi_canvas_t *cv, int x, int y, int w, int h, int radius, uint16_t color)
{
    if (!cv || w <= 0 || h <= 0)
    {
        return;
    }
    if (radius * 2 > w)
    {
        radius = w / 2;
    }
    if (radius * 2 > h)
    {
        radius = h / 2;
    }
    if (radius <= 0)
    {
        canvas_fill_rect(cv, x, y, w, h, color);
        return;
    }

    // Center band (full width)
    canvas_fill_rect(cv, x, y + radius, w, h - 2 * radius, color);

    // Top and bottom rounded bands
    for (int dy = 0; dy < radius; dy++)
    {
        float t = 1.0f - ((float)((radius - dy) * (radius - dy)) / (float)(radius * radius));
        if (t < 0.0f)
        {
            t = 0.0f;
        }
        int inset = radius - (int)(radius * sqrtf(t) + 0.5f);
        int xl = x + inset;
        int xr = x + w - 1 - inset;
        hspan(cv, xl, xr, y + dy, color);                // top
        hspan(cv, xl, xr, y + h - 1 - dy, color);        // bottom
    }
}

void canvas_stroke_ellipse(simi_canvas_t *cv, int cx, int cy, int rx, int ry, int thickness, uint16_t color)
{
    if (!cv || rx <= 0 || ry <= 0 || thickness <= 0)
    {
        return;
    }
    int irx = rx - thickness;
    int iry = ry - thickness;
    for (int dy = -ry; dy <= ry; dy++)
    {
        float to = 1.0f - ((float)(dy * dy) / (float)(ry * ry));
        if (to < 0.0f)
        {
            continue;
        }
        int hwo = (int)(rx * sqrtf(to) + 0.5f);

        int hwi = -1;
        if (irx > 0 && iry > 0 && dy > -iry && dy < iry)
        {
            float ti = 1.0f - ((float)(dy * dy) / (float)(iry * iry));
            if (ti > 0.0f)
            {
                hwi = (int)(irx * sqrtf(ti) + 0.5f);
            }
        }

        if (hwi < 0)
        {
            // Solid row (top/bottom caps of the ring)
            hspan(cv, cx - hwo, cx + hwo, cy + dy, color);
        }
        else
        {
            // Two side segments, hollow center
            hspan(cv, cx - hwo, cx - hwi, cy + dy, color);
            hspan(cv, cx + hwi, cx + hwo, cy + dy, color);
        }
    }
}

void canvas_fill_triangle(simi_canvas_t *cv, int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color)
{
    if (!cv)
    {
        return;
    }
    // Sort vertices by y ascending (v0 top, v2 bottom)
    if (y0 > y1) { int t; t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
    if (y0 > y2) { int t; t = y0; y0 = y2; y2 = t; t = x0; x0 = x2; x2 = t; }
    if (y1 > y2) { int t; t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }

    int total_h = y2 - y0;
    if (total_h == 0)
    {
        return;
    }

    for (int yy = y0; yy <= y2; yy++)
    {
        bool second = (yy > y1) || (y1 == y0);
        int seg_h = second ? (y2 - y1) : (y1 - y0);
        if (seg_h == 0)
        {
            seg_h = 1;
        }
        float a = (float)(yy - y0) / (float)total_h;                 // along long edge v0->v2
        float b = second ? (float)(yy - y1) / (float)seg_h           // along v1->v2
                         : (float)(yy - y0) / (float)seg_h;          // along v0->v1
        int ax = x0 + (int)((x2 - x0) * a + 0.5f);
        int bx = second ? x1 + (int)((x2 - x1) * b + 0.5f)
                        : x0 + (int)((x1 - x0) * b + 0.5f);
        if (ax > bx)
        {
            int t = ax; ax = bx; bx = t;
        }
        hspan(cv, ax, bx, yy, color);
    }
}

void canvas_thick_line(simi_canvas_t *cv, int x0, int y0, int x1, int y1, int thickness, uint16_t color)
{
    if (!cv)
    {
        return;
    }
    int r = thickness / 2;
    if (r < 1)
    {
        r = 1;
    }
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int steps = (dx > dy ? dx : dy);
    if (steps == 0)
    {
        canvas_fill_circle(cv, x0, y0, r, color);
        return;
    }
    for (int i = 0; i <= steps; i++)
    {
        int px = x0 + (x1 - x0) * i / steps;
        int py = y0 + (y1 - y0) * i / steps;
        canvas_fill_circle(cv, px, py, r, color);
    }
}

void canvas_smile(simi_canvas_t *cv, int cx, int cy, int half_w, int curve, int thickness, uint16_t color)
{
    if (!cv || half_w <= 0)
    {
        return;
    }
    int r = thickness / 2;
    if (r < 1)
    {
        r = 1;
    }
    for (int dx = -half_w; dx <= half_w; dx++)
    {
        // y(dx) = cy + curve * (1 - (dx/half_w)^2)
        //   curve > 0 -> valley/∪ (smile), curve < 0 -> hump/∩ (frown)
        float norm = (float)(dx * dx) / (float)(half_w * half_w);
        int yy = cy + (int)(curve * (1.0f - norm) + 0.5f);
        canvas_fill_circle(cv, cx + dx, yy, r, color);
    }
}
