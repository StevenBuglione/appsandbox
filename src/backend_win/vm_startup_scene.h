#ifndef VM_STARTUP_SCENE_H
#define VM_STARTUP_SCENE_H

#include <windows.h>

typedef enum AsbStartupPhase {
    ASB_STARTUP_DISABLED = 0,
    ASB_STARTUP_OPENING = 1,
    ASB_STARTUP_PREPARING = 2,
    ASB_STARTUP_FINISHING = 3,
    ASB_STARTUP_READY = 4,
    ASB_STARTUP_FAILED = 5
} AsbStartupPhase;

typedef enum AsbStartupPosition {
    ASB_STARTUP_BOTTOM_LEFT = 0,
    ASB_STARTUP_CENTER = 1
} AsbStartupPosition;

typedef enum AsbStartupMotion {
    ASB_STARTUP_MOTION_GLYPH = 0,
    ASB_STARTUP_MOTION_NONE = 1
} AsbStartupMotion;

typedef struct AsbStartupOptions {
    BOOL enabled;
    BOOL auto_ready;
    BOOL detailed;
    AsbStartupPosition position;
    AsbStartupMotion motion;
    COLORREF background_color;
    COLORREF foreground_color;
    COLORREF accent_color;
    UINT delayed_message_after_ms;
    const wchar_t *app_name;
    const wchar_t *opening_message;
    const wchar_t *preparing_message;
    const wchar_t *finishing_message;
    const wchar_t *delayed_message;
    const wchar_t *failed_message;
    const wchar_t *failure_detail;
    const wchar_t *mark_path;
} AsbStartupOptions;

typedef struct VmStartupScene VmStartupScene;

typedef struct VmStartupSceneFrame {
    const BYTE *pixels;
    UINT width;
    UINT height;
    UINT stride;
    RECT dirty;
    BOOL full_dirty;
    BOOL updated;
} VmStartupSceneFrame;

VmStartupScene *vm_startup_scene_create(const AsbStartupOptions *options);
void vm_startup_scene_destroy(VmStartupScene *scene);

/* Called only by the render worker. It owns the backing DIB and returns either
   a full update or a small status-strip update for the animated glyph. */
BOOL vm_startup_scene_render(VmStartupScene *scene,
                             UINT width,
                             UINT height,
                             UINT dpi,
                             AsbStartupPhase phase,
                             BOOL detailed,
                             ULONGLONG now_ms,
                             VmStartupSceneFrame *frame);

BOOL vm_startup_scene_is_animated(const VmStartupScene *scene);

#endif /* VM_STARTUP_SCENE_H */
