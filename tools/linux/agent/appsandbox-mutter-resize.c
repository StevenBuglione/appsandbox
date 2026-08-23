/*
 * appsandbox-mutter-resize.c — apply one virtual KMS mode to the active
 * GNOME/Mutter session.
 *
 * asb_drm publishes a replacement preferred mode and emits a KMS hotplug.
 * Mutter notices the connector change but does not reliably switch to the
 * replacement mode after repeated hotplugs, so the root agent launches this
 * one-shot helper as the logged-in desktop user. It uses Mutter's existing
 * DisplayConfig interface and changes no persistent desktop configuration.
 */

#include <gio/gio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MUTTER_BUS_NAME "org.gnome.Mutter.DisplayConfig"
#define MUTTER_OBJECT_PATH "/org/gnome/Mutter/DisplayConfig"
#define MUTTER_INTERFACE "org.gnome.Mutter.DisplayConfig"
#define APPLY_TEMPORARY 1u
#define STATE_RETRY_COUNT 30
#define STATE_RETRY_DELAY_US (50 * 1000)
#define DBUS_TIMEOUT_MS 2000

typedef struct {
    guint32 serial;
    char *connector;
    char *mode_id;
    double scale;
    guint32 transform;
    guint32 layout_mode;
    gboolean has_layout_mode;
} TargetState;

static gboolean scale_is_supported(GVariant *mode, double scale)
{
    GVariant *scales = g_variant_get_child_value(mode, 5);
    GVariantIter iter;
    double candidate;
    gboolean supported = FALSE;

    g_variant_iter_init(&iter, scales);
    while (g_variant_iter_next(&iter, "d", &candidate)) {
        if (fabs(candidate - scale) < 0.001) {
            supported = TRUE;
            break;
        }
    }
    g_variant_unref(scales);
    return supported;
}

static gboolean find_target_state(GVariant *reply, int width, int height,
                                  TargetState *target)
{
    GVariant *monitors = NULL;
    GVariant *logical_monitors = NULL;
    GVariant *properties = NULL;
    double current_scale = 1.0;
    guint32 current_transform = 0;
    gsize monitor_count;
    gboolean found = FALSE;

    g_variant_get_child(reply, 0, "u", &target->serial);
    monitors = g_variant_get_child_value(reply, 1);
    logical_monitors = g_variant_get_child_value(reply, 2);
    properties = g_variant_get_child_value(reply, 3);

    if (g_variant_n_children(logical_monitors) > 0) {
        GVariant *logical = g_variant_get_child_value(logical_monitors, 0);
        g_variant_get_child(logical, 2, "d", &current_scale);
        g_variant_get_child(logical, 3, "u", &current_transform);
        g_variant_unref(logical);
    }

    monitor_count = g_variant_n_children(monitors);
    for (gsize monitor_index = 0; monitor_index < monitor_count && !found;
         monitor_index++) {
        GVariant *monitor = g_variant_get_child_value(monitors, monitor_index);
        GVariant *spec = g_variant_get_child_value(monitor, 0);
        GVariant *modes = g_variant_get_child_value(monitor, 1);
        const char *connector = NULL;
        const char *unused_vendor = NULL;
        const char *unused_product = NULL;
        const char *unused_serial = NULL;
        gsize mode_count;

        g_variant_get(spec, "(&s&s&s&s)", &connector, &unused_vendor,
                      &unused_product, &unused_serial);
        mode_count = g_variant_n_children(modes);

        for (gsize mode_index = 0; mode_index < mode_count; mode_index++) {
            GVariant *mode = g_variant_get_child_value(modes, mode_index);
            const char *mode_id = NULL;
            int mode_width = 0;
            int mode_height = 0;
            double preferred_scale = 1.0;

            g_variant_get_child(mode, 0, "&s", &mode_id);
            g_variant_get_child(mode, 1, "i", &mode_width);
            g_variant_get_child(mode, 2, "i", &mode_height);
            g_variant_get_child(mode, 4, "d", &preferred_scale);
            if (mode_width == width && mode_height == height) {
                target->connector = g_strdup(connector);
                target->mode_id = g_strdup(mode_id);
                target->scale = scale_is_supported(mode, current_scale)
                                    ? current_scale
                                    : preferred_scale;
                target->transform = current_transform;
                found = TRUE;
            }
            g_variant_unref(mode);
            if (found)
                break;
        }

        g_variant_unref(modes);
        g_variant_unref(spec);
        g_variant_unref(monitor);
    }

    {
        gboolean supports_layout_mode = FALSE;
        target->has_layout_mode =
            g_variant_lookup(properties, "supports-changing-layout-mode", "b",
                             &supports_layout_mode) &&
            supports_layout_mode &&
            g_variant_lookup(properties, "layout-mode", "u",
                             &target->layout_mode);
    }
    g_variant_unref(properties);
    g_variant_unref(logical_monitors);
    g_variant_unref(monitors);
    return found;
}

static GVariant *get_current_state(GDBusConnection *connection, GError **error)
{
    return g_dbus_connection_call_sync(
        connection, MUTTER_BUS_NAME, MUTTER_OBJECT_PATH, MUTTER_INTERFACE,
        "GetCurrentState", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
        DBUS_TIMEOUT_MS, NULL, error);
}

static gboolean apply_target(GDBusConnection *connection,
                             const TargetState *target, GError **error)
{
    GVariantBuilder monitor_configs;
    GVariantBuilder monitor_properties;
    GVariantBuilder logical_monitors;
    GVariantBuilder global_properties;
    GVariant *parameters;
    GVariant *reply;

    g_variant_builder_init(&monitor_properties, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_init(&monitor_configs, G_VARIANT_TYPE("a(ssa{sv})"));
    g_variant_builder_add(&monitor_configs, "(ss@a{sv})",
                          target->connector, target->mode_id,
                          g_variant_builder_end(&monitor_properties));

    g_variant_builder_init(&logical_monitors,
                           G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
    g_variant_builder_add(&logical_monitors, "(iidub@a(ssa{sv}))",
                          0, 0, target->scale, target->transform, TRUE,
                          g_variant_builder_end(&monitor_configs));

    g_variant_builder_init(&global_properties, G_VARIANT_TYPE("a{sv}"));
    if (target->has_layout_mode) {
        g_variant_builder_add(&global_properties, "{sv}", "layout-mode",
                              g_variant_new_uint32(target->layout_mode));
    }

    parameters = g_variant_new(
        "(uu@a(iiduba(ssa{sv}))@a{sv})", target->serial, APPLY_TEMPORARY,
        g_variant_builder_end(&logical_monitors),
        g_variant_builder_end(&global_properties));
    reply = g_dbus_connection_call_sync(
        connection, MUTTER_BUS_NAME, MUTTER_OBJECT_PATH, MUTTER_INTERFACE,
        "ApplyMonitorsConfig", parameters, G_VARIANT_TYPE("()"),
        G_DBUS_CALL_FLAGS_NONE, DBUS_TIMEOUT_MS, NULL, error);
    if (!reply)
        return FALSE;
    g_variant_unref(reply);
    return TRUE;
}

int main(int argc, char **argv)
{
    char *width_end = NULL;
    char *height_end = NULL;
    long width;
    long height;
    GDBusConnection *connection;
    GError *error = NULL;
    int exit_code = 2;

    if (argc != 3) {
        fprintf(stderr, "usage: %s <width> <height>\n", argv[0]);
        return 2;
    }
    width = strtol(argv[1], &width_end, 10);
    height = strtol(argv[2], &height_end, 10);
    if (!width_end || *width_end != '\0' || !height_end || *height_end != '\0' ||
        width < 64 || width > 7680 || height < 64 || height > 4320) {
        fprintf(stderr, "invalid mode dimensions\n");
        return 2;
    }

    connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!connection) {
        fprintf(stderr, "session bus unavailable: %s\n", error->message);
        g_error_free(error);
        return 3;
    }

    for (int attempt = 0; attempt < STATE_RETRY_COUNT; attempt++) {
        TargetState target = {0};
        GVariant *state;

        g_clear_error(&error);
        state = get_current_state(connection, &error);
        if (!state) {
            fprintf(stderr, "GetCurrentState failed: %s\n", error->message);
            exit_code = 4;
            break;
        }

        if (!find_target_state(state, (int)width, (int)height, &target)) {
            g_variant_unref(state);
            if (attempt + 1 < STATE_RETRY_COUNT) {
                g_usleep(STATE_RETRY_DELAY_US);
                continue;
            }
            fprintf(stderr, "Mutter did not publish mode %ldx%ld\n", width,
                    height);
            exit_code = 5;
            break;
        }
        g_variant_unref(state);

        g_clear_error(&error);
        if (apply_target(connection, &target, &error)) {
            printf("applied %s mode %s (%ldx%ld)\n", target.connector,
                   target.mode_id, width, height);
            exit_code = 0;
            g_free(target.mode_id);
            g_free(target.connector);
            break;
        }

        fprintf(stderr, "ApplyMonitorsConfig failed: %s\n", error->message);
        g_free(target.mode_id);
        g_free(target.connector);
        exit_code = 6;
        if (attempt + 1 < STATE_RETRY_COUNT)
            g_usleep(STATE_RETRY_DELAY_US);
    }

    g_clear_error(&error);
    g_object_unref(connection);
    return exit_code;
}
