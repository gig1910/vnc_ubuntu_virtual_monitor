#ifndef VNC_MONITOR_DEVICE_PROFILE_H
#define VNC_MONITOR_DEVICE_PROFILE_H

#include <stddef.h>

#define VNC_DEVICE_ID_HEX_LEN 64
#define VNC_DEVICE_NAME_MAX 63
#define VNC_DEVICE_DISPLAY_MIN 64
#define VNC_DEVICE_DISPLAY_MAX 4096

typedef enum {
    DEVICE_DISPLAY_WINDOW = 0,
    DEVICE_DISPLAY_FULLSCREEN = 1
} DeviceDisplayMode;

typedef enum {
    DEVICE_ORIENTATION_PORTRAIT = 0,
    DEVICE_ORIENTATION_LANDSCAPE = 1
} DeviceOrientation;

typedef struct {
    int valid;
    int width;
    int height;
} DeviceDisplaySize;

typedef struct {
    char id[VNC_DEVICE_ID_HEX_LEN + 1];
    char name[VNC_DEVICE_NAME_MAX + 1];
    char *path;
    int existed;
    DeviceDisplayMode last_mode;
    DeviceOrientation last_orientation;
    DeviceDisplaySize sizes[2][2];
} DeviceProfile;

int device_profile_id_valid(const char *device_id);

const char *device_display_mode_name(DeviceDisplayMode mode);
const char *device_orientation_name(DeviceOrientation orientation);

int device_profile_load(DeviceProfile *profile,
                        const char *device_id,
                        int fallback_width,
                        int fallback_height);

int device_profile_get_size(const DeviceProfile *profile,
                            DeviceDisplayMode mode,
                            DeviceOrientation orientation,
                            int *width,
                            int *height);

int device_profile_update_state(DeviceProfile *profile,
                                DeviceDisplayMode mode,
                                DeviceOrientation orientation,
                                int width,
                                int height);

int device_profile_save(DeviceProfile *profile);

int device_profile_layout_scope(const DeviceProfile *profile,
                                DeviceDisplayMode mode,
                                DeviceOrientation orientation,
                                char *out,
                                size_t out_size);

void device_profile_clear(DeviceProfile *profile);

#endif
