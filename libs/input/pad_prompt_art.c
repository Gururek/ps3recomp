/*
 * ps3recomp - PC key-cap art for PS3 button prompts (see pad_prompt_art.h)
 *
 * Every shape is drawn white-on-black into a 4x supersampled 32bpp DIB,
 * box-filtered down to a coverage mask, and composited in software
 * (premultiplied, "over"):
 *   soft shadow  -> plate #101318 a=0.92 -> light border -> top highlight -> label
 * Labels use Segoe UI Semibold with grey antialiasing (no ClearType), shrunk
 * until they fit; a compact name ("Spc", "N8") is used when the full one
 * would get too small. Arrow keys are drawn as vector arrows. A D-pad bound
 * to numpad keys shows "Num" on the hub and one digit per arm.
 *
 * Compiled only when PS3RECOMP_PROMPT_ART is defined (every .c file in libs/input is
 * globbed into ps3recomp_runtime; without the define this unit is empty).
 */

#ifdef PS3RECOMP_PROMPT_ART

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pad_prompt_art.h"

#ifdef _MSC_VER
#pragma comment(lib, "gdi32.lib")
#endif

/* ---------------------------------------------------------------------------
 * Look
 * -----------------------------------------------------------------------*/
#define PPA_SS              4          /* supersampling factor per axis */
#define PPA_PLATE_ALPHA     0.92f
#define PPA_BORDER_ALPHA    0.95f
#define PPA_HILITE_ALPHA    0.16f
#define PPA_SHADOW_ALPHA    0.45f
#define PPA_HALO_ALPHA      0.60f      /* dark halo behind KEY_TEXT captions */
#define PPA_MIN_PX          9.0f       /* below this the compact label is tried */
#define PPA_MIN_PX_SMALL    8.0f       /* same, for D-pad arms */
#define PPA_ABS_MIN_PX      5.0f

static const float k_plate[3]      = {0x10 / 255.f, 0x13 / 255.f, 0x18 / 255.f};
static const float k_border[3]     = {0xC9 / 255.f, 0xCE / 255.f, 0xD6 / 255.f};
static const float k_white[3]      = {1.f, 1.f, 1.f};
static const float k_black[3]      = {0.f, 0.f, 0.f};
static const float k_dim_label[3]  = {0x8E / 255.f, 0x96 / 255.f, 0xA3 / 255.f};
static const float k_lit_plate[3]  = {0xC4 / 255.f, 0x3A / 255.f, 0x2C / 255.f};
static const float k_lit_border[3] = {0xFF / 255.f, 0xB0 / 255.f, 0xA0 / 255.f};

/* ---------------------------------------------------------------------------
 * Key names
 * -----------------------------------------------------------------------*/
typedef struct PpaKeyName { int vk; const char* full; const char* compact; } PpaKeyName;

#define PPA_UP    "\xE2\x86\x91"
#define PPA_DOWN  "\xE2\x86\x93"
#define PPA_LEFT  "\xE2\x86\x90"
#define PPA_RIGHT "\xE2\x86\x92"
#define PPA_DASH  "\xE2\x80\x94"

static const PpaKeyName k_key_names[] = {
    {VK_SPACE, "Space", "Spc"}, {VK_RETURN, "Enter", "Ent"}, {VK_BACK, "Bksp", "Bksp"},
    {VK_TAB, "Tab", "Tab"}, {VK_ESCAPE, "Esc", "Esc"}, {VK_CAPITAL, "Caps", "Caps"},
    {VK_SHIFT, "Shift", "Shft"}, {VK_LSHIFT, "Shift", "Shft"}, {VK_RSHIFT, "RShift", "RShf"},
    {VK_CONTROL, "Ctrl", "Ctrl"}, {VK_LCONTROL, "Ctrl", "Ctrl"}, {VK_RCONTROL, "RCtrl", "RCtl"},
    {VK_MENU, "Alt", "Alt"}, {VK_LMENU, "Alt", "Alt"}, {VK_RMENU, "RAlt", "RAlt"},
    {VK_UP, PPA_UP, PPA_UP}, {VK_DOWN, PPA_DOWN, PPA_DOWN},
    {VK_LEFT, PPA_LEFT, PPA_LEFT}, {VK_RIGHT, PPA_RIGHT, PPA_RIGHT},
    {VK_INSERT, "Ins", "Ins"}, {VK_DELETE, "Del", "Del"}, {VK_HOME, "Home", "Hm"},
    {VK_END, "End", "End"}, {VK_PRIOR, "PgUp", "PU"}, {VK_NEXT, "PgDn", "PD"},
    {VK_MULTIPLY, "Num*", "N*"}, {VK_ADD, "Num+", "N+"}, {VK_SUBTRACT, "Num-", "N-"},
    {VK_DECIMAL, "Num.", "N."}, {VK_DIVIDE, "Num/", "N/"},
    {VK_OEM_1, ";", ";"}, {VK_OEM_PLUS, "=", "="}, {VK_OEM_COMMA, ",", ","},
    {VK_OEM_MINUS, "-", "-"}, {VK_OEM_PERIOD, ".", "."}, {VK_OEM_2, "/", "/"},
    {VK_OEM_3, "`", "`"}, {VK_OEM_4, "[", "["}, {VK_OEM_5, "\\", "\\"},
    {VK_OEM_6, "]", "]"}, {VK_OEM_7, "'", "'"},
    {VK_LBUTTON, "M1", "M1"}, {VK_RBUTTON, "M2", "M2"}, {VK_MBUTTON, "M3", "M3"},
    {VK_XBUTTON1, "M4", "M4"}, {VK_XBUTTON2, "M5", "M5"},
};

void pad_prompt_key_names(int vk, char* full, size_t full_cap,
                          char* compact, size_t compact_cap)
{
    const char* f = NULL;
    const char* c = NULL;
    char fb[16], cb[16];
    if (vk == 0) {
        f = c = PPA_DASH;
    } else if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        fb[0] = (char)vk; fb[1] = 0;
        f = c = fb;
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(fb, sizeof(fb), "F%d", vk - VK_F1 + 1);
        f = c = fb;
    } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(fb, sizeof(fb), "Num%d", vk - VK_NUMPAD0);
        snprintf(cb, sizeof(cb), "N%d", vk - VK_NUMPAD0);
        f = fb; c = cb;
    } else {
        for (size_t i = 0; i < sizeof(k_key_names) / sizeof(k_key_names[0]); i++)
            if (k_key_names[i].vk == vk) { f = k_key_names[i].full; c = k_key_names[i].compact; break; }
        if (!f) f = c = "?";
    }
    if (full && full_cap) snprintf(full, full_cap, "%s", f);
    if (compact && compact_cap) snprintf(compact, compact_cap, "%s", c);
}

void pad_prompt_labels_from_vks(PadPromptLabels* out, const int vks[PAD_PROMPT_BUTTON_COUNT])
{
    for (int a = 0; a < PAD_PROMPT_BUTTON_COUNT; a++)
        pad_prompt_key_names(vks[a], out->full[a], sizeof(out->full[a]),
                             out->compact[a], sizeof(out->compact[a]));
}

/* ---------------------------------------------------------------------------
 * Supersampled mask canvas
 * -----------------------------------------------------------------------*/
typedef struct PpaCanvas {
    HDC dc;
    HBITMAP bmp;
    HGDIOBJ old_bmp;
    uint32_t* px;
    int W, H;          /* supersampled */
    int w, h;          /* output */
} PpaCanvas;

static int ppa_s(float v) { return (int)lroundf(v * (float)PPA_SS); }

static int ppa_canvas_open(PpaCanvas* c, int w, int h)
{
    memset(c, 0, sizeof(*c));
    c->w = w; c->h = h; c->W = w * PPA_SS; c->H = h * PPA_SS;
    c->dc = CreateCompatibleDC(NULL);
    if (!c->dc) return -1;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = c->W;
    bi.bmiHeader.biHeight = -c->H;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    c->bmp = CreateDIBSection(c->dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!c->bmp || !bits) { DeleteDC(c->dc); c->dc = NULL; return -1; }
    c->px = (uint32_t*)bits;
    c->old_bmp = SelectObject(c->dc, c->bmp);
    SetBkMode(c->dc, TRANSPARENT);
    SetPolyFillMode(c->dc, WINDING);
    return 0;
}

static void ppa_canvas_close(PpaCanvas* c)
{
    if (c->dc) {
        SelectObject(c->dc, c->old_bmp);
        DeleteObject(c->bmp);
        DeleteDC(c->dc);
    }
    memset(c, 0, sizeof(*c));
}

static void ppa_canvas_clear(PpaCanvas* c)
{
    GdiFlush();
    memset(c->px, 0, (size_t)c->W * (size_t)c->H * 4u);
}

/* Box-filters the green channel into cov[w*h] (0..1). */
static void ppa_canvas_take(PpaCanvas* c, float* cov)
{
    GdiFlush();
    const float norm = 1.f / (255.f * PPA_SS * PPA_SS);
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            unsigned sum = 0;
            for (int sy = 0; sy < PPA_SS; sy++) {
                const uint32_t* row = c->px + (size_t)(y * PPA_SS + sy) * (size_t)c->W + (size_t)x * PPA_SS;
                for (int sx = 0; sx < PPA_SS; sx++) sum += (row[sx] >> 8) & 0xFFu;
            }
            cov[y * c->w + x] = (float)sum * norm;
        }
}

static void ppa_fill_round_rect(PpaCanvas* c, float x0, float y0, float x1, float y1, float r)
{
    HGDIOBJ ob = SelectObject(c->dc, GetStockObject(WHITE_BRUSH));
    HGDIOBJ op = SelectObject(c->dc, GetStockObject(NULL_PEN));
    int d = ppa_s(2.f * r);
    if (d < 1) d = 1;
    /* With a null pen GDI leaves out the right and bottom edge: add one. */
    RoundRect(c->dc, ppa_s(x0), ppa_s(y0), ppa_s(x1) + 1, ppa_s(y1) + 1, d, d);
    SelectObject(c->dc, op);
    SelectObject(c->dc, ob);
}

static void ppa_fill_poly(PpaCanvas* c, const float* xy, int n)
{
    POINT p[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) { p[i].x = ppa_s(xy[2 * i]); p[i].y = ppa_s(xy[2 * i + 1]); }
    HGDIOBJ ob = SelectObject(c->dc, GetStockObject(WHITE_BRUSH));
    HGDIOBJ op = SelectObject(c->dc, GetStockObject(NULL_PEN));
    Polygon(c->dc, p, n);
    SelectObject(c->dc, op);
    SelectObject(c->dc, ob);
}

/* ---------------------------------------------------------------------------
 * Text
 * -----------------------------------------------------------------------*/
static HFONT ppa_font(float px)
{
    return CreateFontW(-(int)lroundf(px * PPA_SS), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI Semibold");
}

/* Advance width and cap height in output pixels. */
static void ppa_text_measure(HDC dc, const wchar_t* s, int n, float px, float* width, float* cap)
{
    HFONT f = ppa_font(px);
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz = {0, 0};
    GetTextExtentPoint32W(dc, s, n, &sz);
    OUTLINETEXTMETRICW otm;
    memset(&otm, 0, sizeof(otm));
    otm.otmSize = sizeof(otm);
    float c = 0.70f * px;
    if (GetOutlineTextMetricsW(dc, sizeof(otm), &otm) && otm.otmsCapEmHeight)
        c = (float)otm.otmsCapEmHeight / (float)PPA_SS;
    SelectObject(dc, o);
    DeleteObject(f);
    *width = (float)sz.cx / (float)PPA_SS;
    *cap = c;
}

/* Largest font size whose advance width fits box_w and cap height fits cap_h. */
static float ppa_text_fit(HDC dc, const wchar_t* s, int n, float box_w, float cap_h)
{
    const float ref = 40.f;
    float w, c, px = 400.f;
    ppa_text_measure(dc, s, n, ref, &w, &c);
    if (w > 0.f) px = fminf(px, box_w * ref / w);
    if (c > 0.f) px = fminf(px, cap_h * ref / c);
    for (int i = 0; i < 60 && px > 2.f; i++) {
        ppa_text_measure(dc, s, n, px, &w, &c);
        if (w <= box_w + 0.01f && c <= cap_h + 0.01f) break;
        px *= 0.97f;
    }
    return px;
}

static int ppa_arrow_dir(const char* s)
{
    if (!strcmp(s, PPA_UP)) return 1;
    if (!strcmp(s, PPA_DOWN)) return 2;
    if (!strcmp(s, PPA_LEFT)) return 3;
    if (!strcmp(s, PPA_RIGHT)) return 4;
    return 0;
}

static void ppa_draw_arrow(PpaCanvas* c, int dir, float cx, float cy, float len)
{
    const float sh = 0.12f * len, hw = 0.38f * len, hl = 0.50f * len, h = 0.5f * len;
    /* Pointing right, then rotated. */
    const float base[7][2] = {
        {-h, -sh}, {h - hl, -sh}, {h - hl, -hw}, {h, 0.f}, {h - hl, hw}, {h - hl, sh}, {-h, sh},
    };
    float xy[14];
    for (int i = 0; i < 7; i++) {
        float x = base[i][0], y = base[i][1], rx, ry;
        switch (dir) {
        case 1:  rx = y;  ry = -x; break;   /* up */
        case 2:  rx = -y; ry = x;  break;   /* down */
        case 3:  rx = -x; ry = -y; break;   /* left */
        default: rx = x;  ry = y;  break;   /* right */
        }
        xy[2 * i] = cx + rx;
        xy[2 * i + 1] = cy + ry;
    }
    ppa_fill_poly(c, xy, 7);
}

/* Draws a label centred at (cx, cy) inside box_w x cap_h (left-aligned at
 * left_x when left_x >= 0). Falls back to the compact name when the full one
 * would be smaller than min_px. */
static void ppa_draw_label(PpaCanvas* c, const char* full, const char* compact,
                           float cx, float cy, float box_w, float cap_h, float min_px,
                           float left_x)
{
    int dir = ppa_arrow_dir(full);
    if (dir) {
        ppa_draw_arrow(c, dir, cx, cy, fminf(box_w, cap_h * 1.5f));
        return;
    }
    wchar_t ws[32], wc[32];
    int n = MultiByteToWideChar(CP_UTF8, 0, full, -1, ws, 32) - 1;
    if (n <= 0) return;
    float px = ppa_text_fit(c->dc, ws, n, box_w, cap_h);
    if (px < min_px && compact && compact[0] && strcmp(compact, full) != 0) {
        int m = MultiByteToWideChar(CP_UTF8, 0, compact, -1, wc, 32) - 1;
        if (m > 0) {
            float pc = ppa_text_fit(c->dc, wc, m, box_w, cap_h);
            if (pc > px) { memcpy(ws, wc, sizeof(wc)); n = m; px = pc; }
        }
    }
    if (px < PPA_ABS_MIN_PX) px = PPA_ABS_MIN_PX;
    float w, cap;
    ppa_text_measure(c->dc, ws, n, px, &w, &cap);
    HFONT f = ppa_font(px);
    HGDIOBJ o = SelectObject(c->dc, f);
    SetTextColor(c->dc, RGB(255, 255, 255));
    SetTextAlign(c->dc, (left_x >= 0.f ? TA_LEFT : TA_CENTER) | TA_BASELINE);
    TextOutW(c->dc, ppa_s(left_x >= 0.f ? left_x : cx), ppa_s(cy + 0.5f * cap), ws, n);
    SelectObject(c->dc, o);
    DeleteObject(f);
}

/* ---------------------------------------------------------------------------
 * Software compositing (premultiplied RGBA floats)
 * -----------------------------------------------------------------------*/
static void ppa_over(float* acc, const float* cov, int n, const float rgb[3], float alpha)
{
    for (int i = 0; i < n; i++) {
        float a = cov[i] * alpha;
        if (a <= 0.f) continue;
        if (a > 1.f) a = 1.f;
        float* d = acc + 4 * i;
        d[0] = rgb[0] * a + d[0] * (1.f - a);
        d[1] = rgb[1] * a + d[1] * (1.f - a);
        d[2] = rgb[2] * a + d[2] * (1.f - a);
        d[3] = a + d[3] * (1.f - a);
    }
}

static void ppa_box_blur(float* v, float* tmp, int w, int h, int r)
{
    if (r <= 0) return;
    const float inv = 1.f / (float)(2 * r + 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float s = 0.f;
            for (int k = -r; k <= r; k++) {
                int xx = x + k;
                if (xx >= 0 && xx < w) s += v[y * w + xx];
            }
            tmp[y * w + x] = s * inv;
        }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float s = 0.f;
            for (int k = -r; k <= r; k++) {
                int yy = y + k;
                if (yy >= 0 && yy < h) s += tmp[yy * w + x];
            }
            v[y * w + x] = s * inv;
        }
}

/* ---------------------------------------------------------------------------
 * Caps
 * -----------------------------------------------------------------------*/
typedef struct PpaCap {
    float x0, y0, x1, y1;   /* plate rect, output px */
    float radius;
    int lit;                /* lit D-pad arm */
    int dim;                /* label in the dim colour */
    int hub;                /* plate only: no border, highlight or label */
    const char* full;
    const char* compact;
    float label_w;          /* label box width as a fraction of the inner width */
    float label_cap;        /* cap height as a fraction of the inner height */
    float min_px;
} PpaCap;

typedef struct PpaJob {
    PpaCanvas cv;
    int w, h, n;
    float* acc;             /* premultiplied RGBA */
    float* cov;
    float* cov2;
    float* tmp;
} PpaJob;

static float ppa_border_width(const PpaCap* k)
{
    float m = fminf(k->x1 - k->x0, k->y1 - k->y0);
    float b = 0.045f * m;
    return b < 1.f ? 1.f : (b > 2.5f ? 2.5f : b);
}

static void ppa_draw_shadow(PpaJob* j, const PpaCap* caps, int count)
{
    ppa_canvas_clear(&j->cv);
    for (int i = 0; i < count; i++)
        ppa_fill_round_rect(&j->cv, caps[i].x0 - 0.5f, caps[i].y0 + 0.5f,
                            caps[i].x1 + 0.5f, caps[i].y1 + 1.0f, caps[i].radius);
    ppa_canvas_take(&j->cv, j->cov);
    int r = (int)lroundf(0.035f * (float)(j->w < j->h ? j->w : j->h));
    if (r < 1) r = 1;
    ppa_box_blur(j->cov, j->tmp, j->w, j->h, r);
    ppa_box_blur(j->cov, j->tmp, j->w, j->h, r);
    ppa_over(j->acc, j->cov, j->n, k_black, PPA_SHADOW_ALPHA);
}

static void ppa_draw_cap(PpaJob* j, const PpaCap* k)
{
    const float bw = ppa_border_width(k);
    /* plate */
    ppa_canvas_clear(&j->cv);
    ppa_fill_round_rect(&j->cv, k->x0, k->y0, k->x1, k->y1, k->radius);
    ppa_canvas_take(&j->cv, j->cov);
    ppa_over(j->acc, j->cov, j->n, k->lit ? k_lit_plate : k_plate, PPA_PLATE_ALPHA);
    if (k->hub && (!k->full || !k->full[0])) return;

    /* inner shape -> border ring and highlight (not on the D-pad hub) */
    const float ix0 = k->x0 + bw, iy0 = k->y0 + bw, ix1 = k->x1 - bw, iy1 = k->y1 - bw;
    const float ih = iy1 - iy0;
    if (!k->hub) {
        const float ir = k->radius - bw > 0.5f ? k->radius - bw : 0.5f;
        ppa_canvas_clear(&j->cv);
        ppa_fill_round_rect(&j->cv, ix0, iy0, ix1, iy1, ir);
        ppa_canvas_take(&j->cv, j->cov2);
        for (int i = 0; i < j->n; i++) {
            float r = j->cov[i] - j->cov2[i];
            j->cov[i] = r > 0.f ? r : 0.f;
        }
        ppa_over(j->acc, j->cov, j->n, k->lit ? k_lit_border : k_border, PPA_BORDER_ALPHA);

        for (int y = 0; y < j->h; y++) {
            float t = ((float)y + 0.5f - iy0) / (ih > 1.f ? ih : 1.f);
            float g = t < 0.f ? 0.f : (t < 0.5f ? 1.f - t / 0.5f : 0.f);
            for (int x = 0; x < j->w; x++) j->cov2[y * j->w + x] *= g;
        }
        ppa_over(j->acc, j->cov2, j->n, k_white, PPA_HILITE_ALPHA);
    }

    /* label */
    if (!k->full || !k->full[0]) return;
    const float iw = ix1 - ix0;
    ppa_canvas_clear(&j->cv);
    ppa_draw_label(&j->cv, k->full, k->compact, 0.5f * (ix0 + ix1), 0.5f * (iy0 + iy1),
                   k->label_w * iw, k->label_cap * ih, k->min_px, -1.f);
    ppa_canvas_take(&j->cv, j->cov);
    ppa_over(j->acc, j->cov, j->n, k->dim ? k_dim_label : k_white, 1.f);
}

static void ppa_draw_caption(PpaJob* j, const char* text, float x0, float x1, float cy, float cap_h)
{
    if (!text || !text[0] || x1 - x0 < 2.f) return;
    ppa_canvas_clear(&j->cv);
    ppa_draw_label(&j->cv, text, NULL, 0.5f * (x0 + x1), cy, x1 - x0, cap_h, 0.f, x0);
    ppa_canvas_take(&j->cv, j->cov);
    memcpy(j->cov2, j->cov, sizeof(float) * (size_t)j->n);
    ppa_box_blur(j->cov2, j->tmp, j->w, j->h, 1);
    ppa_box_blur(j->cov2, j->tmp, j->w, j->h, 1);
    for (int i = 0; i < j->n; i++) j->cov2[i] = fminf(1.f, j->cov2[i] * 2.5f);
    ppa_over(j->acc, j->cov2, j->n, k_black, PPA_HALO_ALPHA);
    ppa_over(j->acc, j->cov, j->n, k_white, 1.f);
}

static const char* ppa_label(const PadPromptLabels* l, int b, int compact)
{
    if (b < 0 || b >= PAD_PROMPT_BUTTON_COUNT) return "?";
    return compact ? l->compact[b] : l->full[b];
}

int pad_prompt_art_render(const PadPromptArtSpec* spec, const PadPromptLabels* labels,
                          int w, int h, uint8_t* rgba)
{
    if (!spec || !labels || !rgba || w < 4 || h < 4 || w > 2048 || h > 2048) return -1;
    PpaJob j;
    memset(&j, 0, sizeof(j));
    j.w = w; j.h = h; j.n = w * h;
    j.acc = (float*)calloc((size_t)j.n * 4u, sizeof(float));
    j.cov = (float*)malloc(sizeof(float) * (size_t)j.n);
    j.cov2 = (float*)malloc(sizeof(float) * (size_t)j.n);
    j.tmp = (float*)malloc(sizeof(float) * (size_t)j.n);
    int rc = -1;
    if (!j.acc || !j.cov || !j.cov2 || !j.tmp || ppa_canvas_open(&j.cv, w, h) != 0) goto done;

    const float fw = (float)w, fh = (float)h, mn = fw < fh ? fw : fh;
    PpaCap caps[5];
    int ncaps = 0;
    memset(caps, 0, sizeof(caps));

    switch (spec->style) {
    case PAD_PROMPT_STYLE_KEY: {
        float m = fmaxf(1.5f, 0.07f * mn), side = mn - 2.f * m;
        PpaCap* k = &caps[ncaps++];
        k->x0 = 0.5f * (fw - side); k->y0 = 0.5f * (fh - side);
        k->x1 = k->x0 + side; k->y1 = k->y0 + side;
        k->radius = 0.20f * side;
        k->full = ppa_label(labels, spec->button, 0);
        k->compact = ppa_label(labels, spec->button, 1);
        k->label_w = 0.86f; k->label_cap = 0.40f; k->min_px = PPA_MIN_PX;
        break;
    }
    case PAD_PROMPT_STYLE_PLATE: {
        float m = fmaxf(1.5f, 0.08f * fh);
        PpaCap* k = &caps[ncaps++];
        k->x0 = m; k->y0 = m; k->x1 = fw - m; k->y1 = fh - m;
        k->radius = 0.28f * (k->y1 - k->y0);
        k->full = ppa_label(labels, spec->button, 0);
        k->compact = ppa_label(labels, spec->button, 1);
        k->label_w = 0.84f; k->label_cap = 0.46f; k->min_px = PPA_MIN_PX;
        break;
    }
    case PAD_PROMPT_STYLE_KEY_TEXT: {
        float m = fmaxf(1.5f, 0.07f * fh), side = fh - 2.f * m;
        if (side > fw - 2.f * m) side = fw - 2.f * m;
        PpaCap* k = &caps[ncaps++];
        k->x0 = m; k->y0 = 0.5f * (fh - side); k->x1 = m + side; k->y1 = k->y0 + side;
        k->radius = 0.20f * side;
        k->full = ppa_label(labels, spec->button, 0);
        k->compact = ppa_label(labels, spec->button, 1);
        k->label_w = 0.86f; k->label_cap = 0.40f; k->min_px = PPA_MIN_PX;
        break;
    }
    case PAD_PROMPT_STYLE_DPAD: {
        int first = PAD_PROMPT_UP;
        if (spec->button == PAD_PROMPT_GROUP_LSTICK) first = PAD_PROMPT_LS_UP;
        else if (spec->button == PAD_PROMPT_GROUP_RSTICK) first = PAD_PROMPT_RS_UP;
        if (!spec->lit && fw >= 1.3f * fh) {
            /* Wide cell with nothing lit (the right-stick sprites): the
             * keyboard's inverted-T arrow block. It fills the cell, so each
             * key is ~1.5x larger than in a cross that has to fit the height. */
            float m = fmaxf(1.f, 0.04f * mn);
            float u = fminf((fw - 2.f * m) / 3.f, (fh - 2.f * m) / 2.f);
            float g = fmaxf(0.8f, 0.06f * u);
            float ox = 0.5f * (fw - 3.f * u), oy = 0.5f * (fh - 2.f * u);
            const int tcell[4][2] = {{1, 0}, {1, 1}, {0, 1}, {2, 1}};   /* up, down, left, right */
            for (int a = 0; a < 4; a++) {
                PpaCap* k = &caps[ncaps++];
                k->x0 = ox + (float)tcell[a][0] * u + 0.5f * g;
                k->y0 = oy + (float)tcell[a][1] * u + 0.5f * g;
                k->x1 = ox + (float)(tcell[a][0] + 1) * u - 0.5f * g;
                k->y1 = oy + (float)(tcell[a][1] + 1) * u - 0.5f * g;
                k->radius = 0.20f * u;
                k->full = ppa_label(labels, first + a, 0);
                k->compact = ppa_label(labels, first + a, 1);
                k->label_w = 0.84f; k->label_cap = 0.50f; k->min_px = PPA_MIN_PX_SMALL;
            }
            break;
        }
        /* Four numpad keys ("Num8", "Num2", ...): the hub says "Num" and each
         * arm shows only its digit, which can be drawn much larger than "N8". */
        int num = 1;
        for (int a = 0; a < 4 && num; a++) {
            const char* s = ppa_label(labels, first + a, 0);
            num = strncmp(s, "Num", 3) == 0 && s[3] >= '0' && s[3] <= '9' && s[4] == 0;
        }
        float m = fmaxf(1.f, 0.04f * mn), S = mn - 2.f * m, u = S / 3.f;
        float g = fmaxf(0.6f, 0.035f * S);
        float ox = 0.5f * (fw - S), oy = 0.5f * (fh - S);
        /* arm cells: up, down, left, right (grid col,row) */
        const int cell[4][2] = {{1, 0}, {1, 2}, {0, 1}, {2, 1}};
        const unsigned bit[4] = {PAD_PROMPT_LIT_UP, PAD_PROMPT_LIT_DOWN,
                                 PAD_PROMPT_LIT_LEFT, PAD_PROMPT_LIT_RIGHT};
        PpaCap* hub = &caps[ncaps++];
        hub->x0 = ox + u - 0.5f * g; hub->y0 = oy + u - 0.5f * g;
        hub->x1 = ox + 2.f * u + 0.5f * g; hub->y1 = oy + 2.f * u + 0.5f * g;
        hub->radius = 0.12f * u;
        hub->hub = 1;
        if (num) {
            hub->full = hub->compact = "Num";
            hub->label_w = 0.92f; hub->label_cap = 0.40f; hub->min_px = 0.f;
        }
        for (int a = 0; a < 4; a++) {
            PpaCap* k = &caps[ncaps++];
            k->x0 = ox + (float)cell[a][0] * u + 0.5f * g;
            k->y0 = oy + (float)cell[a][1] * u + 0.5f * g;
            k->x1 = ox + (float)(cell[a][0] + 1) * u - 0.5f * g;
            k->y1 = oy + (float)(cell[a][1] + 1) * u - 0.5f * g;
            k->radius = 0.22f * u;
            k->lit = (spec->lit & bit[a]) != 0;
            k->dim = spec->lit && !k->lit;
            k->full = ppa_label(labels, first + a, 0);
            k->compact = ppa_label(labels, first + a, 1);
            if (num) k->full = k->compact = k->full + 3;    /* "8" */
            k->label_w = 0.88f; k->label_cap = num ? 0.60f : 0.50f; k->min_px = PPA_MIN_PX_SMALL;
        }
        break;
    }
    default:
        goto done;
    }

    ppa_draw_shadow(&j, caps, ncaps);
    for (int i = 0; i < ncaps; i++) ppa_draw_cap(&j, &caps[i]);
    if (spec->style == PAD_PROMPT_STYLE_KEY_TEXT) {
        const PpaCap* k = &caps[0];
        float side = k->x1 - k->x0;
        float x0 = k->x1 + 0.16f * side, x1 = fw - 1.f;
        ppa_draw_caption(&j, spec->caption, x0, x1, 0.5f * (k->y0 + k->y1), 0.42f * side);
    }

    /* Straight alpha out; transparent texels carry the plate colour. */
    for (int i = 0; i < j.n; i++) {
        const float* s = j.acc + 4 * i;
        float a = s[3];
        uint8_t* d = rgba + 4 * i;
        if (a < 0.5f / 255.f) {
            for (int c = 0; c < 3; c++) d[c] = (uint8_t)lroundf(k_plate[c] * 255.f);
            d[3] = 0;
            continue;
        }
        for (int c = 0; c < 3; c++) {
            float v = s[c] / a;
            v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
            d[c] = (uint8_t)lroundf(v * 255.f);
        }
        d[3] = (uint8_t)lroundf((a > 1.f ? 1.f : a) * 255.f);
    }
    rc = 0;
done:
    ppa_canvas_close(&j.cv);
    free(j.acc); free(j.cov); free(j.cov2); free(j.tmp);
    return rc;
}

#else  /* !PS3RECOMP_PROMPT_ART */
typedef int pad_prompt_art_disabled;   /* ISO C: a unit needs one declaration */
#endif
