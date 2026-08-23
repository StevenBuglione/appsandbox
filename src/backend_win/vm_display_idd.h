#ifndef VM_DISPLAY_IDD_H
#define VM_DISPLAY_IDD_H

#include <windows.h>
#include "hcs_vm.h"

typedef struct VmDisplayIdd VmDisplayIdd;

typedef struct AsbDisplayOptions {
    BOOL app_mode;
    const wchar_t *window_title;
    const wchar_t *app_user_model_id;
    const wchar_t *icon_path;
    UINT initial_width;
    UINT initial_height;
    UINT minimum_width;
    UINT minimum_height;
    BOOL show_debug_title;
    BOOL show_debug_overlay;
} AsbDisplayOptions;

/* Create IDD display window for VM. Connects to VM's AF_HYPERV channels
   (:0002 frames, :0003 input, :0005 clipboard writer, :0006 clipboard reader)
   and renders received frames via D3D11.
   main_hwnd receives WM_VM_DISPLAY_CLOSED when closed. */
VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance, HWND main_hwnd);

/* Extended creation path used by application-window hosts. String fields are
   copied before this function returns, so callers may pass stack-backed data. */
VmDisplayIdd *vm_display_idd_create_ex(VmInstance *vm, HINSTANCE hInstance,
                                       HWND main_hwnd,
                                       const AsbDisplayOptions *options);

void vm_display_idd_destroy(VmDisplayIdd *display);
BOOL vm_display_idd_is_open(VmDisplayIdd *display);

/* Bring an already-open display window to the foreground/focus.
   Safe to call from any thread; the work is marshaled to the window thread. */
void vm_display_idd_focus(VmDisplayIdd *display);

/* Resize the exact owned display client from the window-owning thread. The
   request is asynchronous; WM_SIZE drives the existing guest resize path. */
BOOL vm_display_idd_resize(VmDisplayIdd *display, UINT width, UINT height);

#endif /* VM_DISPLAY_IDD_H */
