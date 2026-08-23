#include "vm_window_chrome.h"

#include <dwmapi.h>

#pragma comment(lib, "dwmapi.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_COLOR_DEFAULT
#define DWMWA_COLOR_DEFAULT 0xFFFFFFFF
#endif

static BOOL high_contrast_enabled(void)
{
    HIGHCONTRASTW contrast;
    ZeroMemory(&contrast, sizeof(contrast));
    contrast.cbSize = sizeof(contrast);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast),
                               &contrast, 0))
        return FALSE;
    return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

static void set_color(HWND hwnd, DWORD attribute, BOOL enabled, COLORREF color)
{
    COLORREF value = enabled ? color : (COLORREF)DWMWA_COLOR_DEFAULT;
    DwmSetWindowAttribute(hwnd, attribute, &value, sizeof(value));
}

void vm_window_chrome_apply(HWND hwnd,
                            const AsbWindowChromeOptions *options,
                            BOOL active)
{
    BOOL dark;
    BOOL set_dark;
    BOOL high_contrast;
    DWORD corner;

    if (!hwnd || !options) return;

    high_contrast = high_contrast_enabled();
    dark = FALSE;
    set_dark = options->theme != ASB_TITLE_BAR_SYSTEM;
    if (options->theme == ASB_TITLE_BAR_DARK)
        dark = TRUE;
    else if (options->theme == ASB_TITLE_BAR_SYSTEM &&
             options->has_caption_color) {
        UINT luminance = 299u * GetRValue(options->caption_color) +
                         587u * GetGValue(options->caption_color) +
                         114u * GetBValue(options->caption_color);
        dark = luminance < 128000u;
        set_dark = TRUE;
    }
    if (high_contrast) {
        dark = FALSE;
        set_dark = TRUE;
    }
    if (set_dark)
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE,
                              &dark, sizeof(dark));

    corner = (DWORD)options->corner_preference;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));

    /* Inactive windows use the system border so activation remains obvious.
       Caption and text colors stay branded without replacing native controls. */
    set_color(hwnd, DWMWA_CAPTION_COLOR,
              !high_contrast && options->has_caption_color,
              options->caption_color);
    set_color(hwnd, DWMWA_TEXT_COLOR,
              !high_contrast && options->has_text_color,
              options->text_color);
    set_color(hwnd, DWMWA_BORDER_COLOR,
              !high_contrast && active && options->has_border_color,
              options->border_color);
}
