#include "vm_startup_scene.h"

#include <strsafe.h>

#define STARTUP_TEXT_CHARS 192
#define STARTUP_SPINNER_INTERVAL_MS 120

struct VmStartupScene {
    AsbStartupPosition position;
    AsbStartupMotion motion;
    COLORREF background_color;
    COLORREF foreground_color;
    COLORREF accent_color;
    UINT delayed_message_after_ms;
    wchar_t app_name[128];
    wchar_t opening_message[STARTUP_TEXT_CHARS];
    wchar_t preparing_message[STARTUP_TEXT_CHARS];
    wchar_t finishing_message[STARTUP_TEXT_CHARS];
    wchar_t delayed_message[STARTUP_TEXT_CHARS];
    wchar_t failed_message[STARTUP_TEXT_CHARS];
    wchar_t failure_detail[STARTUP_TEXT_CHARS];
    wchar_t mark_path[MAX_PATH];
    HICON mark_icon;
    HDC dc;
    HBITMAP bitmap;
    HGDIOBJ previous_bitmap;
    BYTE *pixels;
    UINT width;
    UINT height;
    UINT stride;
    UINT dpi;
    AsbStartupPhase painted_phase;
    BOOL painted_detailed;
    BOOL delay_visible;
    BOOL animations_enabled;
    UINT spinner_frame;
    ULONGLONG started_ms;
    ULONGLONG last_spinner_ms;
    RECT status_rect;
};

static void copy_text(wchar_t *destination, size_t destination_chars,
                      const wchar_t *source, const wchar_t *fallback)
{
    const wchar_t *value = source && source[0] ? source : fallback;
    if (!value) value = L"";
    StringCchCopyW(destination, destination_chars, value);
}

static int scaled(UINT value, UINT dpi)
{
    return MulDiv((int)value, (int)(dpi ? dpi : 96), 96);
}

static HFONT scene_font(UINT dpi, int points, int weight)
{
    HDC screen = GetDC(NULL);
    int logical_height = -MulDiv(points, (int)(dpi ? dpi : 96), 72);
    HFONT font;
    (void)screen;
    font = CreateFontW(logical_height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
    if (screen) ReleaseDC(NULL, screen);
    return font;
}

static void release_bitmap(VmStartupScene *scene)
{
    if (!scene) return;
    if (scene->dc && scene->previous_bitmap) {
        SelectObject(scene->dc, scene->previous_bitmap);
        scene->previous_bitmap = NULL;
    }
    if (scene->bitmap) {
        DeleteObject(scene->bitmap);
        scene->bitmap = NULL;
    }
    scene->pixels = NULL;
    scene->width = 0;
    scene->height = 0;
    scene->stride = 0;
}

static BOOL ensure_bitmap(VmStartupScene *scene, UINT width, UINT height)
{
    BITMAPINFO info;
    void *pixels = NULL;
    HBITMAP bitmap;

    if (scene->bitmap && scene->width == width && scene->height == height)
        return TRUE;
    if (!width || !height) return FALSE;

    release_bitmap(scene);
    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)width;
    info.bmiHeader.biHeight = -(LONG)height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap = CreateDIBSection(scene->dc, &info, DIB_RGB_COLORS,
                              &pixels, NULL, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        return FALSE;
    }
    scene->previous_bitmap = SelectObject(scene->dc, bitmap);
    scene->bitmap = bitmap;
    scene->pixels = (BYTE *)pixels;
    scene->width = width;
    scene->height = height;
    scene->stride = width * 4;
    return TRUE;
}

static const wchar_t *phase_message(const VmStartupScene *scene,
                                    AsbStartupPhase phase)
{
    switch (phase) {
    case ASB_STARTUP_OPENING: return scene->opening_message;
    case ASB_STARTUP_PREPARING: return scene->preparing_message;
    case ASB_STARTUP_FAILED: return scene->failed_message;
    case ASB_STARTUP_FINISHING:
    case ASB_STARTUP_READY: return scene->finishing_message;
    default: return L"";
    }
}

static const wchar_t *phase_detail(const VmStartupScene *scene,
                                   AsbStartupPhase phase)
{
    switch (phase) {
    case ASB_STARTUP_OPENING: return L"Starting the application runtime";
    case ASB_STARTUP_PREPARING: return L"Preparing graphics and browser services";
    case ASB_STARTUP_FINISHING:
    case ASB_STARTUP_READY: return L"Waiting for the first application frame";
    case ASB_STARTUP_FAILED:
        return scene->failure_detail[0]
            ? scene->failure_detail : L"Startup did not complete";
    default: return L"";
    }
}

static void fill_rect(HDC dc, const RECT *rect, COLORREF color)
{
    HBRUSH brush = CreateSolidBrush(color);
    if (brush) {
        FillRect(dc, rect, brush);
        DeleteObject(brush);
    }
}

static void draw_center_mark(VmStartupScene *scene)
{
    int size = scaled(58, scene->dpi);
    int x = ((int)scene->width - size) / 2;
    int y = ((int)scene->height - size) / 2 - scaled(34, scene->dpi);

    if (scene->position == ASB_STARTUP_CENTER)
        y -= scaled(64, scene->dpi);

    if (scene->mark_icon) {
        DrawIconEx(scene->dc, x, y, scene->mark_icon, size, size,
                   0, NULL, DI_NORMAL);
        return;
    }

    {
        HBRUSH brush = CreateSolidBrush(scene->accent_color);
        HPEN pen = CreatePen(PS_SOLID, 1, scene->accent_color);
        HGDIOBJ old_brush = brush ? SelectObject(scene->dc, brush) : NULL;
        HGDIOBJ old_pen = pen ? SelectObject(scene->dc, pen) : NULL;
        wchar_t initial[2] = { scene->app_name[0] ? scene->app_name[0] : L'L', 0 };
        HFONT font = scene_font(scene->dpi, 22, FW_SEMIBOLD);
        HGDIOBJ old_font = font ? SelectObject(scene->dc, font) : NULL;
        RECT text_rect = { x, y, x + size, y + size };

        Ellipse(scene->dc, x, y, x + size, y + size);
        SetBkMode(scene->dc, TRANSPARENT);
        SetTextColor(scene->dc, scene->background_color);
        DrawTextW(scene->dc, initial, -1, &text_rect,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (old_font) SelectObject(scene->dc, old_font);
        if (font) DeleteObject(font);
        if (old_pen) SelectObject(scene->dc, old_pen);
        if (pen) DeleteObject(pen);
        if (old_brush) SelectObject(scene->dc, old_brush);
        if (brush) DeleteObject(brush);
    }
}

static void calculate_status_rect(VmStartupScene *scene)
{
    int margin = scaled(32, scene->dpi);
    int height = scaled(76, scene->dpi);
    if (scene->position == ASB_STARTUP_CENTER) {
        int width = (int)scene->width - margin * 2;
        scene->status_rect.left = margin;
        scene->status_rect.right = margin + width;
        scene->status_rect.top = (int)scene->height / 2 + scaled(22, scene->dpi);
        scene->status_rect.bottom = scene->status_rect.top + height;
    } else {
        scene->status_rect.left = margin;
        scene->status_rect.right = (int)scene->width - margin;
        scene->status_rect.bottom = (int)scene->height - margin;
        scene->status_rect.top = scene->status_rect.bottom - height;
    }
    if (scene->status_rect.right < scene->status_rect.left)
        scene->status_rect.right = scene->status_rect.left;
    if (scene->status_rect.bottom > (LONG)scene->height)
        scene->status_rect.bottom = (LONG)scene->height;
}

static void draw_status(VmStartupScene *scene, AsbStartupPhase phase,
                        BOOL detailed, BOOL delay_visible)
{
    static const wchar_t *glyphs[] = { L"✦", L"✧", L"·", L"✧" };
    const wchar_t *message = phase_message(scene, phase);
    const wchar_t *detail = L"";
    RECT glyph_rect = scene->status_rect;
    RECT message_rect = scene->status_rect;
    RECT detail_rect = scene->status_rect;
    HFONT message_font = scene_font(scene->dpi, 13, FW_SEMIBOLD);
    HFONT detail_font = scene_font(scene->dpi, 10, FW_NORMAL);
    HGDIOBJ old_font;
    int glyph_width = scaled(28, scene->dpi);
    UINT message_format = DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                          DT_END_ELLIPSIS | DT_NOPREFIX;

    fill_rect(scene->dc, &scene->status_rect, scene->background_color);
    SetBkMode(scene->dc, TRANSPARENT);

    if (scene->position == ASB_STARTUP_CENTER && message_font) {
        SIZE extent = {0};
        int available = scene->status_rect.right - scene->status_rect.left;
        int message_width;
        old_font = SelectObject(scene->dc, message_font);
        GetTextExtentPoint32W(scene->dc, message, (int)wcslen(message), &extent);
        SelectObject(scene->dc, old_font);
        message_width = extent.cx;
        if (message_width > available - glyph_width)
            message_width = available - glyph_width;
        if (message_width < 0) message_width = 0;
        glyph_rect.left = scene->status_rect.left +
            (available - glyph_width - message_width) / 2;
        message_rect.left = glyph_rect.left + glyph_width;
        message_rect.right = message_rect.left + message_width;
    } else {
        message_rect.left += glyph_width;
    }
    glyph_rect.right = glyph_rect.left + glyph_width;
    glyph_rect.bottom = glyph_rect.top + scaled(30, scene->dpi);
    SetTextColor(scene->dc, scene->accent_color);
    old_font = message_font ? SelectObject(scene->dc, message_font) : NULL;
    DrawTextW(scene->dc,
              glyphs[scene->spinner_frame % ARRAYSIZE(glyphs)], -1,
              &glyph_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    message_rect.bottom = message_rect.top + scaled(30, scene->dpi);
    SetTextColor(scene->dc, scene->foreground_color);
    DrawTextW(scene->dc, message, -1, &message_rect,
              message_format);

    if (old_font) SelectObject(scene->dc, old_font);
    if (detailed)
        detail = phase_detail(scene, phase);
    else if (delay_visible)
        detail = scene->delayed_message;

    if (detail[0]) {
        detail_rect.left += scene->position == ASB_STARTUP_CENTER ? 0 : glyph_width;
        detail_rect.top += scaled(30, scene->dpi);
        detail_rect.bottom = detail_rect.top + scaled(26, scene->dpi);
        old_font = detail_font ? SelectObject(scene->dc, detail_font) : NULL;
        SetTextColor(scene->dc, RGB(
            (GetRValue(scene->foreground_color) + GetRValue(scene->background_color)) / 2,
            (GetGValue(scene->foreground_color) + GetGValue(scene->background_color)) / 2,
            (GetBValue(scene->foreground_color) + GetBValue(scene->background_color)) / 2));
        DrawTextW(scene->dc, detail, -1, &detail_rect,
                  (scene->position == ASB_STARTUP_CENTER ? DT_CENTER : DT_LEFT) |
                  DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (old_font) SelectObject(scene->dc, old_font);
    }

    if (message_font) DeleteObject(message_font);
    if (detail_font) DeleteObject(detail_font);
}

VmStartupScene *vm_startup_scene_create(const AsbStartupOptions *options)
{
    VmStartupScene *scene;
    BOOL animations = TRUE;

    if (!options || !options->enabled) return NULL;
    scene = (VmStartupScene *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         sizeof(*scene));
    if (!scene) return NULL;

    scene->position = options->position;
    scene->motion = options->motion;
    scene->background_color = options->background_color;
    scene->foreground_color = options->foreground_color;
    scene->accent_color = options->accent_color;
    scene->delayed_message_after_ms = options->delayed_message_after_ms
        ? options->delayed_message_after_ms : 10000;
    copy_text(scene->app_name, ARRAYSIZE(scene->app_name),
              options->app_name, L"Linguum");
    copy_text(scene->opening_message, ARRAYSIZE(scene->opening_message),
              options->opening_message, L"Opening your application…");
    copy_text(scene->preparing_message, ARRAYSIZE(scene->preparing_message),
              options->preparing_message, L"Getting things ready…");
    copy_text(scene->finishing_message, ARRAYSIZE(scene->finishing_message),
              options->finishing_message, L"Almost there…");
    copy_text(scene->delayed_message, ARRAYSIZE(scene->delayed_message),
              options->delayed_message, L"First launch can take a little longer.");
    copy_text(scene->failed_message, ARRAYSIZE(scene->failed_message),
              options->failed_message, L"Something got in the way.");
    copy_text(scene->failure_detail, ARRAYSIZE(scene->failure_detail),
              options->failure_detail, L"");
    copy_text(scene->mark_path, ARRAYSIZE(scene->mark_path),
              options->mark_path, L"");

    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    scene->animations_enabled =
        options->motion == ASB_STARTUP_MOTION_GLYPH && animations;
    scene->dc = CreateCompatibleDC(NULL);
    if (!scene->dc) {
        HeapFree(GetProcessHeap(), 0, scene);
        return NULL;
    }
    if (scene->mark_path[0]) {
        scene->mark_icon = (HICON)LoadImageW(NULL, scene->mark_path,
                                             IMAGE_ICON, 0, 0,
                                             LR_LOADFROMFILE | LR_DEFAULTSIZE);
    }
    scene->painted_phase = ASB_STARTUP_DISABLED;
    return scene;
}

void vm_startup_scene_refresh_system_settings(VmStartupScene *scene)
{
    BOOL animations = TRUE;
    if (!scene) return;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    scene->animations_enabled =
        scene->motion == ASB_STARTUP_MOTION_GLYPH && animations;
}

void vm_startup_scene_destroy(VmStartupScene *scene)
{
    if (!scene) return;
    release_bitmap(scene);
    if (scene->dc) DeleteDC(scene->dc);
    if (scene->mark_icon) DestroyIcon(scene->mark_icon);
    HeapFree(GetProcessHeap(), 0, scene);
}

BOOL vm_startup_scene_render(VmStartupScene *scene,
                             UINT width,
                             UINT height,
                             UINT dpi,
                             AsbStartupPhase phase,
                             BOOL detailed,
                             ULONGLONG now_ms,
                             VmStartupSceneFrame *frame)
{
    BOOL resized;
    BOOL delay_visible;
    BOOL spinner_due;
    BOOL full_dirty;

    if (!scene || !frame || !width || !height) return FALSE;
    ZeroMemory(frame, sizeof(*frame));
    resized = scene->width != width || scene->height != height || scene->dpi != dpi;
    if (!ensure_bitmap(scene, width, height)) return FALSE;
    scene->dpi = dpi ? dpi : 96;
    if (!scene->started_ms) scene->started_ms = now_ms;
    delay_visible = phase != ASB_STARTUP_FAILED &&
        now_ms - scene->started_ms >= scene->delayed_message_after_ms;
    spinner_due = scene->animations_enabled && phase != ASB_STARTUP_FAILED &&
        (scene->last_spinner_ms == 0 ||
         now_ms - scene->last_spinner_ms >= STARTUP_SPINNER_INTERVAL_MS);
    full_dirty = resized || scene->painted_phase != phase ||
        scene->painted_detailed != detailed ||
        scene->delay_visible != delay_visible;

    if (spinner_due) {
        scene->spinner_frame = (scene->spinner_frame + 1) % 4;
        scene->last_spinner_ms = now_ms;
    }

    calculate_status_rect(scene);
    if (full_dirty) {
        RECT all = { 0, 0, (LONG)width, (LONG)height };
        fill_rect(scene->dc, &all, scene->background_color);
        draw_center_mark(scene);
        draw_status(scene, phase, detailed, delay_visible);
        frame->dirty = all;
        frame->full_dirty = TRUE;
        frame->updated = TRUE;
    } else if (spinner_due) {
        draw_status(scene, phase, detailed, delay_visible);
        frame->dirty = scene->status_rect;
        frame->updated = TRUE;
    }

    scene->painted_phase = phase;
    scene->painted_detailed = detailed;
    scene->delay_visible = delay_visible;
    frame->pixels = scene->pixels;
    frame->width = scene->width;
    frame->height = scene->height;
    frame->stride = scene->stride;
    return TRUE;
}

BOOL vm_startup_scene_is_animated(const VmStartupScene *scene)
{
    return scene && scene->animations_enabled;
}
