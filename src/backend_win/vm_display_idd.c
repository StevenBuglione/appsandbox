/*
 * vm_display_idd.c -- Host-side IDD frame receiver + D3D11 renderer.
 *
 * Connects to the guest VM over AF_HYPERV sockets:
 *   :0002  Frame channel — receives frames, renders via D3D11 textured quad
 *   :0003  Input channel — forwards keyboard/mouse events to guest
 *   :0004  Audio channel — receives audio from guest, renders via WASAPI
 *
 * Clipboard sync (:0005/:0006) is handled by vm_clipboard.c.
 *
 * Pure C, compiled as C.
 */

#include <winsock2.h>
#define COBJMACROS
#include <windows.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <propkey.h>

#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <ksmedia.h>

#pragma warning(push)
#pragma warning(disable: 4201) /* nameless struct/union in SDK headers */
#include <d3d11.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#pragma warning(pop)

#include <stdio.h>
#include <stdarg.h>

#include "vm_display_idd.h"
#include "vm_clipboard.h"
#include "vm_agent.h"
#include "hcs_vm.h"
#include "ui.h"
#include "resource.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "propsys.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif


/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV       34
#define HV_PROTOCOL_RAW 1

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

/* These three are superseded by hcs_service_guid(os_type, port, ...) — kept
   for grep. Windows VMs reach byte-identical GUIDs via the helper; Linux
   VMs reach vsock-template GUIDs. */
static const GUID FRAME_SERVICE_GUID =
    { 0xa5b0cafe, 0x0002, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Input channel service GUID — connects to agent for SendInput injection */
static const GUID INPUT_SERVICE_GUID =
    { 0xa5b0cafe, 0x0003, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* Audio capture channel — connects to guest audio helper (guest→host) */
static const GUID AUDIO_SERVICE_GUID =
    { 0xa5b0cafe, 0x0004, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Audio wire protocol (mirror of tools/agent/appsandbox-audio.c) ---- */

#define AUDIO_HEADER_MAGIC  0x31415341  /* 'ASA1' */

#pragma pack(push, 1)
typedef struct AudioHeader {
    UINT32 magic;
    UINT32 sample_rate;
    UINT16 channels;
    UINT16 bits_per_sample;
    UINT16 format_tag;      /* 1=PCM, 3=IEEE_FLOAT */
    UINT16 block_align;
} AudioHeader;

typedef struct AudioFrameHeader {
    UINT32 bytes;
} AudioFrameHeader;
#pragma pack(pop)

/* Clipboard reader-apply message (posted by vm_clipboard.c to our wndproc) */
#define WM_CLIP_READER_APPLY (WM_APP + 11)

/* ---- Frame protocol constants ---- */

#define FRAME_MAGIC         0x52465341  /* "ASFR" little-endian */
#define DEFAULT_WIDTH       1920
#define DEFAULT_HEIGHT      1080
#define MIN_FRAME_WIDTH     64
#define MIN_FRAME_HEIGHT    64
#define MAX_FRAME_WIDTH     7680
#define MAX_FRAME_HEIGHT    4320
#define MAX_FRAME_STRIDE    (MAX_FRAME_WIDTH * 4)
#define MAX_DIRTY_RECTS     64

/* ---- Input protocol (host → guest) ---- */

#define INPUT_MAGIC         0x4E495341  /* "ASIN" little-endian */
#define INPUT_MOUSE_MOVE    0
#define INPUT_MOUSE_BUTTON  1
#define INPUT_MOUSE_WHEEL   2
#define INPUT_KEY           3
#define INPUT_FRAME_SIZE    4

/* Button IDs for INPUT_MOUSE_BUTTON */
#define INPUT_BTN_LEFT      0
#define INPUT_BTN_RIGHT     1
#define INPUT_BTN_MIDDLE    2

#define INPUT_READY_MAGIC   0x59445249  /* "IRDY" little-endian */
#define INPUT_READY_MAGIC_V2 0x32565249 /* "IRV2": accepts INPUT_FRAME_SIZE */

#pragma pack(push, 1)
typedef struct InputPacket {
    UINT32 magic;   /* INPUT_MAGIC */
    UINT32 type;    /* INPUT_MOUSE_MOVE / BUTTON / WHEEL / KEY / FRAME_SIZE */
    UINT32 param1;
    UINT32 param2;
    UINT32 param3;
} InputPacket;
#pragma pack(pop)

/* ---- Window messages ---- */

#define WM_VM_DISPLAY_CLOSED    (WM_APP + 5)
#define WM_IDD_FRAME_READY      (WM_USER + 100)
#define WM_IDD_FOCUS            (WM_USER + 101)
#define WM_IDD_RESIZE           (WM_USER + 102)
#define WM_IDD_RESIZE_PHASE     (WM_USER + 103)

#define IDT_RESIZE_DEBOUNCE 2002
#define RESIZE_DEBOUNCE_MS  33
#define PRESENT_RETRY_MS     16
#define INITIAL_RESIZE_WAIT_MS 10000
#define RENDER_START_TIMEOUT_MS 15000
#define POINTER_SETTLE_MS    24
#define POINTER_PRESS_MS     48

/* Debug log window */
#define IDC_LOG_LIST      3001
#define MAX_LOG_LINES     200
#define LOG_WINDOW_W      800
#define LOG_WINDOW_H      300

/* ---- HLSL shaders (inline strings) ---- */

static const char g_vs_hlsl[] =
    "struct VS_OUT { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VS_OUT main(uint id : SV_VertexID) {\n"
    "    VS_OUT o;\n"
    "    o.uv = float2((id << 1) & 2, id & 2);\n"
    "    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n";

static const char g_ps_hlsl[] =
    "Texture2D tex : register(t0);\n"
    "SamplerState samp : register(s0);\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "    return tex.Sample(samp, uv);\n"
    "}\n";

/* ---- Frame header from wire ---- */

#pragma pack(push, 1)
typedef struct FrameHeader {
    UINT32 magic;
    UINT32 width;
    UINT32 height;
    UINT32 stride;
    UINT64 frame_seq;
    UINT32 dirty_rect_count;
} FrameHeader;

/* ---- Cursor header from wire (must match VDD_WIRE_CURSOR_HEADER) ---- */

#define CURSOR_MAGIC        0x52435341  /* "ASCR" little-endian */
#define MAX_CURSOR_SIZE     (256 * 256 * 4 * 2)  /* 2x for MASKED_COLOR double-height */

typedef struct CursorHeader {
    UINT32 magic;
    INT32  x;
    INT32  y;
    UINT32 visible;
    UINT32 shape_updated;
    UINT32 shape_id;
    UINT32 width;
    UINT32 height;
    UINT32 pitch;
    UINT32 xhot;
    UINT32 yhot;
    UINT32 cursor_type;     /* 1=MASKED_COLOR, 2=ALPHA */
    UINT32 shape_data_size;
} CursorHeader;
#pragma pack(pop)

/* ---- Display context ---- */

struct VmDisplayIdd {
    VmInstance  *vm;
    wchar_t      vm_name[256];     /* copy of vm->name for safe logging after VM teardown */
    GUID         runtime_id;       /* copy of vm->runtime_id for safe HvSocket after teardown */
    wchar_t      os_type[32];      /* copy of vm->os_type for picking the service GUID variant */
    HINSTANCE    hInstance;
    HWND         main_hwnd;
    HWND         hwnd;
    volatile BOOL open;
    volatile BOOL stop;

    /* Application-window identity and sizing. These are owned copies because
       the display outlives the API request that creates it. */
    BOOL         app_mode;
    BOOL         fixed_backing;
    wchar_t      window_title[256];
    wchar_t      app_user_model_id[256];
    wchar_t      icon_path[MAX_PATH];
    UINT         initial_width;
    UINT         initial_height;
    UINT         backing_width;
    UINT         backing_height;
    UINT         minimum_width;
    UINT         minimum_height;
    BOOL         show_debug_title;
    BOOL         show_debug_overlay;
    HICON        custom_icon;

    /* D3D11 */
    ID3D11Device            *device;
    ID3D11DeviceContext     *ctx;
    IDXGISwapChain          *swap_chain;
    ID3D11RenderTargetView  *rtv;
    ID3D11Texture2D         *frame_tex;
    ID3D11ShaderResourceView *frame_srv;
    UINT                     frame_tex_width;
    UINT                     frame_tex_height;
    ID3D11VertexShader      *vs;
    ID3D11PixelShader       *ps;
    ID3D11SamplerState      *sampler;
    UINT                     render_width;
    UINT                     render_height;

    /* One worker owns every D3D object and call. The native window thread only
       publishes the newest client geometry and signals this worker, so Windows
       move/size/paint dispatch can never wait behind Map, ResizeBuffers, or
       Present. Auto-reset events collapse bursts to the latest visual state. */
    HANDLE         render_event;
    HANDLE         render_ready_event;
    HANDLE         render_thread;
    volatile LONG  render_init_result;
    volatile LONG  render_resize_pending;
    volatile LONG  desired_render_width;
    volatile LONG  desired_render_height;

    /* Frame buffer (CPU-side, updated by recv thread) */
    BYTE          *frame_buf;
    UINT           frame_width;
    UINT           frame_height;
    UINT           frame_stride;
    CRITICAL_SECTION frame_cs;
    volatile BOOL  frame_dirty;
    volatile LONG  frame_message_pending;
    volatile LONG64 received_frame_generation;
    volatile LONG64 presented_frame_generation;
    volatile LONG64 present_count;
    volatile LONG64 last_guest_frame_sequence;
    volatile LONG   presented_render_width;
    volatile LONG   presented_render_height;
    UINT64          uploaded_frame_generation;

    /* Host -> guest display sizing. The window thread only publishes the
       latest physical client size; the worker performs the blocking agent RPC. */
    CRITICAL_SECTION resize_cs;
    CRITICAL_SECTION agent_command_cs;
    HANDLE         resize_event;
    HANDLE         resize_thread;
    UINT           desired_guest_width;
    UINT           desired_guest_height;
    UINT           pending_resize_width;
    UINT           pending_resize_height;
    UINT           last_requested_width;
    UINT           last_requested_height;
    UINT           last_applied_width;
    UINT           last_applied_height;
    BOOL           resize_pending;
    BOOL           in_size_move;

    UINT           render_count;     /* number of renders (for one-shot logging) */
    volatile UINT  recv_count;       /* number of frames received over HvSocket */

    /* Input forwarding */
    volatile SOCKET input_socket;   /* input socket for keyboard/mouse forwarding */
    CRITICAL_SECTION input_send_cs; /* keeps fixed-size stream packets atomic */
    volatile BOOL  input_frame_size_supported;
    BOOL           mouse_in;        /* TRUE while cursor is inside the render area */
    BOOL           tracking;        /* TrackMouseEvent active */

    /* Keyboard hotkey handling */
    wchar_t        vhdx_path[MAX_PATH]; /* copy of vm->vhdx_path for settings file */
    volatile BOOL  transmit_hotkeys; /* TRUE = capture host hotkeys + send to guest */
    volatile BOOL  input_focused;    /* TRUE while our top-level window is active */
    HHOOK          kbd_hook;          /* WH_KEYBOARD_LL handle, NULL when not installed */
    /* Held-key tracking (window-thread only, no lock). Indexed by virtual key.
       held_down[vk]=1 means we forwarded a down with no matching up yet; the
       saved scan/ext let us synthesize an accurate up when flushing. */
    BYTE           held_down[256];
    BYTE           held_scan[256];
    BYTE           held_ext[256];

    /* Guest cursor */
    HCURSOR        guest_cursor;    /* current cursor created from guest bitmap */
    UINT32         cursor_shape_id; /* tracks which shape is current */
    BOOL           cursor_visible;  /* guest cursor visibility */

    /* Debug log window (separate top-level window) */
    HWND           log_hwnd;        /* top-level log window */
    HWND           log_list_hwnd;   /* listbox inside log window */
    HWND           render_hwnd;     /* D3D target: top-level app HWND or diagnostic child */

    /* Clipboard (extracted to vm_clipboard.c) */
    VmClipboard      clipboard;

    /* Audio playback channel (:0004 — guest→host render) */
    volatile SOCKET  audio_socket;
    HANDLE           audio_recv_thread;
    volatile BOOL    audio_muted;

    /* Threads */
    HANDLE         recv_thread;
    HANDLE         window_thread;
};

/* ---- Forward declarations ---- */

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static DWORD WINAPI     idd_window_thread_proc(LPVOID param);
static DWORD WINAPI     idd_recv_thread_proc(LPVOID param);
static DWORD WINAPI     idd_resize_thread_proc(LPVOID param);
static DWORD WINAPI     idd_render_thread_proc(LPVOID param);

/* ---- Window class ---- */

static const wchar_t *IDD_DISPLAY_CLASS = L"AppSandboxIddDisplay";
static const wchar_t *IDD_RENDER_CLASS  = L"AppSandboxIddRender";
static const wchar_t *IDD_LOG_CLASS     = L"AppSandboxIddLog";

/* System menu command IDs — must be < 0xF000 and have low 4 bits clear */
#define IDM_AUDIO_MUTE     0x1000
#define IDM_XMIT_HOTKEYS   0x1010
#define IDM_SHOW_LOG       0x1020
static BOOL g_idd_class_registered;
static WNDPROC g_orig_listbox_proc;

/* Listbox subclass — handles Ctrl+A / Ctrl+C */
static LRESULT CALLBACK idd_log_listbox_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (wp == 'C') {
            LRESULT sel_count = SendMessageW(hwnd, LB_GETSELCOUNT, 0, 0);
            if (sel_count > 0) {
                int *indices = (int *)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)sel_count * sizeof(int));
                if (indices) {
                    LRESULT i;
                    SIZE_T total = 0;
                    wchar_t *text;
                    SendMessageW(hwnd, LB_GETSELITEMS, (WPARAM)sel_count, (LPARAM)indices);
                    for (i = 0; i < sel_count; i++)
                        total += (SIZE_T)SendMessageW(hwnd, LB_GETTEXTLEN, (WPARAM)indices[i], 0) + 2;
                    total++;
                    text = (wchar_t *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total * sizeof(wchar_t));
                    if (text) {
                        wchar_t *p = text;
                        for (i = 0; i < sel_count; i++) {
                            LRESULT len = SendMessageW(hwnd, LB_GETTEXT, (WPARAM)indices[i], (LPARAM)p);
                            p += len;
                            *p++ = L'\r'; *p++ = L'\n';
                        }
                        if (OpenClipboard(hwnd)) {
                            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                            if (hMem) {
                                void *dst = GlobalLock(hMem);
                                if (dst) {
                                    memcpy(dst, text, (SIZE_T)(p - text + 1) * sizeof(wchar_t));
                                    GlobalUnlock(hMem);
                                    EmptyClipboard();
                                    SetClipboardData(CF_UNICODETEXT, hMem);
                                } else {
                                    GlobalFree(hMem);
                                }
                            }
                            CloseClipboard();
                        }
                        HeapFree(GetProcessHeap(), 0, text);
                    }
                    HeapFree(GetProcessHeap(), 0, indices);
                }
            }
            return 0;
        }
        if (wp == 'A') {
            LRESULT cnt = SendMessageW(hwnd, LB_GETCOUNT, 0, 0);
            SendMessageW(hwnd, LB_SELITEMRANGE, TRUE, MAKELPARAM(0, (WORD)(cnt - 1)));
            return 0;
        }
    }
    return CallWindowProcW(g_orig_listbox_proc, hwnd, msg, wp, lp);
}

/* Log window proc — resizes listbox child to fill the window */
static LRESULT CALLBACK idd_log_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static HBRUSH s_dark_brush = NULL;

    switch (msg) {
    case WM_SIZE: {
        HWND list = GetDlgItem(hwnd, IDC_LOG_LIST);
        if (list) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            MoveWindow(list, 0, 0, rc.right, rc.bottom, TRUE);
        }
        return 0;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        /* Match AppSandbox #log-panel: --ctrl-bg #2d2d2d, text #ccc */
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, RGB(204, 204, 204));
        SetBkColor(hdc, RGB(45, 45, 45));
        if (!s_dark_brush) s_dark_brush = CreateSolidBrush(RGB(45, 45, 45));
        return (LRESULT)s_dark_brush;
    }
    case WM_CLOSE:
        /* Just hide — the main IDD window owns the lifecycle */
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Render child window proc — forwards input and coalesces expose requests. */
static LRESULT CALLBACK idd_render_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        HWND parent;
        VmDisplayIdd *d;

        /* Validate synchronously, but never synchronously call back into the
           parent. MoveWindow(..., TRUE) used to make rapid resizing re-enter
           D3D rendering here through SendMessage and block the UI thread. */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        parent = GetParent(hwnd);
        d = parent
            ? (VmDisplayIdd *)GetWindowLongPtrW(parent, GWLP_USERDATA)
            : NULL;
        if (d && !d->stop &&
            InterlockedCompareExchange(&d->frame_message_pending, 1, 0) == 0 &&
            !PostMessageW(parent, WM_IDD_FRAME_READY, 0, 0))
            InterlockedExchange(&d->frame_message_pending, 0);
        return 0;
    }
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL:
    case WM_MOUSELEAVE:
    case WM_KEYDOWN: case WM_KEYUP:
    case WM_SYSKEYDOWN: case WM_SYSKEYUP:
    case WM_SETCURSOR:
        return SendMessageW(GetParent(hwnd), msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ensure_idd_class(HINSTANCE hInst)
{
    WNDCLASSEXW wc;
    if (g_idd_class_registered) return;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    /* D3D/DWM own the client pixels. HREDRAW/VREDRAW plus a class brush asks
       USER32 to invalidate and erase the entire client while an edge is being
       dragged, which can expose one black compositor frame before the swap
       chain is presented. WM_SIZE and WM_PAINT already schedule the exact
       render work we need. */
    wc.style         = 0;
    wc.lpfnWndProc   = idd_wnd_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = IDD_DISPLAY_CLASS;
    RegisterClassExW(&wc);

    /* Render child — D3D11 swap chain targets this window.
       hCursor=NULL so WM_SETCURSOR can set the guest cursor. */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = idd_render_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = NULL;
    wc.hbrBackground = NULL;
    wc.lpszClassName = IDD_RENDER_CLASS;
    RegisterClassExW(&wc);

    /* Separate log window — dark background to match AppSandbox main window */
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = idd_log_proc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hIconSm       = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPSANDBOX));
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(RGB(30, 30, 30));
    wc.lpszClassName = IDD_LOG_CLASS;
    RegisterClassExW(&wc);

    g_idd_class_registered = TRUE;
}

static void idd_update_window_title(VmDisplayIdd *d)
{
    wchar_t title[300];
    const wchar_t *base;

    if (!d || !d->hwnd) return;
    base = d->window_title[0] ? d->window_title : d->vm_name;

    if (d->app_mode && !d->show_debug_title) {
        SetWindowTextW(d->hwnd, base);
        return;
    }

    if (d->app_mode) {
        swprintf_s(title, 300, L"%s%s — %ux%u",
                   d->audio_muted ? L"\U0001F507 " : L"", base,
                   d->frame_width, d->frame_height);
    } else {
        swprintf_s(title, 300, L"%s%s - IDD Display %ux%u",
                   d->audio_muted ? L"\U0001F507 " : L"", base,
                   d->frame_width, d->frame_height);
    }
    SetWindowTextW(d->hwnd, title);
}

static void idd_set_window_app_id(HWND hwnd, const wchar_t *app_id)
{
    IPropertyStore *store = NULL;
    PROPVARIANT value;

    if (!hwnd || !app_id || !app_id[0]) return;
    if (FAILED(SHGetPropertyStoreForWindow(hwnd, &IID_IPropertyStore,
                                            (void **)&store)) || !store)
        return;

    PropVariantInit(&value);
    value.vt = VT_LPWSTR;
    value.pwszVal = (LPWSTR)app_id;
    store->lpVtbl->SetValue(store, &PKEY_AppUserModel_ID, &value);
    store->lpVtbl->Commit(store);
    store->lpVtbl->Release(store);
}



/* ---- Debug log panel ---- */

static void idd_log(VmDisplayIdd *d, const wchar_t *fmt, ...)
{
    wchar_t buf[512];
    va_list ap;
    LRESULT count;

    if (!d || !d->log_list_hwnd || d->stop) return;

    va_start(ap, fmt);
    vswprintf_s(buf, 512, fmt, ap);
    va_end(ap);

    count = SendMessageW(d->log_list_hwnd, LB_ADDSTRING, 0, (LPARAM)buf);

    /* Trim old entries */
    while (count > MAX_LOG_LINES) {
        SendMessageW(d->log_list_hwnd, LB_DELETESTRING, 0, 0);
        count--;
    }

    /* Scroll to bottom and force repaint even when not focused */
    SendMessageW(d->log_list_hwnd, LB_SETTOPINDEX, (WPARAM)(count - 1), 0);
    UpdateWindow(d->log_list_hwnd);
}

static BOOL idd_agent_send(VmDisplayIdd *d, const char *command,
                           char *response, int response_max, DWORD timeout_ms)
{
    BOOL ok;

    EnterCriticalSection(&d->agent_command_cs);
    ok = !d->stop && d->vm &&
         vm_agent_send(d->vm, command, response, response_max, timeout_ms);
    LeaveCriticalSection(&d->agent_command_cs);
    return ok;
}

static void idd_update_desired_resize(VmDisplayIdd *d, HWND hwnd)
{
    RECT rc;
    UINT width, height;
    BOOL changed = FALSE;

    if (!d || !hwnd || !GetClientRect(hwnd, &rc)) return;
    width = d->fixed_backing ? d->backing_width : (UINT)rc.right;
    height = d->fixed_backing ? d->backing_height : (UINT)rc.bottom;
    if (width < MIN_FRAME_WIDTH || height < MIN_FRAME_HEIGHT ||
        width > MAX_FRAME_WIDTH || height > MAX_FRAME_HEIGHT)
        return;

    EnterCriticalSection(&d->resize_cs);
    if (d->desired_guest_width != width || d->desired_guest_height != height) {
        d->desired_guest_width = width;
        d->desired_guest_height = height;
        changed = TRUE;
    }
    LeaveCriticalSection(&d->resize_cs);

    if (changed)
        idd_log(d, L"Host target: %ux%u physical client pixels.", width, height);
}

static void idd_publish_render_size(VmDisplayIdd *d, HWND hwnd)
{
    RECT rc;
    LONG width, height;

    if (!d || !hwnd || !GetClientRect(hwnd, &rc)) return;
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    if (width < MIN_FRAME_WIDTH || height < MIN_FRAME_HEIGHT ||
        width > MAX_FRAME_WIDTH || height > MAX_FRAME_HEIGHT)
        return;

    InterlockedExchange(&d->desired_render_width, width);
    InterlockedExchange(&d->desired_render_height, height);
}

static void idd_queue_desired_resize(VmDisplayIdd *d);

static void idd_request_render(VmDisplayIdd *d, BOOL resize_swap_chain)
{
    if (!d || d->stop) return;
    if (resize_swap_chain)
        InterlockedExchange(&d->render_resize_pending, 1);
    if (d->render_event)
        SetEvent(d->render_event);
}

static void idd_begin_interactive_resize(VmDisplayIdd *d, HWND hwnd)
{
    if (!d || !hwnd) return;
    d->in_size_move = TRUE;
    KillTimer(hwnd, IDT_RESIZE_DEBOUNCE);
}

static void idd_end_interactive_resize(VmDisplayIdd *d, HWND hwnd)
{
    if (!d || !hwnd) return;
    d->in_size_move = FALSE;
    KillTimer(hwnd, IDT_RESIZE_DEBOUNCE);
    idd_publish_render_size(d, hwnd);
    idd_request_render(d, TRUE);
    idd_update_desired_resize(d, hwnd);
    idd_queue_desired_resize(d);
}

static void idd_queue_desired_resize(VmDisplayIdd *d)
{
    BOOL queued = FALSE;

    if (!d || d->stop) return;

    EnterCriticalSection(&d->resize_cs);
    if (d->desired_guest_width >= MIN_FRAME_WIDTH &&
        d->desired_guest_height >= MIN_FRAME_HEIGHT &&
        (d->desired_guest_width != d->last_requested_width ||
         d->desired_guest_height != d->last_requested_height)) {
        d->pending_resize_width = d->desired_guest_width;
        d->pending_resize_height = d->desired_guest_height;
        d->resize_pending = TRUE;
        queued = TRUE;
    }
    LeaveCriticalSection(&d->resize_cs);

    if (queued) SetEvent(d->resize_event);
}

static void idd_note_applied_frame(VmDisplayIdd *d, UINT width, UINT height)
{
    UINT desired_width, desired_height;
    BOOL changed = FALSE;

    EnterCriticalSection(&d->resize_cs);
    desired_width = d->desired_guest_width;
    desired_height = d->desired_guest_height;
    if (d->last_applied_width != width || d->last_applied_height != height) {
        d->last_applied_width = width;
        d->last_applied_height = height;
        changed = TRUE;
    }
    LeaveCriticalSection(&d->resize_cs);

    if (changed) {
        idd_log(d, L"Frame received: %ux%u.", width, height);
        if (width == desired_width && height == desired_height)
            idd_log(d, L"Resize settled at exact 1:1 size %ux%u.", width, height);
    }
}

static BOOL idd_resize_is_settled(VmDisplayIdd *d)
{
    BOOL settled;

    EnterCriticalSection(&d->resize_cs);
    settled = d->desired_guest_width >= MIN_FRAME_WIDTH &&
              d->desired_guest_height >= MIN_FRAME_HEIGHT &&
              d->last_applied_width == d->desired_guest_width &&
              d->last_applied_height == d->desired_guest_height;
    LeaveCriticalSection(&d->resize_cs);

    return settled;
}

static DWORD WINAPI idd_resize_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;

    while (!d->stop) {
        DWORD wait_result = WaitForSingleObject(d->resize_event, INFINITE);
        if (wait_result != WAIT_OBJECT_0 || d->stop) break;

        for (;;) {
            UINT width = 0, height = 0;
            char command[64];
            char response[256] = { 0 };
            BOOL ok;

            EnterCriticalSection(&d->resize_cs);
            if (d->resize_pending) {
                width = d->pending_resize_width;
                height = d->pending_resize_height;
                d->resize_pending = FALSE;
                if (width == d->last_requested_width &&
                    height == d->last_requested_height) {
                    width = 0;
                    height = 0;
                } else {
                    d->last_requested_width = width;
                    d->last_requested_height = height;
                }
            }
            LeaveCriticalSection(&d->resize_cs);

            if (!width || !height || d->stop) break;

            sprintf_s(command, sizeof(command), "display_resize:%ux%u@60",
                      width, height);
            ok = idd_agent_send(d, command, response, sizeof(response), 5000);
            if (ok) {
                idd_log(d, L"Guest requested: %ux%u@60 (accepted).", width, height);
            } else {
                idd_log(d, L"Guest requested: %ux%u@60 (rejected: %S).",
                        width, height, response[0] ? response : "no response");
            }

            EnterCriticalSection(&d->resize_cs);
            ok = d->resize_pending;
            LeaveCriticalSection(&d->resize_cs);
            if (!ok) break;
        }
    }

    return 0;
}

/* ---- Send input packet to guest ---- */

static UINT g_input_send_count = 0;

static BOOL send_input(VmDisplayIdd *d, UINT32 type, UINT32 p1, UINT32 p2, UINT32 p3)
{
    InputPacket pkt;
    SOCKET s;
    int ret;
    BOOL sent = FALSE;
    int log_error = 0;
    int log_value = 0;
    UINT send_count = 0;

    EnterCriticalSection(&d->input_send_cs);
    s = d->input_socket;
    if (s == INVALID_SOCKET) goto done;

    pkt.magic  = INPUT_MAGIC;
    pkt.type   = type;
    pkt.param1 = p1;
    pkt.param2 = p2;
    pkt.param3 = p3;

    /* Non-blocking send — drop packet if buffer full rather than stall the UI */
    ret = send(s, (const char *)&pkt, (int)sizeof(pkt), 0);
    if (ret == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            /* Send buffer full: drop this packet (as the comment intends)
               without tearing down the socket. */
            goto done;
        }
        log_error = 1;
        log_value = err;
        /* Mark dead — recv thread owns the socket and will close + reconnect */
        d->input_socket = INVALID_SOCKET;
        goto done;
    }
    if (ret != (int)sizeof(pkt)) {
        /* Partial send on a stream socket: the guest reads fixed-size 20-byte
           InputPackets, so a truncated packet permanently misaligns the wire.
           Flag for reconnect so the channel resynchronises. */
        log_error = 2;
        log_value = ret;
        d->input_socket = INVALID_SOCKET;
        goto done;
    }

    sent = TRUE;
    g_input_send_count++;
    send_count = g_input_send_count;

done:
    LeaveCriticalSection(&d->input_send_cs);

    /* Never synchronously message the UI-owned log window while holding the
       stream lock (or a caller's frame lock). */
    if (type == INPUT_FRAME_SIZE) {
        /* FRAME_SIZE is sent while the caller holds frame_cs to order it
           before mouse packets for the new framebuffer. That caller logs the
           surrounding connection/resolution event after releasing frame_cs. */
    } else if (log_error == 1) {
        idd_log(d, L"INPUT SEND ERR %d - flagging for reconnect.", log_value);
    } else if (log_error == 2) {
        idd_log(d, L"INPUT SEND short (%d/%d) - flagging for reconnect.",
                log_value, (int)sizeof(pkt));
    } else if (sent && type != INPUT_MOUSE_MOVE) {
        static const wchar_t *type_names[] = {
            L"MOUSE_MOVE", L"MOUSE_BTN", L"MOUSE_WHEEL", L"KEY", L"FRAME_SIZE"
        };
        const wchar_t *name = type < 5 ? type_names[type] : L"?";
        idd_log(d, L"INPUT %s p1=%u p2=%u p3=%u (#%u)",
                name, p1, p2, p3, send_count);
    }
    return sent;
}

static BOOL idd_pointer_coordinates_valid(VmDisplayIdd *d, UINT x, UINT y)
{
    UINT width;
    UINT height;

    if (!d) return FALSE;
    EnterCriticalSection(&d->frame_cs);
    width = d->frame_width;
    height = d->frame_height;
    LeaveCriticalSection(&d->frame_cs);
    return width > 0 && height > 0 && x < width && y < height;
}

BOOL vm_display_idd_pointer_click(VmDisplayIdd *d, UINT x, UINT y)
{
    BOOL moved;
    BOOL pressed;
    BOOL released;

    if (!idd_pointer_coordinates_valid(d, x, y)) return FALSE;
    moved = send_input(d, INPUT_MOUSE_MOVE, x, y, 0);
    if (moved) Sleep(POINTER_SETTLE_MS);
    pressed = moved && send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 1, 0);
    if (pressed) Sleep(POINTER_PRESS_MS);
    released = send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
    return moved && pressed && released;
}

BOOL vm_display_idd_pointer_drag(VmDisplayIdd *d,
                                 UINT start_x, UINT start_y,
                                 UINT end_x, UINT end_y,
                                 UINT steps)
{
    UINT step;
    BOOL ok;
    BOOL released;

    if (steps < 2 || steps > 120 ||
        !idd_pointer_coordinates_valid(d, start_x, start_y) ||
        !idd_pointer_coordinates_valid(d, end_x, end_y))
        return FALSE;

    ok = send_input(d, INPUT_MOUSE_MOVE, start_x, start_y, 0);
    if (ok) Sleep(POINTER_SETTLE_MS);
    ok = ok && send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 1, 0);
    for (step = 1; ok && step <= steps; ++step) {
        const UINT x = (UINT)(((UINT64)start_x * (steps - step) +
                               (UINT64)end_x * step) / steps);
        const UINT y = (UINT)(((UINT64)start_y * (steps - step) +
                               (UINT64)end_y * step) / steps);
        ok = send_input(d, INPUT_MOUSE_MOVE, x, y, 0);
        Sleep(4U);
    }
    if (ok) Sleep(PRESENT_RETRY_MS);
    released = send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
    return ok && released;
}

/* ==================================================================
 * Per-VM display settings (display_settings.json beside disk.vhdx)
 *
 * Mirrors the vm_state.json pattern in asb_core.c but is owned entirely
 * by the IDD display: the file is created lazily the first time a VM's
 * display opens, so both new and pre-existing VMs get one on demand.
 * ================================================================== */

static void idd_display_settings_path(const wchar_t *vhdx_path, wchar_t *out, size_t out_chars)
{
    wchar_t dir[MAX_PATH];
    const wchar_t *last_slash;
    wcscpy_s(dir, MAX_PATH, vhdx_path);
    last_slash = wcsrchr(dir, L'\\');
    if (last_slash) dir[last_slash - dir] = L'\0';
    swprintf_s(out, out_chars, L"%s\\display_settings.json", dir);
}

static void idd_display_settings_save(const wchar_t *vhdx_path, BOOL transmit_hotkeys)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    if (!vhdx_path || vhdx_path[0] == L'\0') return;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);
    if (_wfopen_s(&f, path, L"w") != 0 || !f) return;
    fprintf(f, "{\"transmitKeyboardHotkeys\":%d}\n", transmit_hotkeys ? 1 : 0);
    fclose(f);
}

/* Read the persisted setting; if the file is absent, create it with the
   default (off) and return FALSE. Returns the transmit-hotkeys value. */
static BOOL idd_display_settings_load_or_create(const wchar_t *vhdx_path)
{
    wchar_t path[MAX_PATH];
    FILE *f;
    char buf[256];
    BOOL transmit = FALSE;

    if (!vhdx_path || vhdx_path[0] == L'\0') return FALSE;
    idd_display_settings_path(vhdx_path, path, MAX_PATH);

    if (_wfopen_s(&f, path, L"r") != 0 || !f) {
        /* Lazy creation: file doesn't exist yet (new or pre-existing VM). */
        idd_display_settings_save(vhdx_path, FALSE);
        return FALSE;
    }
    if (fgets(buf, sizeof(buf), f)) {
        if (strstr(buf, "\"transmitKeyboardHotkeys\":1"))
            transmit = TRUE;
    }
    fclose(f);
    return transmit;
}

/* ==================================================================
 * Keyboard hotkey capture
 * ================================================================== */

/* Maximal reserved-hotkey set: keys the host shell would normally consume.
   In Default mode these are withheld from the guest (host handles them); in
   Transmit mode they are captured and forwarded to the guest instead.
   alt_down must reflect whether Alt is currently held (LLKHF_ALTDOWN from the
   low-level hook, or GetKeyState(VK_MENU) from the wndproc path).
   Note: Ctrl+Alt+Del and Win+L are secure (SAS) sequences that no user-mode
   hook can intercept — they always reach the host. */
static BOOL idd_is_reserved_hotkey(DWORD vk, BOOL alt_down)
{
    switch (vk) {
    case VK_LWIN:
    case VK_RWIN:
    case VK_SNAPSHOT:   /* PrintScreen */
    case VK_APPS:       /* Menu / context key */
        return TRUE;
    case VK_TAB:
        return alt_down;                                  /* Alt+Tab */
    case VK_ESCAPE:
        /* Alt+Esc / Ctrl+Esc. GetAsyncKeyState reads real-time physical state,
           which is reliable both in the wndproc and inside the low-level hook
           (where the synchronized GetKeyState value may not be updated yet). */
        return alt_down || (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    default:
        return FALSE;
    }
}

/* Forward a key event to the guest and track held state so a later focus
   change can release anything still down. Runs on the window thread only. */
static void idd_forward_key(VmDisplayIdd *d, DWORD vk, DWORD scan, BOOL ext, BOOL up)
{
    UINT32 flags = 0;
    if (ext) flags |= 1;
    if (up)  flags |= 2;
    send_input(d, INPUT_KEY, vk, scan, flags);

    if (vk < 256) {
        if (up) {
            d->held_down[vk] = 0;
        } else {
            d->held_down[vk] = 1;
            d->held_scan[vk] = (BYTE)scan;
            d->held_ext[vk]  = (BYTE)(ext ? 1 : 0);
        }
    }
}

/* Send key-up for every key we believe is still held in the guest, then
   clear tracking. Called when our window loses activation, when Transmit
   mode is turned off, and on teardown — this is the core stuck-key fix. */
static void idd_flush_held_keys(VmDisplayIdd *d)
{
    int vk;
    for (vk = 0; vk < 256; vk++) {
        if (d->held_down[vk]) {
            UINT32 flags = 2 | (d->held_ext[vk] ? 1 : 0);
            send_input(d, INPUT_KEY, (UINT32)vk, d->held_scan[vk], flags);
            d->held_down[vk] = 0;
        }
    }
}

/* Thread-local owner: a WH_KEYBOARD_LL callback runs on the thread that
   installed it (our window thread), so this resolves each display's context
   without a global registry, keeping multiple displays independent. */
static __declspec(thread) VmDisplayIdd *t_hook_display;

static LRESULT CALLBACK idd_ll_keyboard_proc(int code, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d = t_hook_display;

    if (code == HC_ACTION && d && !d->stop &&
        d->input_focused && d->transmit_hotkeys) {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL up  = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        BOOL alt = (k->flags & LLKHF_ALTDOWN) != 0;
        if (idd_is_reserved_hotkey(k->vkCode, alt)) {
            idd_forward_key(d, k->vkCode, k->scanCode,
                            (k->flags & LLKHF_EXTENDED) != 0, up);
            return 1;  /* swallow so the host shell doesn't act on it */
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void idd_install_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) return;
    t_hook_display = d;
    d->kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, idd_ll_keyboard_proc,
                                     d->hInstance, 0);
    if (!d->kbd_hook)
        idd_log(d, L"Hotkey hook install failed (err %lu).", GetLastError());
    else
        idd_log(d, L"Hotkey capture enabled.");
}

static void idd_remove_kbd_hook(VmDisplayIdd *d)
{
    if (d->kbd_hook) {
        UnhookWindowsHookEx(d->kbd_hook);
        d->kbd_hook = NULL;
        idd_log(d, L"Hotkey capture disabled.");
    }
    t_hook_display = NULL;
}

/* Compute letterboxed/pillarboxed viewport within client rect */
static void compute_letterbox(UINT client_w, UINT client_h,
                              UINT frame_w, UINT frame_h,
                              float *out_x, float *out_y,
                              float *out_w, float *out_h)
{
    float scale_x, scale_y, scale;
    if (client_w == 0 || client_h == 0 || frame_w == 0 || frame_h == 0) {
        *out_x = 0; *out_y = 0; *out_w = 0; *out_h = 0;
        return;
    }
    if (client_w == frame_w && client_h == frame_h) {
        *out_x = 0; *out_y = 0;
        *out_w = (float)client_w; *out_h = (float)client_h;
        return;
    }
    scale_x = (float)client_w / (float)frame_w;
    scale_y = (float)client_h / (float)frame_h;
    scale = scale_x < scale_y ? scale_x : scale_y;
    *out_w = (float)frame_w * scale;
    *out_h = (float)frame_h * scale;
    *out_x = ((float)client_w - *out_w) * 0.5f;
    *out_y = ((float)client_h - *out_h) * 0.5f;
}

/* Map window client coordinates to VM framebuffer coordinates */
static void window_to_vm_coords(HWND hwnd, int wx, int wy, BOOL stretch,
                                 BOOL crop,
                                 UINT vm_w, UINT vm_h,
                                 UINT *vx, UINT *vy)
{
    RECT rc;
    float vp_x, vp_y, vp_w, vp_h;
    float local_x, local_y;

    GetClientRect(hwnd, &rc);
    if (crop) {
        *vx = wx < 0 ? 0 : (UINT)wx;
        *vy = wy < 0 ? 0 : (UINT)wy;
        if (*vx >= vm_w) *vx = vm_w - 1;
        if (*vy >= vm_h) *vy = vm_h - 1;
        return;
    }
    if (stretch) {
        vp_x = 0;
        vp_y = 0;
        vp_w = (float)rc.right;
        vp_h = (float)rc.bottom;
    } else {
        compute_letterbox((UINT)rc.right, (UINT)rc.bottom, vm_w, vm_h,
                          &vp_x, &vp_y, &vp_w, &vp_h);
    }

    if (vp_w <= 0 || vp_h <= 0) {
        *vx = 0;
        *vy = 0;
        return;
    }

    local_x = ((float)wx - vp_x) / vp_w * (float)vm_w;
    local_y = ((float)wy - vp_y) / vp_h * (float)vm_h;

    if (local_x < 0) local_x = 0;
    if (local_y < 0) local_y = 0;
    *vx = (UINT)local_x;
    *vy = (UINT)local_y;
    if (*vx >= vm_w) *vx = vm_w - 1;
    if (*vy >= vm_h) *vy = vm_h - 1;
}

static UINT legacy_linux_input_coordinate(UINT coordinate, UINT frame_extent,
                                          UINT legacy_extent)
{
    UINT64 scaled;

    if (frame_extent <= 1 || legacy_extent == 0) return 0;
    if (coordinate >= frame_extent - 1) return legacy_extent;
    scaled = ((UINT64)coordinate * legacy_extent + (frame_extent - 1) / 2) /
             (frame_extent - 1);
    if (scaled > legacy_extent) scaled = legacy_extent;
    return (UINT)scaled;
}

static BOOL frame_required_capacity(UINT32 width, UINT32 height, UINT32 stride,
                                    SIZE_T *capacity_out)
{
    SIZE_T capacity;

    if (!capacity_out ||
        width < MIN_FRAME_WIDTH || height < MIN_FRAME_HEIGHT ||
        width > MAX_FRAME_WIDTH || height > MAX_FRAME_HEIGHT ||
        stride < width * 4 || stride > MAX_FRAME_STRIDE)
        return FALSE;

    if ((SIZE_T)height > ((SIZE_T)-1) / (SIZE_T)stride)
        return FALSE;

    capacity = (SIZE_T)stride * (SIZE_T)height;
    if (capacity > (SIZE_T)MAX_FRAME_STRIDE * (SIZE_T)MAX_FRAME_HEIGHT)
        return FALSE;

    *capacity_out = capacity;
    return TRUE;
}

/* ---- Reliable recv: read exactly `len` bytes ---- */

static BOOL recv_exact(SOCKET s, void *buf, int len)
{
    char *p = (char *)buf;
    int remaining = len;
    while (remaining > 0) {
        int n = recv(s, p, remaining, 0);
        if (n <= 0) return FALSE;
        p += n;
        remaining -= n;
    }
    return TRUE;
}

/* ---- Non-blocking connect with timeout ---- */

static SOCKET connect_to_hv_service(const GUID *vm_runtime_id, const GUID *service_guid, int timeout_ms)
{
    SOCKET s;
    SOCKADDR_HV addr;
    u_long nonblock;
    fd_set wfds, efds;
    struct timeval tv;
    DWORD sock_timeout;
    static const GUID zero_guid = {0};

    if (memcmp(vm_runtime_id, &zero_guid, sizeof(GUID)) == 0)
        return INVALID_SOCKET;

    s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;

    /* Non-blocking connect */
    nonblock = 1;
    ioctlsocket(s, FIONBIO, &nonblock);

    memset(&addr, 0, sizeof(addr));
    addr.Family   = AF_HYPERV;
    addr.VmId     = *vm_runtime_id;
    addr.ServiceId = *service_guid;

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            return INVALID_SOCKET;
        }

        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &wfds, &efds, &tv) <= 0 || FD_ISSET(s, &efds)) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }

    /* Back to blocking with recv timeout */
    nonblock = 0;
    ioctlsocket(s, FIONBIO, &nonblock);
    sock_timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&sock_timeout, sizeof(sock_timeout));

    return s;
}

/* ==================================================================
 * Audio playback — guest -> host
 * ================================================================== */

static const CLSID AUDIO_CLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const IID AUDIO_IID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const IID AUDIO_IID_IAudioClient =
    { 0x1CB9AD4C, 0xDBFA, 0x4C32, { 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2 } };
static const IID AUDIO_IID_IAudioRenderClient =
    { 0xF294ACFC, 0x3146, 0x4483, { 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2 } };

static DWORD WINAPI audio_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    HRESULT hr;
    BOOL com_ok = FALSE;
    IMMDeviceEnumerator *pEnum = NULL;
    IMMDevice *pDev = NULL;
    IAudioClient *pAC = NULL;
    IAudioRenderClient *pRC = NULL;
    WAVEFORMATEX *renderfmt = NULL;
    BYTE *scratch = NULL;
    UINT32 scratch_cap = 0;
    UINT32 buf_frames = 0;
    UINT32 render_block_align = 0;
    AudioHeader hdr;
    BOOL stream_started = FALSE;
    BOOL session_logged = FALSE;
    int  header_misses = 0;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE)
        com_ok = TRUE;

    /* Outer retry loop — keep trying to connect to the guest capture helper */
    while (!d->stop) {
        GUID svc; hcs_service_guid(d->os_type, 4, &svc);
        SOCKET s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
        if (s == INVALID_SOCKET) {
            int wait;
            for (wait = 0; wait < 2000 && !d->stop; wait += 200)
                Sleep(200);
            continue;
        }

        /* Blocking recv for the session */
        {
            DWORD no_timeout = 0;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&no_timeout, sizeof(no_timeout));
        }
        d->audio_socket = s;

        /* Read one-shot AudioHeader.
           The guest closes without sending a header when the VAD endpoint
           isn't ready yet (e.g. before user login / audio stack init).
           Log only the first miss per run so we don't spam. */
        if (!recv_exact(s, &hdr, sizeof(hdr)) || hdr.magic != AUDIO_HEADER_MAGIC) {
            if (header_misses == 0)
                idd_log(d, L"Audio header bad/absent - guest not ready, retrying quietly.");
            header_misses++;
            goto session_cleanup;
        }

        idd_log(d, L"Audio connected (GUID :0004).");
        session_logged = TRUE;
        header_misses = 0;

        idd_log(d, L"Audio header: %lu Hz, %u ch, %u bits, tag=%u.",
                 hdr.sample_rate, hdr.channels, hdr.bits_per_sample, hdr.format_tag);

        /* Set up WASAPI render on the default endpoint */
        hr = CoCreateInstance(&AUDIO_CLSID_MMDeviceEnumerator, NULL,
                               CLSCTX_ALL, &AUDIO_IID_IMMDeviceEnumerator,
                               (void **)&pEnum);
        if (FAILED(hr)) { idd_log(d, L"Audio: CoCreateInstance failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(pEnum, eRender, eConsole, &pDev);
        if (FAILED(hr)) { idd_log(d, L"Audio: GetDefaultAudioEndpoint failed 0x%08lX.", hr); goto session_cleanup; }

        hr = IMMDevice_Activate(pDev, &AUDIO_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&pAC);
        if (FAILED(hr)) { idd_log(d, L"Audio: Activate failed 0x%08lX.", hr); goto session_cleanup; }

        /* Build the source format exactly matching what the guest is sending.
           We deliberately do NOT call GetMixFormat: we never use the host's
           mix format (Initialize runs with AUTOCONVERTPCM, so WASAPI converts
           our renderfmt to whatever the endpoint needs). On some endpoints
           (seen on AMD HD Audio) GetMixFormat returns
           AUDCLNT_E_UNSUPPORTED_FORMAT for the configured default format,
           which would needlessly kill the whole audio path. */
        renderfmt = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        if (!renderfmt) goto session_cleanup;
        ZeroMemory(renderfmt, sizeof(WAVEFORMATEX));
        renderfmt->wFormatTag      = hdr.format_tag;
        renderfmt->nChannels       = hdr.channels;
        renderfmt->nSamplesPerSec  = hdr.sample_rate;
        renderfmt->wBitsPerSample  = hdr.bits_per_sample;
        renderfmt->nBlockAlign     = hdr.block_align ? hdr.block_align
                                     : (WORD)((hdr.channels * hdr.bits_per_sample) / 8);
        renderfmt->nAvgBytesPerSec = renderfmt->nSamplesPerSec * renderfmt->nBlockAlign;
        renderfmt->cbSize          = 0;
        render_block_align         = renderfmt->nBlockAlign;

        /* 40ms buffer. HV socket is effectively memcpy and the guest polls
           every 5ms, so scheduler jitter is the only thing we need to absorb. */
        hr = IAudioClient_Initialize(pAC, AUDCLNT_SHAREMODE_SHARED,
                                      AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                      AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                      400000, 0, renderfmt, NULL);
        if (FAILED(hr)) {
            idd_log(d, L"Audio: IAudioClient::Initialize failed 0x%08lX.", hr);
            goto session_cleanup;
        }

        hr = IAudioClient_GetBufferSize(pAC, &buf_frames);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_GetService(pAC, &AUDIO_IID_IAudioRenderClient, (void **)&pRC);
        if (FAILED(hr)) goto session_cleanup;

        hr = IAudioClient_Start(pAC);
        if (FAILED(hr)) goto session_cleanup;
        stream_started = TRUE;

        idd_log(d, L"Audio render started: buf=%lu frames.", buf_frames);

        /* Receive loop */
        while (!d->stop) {
            AudioFrameHeader fh;
            UINT32 frames_in_payload;
            UINT32 padding;
            UINT32 free_frames;
            UINT32 frames_to_write;
            BYTE *dst;

            if (!recv_exact(s, &fh, sizeof(fh))) break;
            if (fh.bytes == 0 || fh.bytes > 4 * 1024 * 1024) break;

            if (fh.bytes > scratch_cap) {
                BYTE *nb = scratch
                    ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, scratch, fh.bytes)
                    : (BYTE *)HeapAlloc(GetProcessHeap(), 0, fh.bytes);
                if (!nb) break;
                scratch = nb;
                scratch_cap = fh.bytes;
            }
            if (!recv_exact(s, scratch, (int)fh.bytes)) break;

            /* Host-side mute: keep draining the socket but don't push to WASAPI */
            if (d->audio_muted) continue;

            frames_in_payload = fh.bytes / render_block_align;
            if (frames_in_payload == 0) continue;

            /* Push into WASAPI buffer; drop if no room (low-latency, no stalling) */
            hr = IAudioClient_GetCurrentPadding(pAC, &padding);
            if (FAILED(hr)) break;
            free_frames = buf_frames - padding;
            frames_to_write = frames_in_payload;
            if (frames_to_write > free_frames)
                frames_to_write = free_frames;
            if (frames_to_write == 0)
                continue;

            hr = IAudioRenderClient_GetBuffer(pRC, frames_to_write, &dst);
            if (FAILED(hr)) break;
            memcpy(dst, scratch, (size_t)frames_to_write * render_block_align);
            IAudioRenderClient_ReleaseBuffer(pRC, frames_to_write, 0);
        }

session_cleanup:
        if (stream_started && pAC) {
            IAudioClient_Stop(pAC);
            stream_started = FALSE;
        }
        if (pRC)    { IAudioRenderClient_Release(pRC);    pRC = NULL; }
        if (pAC)    { IAudioClient_Release(pAC);          pAC = NULL; }
        if (pDev)   { IMMDevice_Release(pDev);            pDev = NULL; }
        if (pEnum)  { IMMDeviceEnumerator_Release(pEnum); pEnum = NULL; }
        if (renderfmt) { CoTaskMemFree(renderfmt); renderfmt = NULL; }

        if (d->audio_socket != INVALID_SOCKET) {
            closesocket(d->audio_socket);
            d->audio_socket = INVALID_SOCKET;
        }
        if (session_logged) {
            idd_log(d, L"Audio session ended.");
            session_logged = FALSE;
        }

        if (d->stop) break;
        /* Back off when the guest keeps rejecting us (VAD not ready) */
        Sleep(header_misses > 3 ? 5000 : 500);
    }

    if (scratch) HeapFree(GetProcessHeap(), 0, scratch);
    if (com_ok)  CoUninitialize();
    d->audio_recv_thread = NULL;
    return 0;
}


/* ==================================================================
 * D3D11 initialization and teardown
 * ================================================================== */

static BOOL d3d_compile_shader(const char *hlsl, const char *entry,
                               const char *target, ID3DBlob **out)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), NULL, NULL, NULL,
                            entry, target, 0, 0, out, &errors);
    if (FAILED(hr)) {
        if (errors) {
            ui_log(L"Shader compile error: %S",
                   (const char *)errors->lpVtbl->GetBufferPointer(errors));
            errors->lpVtbl->Release(errors);
        }
        return FALSE;
    }
    if (errors) errors->lpVtbl->Release(errors);
    return TRUE;
}

static BOOL d3d_ensure_frame_texture(VmDisplayIdd *d, UINT width, UINT height)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc;
    ID3D11Texture2D *new_tex = NULL;
    ID3D11ShaderResourceView *new_srv = NULL;
    HRESULT hr;

    if (!d->device || width < MIN_FRAME_WIDTH || height < MIN_FRAME_HEIGHT ||
        width > MAX_FRAME_WIDTH || height > MAX_FRAME_HEIGHT)
        return FALSE;

    if (d->frame_tex && d->frame_srv &&
        d->frame_tex_width == width && d->frame_tex_height == height)
        return TRUE;

    ZeroMemory(&td, sizeof(td));
    td.Width              = width;
    td.Height             = height;
    td.MipLevels          = 1;
    td.ArraySize          = 1;
    td.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count   = 1;
    td.Usage              = D3D11_USAGE_DYNAMIC;
    td.BindFlags          = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags     = D3D11_CPU_ACCESS_WRITE;

    hr = d->device->lpVtbl->CreateTexture2D(d->device, &td, NULL, &new_tex);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateTexture2D %ux%u failed (0x%08X)", width, height, hr);
        return FALSE;
    }

    ZeroMemory(&srv_desc, sizeof(srv_desc));
    srv_desc.Format                    = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels       = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    hr = d->device->lpVtbl->CreateShaderResourceView(d->device,
            (ID3D11Resource *)new_tex, &srv_desc, &new_srv);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateShaderResourceView %ux%u failed (0x%08X)", width, height, hr);
        new_tex->lpVtbl->Release(new_tex);
        return FALSE;
    }

    if (d->ctx && d->frame_srv) {
        ID3D11ShaderResourceView *null_srv = NULL;
        d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 1, &null_srv);
    }
    if (d->frame_srv) d->frame_srv->lpVtbl->Release(d->frame_srv);
    if (d->frame_tex) d->frame_tex->lpVtbl->Release(d->frame_tex);

    d->frame_tex = new_tex;
    d->frame_srv = new_srv;
    d->frame_tex_width = width;
    d->frame_tex_height = height;
    idd_log(d, L"D3D frame texture changed to %ux%u.", width, height);
    return TRUE;
}

static BOOL d3d_init(VmDisplayIdd *d)
{
    DXGI_SWAP_CHAIN_DESC1 scd;
    D3D_FEATURE_LEVEL feature_level;
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    IDXGISwapChain1 *swap_chain = NULL;
    D3D11_SAMPLER_DESC sd;
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    UINT initial_width;
    UINT initial_height;
    HRESULT hr;

    initial_width = (UINT)InterlockedCompareExchange(
            &d->desired_render_width, 0, 0);
    initial_height = (UINT)InterlockedCompareExchange(
            &d->desired_render_height, 0, 0);
    if (initial_width < MIN_FRAME_WIDTH || initial_width > MAX_FRAME_WIDTH)
        initial_width = d->initial_width;
    if (initial_height < MIN_FRAME_HEIGHT || initial_height > MAX_FRAME_HEIGHT)
        initial_height = d->initial_height;

    /* Create a two-buffer flip-model chain. App-mode targets the top-level
       client directly (rather than an asynchronously resized child HWND), so
       DWM always has one complete surface to stretch during live edge drag. */
    ZeroMemory(&scd, sizeof(scd));
    scd.Width            = initial_width;
    scd.Height           = initial_height;
    scd.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount      = 2;
    scd.Scaling          = DXGI_SCALING_STRETCH;
    scd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.AlphaMode        = DXGI_ALPHA_MODE_IGNORE;

    hr = D3D11CreateDevice(
        NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        NULL, 0, D3D11_SDK_VERSION,
        &d->device, &feature_level, &d->ctx);

    if (FAILED(hr)) {
        ui_log(L"IDD: D3D11CreateDevice failed (0x%08X)", hr);
        return FALSE;
    }
    hr = d->device->lpVtbl->QueryInterface(
            d->device, &IID_IDXGIDevice, (void **)&dxgi_device);
    if (SUCCEEDED(hr))
        hr = dxgi_device->lpVtbl->GetAdapter(dxgi_device, &adapter);
    if (SUCCEEDED(hr))
        hr = adapter->lpVtbl->GetParent(
                adapter, &IID_IDXGIFactory2, (void **)&factory);
    if (SUCCEEDED(hr))
        hr = factory->lpVtbl->CreateSwapChainForHwnd(
                factory, (IUnknown *)d->device, d->render_hwnd,
                &scd, NULL, NULL, &swap_chain);
    if (SUCCEEDED(hr))
        hr = swap_chain->lpVtbl->QueryInterface(
                swap_chain, &IID_IDXGISwapChain, (void **)&d->swap_chain);
    if (factory)
        factory->lpVtbl->MakeWindowAssociation(
                factory, d->hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (swap_chain) swap_chain->lpVtbl->Release(swap_chain);
    if (factory) factory->lpVtbl->Release(factory);
    if (adapter) adapter->lpVtbl->Release(adapter);
    if (dxgi_device) dxgi_device->lpVtbl->Release(dxgi_device);
    if (FAILED(hr)) {
        ui_log(L"IDD: flip-model swap-chain creation failed (0x%08X)", hr);
        return FALSE;
    }
    d->render_width = initial_width;
    d->render_height = initial_height;

    /* Create render target view from back buffer */
    {
        ID3D11Texture2D *back_buf = NULL;
        hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                       &IID_ID3D11Texture2D, (void **)&back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: GetBuffer failed (0x%08X)", hr);
            return FALSE;
        }
        hr = d->device->lpVtbl->CreateRenderTargetView(d->device,
                (ID3D11Resource *)back_buf, NULL, &d->rtv);
        back_buf->lpVtbl->Release(back_buf);
        if (FAILED(hr)) {
            ui_log(L"IDD: CreateRenderTargetView failed (0x%08X)", hr);
            return FALSE;
        }
    }

    if (!d3d_ensure_frame_texture(d, d->frame_width, d->frame_height)) {
        ui_log(L"IDD: Failed to create initial frame texture.");
        return FALSE;
    }

    /* Compile and create vertex shader */
    if (!d3d_compile_shader(g_vs_hlsl, "main", "vs_4_0", &vs_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreateVertexShader(d->device,
            vs_blob->lpVtbl->GetBufferPointer(vs_blob),
            vs_blob->lpVtbl->GetBufferSize(vs_blob),
            NULL, &d->vs);
    vs_blob->lpVtbl->Release(vs_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateVertexShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Compile and create pixel shader */
    if (!d3d_compile_shader(g_ps_hlsl, "main", "ps_4_0", &ps_blob))
        return FALSE;
    hr = d->device->lpVtbl->CreatePixelShader(d->device,
            ps_blob->lpVtbl->GetBufferPointer(ps_blob),
            ps_blob->lpVtbl->GetBufferSize(ps_blob),
            NULL, &d->ps);
    ps_blob->lpVtbl->Release(ps_blob);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreatePixelShader failed (0x%08X)", hr);
        return FALSE;
    }

    /* Sampler state (linear filtering) */
    ZeroMemory(&sd, sizeof(sd));
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    hr = d->device->lpVtbl->CreateSamplerState(d->device, &sd, &d->sampler);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateSamplerState failed (0x%08X)", hr);
        return FALSE;
    }

    return TRUE;
}

static BOOL d3d_create_render_target(VmDisplayIdd *d)
{
    HRESULT hr;
    ID3D11Texture2D *back_buf = NULL;

    if (!d->swap_chain || !d->device) return FALSE;

    hr = d->swap_chain->lpVtbl->GetBuffer(d->swap_chain, 0,
                                   &IID_ID3D11Texture2D, (void **)&back_buf);
    if (FAILED(hr)) {
        ui_log(L"IDD: GetBuffer after resize failed (0x%08X)", hr);
        return FALSE;
    }
    hr = d->device->lpVtbl->CreateRenderTargetView(d->device,
            (ID3D11Resource *)back_buf, NULL, &d->rtv);
    back_buf->lpVtbl->Release(back_buf);
    if (FAILED(hr)) {
        ui_log(L"IDD: CreateRenderTargetView after resize failed (0x%08X)", hr);
        return FALSE;
    }
    return TRUE;
}

static BOOL d3d_resize_swap_chain(VmDisplayIdd *d, UINT width, UINT height)
{
    HRESULT hr;

    if (!d->swap_chain) return FALSE;
    if (width < MIN_FRAME_WIDTH || height < MIN_FRAME_HEIGHT ||
        width > MAX_FRAME_WIDTH || height > MAX_FRAME_HEIGHT)
        return FALSE;
    if (d->render_width == width && d->render_height == height && d->rtv)
        return TRUE;

    /* Release old render target */
    if (d->rtv) {
        d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 0, NULL, NULL);
        d->rtv->lpVtbl->Release(d->rtv);
        d->rtv = NULL;
    }

    idd_log(d, L"Resize: render_hwnd client=%ux%u, frame=%ux%u",
            width, height, d->frame_width, d->frame_height);

    hr = d->swap_chain->lpVtbl->ResizeBuffers(d->swap_chain, 0,
            width, height,
            DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        ui_log(L"IDD: ResizeBuffers failed (0x%08X)", hr);
        /* ResizeBuffers can fail without invalidating the old back buffer.
           Reacquire a render target so the last complete frame stays usable. */
        d3d_create_render_target(d);
        return FALSE;
    }

    if (!d3d_create_render_target(d))
        return FALSE;
    d->render_width = width;
    d->render_height = height;
    return TRUE;
}

static BOOL d3d_render_frame(VmDisplayIdd *d)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT vp;
    HRESULT hr;
    float clear_color[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    BOOL frame_uploaded = FALSE;

    if (!d->device || !d->ctx || !d->swap_chain || !d->rtv)
        return FALSE;

    /* Upload frame data to GPU texture if dirty */
    if (d->frame_dirty) {
        EnterCriticalSection(&d->frame_cs);
        if (!d3d_ensure_frame_texture(d, d->frame_width, d->frame_height)) {
            LeaveCriticalSection(&d->frame_cs);
            return FALSE;
        }
        hr = d->ctx->lpVtbl->Map(d->ctx,
                (ID3D11Resource *)d->frame_tex, 0,
                D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            UINT row;
            UINT copy_stride = d->frame_width * 4;
            if (copy_stride > mapped.RowPitch)
                copy_stride = mapped.RowPitch;
            if (copy_stride > d->frame_stride)
                copy_stride = d->frame_stride;

            for (row = 0; row < d->frame_height; row++) {
                memcpy((BYTE *)mapped.pData + row * mapped.RowPitch,
                       d->frame_buf + row * d->frame_stride,
                       copy_stride);
            }
            d->ctx->lpVtbl->Unmap(d->ctx,
                    (ID3D11Resource *)d->frame_tex, 0);
            d->frame_dirty = FALSE;
            d->uploaded_frame_generation = (UINT64)InterlockedCompareExchange64(
                    &d->received_frame_generation, 0, 0);
            frame_uploaded = TRUE;
        }
        LeaveCriticalSection(&d->frame_cs);
    }

    /* A fixed-capacity application canvas is cropped 1:1. Legacy application
       mode still fills its client, and diagnostic mode remains letterboxed. */
    {
        float vp_x, vp_y, vp_w, vp_h;
        if (d->app_mode && d->fixed_backing) {
            /* The guest framebuffer is a fixed-capacity canvas. Rendering its
               native-sized viewport and letting the render target clip the
               right/bottom remainder preserves 1:1 text and video pixels while
               the product window changes size. The scene controller resizes
               Firefox logically inside that canvas. */
            vp_x = 0;
            vp_y = 0;
            vp_w = (float)d->frame_width;
            vp_h = (float)d->frame_height;
        } else if (d->app_mode) {
            vp_x = 0;
            vp_y = 0;
            vp_w = (float)d->render_width;
            vp_h = (float)d->render_height;
        } else {
            compute_letterbox(d->render_width, d->render_height,
                              d->frame_width, d->frame_height,
                              &vp_x, &vp_y, &vp_w, &vp_h);
        }
        ZeroMemory(&vp, sizeof(vp));
        vp.TopLeftX = vp_x;
        vp.TopLeftY = vp_y;
        vp.Width    = vp_w;
        vp.Height   = vp_h;
        vp.MaxDepth = 1.0f;
    }

    /* Preserve the clean product title in application mode. */
    if (frame_uploaded && d->hwnd &&
        (!d->app_mode || d->show_debug_title)) {
        idd_update_window_title(d);
    }
    d->render_count++;

    d->ctx->lpVtbl->OMSetRenderTargets(d->ctx, 1, &d->rtv, NULL);
    d->ctx->lpVtbl->RSSetViewports(d->ctx, 1, &vp);
    d->ctx->lpVtbl->ClearRenderTargetView(d->ctx, d->rtv, clear_color);

    d->ctx->lpVtbl->IASetPrimitiveTopology(d->ctx,
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d->ctx->lpVtbl->IASetInputLayout(d->ctx, NULL);

    d->ctx->lpVtbl->VSSetShader(d->ctx, d->vs, NULL, 0);
    d->ctx->lpVtbl->PSSetShader(d->ctx, d->ps, NULL, 0);
    d->ctx->lpVtbl->PSSetShaderResources(d->ctx, 0, 1, &d->frame_srv);
    d->ctx->lpVtbl->PSSetSamplers(d->ctx, 0, 1, &d->sampler);

    /* Draw fullscreen triangle (3 vertices, no vertex buffer) */
    d->ctx->lpVtbl->Draw(d->ctx, 3, 0);

    hr = d->swap_chain->lpVtbl->Present(
            d->swap_chain, 0, DXGI_PRESENT_DO_NOT_WAIT);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        return TRUE;
    } else if (FAILED(hr)) {
        idd_log(d, L"D3D Present failed (0x%08X).", hr);
    } else {
        InterlockedIncrement64(&d->present_count);
        InterlockedExchange(&d->presented_render_width, (LONG)d->render_width);
        InterlockedExchange(&d->presented_render_height, (LONG)d->render_height);
        if (d->uploaded_frame_generation != 0) {
            InterlockedExchange64(
                    &d->presented_frame_generation,
                    (LONG64)d->uploaded_frame_generation);
            d->uploaded_frame_generation = 0;
        }
    }
    return d->frame_dirty;
}

static void d3d_cleanup(VmDisplayIdd *d)
{
    if (d->sampler)    { d->sampler->lpVtbl->Release(d->sampler);       d->sampler = NULL; }
    if (d->ps)         { d->ps->lpVtbl->Release(d->ps);                 d->ps = NULL; }
    if (d->vs)         { d->vs->lpVtbl->Release(d->vs);                 d->vs = NULL; }
    if (d->frame_srv)  { d->frame_srv->lpVtbl->Release(d->frame_srv);   d->frame_srv = NULL; }
    if (d->frame_tex)  { d->frame_tex->lpVtbl->Release(d->frame_tex);   d->frame_tex = NULL; }
    d->frame_tex_width = 0;
    d->frame_tex_height = 0;
    if (d->rtv)        { d->rtv->lpVtbl->Release(d->rtv);               d->rtv = NULL; }
    if (d->swap_chain) { d->swap_chain->lpVtbl->Release(d->swap_chain); d->swap_chain = NULL; }
    if (d->ctx)        { d->ctx->lpVtbl->Release(d->ctx);               d->ctx = NULL; }
    if (d->device)     { d->device->lpVtbl->Release(d->device);         d->device = NULL; }
    d->render_width = 0;
    d->render_height = 0;
}

static DWORD WINAPI idd_render_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    BOOL present_retry = FALSE;

    if (!d3d_init(d)) {
        InterlockedExchange(&d->render_init_result, -1);
        SetEvent(d->render_ready_event);
        d3d_cleanup(d);
        return 1;
    }

    InterlockedExchange(&d->render_init_result, 1);
    SetEvent(d->render_ready_event);
    present_retry = d3d_render_frame(d);

    while (!d->stop) {
        DWORD wait_result = WaitForSingleObject(
                d->render_event,
                present_retry ? PRESENT_RETRY_MS : INFINITE);
        UINT desired_width;
        UINT desired_height;
        BOOL resize_requested;

        if (d->stop) break;
        if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT)
            break;

        desired_width = (UINT)InterlockedCompareExchange(
                &d->desired_render_width, 0, 0);
        desired_height = (UINT)InterlockedCompareExchange(
                &d->desired_render_height, 0, 0);
        resize_requested =
            InterlockedExchange(&d->render_resize_pending, 0) != 0;

        if (resize_requested &&
            !d3d_resize_swap_chain(d, desired_width, desired_height)) {
            /* A transient device/compositor conflict must not lose the newest
               geometry. Retry on this worker while the native UI keeps moving. */
            InterlockedExchange(&d->render_resize_pending, 1);
            present_retry = TRUE;
            continue;
        }

        present_retry = d3d_render_frame(d);
    }

    d3d_cleanup(d);
    return 0;
}

/* ==================================================================
 * Guest cursor — create HCURSOR from received bitmap
 * ================================================================== */

/* Cursor types from IddCx IDDCX_CURSOR_SHAPE_TYPE */
#define CURSOR_TYPE_MASKED_COLOR  1
#define CURSOR_TYPE_ALPHA         2

static HCURSOR create_cursor_from_bitmap(UINT width, UINT height,
                                          UINT xhot, UINT yhot,
                                          UINT cursor_type, UINT pitch,
                                          UINT shape_data_size,
                                          const BYTE *shape_data)
{
    HCURSOR result = NULL;
    BITMAPINFO bmi;
    HBITMAP hColor = NULL, hMask = NULL;
    ICONINFO ii;
    HDC hdc;

    if (width == 0 || height == 0 || width > 256 || height > 256)
        return NULL;

    /* shape_data holds exactly shape_data_size bytes; both copy paths read up
       to (height-1)*pitch + width*4 bytes. Require the stride to cover a row
       and the total to cover all rows, else a tiny buffer with large
       width/height/pitch would over-read. */
    if (pitch < width * 4 || (UINT64)pitch * height > (UINT64)shape_data_size)
        return NULL;

    hdc = GetDC(NULL);

    if (cursor_type == CURSOR_TYPE_MASKED_COLOR) {
        /* MASKED_COLOR (IddCx 1.10 / QueryHardwareCursor3): single 32bpp BGRA
           image where the alpha channel encodes the AND mask.
           Height is the ACTUAL cursor height (not doubled).
           Per pixel: A = AND mask (0xFF = transparent/XOR, 0x00 = opaque),
                      B,G,R = XOR color values.
           We extract A into a 1bpp monochrome hbmMask and BGR into hbmColor. */
        UINT mask_row_bytes = (width + 7) / 8;
        UINT mask_pitch = ((mask_row_bytes + 3) & ~3u);
        void *color_bits = NULL;
        BYTE *mask_buf;
        UINT row, col;

        /* Build 1bpp AND mask from alpha channel */
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      mask_pitch * height);
        if (!mask_buf) { ReleaseDC(NULL, hdc); return NULL; }

        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                BYTE alpha = src_row[col * 4 + 3];  /* A channel */
                if (alpha != 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        /* XOR color bitmap (32bpp, top-down) — copy full BGRA data */
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = (LONG)width;
        bmi.bmiHeader.biHeight      = -(LONG)height;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        hColor = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (hColor && color_bits) {
            UINT dst_pitch = width * 4;
            for (row = 0; row < height; row++) {
                const BYTE *src = shape_data + row * pitch;
                BYTE *dst = (BYTE *)color_bits + row * dst_pitch;
                for (col = 0; col < width; col++) {
                    dst[col * 4 + 0] = src[col * 4 + 0];  /* B */
                    dst[col * 4 + 1] = src[col * 4 + 1];  /* G */
                    dst[col * 4 + 2] = src[col * 4 + 2];  /* R */
                    dst[col * 4 + 3] = 0;                  /* A = 0, AND mask handles transparency */
                }
            }
        }

        ReleaseDC(NULL, hdc);

        if (!hColor || !hMask) {
            if (hColor) DeleteObject(hColor);
            if (hMask)  DeleteObject(hMask);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;

    } else {
        /* ALPHA (type 2): 32bpp BGRA with premultiplied alpha.
           Height is the real cursor height.

           The 32-bpp DIB section with BI_RGB doesn't tell Windows that
           the top byte is alpha — Windows treats it as 24-bit RGB +
           padding and, with an all-zero AND mask, renders EVERY pixel
           opaque. For cursors whose transparent regions are stored as
           (0,0,0,0) (e.g. ours from Linux), that paints a black square
           around the cursor. Windows-VDD cursors happen not to trigger
           this because their transparent regions have non-zero RGB.

           Fix: build the AND mask from the alpha channel — bit set
           (1) = transparent, bit cleared (0) = opaque. Threshold low
           (any alpha > 0 = opaque) preserves anti-aliased edges, the
           alpha channel then handles smooth blending of those edges.

           Use BITMAPV4HEADER with explicit alpha mask instead of
           BITMAPINFOHEADER+BI_RGB. The latter is ambiguous at 32-bpp:
           some Windows paths treat it as XRGB (alpha ignored, RGB
           rendered at full opacity → cursor body looks too bright).
           V4 with bV4AlphaMask = 0xFF000000 settles it. */
        void *color_bits = NULL;
        UINT row, col, dst_pitch;
        UINT mask_row_bytes, mask_pitch;
        BYTE *mask_buf;
        BITMAPV4HEADER bv4;

        ZeroMemory(&bv4, sizeof(bv4));
        bv4.bV4Size          = sizeof(BITMAPV4HEADER);
        bv4.bV4Width         = (LONG)width;
        bv4.bV4Height        = -(LONG)height;
        bv4.bV4Planes        = 1;
        bv4.bV4BitCount      = 32;
        bv4.bV4V4Compression = BI_BITFIELDS;
        bv4.bV4RedMask       = 0x00FF0000;
        bv4.bV4GreenMask     = 0x0000FF00;
        bv4.bV4BlueMask      = 0x000000FF;
        bv4.bV4AlphaMask     = 0xFF000000;

        hColor = CreateDIBSection(hdc, (BITMAPINFO *)&bv4,
                                  DIB_RGB_COLORS, &color_bits, NULL, 0);
        if (!hColor || !color_bits) {
            ReleaseDC(NULL, hdc);
            return NULL;
        }

        dst_pitch = width * 4;
        for (row = 0; row < height; row++) {
            memcpy((BYTE *)color_bits + row * dst_pitch,
                   shape_data + row * pitch,
                   dst_pitch);
        }

        /* AND mask, 1bpp, DWORD-aligned rows. Bit set = transparent. */
        mask_row_bytes = (width + 7) / 8;
        mask_pitch     = (mask_row_bytes + 3) & ~3u;
        mask_buf = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      (size_t)mask_pitch * height);
        if (!mask_buf) {
            DeleteObject(hColor);
            ReleaseDC(NULL, hdc);
            return NULL;
        }
        for (row = 0; row < height; row++) {
            const BYTE *src_row = shape_data + row * pitch;
            BYTE *dst_row = mask_buf + row * mask_pitch;
            for (col = 0; col < width; col++) {
                if (src_row[col * 4 + 3] == 0)
                    dst_row[col / 8] |= (0x80 >> (col & 7));
            }
        }
        hMask = CreateBitmap((int)width, (int)height, 1, 1, mask_buf);
        HeapFree(GetProcessHeap(), 0, mask_buf);

        ReleaseDC(NULL, hdc);

        if (!hMask) {
            DeleteObject(hColor);
            return NULL;
        }

        ii.fIcon    = FALSE;
        ii.xHotspot = xhot;
        ii.yHotspot = yhot;
        ii.hbmMask  = hMask;
        ii.hbmColor = hColor;
    }

    result = (HCURSOR)CreateIconIndirect(&ii);

    DeleteObject(hColor);
    DeleteObject(hMask);

    return result;
}

/* ==================================================================
 * Recv thread - connects to VM, receives frames, updates frame_buf
 * ================================================================== */

static void clip_log_callback(const wchar_t *msg, void *user_data)
{
    VmDisplayIdd *d = (VmDisplayIdd *)user_data;
    idd_log(d, L"%s", msg);
}

static DWORD WINAPI idd_recv_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    WSADATA wsa;
    BYTE *recv_buf = NULL;
    SIZE_T recv_capacity = 0;

    WSAStartup(MAKEWORD(2, 2), &wsa);

    /* Tell the agent to respawn input helper in console session */
    if (!d->stop && d->vm && d->vm->agent_online) {
        idd_log(d, L"Sending idd_connect to agent...");
        idd_agent_send(d, "idd_connect", NULL, 0, 5000);
    }

    /* Input socket lives independently of the frame channel — survives
       frame disconnections so the user can still send input to wake
       the VM screen if the display path goes inactive. */
    {
        SOCKET input_s = INVALID_SOCKET;

    while (!d->stop) {
        SOCKET s;
        FrameHeader hdr;

        /* Ensure input channel is connected (independent of frame channel) */
        if (input_s == INVALID_SOCKET) {
            GUID svc; hcs_service_guid(d->os_type, 3, &svc);
            input_s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
            if (input_s != INVALID_SOCKET) {
                UINT32 ready_magic = 0;
                if (recv_exact(input_s, &ready_magic, sizeof(ready_magic)) &&
                    (ready_magic == INPUT_READY_MAGIC ||
                     ready_magic == INPUT_READY_MAGIC_V2)) {
                    DWORD zero_timeout = 0;
                    u_long nb = 1;
                    setsockopt(input_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
                    ioctlsocket(input_s, FIONBIO, &nb);
                    EnterCriticalSection(&d->frame_cs);
                    d->input_socket = input_s;
                    d->input_frame_size_supported =
                        ready_magic == INPUT_READY_MAGIC_V2;
                    if (d->input_frame_size_supported)
                        send_input(d, INPUT_FRAME_SIZE, d->frame_width, d->frame_height, 0);
                    LeaveCriticalSection(&d->frame_cs);
                    g_input_send_count = 0;
                    idd_log(d, L"Input connected + ready (GUID :0003).");
                } else {
                    idd_log(d, L"Input handshake failed - closing.");
                    closesocket(input_s);
                    input_s = INVALID_SOCKET;
                }
            }
        }

        /* Clipboard module (handles :0005 + :0006 internally) */
        if (!d->clipboard) {
            d->clipboard = vm_clipboard_create(&d->runtime_id, d->os_type,
                                               d->hwnd, clip_log_callback, d);
            if (d->clipboard)
                idd_log(d, L"Clipboard module created.");
        }

        /* Ensure audio recv thread is running (:0004, guest→host).
           The thread handles connecting on its own — the helper may not be up yet. */
        if (!d->audio_recv_thread) {
            d->audio_recv_thread = CreateThread(NULL, 0, audio_recv_thread_proc, d, 0, NULL);
            idd_log(d, L"Audio: Started recv thread (will connect when helper is available).");
        }

        /* Try to connect frame channel (VDD driver, GUID :0002) */
        idd_log(d, L"Connecting to frame service...");
        {
            GUID svc; hcs_service_guid(d->os_type, 2, &svc);
            s = connect_to_hv_service(&d->runtime_id, &svc, 3000);
        }
        if (s == INVALID_SOCKET) {
            int wait;
            idd_log(d, L"Connection failed, retrying in 3s.");
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
            continue;
        }

        idd_log(d, L"Frame channel connected.");

        /* Receive loop — reads magic first to dispatch frame vs cursor */
        while (!d->stop) {
            RECT dirty_rects[MAX_DIRTY_RECTS];
            UINT32 data_size;
            UINT32 rect_count;
            UINT32 i;
            UINT32 magic;
            SIZE_T required_capacity;

            /* Peek at magic to determine message type */
            if (!recv_exact(s, &magic, sizeof(magic)))
                break;

            if (magic == CURSOR_MAGIC) {
                /* Read rest of cursor header (already read magic) */
                CursorHeader chdr;
                chdr.magic = magic;
                if (!recv_exact(s, (BYTE *)&chdr + sizeof(UINT32),
                                sizeof(CursorHeader) - sizeof(UINT32)))
                    break;

                d->cursor_visible = chdr.visible;

                if (chdr.shape_updated && chdr.shape_data_size > 0) {
                    BYTE *cursor_buf;
                    if (chdr.shape_data_size > MAX_CURSOR_SIZE) {
                        idd_log(d, L"Cursor data too large (%u), reconnecting.",
                               chdr.shape_data_size);
                        break;
                    }
                    cursor_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                                    chdr.shape_data_size);
                    if (!cursor_buf) break;

                    if (!recv_exact(s, cursor_buf, (int)chdr.shape_data_size)) {
                        HeapFree(GetProcessHeap(), 0, cursor_buf);
                        break;
                    }

                    /* Create new cursor from bitmap */
                    {
                        HCURSOR new_cursor = create_cursor_from_bitmap(
                            chdr.width, chdr.height, chdr.xhot, chdr.yhot,
                            chdr.cursor_type, chdr.pitch,
                            chdr.shape_data_size, cursor_buf);
                        if (new_cursor) {
                            HCURSOR old = d->guest_cursor;
                            d->guest_cursor = new_cursor;
                            d->cursor_shape_id = chdr.shape_id;
                            if (old) DestroyCursor(old);
                            /* Force cursor update if mouse is in window */
                            if (d->render_hwnd)
                                PostMessageW(d->render_hwnd, WM_SETCURSOR,
                                             (WPARAM)d->render_hwnd,
                                             MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
                        }
                    }

                    HeapFree(GetProcessHeap(), 0, cursor_buf);
                }
                continue;  /* back to message loop */
            }

            if (magic != FRAME_MAGIC) {
                idd_log(d, L"Bad magic 0x%08X, reconnecting.", magic);
                break;
            }

            /* Read rest of frame header (already read magic) */
            hdr.magic = magic;
            if (!recv_exact(s, (BYTE *)&hdr + sizeof(UINT32),
                            sizeof(FrameHeader) - sizeof(UINT32)))
                break;

            /* Sanity checks */
            if (!frame_required_capacity(hdr.width, hdr.height, hdr.stride,
                                         &required_capacity)) {
                idd_log(d, L"Invalid frame dimensions %ux%u stride %u.",
                       hdr.width, hdr.height, hdr.stride);
                break;
            }

            if (required_capacity > recv_capacity) {
                BYTE *new_recv_buf;
                if (recv_buf) {
                    new_recv_buf = (BYTE *)HeapReAlloc(GetProcessHeap(), 0,
                                                       recv_buf, required_capacity);
                } else {
                    new_recv_buf = (BYTE *)HeapAlloc(GetProcessHeap(), 0,
                                                     required_capacity);
                }
                if (!new_recv_buf) {
                    idd_log(d, L"Failed to allocate %llu-byte frame receive buffer.",
                            (unsigned long long)required_capacity);
                    break;
                }
                recv_buf = new_recv_buf;
                recv_capacity = required_capacity;
            }

            rect_count = hdr.dirty_rect_count;
            if (rect_count > MAX_DIRTY_RECTS) {
                idd_log(d, L"Too many dirty rects (%u), reconnecting.", rect_count);
                break;
            }

            /* Read dirty rects */
            if (rect_count > 0) {
                if (!recv_exact(s, dirty_rects, (int)(rect_count * sizeof(RECT))))
                    break;
            }

            /* Read data_size */
            if (!recv_exact(s, &data_size, 4))
                break;

            if ((SIZE_T)data_size > required_capacity) {
                idd_log(d, L"Frame data too large (%u > %llu bytes), reconnecting.",
                        data_size, (unsigned long long)required_capacity);
                break;
            }

            /* Read pixel data */
            if (data_size > 0) {
                if (!recv_exact(s, recv_buf, (int)data_size))
                    break;
            }

            /* Update CPU-side frame buffer */
            EnterCriticalSection(&d->frame_cs);

            /* Reallocate frame_buf if resolution changed */
            if (hdr.width != d->frame_width || hdr.height != d->frame_height) {
                UINT new_stride = hdr.width * 4;
                SIZE_T new_size = (SIZE_T)new_stride * (SIZE_T)hdr.height;
                BYTE *new_buf   = (BYTE *)HeapAlloc(GetProcessHeap(),
                                                     HEAP_ZERO_MEMORY, new_size);
                if (new_buf) {
                    if (d->frame_buf)
                        HeapFree(GetProcessHeap(), 0, d->frame_buf);
                    d->frame_buf    = new_buf;
                    d->frame_width  = hdr.width;
                    d->frame_height = hdr.height;
                    d->frame_stride = new_stride;
                    if (d->input_frame_size_supported)
                        send_input(d, INPUT_FRAME_SIZE, hdr.width, hdr.height, 0);
                    idd_log(d, L"Frame resolution changed: %ux%u (stride=%u)",
                            hdr.width, hdr.height, hdr.stride);
                    idd_log(d, L"Resolution changed to %ux%u.", hdr.width, hdr.height);
                } else {
                    LeaveCriticalSection(&d->frame_cs);
                    break;
                }
            }

            if (rect_count == 0) {
                /* Full frame update */
                UINT row;
                UINT copy_w = hdr.width * 4;
                BYTE *src = recv_buf;
                /* Bound source rows by the bytes actually received: width/height/
                   stride and data_size are independent guest-controlled fields, so
                   only data_size/stride rows of recv_buf hold valid pixels. */
                UINT src_rows = hdr.stride ? (UINT)(data_size / hdr.stride) : 0;
                if (copy_w > d->frame_stride) copy_w = d->frame_stride;
                if (copy_w > hdr.stride)      copy_w = hdr.stride;

                for (row = 0; row < hdr.height && row < d->frame_height &&
                              row < src_rows; row++) {
                    memcpy(d->frame_buf + row * d->frame_stride,
                           src + row * hdr.stride,
                           copy_w);
                }
            } else {
                /* Dirty rect updates — pixel data is per-rect rows concatenated */
                BYTE *src = recv_buf;
                for (i = 0; i < rect_count; i++) {
                    LONG left   = dirty_rects[i].left;
                    LONG top    = dirty_rects[i].top;
                    LONG right  = dirty_rects[i].right;
                    LONG bottom = dirty_rects[i].bottom;
                    UINT rect_w, rect_h, row;
                    UINT rect_row_bytes;

                    /* Clamp to frame bounds */
                    if (left < 0) left = 0;
                    if (top  < 0) top  = 0;
                    if (right  > (LONG)d->frame_width)  right  = (LONG)d->frame_width;
                    if (bottom > (LONG)d->frame_height) bottom = (LONG)d->frame_height;
                    if (left >= right || top >= bottom) continue;

                    rect_w = (UINT)(right - left);
                    rect_h = (UINT)(bottom - top);
                    rect_row_bytes = rect_w * 4;

                    /* Refuse to read past the bytes actually received into recv_buf. */
                    if ((size_t)(src - recv_buf) + (size_t)rect_row_bytes * rect_h >
                        (size_t)data_size)
                        break;

                    for (row = 0; row < rect_h; row++) {
                        UINT dst_y = (UINT)top + row;
                        memcpy(d->frame_buf + dst_y * d->frame_stride + (UINT)left * 4,
                               src + row * rect_row_bytes,
                               rect_row_bytes);
                    }
                    src += rect_row_bytes * rect_h;
                }
            }

            d->frame_dirty = TRUE;
            d->recv_count++;
            InterlockedExchange64(&d->last_guest_frame_sequence, (LONG64)hdr.frame_seq);
            InterlockedIncrement64(&d->received_frame_generation);
            LeaveCriticalSection(&d->frame_cs);
            idd_note_applied_frame(d, hdr.width, hdr.height);

            /* Auto-reset signaling collapses an arbitrary frame burst to one
               render-worker wake without involving the native UI queue. */
            idd_request_render(d, FALSE);

            /* Reconnect input socket if send_input flagged it dead */
            if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
                closesocket(input_s);
                input_s = INVALID_SOCKET;
                idd_log(d, L"Input socket closed, will reconnect...");
            }
            if (input_s == INVALID_SOCKET) {
                GUID svc; hcs_service_guid(d->os_type, 3, &svc);
                SOCKET new_s = connect_to_hv_service(&d->runtime_id, &svc, 1000);
                if (new_s != INVALID_SOCKET) {
                    UINT32 ready_magic = 0;
                    if (recv_exact(new_s, &ready_magic, sizeof(ready_magic)) &&
                        (ready_magic == INPUT_READY_MAGIC ||
                         ready_magic == INPUT_READY_MAGIC_V2)) {
                        DWORD zero_timeout = 0;
                        u_long nb = 1;
                        setsockopt(new_s, SOL_SOCKET, SO_RCVTIMEO, (char *)&zero_timeout, sizeof(zero_timeout));
                        ioctlsocket(new_s, FIONBIO, &nb);
                        input_s = new_s;
                        EnterCriticalSection(&d->frame_cs);
                        d->input_socket = new_s;
                        d->input_frame_size_supported =
                            ready_magic == INPUT_READY_MAGIC_V2;
                        if (d->input_frame_size_supported)
                            send_input(d, INPUT_FRAME_SIZE, d->frame_width, d->frame_height, 0);
                        LeaveCriticalSection(&d->frame_cs);
                        g_input_send_count = 0;
                        idd_log(d, L"Input reconnected + ready (GUID :0003).");
                    } else {
                        closesocket(new_s);
                    }
                }
                /* If connect/handshake fails, will retry next frame */
            }
        }

        /* Frame channel lost — close it but keep input alive */
        closesocket(s);
        idd_log(d, L"Frame channel disconnected, reconnecting...");

        /* Check if input is still alive (send_input may have flagged it dead) */
        if (d->input_socket == INVALID_SOCKET && input_s != INVALID_SOCKET) {
            closesocket(input_s);
            input_s = INVALID_SOCKET;
            idd_log(d, L"Input socket flagged dead, will reconnect.");
        }

        /* Wait before reconnecting frame channel */
        {
            int wait;
            for (wait = 0; wait < 3000 && !d->stop; wait += 500)
                Sleep(500);
        }
    }

    /* Final cleanup — close input socket on thread exit */
    d->input_socket = INVALID_SOCKET;
    if (input_s != INVALID_SOCKET) {
        closesocket(input_s);
    }
    idd_log(d, L"Input disconnected.");

    } /* end input_s scope */

    if (recv_buf)
        HeapFree(GetProcessHeap(), 0, recv_buf);

    WSACleanup();
    return 0;
}

/* ==================================================================
 * Window thread — creates window, initializes D3D11, runs message pump
 * ================================================================== */

static BOOL idd_wait_for_render_start(VmDisplayIdd *d)
{
    DWORD started = GetTickCount();

    for (;;) {
        DWORD elapsed = GetTickCount() - started;
        DWORD remaining;
        DWORD wait_result;

        if (elapsed >= RENDER_START_TIMEOUT_MS)
            return FALSE;
        remaining = RENDER_START_TIMEOUT_MS - elapsed;
        wait_result = MsgWaitForMultipleObjects(
                1, &d->render_ready_event, FALSE, remaining, QS_ALLINPUT);
        if (wait_result == WAIT_OBJECT_0)
            return InterlockedCompareExchange(
                    &d->render_init_result, 0, 0) == 1;
        if (wait_result != WAIT_OBJECT_0 + 1)
            return FALSE;

        /* Swap-chain creation may synchronously message its target HWND. Keep
           this hidden-startup thread dispatching so cross-thread D3D setup can
           finish without giving D3D ownership back to the window thread. */
        {
            MSG msg;
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    PostQuitMessage((int)msg.wParam);
                    return FALSE;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
}

static DWORD WINAPI idd_window_thread_proc(LPVOID param)
{
    VmDisplayIdd *d = (VmDisplayIdd *)param;
    wchar_t title[300];
    MSG msg;

    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ensure_idd_class(d->hInstance);

    if (d->app_mode)
        wcscpy_s(title, 300, d->window_title[0]
                              ? d->window_title : L"Linguum Runtime POC");
    else
        swprintf_s(title, 300, L"%s - IDD Display", d->vm_name);

    /* Compute the outer size from the requested physical client pixels. */
    {
        DWORD style   = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
        DWORD exstyle = d->app_mode ? WS_EX_APPWINDOW : 0;
        RECT wr = { 0, 0, (LONG)d->initial_width, (LONG)d->initial_height };
        UINT dpi = GetDpiForSystem();
        AdjustWindowRectExForDpi(&wr, style, FALSE, exstyle, dpi);

        d->hwnd = CreateWindowExW(
            exstyle, IDD_DISPLAY_CLASS, title, style,
            CW_USEDEFAULT, CW_USEDEFAULT,
            wr.right - wr.left, wr.bottom - wr.top,
            NULL, NULL, d->hInstance, d);
    }

    if (!d->hwnd) {
        ui_log(L"IDD: CreateWindowEx failed (0x%08X)", GetLastError());
        d->open = FALSE;
        return 1;
    }

    idd_set_window_app_id(d->hwnd, d->app_user_model_id);
    if (d->icon_path[0]) {
        d->custom_icon = (HICON)LoadImageW(NULL, d->icon_path, IMAGE_ICON,
                                          0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE);
        if (d->custom_icon) {
            SendMessageW(d->hwnd, WM_SETICON, ICON_BIG, (LPARAM)d->custom_icon);
            SendMessageW(d->hwnd, WM_SETICON, ICON_SMALL, (LPARAM)d->custom_icon);
        }
    }

    /* Dark mode title bar to match AppSandbox main window */
    {
        BOOL dark = TRUE;
        DwmSetWindowAttribute(d->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }

    /* VM/debug controls never appear on an application-mode window. */
    if (!d->app_mode) {
        HMENU sysmenu = GetSystemMenu(d->hwnd, FALSE);
        if (sysmenu) {
            AppendMenuW(sysmenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(sysmenu, MF_STRING, IDM_AUDIO_MUTE, L"Mute audio");
            AppendMenuW(sysmenu, MF_STRING, IDM_XMIT_HOTKEYS, L"Transmit Keyboard Hotkeys");
            AppendMenuW(sysmenu, MF_STRING, IDM_SHOW_LOG, L"Show Log");
            CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                          MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    /* A product application window renders into its top-level client. This
       removes the parent/child resize race that can briefly uncover the
       parent's background during native edge drag. Diagnostic mode retains
       the child HWND because its historical input/debug layout depends on it. */
    if (d->app_mode) {
        d->render_hwnd = d->hwnd;
    } else {
        RECT rc;
        GetClientRect(d->hwnd, &rc);

        d->render_hwnd = CreateWindowExW(
            0, IDD_RENDER_CLASS, NULL,
            WS_CHILD | WS_VISIBLE,
            0, 0, rc.right, rc.bottom,
            d->hwnd, NULL, d->hInstance, NULL);
    }
    if (!d->render_hwnd) {
        ui_log(L"IDD: Failed to establish render target (0x%08X).", GetLastError());
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }
    idd_publish_render_size(d, d->hwnd);

    /* The application gate has no App Sandbox debug-log window. */
    if (d->show_debug_overlay) {
        wchar_t log_title[300];
        HFONT font;
        swprintf_s(log_title, 300, L"%s - IDD Log", d->vm_name);

        /* Created hidden — shown on demand via the "Show Log" system-menu item. */
        d->log_hwnd = CreateWindowExW(
            0, L"AppSandboxIddLog", log_title,
            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
            CW_USEDEFAULT, CW_USEDEFAULT, LOG_WINDOW_W, LOG_WINDOW_H,
            NULL, NULL, d->hInstance, NULL);

        if (d->log_hwnd) {
            /* Dark mode title bar to match AppSandbox main window */
            BOOL dark = TRUE;
            DwmSetWindowAttribute(d->log_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

            /* Fill the log window with a listbox */
            RECT lrc;
            GetClientRect(d->log_hwnd, &lrc);
            d->log_list_hwnd = CreateWindowExW(
                0, L"LISTBOX", NULL,
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_EXTENDEDSEL | LBS_HASSTRINGS,
                0, 0, lrc.right, lrc.bottom,
                d->log_hwnd, (HMENU)(INT_PTR)IDC_LOG_LIST, d->hInstance, NULL);

            /* Subclass the listbox so Ctrl+A / Ctrl+C work when it has focus */
            if (d->log_list_hwnd)
                g_orig_listbox_proc = (WNDPROC)SetWindowLongPtrW(
                    d->log_list_hwnd, GWLP_WNDPROC, (LONG_PTR)idd_log_listbox_proc);

            font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");
            if (font && d->log_list_hwnd)
                SendMessageW(d->log_list_hwnd, WM_SETFONT, (WPARAM)font, TRUE);
        }
        idd_log(d, L"IDD display started.");
    }

    /* The render worker is the sole D3D owner. Wait only during hidden startup;
       after the window is shown the message thread never waits on rendering. */
    d->render_thread = CreateThread(NULL, 0, idd_render_thread_proc, d, 0, NULL);
    if (!d->render_thread || !idd_wait_for_render_start(d)) {
        ui_log(L"IDD: D3D11 render worker initialization failed.");
        d->stop = TRUE;
        if (d->render_event) SetEvent(d->render_event);
        if (d->render_thread) {
            WaitForSingleObject(d->render_thread, RENDER_START_TIMEOUT_MS);
            CloseHandle(d->render_thread);
            d->render_thread = NULL;
        }
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    /* Start the recv thread now that the window and D3D11 are ready */
    d->recv_thread = CreateThread(NULL, 0, idd_recv_thread_proc, d, 0, NULL);
    if (!d->recv_thread) {
        ui_log(L"IDD: Failed to create recv thread.");
        d->stop = TRUE;
        if (d->render_event) SetEvent(d->render_event);
        WaitForSingleObject(d->render_thread, RENDER_START_TIMEOUT_MS);
        CloseHandle(d->render_thread);
        d->render_thread = NULL;
        DestroyWindow(d->hwnd);
        d->hwnd = NULL;
        d->open = FALSE;
        return 1;
    }

    /* Resolve the guest to the exact native client size before revealing the
       application window. Cage may have just restarted at its firmware mode;
       showing the host first exposes that stale frame until a later WM_SIZE.
       The receiver updates last_applied_* independently of this UI thread. */
    idd_update_desired_resize(d, d->hwnd);
    idd_queue_desired_resize(d);
    {
        DWORD started = GetTickCount();
        while (!d->stop && !idd_resize_is_settled(d) &&
               GetTickCount() - started < INITIAL_RESIZE_WAIT_MS)
            Sleep(25);
    }

    /* Bring the correctly sized display window to the foreground on open. */
    ShowWindow(d->hwnd, SW_SHOW);
    BringWindowToTop(d->hwnd);
    SetForegroundWindow(d->hwnd);

    /* Install the hotkey hook on this (message-pumping) thread if the
       persisted setting has Transmit mode enabled. */
    if (d->transmit_hotkeys)
        idd_install_kbd_hook(d);

    /* Message pump */
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}

/* ==================================================================
 * Window procedure
 * ================================================================== */

static LRESULT CALLBACK idd_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    VmDisplayIdd *d;

    if (msg == WM_CREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        AddClipboardFormatListener(hwnd);
        return 0;
    }

    d = (VmDisplayIdd *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_SYSCOMMAND:
        if (d && (wp & 0xFFF0) == IDM_AUDIO_MUTE) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->audio_muted = !d->audio_muted;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_AUDIO_MUTE,
                              MF_BYCOMMAND | (d->audio_muted ? MF_CHECKED : MF_UNCHECKED));
            }
            idd_update_window_title(d);
            idd_log(d, d->audio_muted ? L"Audio muted." : L"Audio unmuted.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_XMIT_HOTKEYS) {
            HMENU sysmenu = GetSystemMenu(hwnd, FALSE);
            d->transmit_hotkeys = !d->transmit_hotkeys;
            if (sysmenu) {
                CheckMenuItem(sysmenu, IDM_XMIT_HOTKEYS,
                              MF_BYCOMMAND | (d->transmit_hotkeys ? MF_CHECKED : MF_UNCHECKED));
            }
            if (d->transmit_hotkeys) {
                idd_install_kbd_hook(d);
            } else {
                idd_remove_kbd_hook(d);
                /* Release anything the guest may be holding from this mode. */
                idd_flush_held_keys(d);
            }
            idd_display_settings_save(d->vhdx_path, d->transmit_hotkeys);
            idd_log(d, d->transmit_hotkeys
                        ? L"Transmit Keyboard Hotkeys: ON."
                        : L"Transmit Keyboard Hotkeys: OFF.");
            return 0;
        }
        if (d && (wp & 0xFFF0) == IDM_SHOW_LOG) {
            /* Reveal the log window (hidden by default). Closing it via its
               own [X] just hides it again (see idd_log_proc WM_CLOSE), so
               this item can re-open it any number of times. */
            if (d->log_hwnd && IsWindow(d->log_hwnd)) {
                if (IsIconic(d->log_hwnd))
                    ShowWindow(d->log_hwnd, SW_RESTORE);
                else
                    ShowWindow(d->log_hwnd, SW_SHOW);
                BringWindowToTop(d->log_hwnd);
                SetForegroundWindow(d->log_hwnd);
            }
            return 0;
        }
        break;

    case WM_CLOSE:
        if (d) {
            BOOL user_initiated = d->open;

            RemoveClipboardFormatListener(hwnd);

            /* Remove the hotkey hook and release any keys still held in the
               guest before tearing the window down. */
            idd_remove_kbd_hook(d);
            idd_flush_held_keys(d);

            /* Stop recv threads */
            d->stop = TRUE;
            KillTimer(hwnd, IDT_RESIZE_DEBOUNCE);
            if (d->resize_event) SetEvent(d->resize_event);
            if (d->render_event) SetEvent(d->render_event);

            if (d->resize_thread) {
                WaitForSingleObject(d->resize_thread, INFINITE);
                CloseHandle(d->resize_thread);
                d->resize_thread = NULL;
            }

            /* Destroy clipboard module */
            if (d->clipboard) {
                vm_clipboard_destroy(d->clipboard);
                d->clipboard = NULL;
            }

            /* Wait for audio recv thread (:0004) */
            if (d->audio_recv_thread) {
                if (d->audio_socket != INVALID_SOCKET) {
                    closesocket(d->audio_socket);
                    d->audio_socket = INVALID_SOCKET;
                }
                WaitForSingleObject(d->audio_recv_thread, 2000);
                CloseHandle(d->audio_recv_thread);
                d->audio_recv_thread = NULL;
            }

            /* Wait briefly for recv thread to exit */
            if (d->recv_thread) {
                WaitForSingleObject(d->recv_thread, 2000);
                CloseHandle(d->recv_thread);
                d->recv_thread = NULL;
            }

            /* All D3D cleanup occurs on its owner thread before either HWND is
               destroyed. This wait is a shutdown-only path; normal move, size,
               paint, input, and close dispatch never execute a D3D call. */
            if (d->render_thread) {
                WaitForSingleObject(d->render_thread,
                                    RENDER_START_TIMEOUT_MS);
                CloseHandle(d->render_thread);
                d->render_thread = NULL;
            }

            d->open = FALSE;

            /* Close the separate log window */
            if (d->log_hwnd && IsWindow(d->log_hwnd))
                DestroyWindow(d->log_hwnd);
            d->log_hwnd = NULL;
            d->log_list_hwnd = NULL;

            /* Clean up guest cursor */
            if (d->guest_cursor) {
                DestroyCursor(d->guest_cursor);
                d->guest_cursor = NULL;
            }
            if (d->custom_icon) {
                DestroyIcon(d->custom_icon);
                d->custom_icon = NULL;
            }

            /* Notify main UI only if user closed the window */
            if (user_initiated && d->main_hwnd && d->vm)
                PostMessageW(d->main_hwnd, WM_VM_DISPLAY_CLOSED,
                             1, (LPARAM)d->vm);
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, IDT_RESIZE_DEBOUNCE);
        if (d) idd_remove_kbd_hook(d);  /* safety net if WM_CLOSE was bypassed */
        if (d) {
            /* Application mode aliases render_hwnd to the top-level HWND. Do
               not leave a destroyed HWND behind for late cursor/focus guards. */
            if (d->render_hwnd == hwnd)
                d->render_hwnd = NULL;
            d->hwnd = NULL;
        }
        PostQuitMessage(0);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        DWORD style   = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
        DWORD exstyle = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
        UINT dpi = GetDpiForWindow(hwnd);
        RECT wr;
        wr.left = 0; wr.top = 0;
        wr.right = d ? (LONG)d->minimum_width : 320;
        wr.bottom = d ? (LONG)d->minimum_height : 180;
        AdjustWindowRectExForDpi(&wr, style, FALSE, exstyle, dpi);
        mmi->ptMinTrackSize.x = wr.right - wr.left;
        mmi->ptMinTrackSize.y = wr.bottom - wr.top;

        /* DefWindowProc seeds this with the monitor work-area limit. The
           virtual display supports larger client sizes, including a full
           2560x1440 client on a 2560x1440 host with decorations off-screen. */
        wr.left = 0; wr.top = 0;
        wr.right = d && d->fixed_backing
                       ? (LONG)d->backing_width : MAX_FRAME_WIDTH;
        wr.bottom = d && d->fixed_backing
                        ? (LONG)d->backing_height : MAX_FRAME_HEIGHT;
        AdjustWindowRectExForDpi(&wr, style, FALSE, exstyle, dpi);
        mmi->ptMaxTrackSize.x = wr.right - wr.left;
        mmi->ptMaxTrackSize.y = wr.bottom - wr.top;
        return 0;
    }

    case WM_DPICHANGED:
    {
        const RECT *suggested = (const RECT *)lp;
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_SIZE:
        if (d) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            if (d->render_hwnd && d->render_hwnd != hwnd)
                SetWindowPos(d->render_hwnd, NULL, 0, 0, rc.right, rc.bottom,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
            if (wp != SIZE_MINIMIZED) {
                idd_publish_render_size(d, hwnd);
                idd_update_desired_resize(d, hwnd);
                /* The render worker owns ResizeBuffers, so publish every native
                   client size to it. Relying on DWM to stretch an old-sized
                   flip-model buffer left parts of the client stale or black on
                   some Windows 11 compositor paths. The auto-reset event and
                   latest-size atomics coalesce a drag burst without blocking
                   this window thread. */
                idd_request_render(d, TRUE);

                /* Legacy application mode keeps the guest output following the
                   native window. Fixed-backing mode publishes the unchanged
                   capacity here, so this latest-value worker naturally no-ops;
                   its application controller changes only logical scene size. */
                idd_queue_desired_resize(d);
                if (!d->in_size_move)
                    SetTimer(hwnd, IDT_RESIZE_DEBOUNCE,
                             RESIZE_DEBOUNCE_MS, NULL);
            }
        }
        return 0;

    case WM_ENTERSIZEMOVE:
        idd_begin_interactive_resize(d, hwnd);
        return 0;

    case WM_EXITSIZEMOVE:
        idd_end_interactive_resize(d, hwnd);
        return 0;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        idd_request_render(d, FALSE);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_RESIZE_DEBOUNCE && d) {
            KillTimer(hwnd, IDT_RESIZE_DEBOUNCE);
            idd_publish_render_size(d, hwnd);
            idd_request_render(d, TRUE);
            idd_queue_desired_resize(d);
        }
        return 0;

    case WM_IDD_FRAME_READY:
        if (d) {
            InterlockedExchange(&d->frame_message_pending, 0);
            idd_request_render(d, FALSE);
        }
        return 0;

    case WM_CLIPBOARDUPDATE:
        if (d && d->clipboard) {
            vm_clipboard_on_clipboard_update(d->clipboard);
        }
        return 0;

    case WM_CLIP_READER_APPLY:
        if (d && d->clipboard) {
            vm_clipboard_on_reader_apply(d->clipboard);
        }
        return 0;

    /* Sent with a bounded timeout by vm_display_idd_focus() from another thread to raise an
       already-open window. Runs on the window's own thread. Restores from
       minimized (the creation path never had to handle that) then brings
       the window forward; SetForegroundWindow succeeds because the user
       just clicked our foreground main window to trigger this. */
    case WM_IDD_FOCUS:
        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);
        else
            ShowWindow(hwnd, SW_SHOW);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        if (d && d->render_hwnd)
            SetFocus(d->render_hwnd);
        return 1;

    /* Posted by the authenticated headless API. Keep all HWND mutation on the
       owning window thread so an unelevated client never needs window access. */
    case WM_IDD_RESIZE:
        if (d) {
            UINT width = (UINT)wp;
            UINT height = (UINT)lp;
            DWORD style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
            DWORD exstyle = (DWORD)GetWindowLongW(hwnd, GWL_EXSTYLE);
            UINT dpi = GetDpiForWindow(hwnd);
            RECT wr = { 0, 0, (LONG)width, (LONG)height };
            RECT client;
            if (width < d->minimum_width || height < d->minimum_height ||
                width > MAX_FRAME_WIDTH || height > MAX_FRAME_HEIGHT)
                return 0;
            if (IsIconic(hwnd) || IsZoomed(hwnd))
                ShowWindow(hwnd, SW_RESTORE);
            if (!AdjustWindowRectExForDpi(&wr, style, FALSE, exstyle, dpi) ||
                !SetWindowPos(hwnd, NULL, 0, 0,
                              wr.right - wr.left, wr.bottom - wr.top,
                              SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) ||
                !GetClientRect(hwnd, &client) ||
                client.right - client.left != (LONG)width ||
                client.bottom - client.top != (LONG)height) {
                ui_log(L"IDD: owned display resize failed (Win32 %lu).",
                       GetLastError());
                return 0;
            }
            return 1;
        }
        return 0;

    case WM_IDD_RESIZE_PHASE:
        if (d && (wp == 0 || wp == 1)) {
            if (wp == 1)
                idd_begin_interactive_resize(d, hwnd);
            else
                idd_end_interactive_resize(d, hwnd);
            return 1;
        }
        return 0;

    case WM_SETFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, TRUE);
        break;

    case WM_KILLFOCUS:
        if (d && d->clipboard)
            vm_clipboard_set_sync_enabled(d->clipboard, FALSE);
        break;

    /* Top-level activation gates keyboard forwarding. Tracking activation
       (not WM_KILLFOCUS) is correct because focus moves between this window
       and its render child without losing activation. Losing activation —
       e.g. the user clicks another window or Alt+Tabs away with a key held —
       flushes held keys so nothing sticks down in the guest. */
    case WM_ACTIVATE:
        if (d) {
            d->input_focused = (LOWORD(wp) != WA_INACTIVE);
            if (!d->input_focused)
                idd_flush_held_keys(d);
        }
        break;

    case WM_ERASEBKGND:
        return 1;  /* We handle all painting via D3D11 */

    /* ---- Mouse tracking (events forwarded from render child) ---- */
    case WM_MOUSEMOVE:
        if (d) {
            if (!d->tracking && d->render_hwnd) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = d->render_hwnd;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
                d->tracking = TRUE;
            }
            d->mouse_in = TRUE;

            {
                UINT vx, vy, frame_width, frame_height;
                EnterCriticalSection(&d->frame_cs);
                frame_width = d->frame_width;
                frame_height = d->frame_height;
                /* lp coords are relative to render child */
                window_to_vm_coords(d->render_hwnd,
                                    (int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
                                    d->app_mode && !d->fixed_backing,
                                    d->app_mode && d->fixed_backing,
                                    frame_width, frame_height, &vx, &vy);
                if (!_wcsicmp(d->os_type, L"Linux") &&
                    !d->input_frame_size_supported) {
                    vx = legacy_linux_input_coordinate(vx, frame_width, DEFAULT_WIDTH);
                    vy = legacy_linux_input_coordinate(vy, frame_height, DEFAULT_HEIGHT);
                }
                LeaveCriticalSection(&d->frame_cs);
                send_input(d, INPUT_MOUSE_MOVE, vx, vy, 0);
            }
        }
        return 0;

    case WM_MOUSELEAVE:
        if (d) {
            d->mouse_in = FALSE;
            d->tracking = FALSE;
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            if (d && d->guest_cursor)
                SetCursor(d->guest_cursor);
            else
                SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;

    /* ---- Mouse button/wheel forwarding (only when cursor is in render area) ---- */
    case WM_LBUTTONDOWN:
        if (d && d->mouse_in) {
            if (d->render_hwnd) SetCapture(d->render_hwnd);
            send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 1, 0);
        }
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_LEFT, 0, 0);
        return 0;

    case WM_RBUTTONDOWN:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 1, 0);
        return 0;
    case WM_RBUTTONUP:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_RIGHT, 0, 0);
        return 0;

    case WM_MBUTTONDOWN:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 1, 0);
        return 0;
    case WM_MBUTTONUP:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_BUTTON, INPUT_BTN_MIDDLE, 0, 0);
        return 0;

    case WM_MOUSEWHEEL:
        if (d && d->mouse_in) send_input(d, INPUT_MOUSE_WHEEL, (UINT32)(INT32)GET_WHEEL_DELTA_WPARAM(wp), 0, 0);
        return 0;

    /* ---- Keyboard input forwarding (gated on window activation) ----
       In Transmit mode reserved hotkeys are captured by the low-level hook
       and never reach here. The mode check below covers Default mode: a
       reserved hotkey is neither forwarded to the guest nor consumed — it
       falls through to DefWindowProc so the host handles it normally (this
       is what avoids the old stuck-key bug). Normal keys are forwarded while
       the window is the active foreground window. */
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
    {
        UINT32 scan = (UINT32)((lp >> 16) & 0xFF);
        BOOL ext = (lp & (1 << 24)) != 0;
        BOOL up  = (msg == WM_KEYUP || msg == WM_SYSKEYUP);
        if (!d) break;
        if (!d->transmit_hotkeys &&
            idd_is_reserved_hotkey((DWORD)wp, (GetKeyState(VK_MENU) & 0x8000) != 0))
            break;  /* Default mode: let the host handle this hotkey. */
        if (d->input_focused)
            idd_forward_key(d, (DWORD)wp, scan, ext, up);
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ==================================================================
 * Public API
 * ================================================================== */

VmDisplayIdd *vm_display_idd_create_ex(VmInstance *vm, HINSTANCE hInstance,
                                       HWND main_hwnd,
                                       const AsbDisplayOptions *options)
{
    VmDisplayIdd *d;

    if (!vm) return NULL;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    d = (VmDisplayIdd *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                   sizeof(VmDisplayIdd));
    if (!d) return NULL;

    d->vm           = vm;
    wcscpy_s(d->vm_name, 256, vm->name);
    wcscpy_s(d->vhdx_path, MAX_PATH, vm->vhdx_path);
    d->runtime_id   = vm->runtime_id;
    wcscpy_s(d->os_type, 32, vm->os_type);
    d->hInstance    = hInstance;
    d->main_hwnd   = main_hwnd;
    d->open         = TRUE;
    d->stop         = FALSE;
    d->input_socket       = INVALID_SOCKET;
    d->audio_socket       = INVALID_SOCKET;
    d->clipboard          = NULL;
    d->app_mode           = options ? options->app_mode : FALSE;
    d->initial_width      = options && options->initial_width
                                ? options->initial_width : DEFAULT_WIDTH;
    d->initial_height     = options && options->initial_height
                                ? options->initial_height : DEFAULT_HEIGHT;
    d->backing_width      = options && options->backing_width
                                ? options->backing_width : d->initial_width;
    d->backing_height     = options && options->backing_height
                                ? options->backing_height : d->initial_height;
    d->fixed_backing      = d->app_mode &&
                            (d->backing_width != d->initial_width ||
                             d->backing_height != d->initial_height);
    d->minimum_width      = options && options->minimum_width
                                ? options->minimum_width : 320;
    d->minimum_height     = options && options->minimum_height
                                ? options->minimum_height : 180;
    d->show_debug_title   = options ? options->show_debug_title : TRUE;
    d->show_debug_overlay = options ? options->show_debug_overlay : TRUE;
    d->desired_render_width = (LONG)d->initial_width;
    d->desired_render_height = (LONG)d->initial_height;
    if (options && options->window_title)
        wcsncpy_s(d->window_title, 256, options->window_title, _TRUNCATE);
    if (options && options->app_user_model_id)
        wcsncpy_s(d->app_user_model_id, 256,
                  options->app_user_model_id, _TRUNCATE);
    if (options && options->icon_path)
        wcsncpy_s(d->icon_path, MAX_PATH, options->icon_path, _TRUNCATE);

    /* Load the per-VM display setting, creating display_settings.json with
       the default (off) if this VM doesn't have one yet. The hook itself is
       installed later, on the window thread, once the window exists. */
    d->transmit_hotkeys = idd_display_settings_load_or_create(vm->vhdx_path);
    if (d->app_mode)
        d->transmit_hotkeys = FALSE;

    /* Initialize frame buffer at default resolution */
    d->frame_width  = DEFAULT_WIDTH;
    d->frame_height = DEFAULT_HEIGHT;
    d->frame_stride = DEFAULT_WIDTH * 4;
    d->frame_buf    = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                         d->frame_stride * d->frame_height);
    if (!d->frame_buf) {
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    InitializeCriticalSection(&d->frame_cs);
    InitializeCriticalSection(&d->input_send_cs);
    InitializeCriticalSection(&d->resize_cs);
    InitializeCriticalSection(&d->agent_command_cs);

    d->resize_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    d->render_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    d->render_ready_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!d->resize_event || !d->render_event || !d->render_ready_event) {
        if (d->render_ready_event) CloseHandle(d->render_ready_event);
        if (d->render_event) CloseHandle(d->render_event);
        if (d->resize_event) CloseHandle(d->resize_event);
        DeleteCriticalSection(&d->agent_command_cs);
        DeleteCriticalSection(&d->resize_cs);
        DeleteCriticalSection(&d->input_send_cs);
        DeleteCriticalSection(&d->frame_cs);
        HeapFree(GetProcessHeap(), 0, d->frame_buf);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    d->resize_thread = CreateThread(NULL, 0, idd_resize_thread_proc, d, 0, NULL);
    if (!d->resize_thread) {
        CloseHandle(d->render_ready_event);
        CloseHandle(d->render_event);
        CloseHandle(d->resize_event);
        DeleteCriticalSection(&d->agent_command_cs);
        DeleteCriticalSection(&d->resize_cs);
        DeleteCriticalSection(&d->input_send_cs);
        DeleteCriticalSection(&d->frame_cs);
        HeapFree(GetProcessHeap(), 0, d->frame_buf);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    /* Start the window thread (which will then start the recv thread) */
    d->window_thread = CreateThread(NULL, 0, idd_window_thread_proc, d, 0, NULL);
    if (!d->window_thread) {
        ui_log(L"IDD: Failed to create window thread.");
        d->stop = TRUE;
        SetEvent(d->resize_event);
        WaitForSingleObject(d->resize_thread, INFINITE);
        CloseHandle(d->resize_thread);
        CloseHandle(d->render_ready_event);
        CloseHandle(d->render_event);
        CloseHandle(d->resize_event);
        DeleteCriticalSection(&d->agent_command_cs);
        DeleteCriticalSection(&d->resize_cs);
        DeleteCriticalSection(&d->input_send_cs);
        DeleteCriticalSection(&d->frame_cs);
        HeapFree(GetProcessHeap(), 0, d->frame_buf);
        HeapFree(GetProcessHeap(), 0, d);
        return NULL;
    }

    return d;
}

VmDisplayIdd *vm_display_idd_create(VmInstance *vm, HINSTANCE hInstance,
                                    HWND main_hwnd)
{
    return vm_display_idd_create_ex(vm, hInstance, main_hwnd, NULL);
}

void vm_display_idd_destroy(VmDisplayIdd *display)
{
    if (!display) return;

    /* Signal stop */
    display->stop = TRUE;
    display->open = FALSE;
    if (display->resize_event) SetEvent(display->resize_event);
    if (display->render_event) SetEvent(display->render_event);

    /* Close the window to unblock the message pump */
    if (display->hwnd && IsWindow(display->hwnd))
        PostMessageW(display->hwnd, WM_CLOSE, 0, 0);

    /* Wait for window thread (pumping messages to stay responsive) */
    if (display->window_thread) {
        DWORD result;
        do {
            result = MsgWaitForMultipleObjects(
                1, &display->window_thread, FALSE, 5000, QS_ALLINPUT);
            if (result == WAIT_OBJECT_0 + 1) {
                MSG msg;
                while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
            }
        } while (result == WAIT_OBJECT_0 + 1);
        CloseHandle(display->window_thread);
    }

    /* recv_thread is cleaned up by WM_CLOSE handler, but guard just in case */
    if (display->recv_thread) {
        WaitForSingleObject(display->recv_thread, 3000);
        CloseHandle(display->recv_thread);
        display->recv_thread = NULL;
    }

    if (display->render_thread) {
        WaitForSingleObject(display->render_thread,
                            RENDER_START_TIMEOUT_MS);
        CloseHandle(display->render_thread);
        display->render_thread = NULL;
    }

    if (display->resize_thread) {
        WaitForSingleObject(display->resize_thread, INFINITE);
        CloseHandle(display->resize_thread);
        display->resize_thread = NULL;
    }

    if (display->resize_event) {
        CloseHandle(display->resize_event);
        display->resize_event = NULL;
    }
    if (display->render_event) {
        CloseHandle(display->render_event);
        display->render_event = NULL;
    }
    if (display->render_ready_event) {
        CloseHandle(display->render_ready_event);
        display->render_ready_event = NULL;
    }

    /* Clipboard is cleaned up by WM_CLOSE handler, but guard */
    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    /* audio_recv_thread guard */
    if (display->audio_recv_thread) {
        if (display->audio_socket != INVALID_SOCKET) {
            closesocket(display->audio_socket);
            display->audio_socket = INVALID_SOCKET;
        }
        WaitForSingleObject(display->audio_recv_thread, 3000);
        CloseHandle(display->audio_recv_thread);
    }

    DeleteCriticalSection(&display->agent_command_cs);
    DeleteCriticalSection(&display->resize_cs);
    DeleteCriticalSection(&display->input_send_cs);
    DeleteCriticalSection(&display->frame_cs);

    if (display->clipboard) {
        vm_clipboard_destroy(display->clipboard);
        display->clipboard = NULL;
    }

    if (display->frame_buf)
        HeapFree(GetProcessHeap(), 0, display->frame_buf);

    if (display->custom_icon)
        DestroyIcon(display->custom_icon);

    HeapFree(GetProcessHeap(), 0, display);
}

BOOL vm_display_idd_is_open(VmDisplayIdd *display)
{
    if (!display) return FALSE;
    /* open is set before the window thread starts and cleared on every close or
       creation failure. Treat that short startup interval as open so status
       polling cannot reap and free this context before the thread publishes
       its HWND. */
    return display->open;
}

BOOL vm_display_idd_focus(VmDisplayIdd *display)
{
    DWORD_PTR applied = 0;
    if (!display || !display->open ||
        !display->hwnd || !IsWindow(display->hwnd))
        return FALSE;
    /* Wait only for the owning UI thread to apply activation. This does not
       involve D3D, guest I/O, or the render worker and closes the race where
       an authenticated pointer transaction followed an unprocessed focus
       PostMessage. */
    return SendMessageTimeoutW(display->hwnd, WM_IDD_FOCUS, 0, 0,
                               SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
                               1000, &applied) != 0 && applied == 1;
}

BOOL vm_display_idd_resize(VmDisplayIdd *display, UINT width, UINT height)
{
    DWORD_PTR applied = 0;
    if (!display || !display->open ||
        !display->hwnd || !IsWindow(display->hwnd) ||
        width < display->minimum_width || height < display->minimum_height ||
        width > MAX_FRAME_WIDTH || height > MAX_FRAME_HEIGHT ||
        (display->fixed_backing &&
         (width > display->backing_width || height > display->backing_height)))
        return FALSE;
    return SendMessageTimeoutW(display->hwnd, WM_IDD_RESIZE,
                               (WPARAM)width, (LPARAM)height,
                               SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
                               5000, &applied) != 0 && applied == 1;
}

BOOL vm_display_idd_set_resize_phase(VmDisplayIdd *display, BOOL active)
{
    DWORD_PTR applied = 0;
    if (!display || !display->open ||
        !display->hwnd || !IsWindow(display->hwnd))
        return FALSE;
    return SendMessageTimeoutW(display->hwnd, WM_IDD_RESIZE_PHASE,
                               active ? 1 : 0, 0,
                               SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT,
                               5000, &applied) != 0 && applied == 1;
}

BOOL vm_display_idd_get_runtime_state(VmDisplayIdd *display,
                                      AsbDisplayRuntimeState *state)
{
    if (!display || !state || !display->open || display->stop)
        return FALSE;

    ZeroMemory(state, sizeof(*state));
    state->received_frames = (UINT64)InterlockedCompareExchange64(
            &display->received_frame_generation, 0, 0);
    state->presented_frames = (UINT64)InterlockedCompareExchange64(
            &display->presented_frame_generation, 0, 0);
    state->present_count = (UINT64)InterlockedCompareExchange64(
            &display->present_count, 0, 0);
    state->guest_frame_sequence = (UINT64)InterlockedCompareExchange64(
            &display->last_guest_frame_sequence, 0, 0);
    state->render_width = (UINT)InterlockedCompareExchange(
            &display->presented_render_width, 0, 0);
    state->render_height = (UINT)InterlockedCompareExchange(
            &display->presented_render_height, 0, 0);
    EnterCriticalSection(&display->frame_cs);
    state->frame_width = display->frame_width;
    state->frame_height = display->frame_height;
    LeaveCriticalSection(&display->frame_cs);
    return TRUE;
}
