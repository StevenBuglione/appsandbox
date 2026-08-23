#ifndef VM_DISPLAY_IDD_H
#define VM_DISPLAY_IDD_H

#include <windows.h>
#include "hcs_vm.h"

typedef struct VmDisplayIdd VmDisplayIdd;

typedef struct AsbDisplayRuntimeState {
    UINT64 received_frames;
    UINT64 presented_frames;
    UINT64 present_count;
    UINT64 guest_frame_sequence;
    UINT render_width;
    UINT render_height;
    UINT frame_width;
    UINT frame_height;
} AsbDisplayRuntimeState;

typedef struct AsbDisplayOptions {
    BOOL app_mode;
    const wchar_t *window_title;
    const wchar_t *app_user_model_id;
    const wchar_t *icon_path;
    UINT initial_width;
    UINT initial_height;
    UINT backing_width;
    UINT backing_height;
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
BOOL vm_display_idd_focus(VmDisplayIdd *display);

/* Resize the exact owned display client from the window-owning thread. Returns
   only after the native client rectangle matches. Legacy mode then drives the
   asynchronous guest resize path; fixed-backing mode keeps its capacity. */
BOOL vm_display_idd_resize(VmDisplayIdd *display, UINT width, UINT height);

/* Mark the beginning or end of one interactive host resize transaction. This
   is deliberately VM/display scoped: callers never provide an HWND or an
   arbitrary window message. A display with a fixed backing size never
   modesets here; its application controller changes only logical scene size. */
BOOL vm_display_idd_set_resize_phase(VmDisplayIdd *display, BOOL active);

/* Send bounded pointer gestures through the input channel already owned by
   this exact display. Coordinates are guest-frame coordinates; no HWND,
   arbitrary packet, or global input hook is exposed to API callers. */
BOOL vm_display_idd_pointer_click(VmDisplayIdd *display, UINT x, UINT y);
BOOL vm_display_idd_pointer_drag(VmDisplayIdd *display,
                                 UINT start_x, UINT start_y,
                                 UINT end_x, UINT end_y,
                                 UINT steps);

/* Snapshot monotonic frame/presentation progress without touching the frame
   channel or any HWND. render_width/render_height describe the last successful
   DXGI presentation, so unattended controllers can synchronize input to pixels
   that the native host has actually presented. */
BOOL vm_display_idd_get_runtime_state(VmDisplayIdd *display,
                                      AsbDisplayRuntimeState *state);

#endif /* VM_DISPLAY_IDD_H */
