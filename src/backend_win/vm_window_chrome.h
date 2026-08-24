#ifndef VM_WINDOW_CHROME_H
#define VM_WINDOW_CHROME_H

#include <windows.h>

typedef enum AsbTitleBarTheme {
    ASB_TITLE_BAR_SYSTEM = 0,
    ASB_TITLE_BAR_LIGHT = 1,
    ASB_TITLE_BAR_DARK = 2
} AsbTitleBarTheme;

typedef enum AsbWindowCornerPreference {
    ASB_WINDOW_CORNER_SYSTEM = 0,
    ASB_WINDOW_CORNER_SQUARE = 1,
    ASB_WINDOW_CORNER_ROUNDED = 2,
    ASB_WINDOW_CORNER_ROUNDED_SMALL = 3
} AsbWindowCornerPreference;

typedef enum AsbTitleBarLayout {
    ASB_TITLE_BAR_CAPTION_ONLY = 0,
    ASB_TITLE_BAR_COMPACT = 1
} AsbTitleBarLayout;

typedef struct AsbWindowChromeOptions {
    AsbTitleBarTheme theme;
    AsbWindowCornerPreference corner_preference;
    AsbTitleBarLayout layout;
    BOOL sidebar_toggle_visible;
    BOOL navigation_visible;
    BOOL desktop_menu_visible;
    BOOL has_caption_color;
    BOOL has_text_color;
    BOOL has_border_color;
    COLORREF caption_color;
    COLORREF text_color;
    COLORREF border_color;
} AsbWindowChromeOptions;

/* Applies only documented DWM attributes. The system continues to own the
   non-client frame, caption buttons, system menu, resize borders, and Snap. */
void vm_window_chrome_apply(HWND hwnd,
                            const AsbWindowChromeOptions *options,
                            BOOL active);

#endif /* VM_WINDOW_CHROME_H */
