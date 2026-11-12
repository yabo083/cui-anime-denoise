/* sheet.c -- the --all comparison sheet for the CLI: the original plus the
 * seven method results in a labelled 4x2 grid, identical layout to
 * denoise.py's sheet():
 *
 *     g = 20, top = max(40, w // 18), cols = 4, rows = 2
 *     canvas = (4*(w+g)-g, 2*(h+top+g)), background #3C3C3C
 *     cell i at (i%4*(w+g), i//4*(h+top+g)), image at +top, label above it
 *
 * Labels are drawn with GDI on a 32bpp DIB. The bundled HarmonyOS font is
 * loaded privately (AddFontResourceExW) so it neither needs installing nor
 * leaks into the system's font table; Arial is the fallback.
 */
#include "../img.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static wchar_t *u2w(const char *s)
{
    int n;
    wchar_t *w;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, n) != n) {
        free(w);
        return NULL;
    }
    return w;
}

static int CALLBACK on_font_enum(const LOGFONTW *lf, const TEXTMETRICW *tm,
                                 DWORD type, LPARAM lp)
{
    (void)lf; (void)tm; (void)type;
    *(int *)lp = 1;
    return 0;   /* found: stop enumerating */
}

static int face_exists(const wchar_t *name)
{
    HDC dc = GetDC(NULL);
    LOGFONTW probe;
    int found = 0;
    if (!dc) return 0;
    memset(&probe, 0, sizeof probe);
    probe.lfCharSet = DEFAULT_CHARSET;
    wcsncpy(probe.lfFaceName, name, LF_FACESIZE - 1);
    EnumFontFamiliesExW(dc, &probe, on_font_enum, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

/* Loads the bundled HarmonyOS font privately; NULL when not found. */
static HANDLE load_private_font(const wchar_t *exe_dir)
{
    wchar_t path[1024];
    if (!exe_dir) return NULL;
    swprintf(path, 1024, L"%sassets\\fonts\\HarmonyOS-Sans-SC-Medium.ttf", exe_dir);
    if (!AddFontResourceExW(path, FR_PRIVATE, NULL)) return NULL;
    return (HANDLE)1;   /* marker: call remove_private_font */
}

static void remove_private_font(const wchar_t *exe_dir)
{
    wchar_t path[1024];
    if (!exe_dir) return;
    swprintf(path, 1024, L"%sassets\\fonts\\HarmonyOS-Sans-SC-Medium.ttf", exe_dir);
    RemoveFontResourceExW(path, FR_PRIVATE, NULL);
}

/* Picks the first family name that resolves; "Arial" always does. */
static const wchar_t *pick_face(void)
{
    static const wchar_t *const candidates[] = {
        L"HarmonyOS Sans SC Medium", L"HarmonyOS Sans SC", L"Arial"
    };
    for (int i = 0; i < 3; i++)
        if (face_exists(candidates[i])) return candidates[i];
    return L"Arial";
}

/* Draws the white labels onto the canvas through a 32bpp top-down DIB. */
static void draw_labels(const wchar_t *exe_dir, img_pixels *canvas,
                        int cw, int ch, int w, int h, int g, int top,
                        const char *const labels[8])
{
    wchar_t face[LF_FACESIZE];
    HDC dc = NULL, mem = NULL;
    HBITMAP dib = NULL, old_dib = NULL;
    HFONT font = NULL, old_font = NULL;
    BITMAPINFO bi;
    void *bits = NULL;
    HANDLE loaded;

    loaded = load_private_font(exe_dir);

    dc = GetDC(NULL);
    if (!dc) goto done;
    mem = CreateCompatibleDC(dc);
    if (!mem) goto done;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cw;
    bi.bmiHeader.biHeight = -ch;           /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib || !bits) goto done;
    old_dib = (HBITMAP)SelectObject(mem, dib);

    /* canvas RGBA -> DIB BGRA */
    for (size_t i = 0, n = (size_t)cw * ch; i < n; i++) {
        ((uint8_t *)bits)[i * 4 + 0] = canvas->rgba[i * 4 + 2];
        ((uint8_t *)bits)[i * 4 + 1] = canvas->rgba[i * 4 + 1];
        ((uint8_t *)bits)[i * 4 + 2] = canvas->rgba[i * 4 + 0];
        ((uint8_t *)bits)[i * 4 + 3] = 255;
    }

    {
        const wchar_t *f = pick_face();
        wcsncpy(face, f, LF_FACESIZE - 1);
        face[LF_FACESIZE - 1] = 0;
    }
    font = CreateFontW(-(top / 2), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
    if (font) old_font = (HFONT)SelectObject(mem, font);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    SetTextAlign(mem, TA_LEFT | TA_TOP | TA_NOUPDATECP);

    for (int i = 0; i < 8; i++) {
        wchar_t *wl;
        int x = (i % 4) * (w + g);
        int y = (i / 4) * (h + top + g);
        if (!labels[i]) continue;
        wl = u2w(labels[i]);
        if (wl) {
            TextOutW(mem, x + 12, y + top / 5, wl, (int)wcslen(wl));
            free(wl);
        }
    }

    /* back to RGBA (alpha stays opaque) */
    for (size_t i = 0, n = (size_t)cw * ch; i < n; i++) {
        canvas->rgba[i * 4 + 0] = ((const uint8_t *)bits)[i * 4 + 2];
        canvas->rgba[i * 4 + 1] = ((const uint8_t *)bits)[i * 4 + 1];
        canvas->rgba[i * 4 + 2] = ((const uint8_t *)bits)[i * 4 + 0];
        canvas->rgba[i * 4 + 3] = 255;
    }

done:
    if (old_font) SelectObject(mem, old_font);
    if (font) DeleteObject(font);
    if (old_dib) SelectObject(mem, old_dib);
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    if (dc) ReleaseDC(NULL, dc);
    if (loaded) remove_private_font(exe_dir);
}

/* cells[0] is the original; cells[1..7] the method results, all the same
 * size. The sheet is returned in `out` (RGBA8, alpha 255). */
bool cli_sheet_make(const wchar_t *exe_dir, const img_pixels *cells[8],
                    const char *const labels[8], img_pixels *out)
{
    int w, h, g = 20, top, cw, ch;
    img_pixels sheet = { NULL, 0, 0 };

    if (!cells || !cells[0] || !cells[0]->rgba || !out) return false;
    w = cells[0]->w;
    h = cells[0]->h;
    if (w <= 0 || h <= 0) return false;
    for (int i = 1; i < 8; i++) {
        if (!cells[i] || !cells[i]->rgba || cells[i]->w != w || cells[i]->h != h)
            return false;
    }

    top = w / 18 > 40 ? w / 18 : 40;
    cw = 4 * (w + g) - g;
    ch = 2 * (h + top + g);
    if ((uint64_t)cw * (uint64_t)ch * 4u > (uint64_t)2048u * 1024u * 1024u)
        return false;
    sheet.rgba = (uint8_t *)malloc((size_t)cw * (size_t)ch * 4u);
    if (!sheet.rgba) return false;
    sheet.w = cw;
    sheet.h = ch;

    /* #3C3C3C background, then each cell's RGB at (x, y+top) */
    for (size_t i = 0, n = (size_t)cw * ch; i < n; i++) {
        sheet.rgba[i * 4 + 0] = 60;
        sheet.rgba[i * 4 + 1] = 60;
        sheet.rgba[i * 4 + 2] = 60;
        sheet.rgba[i * 4 + 3] = 255;
    }
    for (int i = 0; i < 8; i++) {
        int ox = (i % 4) * (w + g);
        int oy = (i / 4) * (h + top + g) + top;
        const img_pixels *im = cells[i];
        for (int y = 0; y < h; y++) {
            const uint8_t *s = im->rgba + (size_t)y * w * 4u;
            uint8_t *d = sheet.rgba + ((size_t)(oy + y) * cw + ox) * 4u;
            for (int x = 0; x < w; x++, s += 4, d += 4) {
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;   /* the reference pastes RGB: alpha is dropped */
            }
        }
    }

    draw_labels(exe_dir, &sheet, cw, ch, w, h, g, top, labels);
    *out = sheet;
    return true;
}
