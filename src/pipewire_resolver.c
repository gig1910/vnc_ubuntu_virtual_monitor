#include "pipewire_resolver.h"

#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>
#include <spa/utils/hook.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    uint32_t target_id;
    uint64_t serial;
    int found;
} ResolverState;

static double
now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return
        (double)ts.tv_sec +
        (double)ts.tv_nsec /
        1000000000.0;
}

static void
registry_global(
    void *data,
    uint32_t id,
    uint32_t permissions,
    const char *type,
    uint32_t version,
    const struct spa_dict *props)
{
    (void)permissions;
    (void)type;
    (void)version;

    ResolverState *state = data;

    if (
        state->found ||
        id != state->target_id ||
        !props
    )
        return;

    const char *serial =
        spa_dict_lookup(
            props,
            PW_KEY_OBJECT_SERIAL
        );

    if (!serial)
        return;

    errno = 0;
    char *end = NULL;

    unsigned long long value =
        strtoull(
            serial,
            &end,
            10
        );

    if (
        errno == 0 &&
        end &&
        *end == '\0'
    ) {
        state->serial =
            (uint64_t)value;

        state->found = 1;
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = registry_global
};

int
pipewire_resolve_object_serial(
    uint32_t node_id,
    uint64_t *serial_out,
    int timeout_ms)
{
    if (!serial_out)
        return -1;

    pw_init(NULL, NULL);

    struct pw_main_loop *main_loop =
        pw_main_loop_new(NULL);

    if (!main_loop)
        return -1;

    struct pw_context *context =
        pw_context_new(
            pw_main_loop_get_loop(
                main_loop
            ),
            NULL,
            0
        );

    if (!context) {
        pw_main_loop_destroy(main_loop);
        return -1;
    }

    struct pw_core *core =
        pw_context_connect(
            context,
            NULL,
            0
        );

    if (!core) {
        pw_context_destroy(context);
        pw_main_loop_destroy(main_loop);
        return -1;
    }

    struct pw_registry *registry =
        pw_core_get_registry(
            core,
            PW_VERSION_REGISTRY,
            0
        );

    if (!registry) {
        pw_core_disconnect(core);
        pw_context_destroy(context);
        pw_main_loop_destroy(main_loop);
        return -1;
    }

    ResolverState state = {
        .target_id = node_id
    };

    struct spa_hook listener;

    pw_registry_add_listener(
        registry,
        &listener,
        &registry_events,
        &state
    );

    double deadline =
        now_seconds() +
        (double)timeout_ms /
        1000.0;

    while (
        !state.found &&
        now_seconds() < deadline
    ) {
        pw_loop_iterate(
            pw_main_loop_get_loop(
                main_loop
            ),
            50
        );
    }

    spa_hook_remove(&listener);
    pw_proxy_destroy(
        (struct pw_proxy *)registry
    );
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(main_loop);

    if (!state.found)
        return -1;

    *serial_out = state.serial;
    return 0;
}
