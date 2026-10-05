#include "device_profile.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int
display_size_valid(int width, int height)
{
    return width >= VNC_DEVICE_DISPLAY_MIN &&
           height >= VNC_DEVICE_DISPLAY_MIN &&
           width <= VNC_DEVICE_DISPLAY_MAX &&
           height <= VNC_DEVICE_DISPLAY_MAX;
}

int
device_profile_id_valid(const char *device_id)
{
    if (!device_id || strlen(device_id) != VNC_DEVICE_ID_HEX_LEN)
        return 0;

    for (size_t i = 0; i < VNC_DEVICE_ID_HEX_LEN; i++) {
        if (!g_ascii_isxdigit((guchar)device_id[i]))
            return 0;
    }

    return 1;
}

const char *
device_display_mode_name(DeviceDisplayMode mode)
{
    return mode == DEVICE_DISPLAY_FULLSCREEN ? "fullscreen" : "window";
}

const char *
device_orientation_name(DeviceOrientation orientation)
{
    return orientation == DEVICE_ORIENTATION_LANDSCAPE ?
        "landscape" : "portrait";
}

static int
mode_valid(DeviceDisplayMode mode)
{
    return mode == DEVICE_DISPLAY_WINDOW ||
           mode == DEVICE_DISPLAY_FULLSCREEN;
}

static int
orientation_valid(DeviceOrientation orientation)
{
    return orientation == DEVICE_ORIENTATION_PORTRAIT ||
           orientation == DEVICE_ORIENTATION_LANDSCAPE;
}

static void
load_size_group(GKeyFile *keyfile,
                DeviceProfile *profile,
                DeviceDisplayMode mode,
                DeviceOrientation orientation)
{
    const char *mode_name = device_display_mode_name(mode);
    const char *orientation_name = device_orientation_name(orientation);
    char *group = g_strdup_printf("%s.%s", mode_name, orientation_name);

    GError *error = NULL;
    int width = g_key_file_get_integer(keyfile, group, "width", &error);
    if (error) {
        g_clear_error(&error);
        g_free(group);
        return;
    }

    int height = g_key_file_get_integer(keyfile, group, "height", &error);
    if (error || !display_size_valid(width, height)) {
        g_clear_error(&error);
        g_free(group);
        return;
    }

    profile->sizes[mode][orientation].valid = 1;
    profile->sizes[mode][orientation].width = width;
    profile->sizes[mode][orientation].height = height;
    g_free(group);
}

static int
parse_mode(const char *value, DeviceDisplayMode *out)
{
    if (!value || !out)
        return -1;
    if (strcmp(value, "window") == 0) {
        *out = DEVICE_DISPLAY_WINDOW;
        return 0;
    }
    if (strcmp(value, "fullscreen") == 0) {
        *out = DEVICE_DISPLAY_FULLSCREEN;
        return 0;
    }
    return -1;
}

static int
parse_orientation(const char *value, DeviceOrientation *out)
{
    if (!value || !out)
        return -1;
    if (strcmp(value, "portrait") == 0) {
        *out = DEVICE_ORIENTATION_PORTRAIT;
        return 0;
    }
    if (strcmp(value, "landscape") == 0) {
        *out = DEVICE_ORIENTATION_LANDSCAPE;
        return 0;
    }
    return -1;
}

int
device_profile_load(DeviceProfile *profile,
                    const char *device_id,
                    int fallback_width,
                    int fallback_height)
{
    if (!profile || !device_profile_id_valid(device_id) ||
        !display_size_valid(fallback_width, fallback_height)) {
        errno = EINVAL;
        return -1;
    }

    memset(profile, 0, sizeof(*profile));
    g_strlcpy(profile->id, device_id, sizeof(profile->id));
    profile->last_mode = DEVICE_DISPLAY_WINDOW;
    profile->last_orientation =
        fallback_width >= fallback_height ?
            DEVICE_ORIENTATION_LANDSCAPE :
            DEVICE_ORIENTATION_PORTRAIT;

    DeviceDisplaySize *fallback =
        &profile->sizes[profile->last_mode][profile->last_orientation];
    fallback->valid = 1;
    fallback->width = fallback_width;
    fallback->height = fallback_height;

    snprintf(profile->name, sizeof(profile->name),
             "Browser %.8s", device_id);

    const char *config_dir = g_get_user_config_dir();
    char *root = g_build_filename(config_dir, "vnc-monitor-server", NULL);
    char *devices = g_build_filename(root, "devices", NULL);

    if (g_mkdir_with_parents(devices, 0700) < 0) {
        int saved = errno;
        g_free(devices);
        g_free(root);
        errno = saved;
        device_profile_clear(profile);
        return -1;
    }
    (void)g_chmod(root, 0700);
    (void)g_chmod(devices, 0700);

    char *filename = g_strdup_printf("%s.ini", device_id);
    profile->path = g_build_filename(devices, filename, NULL);
    g_free(filename);
    g_free(devices);
    g_free(root);

    if (!g_file_test(profile->path, G_FILE_TEST_IS_REGULAR))
        return 0;

    GKeyFile *keyfile = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_file(keyfile,
                                   profile->path,
                                   G_KEY_FILE_NONE,
                                   &error)) {
        fprintf(stderr, "Cannot read device profile %s: %s\n",
                profile->path,
                error ? error->message : "unknown error");
        g_clear_error(&error);
        g_key_file_unref(keyfile);
        device_profile_clear(profile);
        return -1;
    }

    char *stored_id = g_key_file_get_string(keyfile, "device", "id", &error);
    if (error || !stored_id || strcmp(stored_id, device_id) != 0) {
        fprintf(stderr, "Invalid device profile identity: %s\n", profile->path);
        g_clear_error(&error);
        g_free(stored_id);
        g_key_file_unref(keyfile);
        device_profile_clear(profile);
        errno = EPROTO;
        return -1;
    }
    g_free(stored_id);

    char *name = g_key_file_get_string(keyfile, "device", "name", NULL);
    if (name && *name && strlen(name) <= VNC_DEVICE_NAME_MAX)
        g_strlcpy(profile->name, name, sizeof(profile->name));
    g_free(name);

    char *mode = g_key_file_get_string(keyfile, "device", "last-mode", NULL);
    DeviceDisplayMode parsed_mode;
    if (parse_mode(mode, &parsed_mode) == 0)
        profile->last_mode = parsed_mode;
    g_free(mode);

    char *orientation =
        g_key_file_get_string(keyfile, "device", "last-orientation", NULL);
    DeviceOrientation parsed_orientation;
    if (parse_orientation(orientation, &parsed_orientation) == 0)
        profile->last_orientation = parsed_orientation;
    g_free(orientation);

    for (int m = DEVICE_DISPLAY_WINDOW;
         m <= DEVICE_DISPLAY_FULLSCREEN;
         m++) {
        for (int o = DEVICE_ORIENTATION_PORTRAIT;
             o <= DEVICE_ORIENTATION_LANDSCAPE;
             o++) {
            load_size_group(keyfile, profile,
                            (DeviceDisplayMode)m,
                            (DeviceOrientation)o);
        }
    }

    profile->existed = 1;
    g_key_file_unref(keyfile);
    return 0;
}

int
device_profile_get_size(const DeviceProfile *profile,
                        DeviceDisplayMode mode,
                        DeviceOrientation orientation,
                        int *width,
                        int *height)
{
    if (!profile || !mode_valid(mode) || !orientation_valid(orientation) ||
        !width || !height)
        return -1;

    const DeviceDisplaySize *size = &profile->sizes[mode][orientation];
    if (!size->valid)
        return 1;

    *width = size->width;
    *height = size->height;
    return 0;
}

int
device_profile_update_state(DeviceProfile *profile,
                            DeviceDisplayMode mode,
                            DeviceOrientation orientation,
                            int width,
                            int height)
{
    if (!profile || !mode_valid(mode) || !orientation_valid(orientation) ||
        !display_size_valid(width, height)) {
        errno = EINVAL;
        return -1;
    }

    DeviceDisplaySize *size = &profile->sizes[mode][orientation];
    size->valid = 1;
    size->width = width;
    size->height = height;
    profile->last_mode = mode;
    profile->last_orientation = orientation;
    return 0;
}

int
device_profile_save(DeviceProfile *profile)
{
    if (!profile || !device_profile_id_valid(profile->id) || !profile->path) {
        errno = EINVAL;
        return -1;
    }

    GKeyFile *keyfile = g_key_file_new();
    g_key_file_set_string(keyfile, "device", "id", profile->id);
    g_key_file_set_string(keyfile, "device", "name", profile->name);
    g_key_file_set_string(keyfile, "device", "last-mode",
                          device_display_mode_name(profile->last_mode));
    g_key_file_set_string(keyfile, "device", "last-orientation",
                          device_orientation_name(profile->last_orientation));

    for (int m = DEVICE_DISPLAY_WINDOW;
         m <= DEVICE_DISPLAY_FULLSCREEN;
         m++) {
        for (int o = DEVICE_ORIENTATION_PORTRAIT;
             o <= DEVICE_ORIENTATION_LANDSCAPE;
             o++) {
            DeviceDisplaySize *size = &profile->sizes[m][o];
            if (!size->valid)
                continue;

            char *group =
                g_strdup_printf("%s.%s",
                                device_display_mode_name((DeviceDisplayMode)m),
                                device_orientation_name((DeviceOrientation)o));
            g_key_file_set_integer(keyfile, group, "width", size->width);
            g_key_file_set_integer(keyfile, group, "height", size->height);
            g_free(group);
        }
    }

    gsize length = 0;
    GError *error = NULL;
    char *data = g_key_file_to_data(keyfile, &length, &error);
    g_key_file_unref(keyfile);
    if (!data) {
        g_clear_error(&error);
        return -1;
    }

    gboolean ok = g_file_set_contents(profile->path, data, (gssize)length, &error);
    g_free(data);
    if (!ok) {
        fprintf(stderr, "Cannot save device profile %s: %s\n",
                profile->path,
                error ? error->message : "unknown error");
        g_clear_error(&error);
        return -1;
    }

    if (g_chmod(profile->path, 0600) < 0)
        return -1;

    profile->existed = 1;
    return 0;
}

int
device_profile_layout_scope(const DeviceProfile *profile,
                            DeviceDisplayMode mode,
                            DeviceOrientation orientation,
                            char *out,
                            size_t out_size)
{
    if (!profile || !device_profile_id_valid(profile->id) ||
        !mode_valid(mode) || !orientation_valid(orientation) ||
        !out || out_size == 0) {
        errno = EINVAL;
        return -1;
    }

    int n = snprintf(out, out_size,
                     "device-%s-%s-%s",
                     profile->id,
                     device_display_mode_name(mode),
                     device_orientation_name(orientation));
    if (n < 0 || (size_t)n >= out_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

void
device_profile_clear(DeviceProfile *profile)
{
    if (!profile)
        return;

    g_clear_pointer(&profile->path, g_free);
    memset(profile, 0, sizeof(*profile));
}
