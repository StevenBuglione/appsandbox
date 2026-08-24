#ifndef VM_NATIVE_TITLEBAR_H
#define VM_NATIVE_TITLEBAR_H

#include <windows.h>

#include "vm_window_chrome.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VmNativeTitleBar VmNativeTitleBar;

typedef enum AsbNativeTitleBarAction {
    ASB_NATIVE_TITLE_BAR_TOGGLE_SIDEBAR = 1,
    ASB_NATIVE_TITLE_BAR_BACK = 2,
    ASB_NATIVE_TITLE_BAR_FORWARD = 3
} AsbNativeTitleBarAction;

/* Creates a compact Windows App SDK title bar inside the supplied existing
   top-level HWND. The caller owns the window, message pump, and COM apartment. */
VmNativeTitleBar *vm_native_titlebar_create(
    HWND hwnd,
    const AsbWindowChromeOptions *options,
    UINT action_message);

void vm_native_titlebar_destroy(VmNativeTitleBar *title_bar);
/* Detach the island while the parent HWND still exists. The remaining WinUI
   thread objects are released after the owning message loop exits. */
void vm_native_titlebar_close_island(VmNativeTitleBar *title_bar);
void vm_native_titlebar_resize(VmNativeTitleBar *title_bar);
void vm_native_titlebar_refresh(VmNativeTitleBar *title_bar);
BOOL vm_native_titlebar_pretranslate_message(
    VmNativeTitleBar *title_bar,
    MSG *message);
UINT vm_native_titlebar_height(const VmNativeTitleBar *title_bar);

#ifdef __cplusplus
}
#endif

#endif /* VM_NATIVE_TITLEBAR_H */
