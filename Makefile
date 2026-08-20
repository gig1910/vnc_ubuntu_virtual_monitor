CC ?= gcc

CPPFLAGS += -Iinclude
CFLAGS   += -O2 -Wall -Wextra -pthread

PACKAGES := libvncserver openssl nettle glib-2.0 gio-2.0 \
	gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 libpipewire-0.3

PKG_CFLAGS := $(shell pkg-config --cflags $(PACKAGES))
PKG_LIBS   := $(shell pkg-config --libs $(PACKAGES))

SOURCES := \
	src/main.c \
	src/runtime_config.c \
	src/benchmark.c \
	src/shutdown_signal.c \
	src/io.c \
	src/auth_client.c \
	src/ra2.c \
	src/ra2_identity.c \
	src/test_pattern.c \
	src/frame_bridge.c \
	src/frame_diff.c \
	src/adaptive_rfb_copyrect.c \
	src/pipeline_stats.c \
	src/monitor_layout_cache.c \
	src/mutter_virtual_monitor.c \
	src/mutter_environment.c \
	src/pipewire_resolver.c \
	src/pipewire_capture.c \
	src/gstreamer_capture.c \
	src/real_monitor.c \
	src/rfb_backend.c \
	src/rfb_proxy.c \
	src/ra2_stream_coalescer.c

OBJECTS := $(SOURCES:.c=.o)

TARGET := vnc-monitor-test

BUILD_DIR := build
PAM_BUILD_DIR := $(BUILD_DIR)/pam-service
PAM_HELPER := auth-helper/vnc-monitor-auth-helper
PAM_SOCKET_TEMPLATE := auth-helper/vnc-monitor-auth.socket.in
PAM_SOCKET_GENERATED := $(PAM_BUILD_DIR)/vnc-monitor-auth.socket

# The VNC process runs as the user invoking make. The privileged PAM helper is
# root-owned, but its Unix socket should only be accessible to this user.
AUTH_SOCKET_USER ?= $(shell id -un)
AUTH_SOCKET_GROUP ?= $(shell id -gn)

.PHONY: all clean \
	auth-helper pam-service install-pam-service pam-service-status uninstall-pam-service \
	install-support status-support uninstall-support cleanup-obsolete-support

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(PKG_LIBS) -ljpeg -pthread

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

$(PAM_HELPER): auth-helper/vnc-monitor-auth-helper.c
	$(CC) -O2 -Wall -Wextra $< -o $@ -lpam

auth-helper: $(PAM_HELPER)

$(PAM_SOCKET_GENERATED): $(PAM_SOCKET_TEMPLATE)
	@mkdir -p "$(PAM_BUILD_DIR)"
	@case '$(AUTH_SOCKET_USER)' in *[!A-Za-z0-9_.-]*|'') echo 'Invalid AUTH_SOCKET_USER: $(AUTH_SOCKET_USER)' >&2; exit 1;; esac
	@case '$(AUTH_SOCKET_GROUP)' in *[!A-Za-z0-9_.-]*|'') echo 'Invalid AUTH_SOCKET_GROUP: $(AUTH_SOCKET_GROUP)' >&2; exit 1;; esac
	@sed \
		-e 's/@SOCKET_USER@/$(AUTH_SOCKET_USER)/g' \
		-e 's/@SOCKET_GROUP@/$(AUTH_SOCKET_GROUP)/g' \
		"$<" > "$@"

# Build the root PAM authentication helper and render the systemd socket unit
# for the current user. This target performs no privileged/system changes.
pam-service: auth-helper $(PAM_SOCKET_GENERATED)
	@printf '%s\n' \
		'PAM service build ready:' \
		'  helper:  $(PAM_HELPER)' \
		'  PAM:     auth-helper/vnc-monitor.pam' \
		'  socket:  $(PAM_SOCKET_GENERATED)' \
		'  service: auth-helper/vnc-monitor-auth@.service' \
		'  user:    $(AUTH_SOCKET_USER):$(AUTH_SOCKET_GROUP)'

# Do NOT run "sudo make install-pam-service": this target deliberately invokes
# sudo only for the files/commands that require root and keeps user detection
# tied to the normal invoking account.
install-pam-service: pam-service
	@printf '%s\n' 'Installing VNC Monitor PAM/systemd support (sudo will be requested if needed)...'
	sudo install -Dm0755 "$(PAM_HELPER)" /usr/local/libexec/vnc-monitor-auth-helper
	sudo install -Dm0644 auth-helper/vnc-monitor.pam /etc/pam.d/vnc-monitor
	sudo install -Dm0644 "$(PAM_SOCKET_GENERATED)" /etc/systemd/system/vnc-monitor-auth.socket
	sudo install -Dm0644 auth-helper/vnc-monitor-auth@.service /etc/systemd/system/vnc-monitor-auth@.service
	sudo systemctl daemon-reload
	sudo systemctl enable --now vnc-monitor-auth.socket
	@$(MAKE) --no-print-directory pam-service-status

pam-service-status:
	@printf '%s\n' '===== PAM SERVICE ====='
	@systemctl is-enabled vnc-monitor-auth.socket 2>/dev/null || true
	@systemctl is-active vnc-monitor-auth.socket 2>/dev/null || true
	@systemctl status vnc-monitor-auth.socket --no-pager -l 2>/dev/null | sed -n '1,14p' || true
	@printf '%s\n' '===== AUTH SOCKET ====='
	@ls -l /run/vnc-monitor-auth.sock 2>/dev/null || echo '/run/vnc-monitor-auth.sock is not present'
	@printf '%s\n' '===== INSTALLED FILES ====='
	@ls -l \
		/usr/local/libexec/vnc-monitor-auth-helper \
		/etc/pam.d/vnc-monitor \
		/etc/systemd/system/vnc-monitor-auth.socket \
		/etc/systemd/system/vnc-monitor-auth@.service 2>/dev/null || true

uninstall-pam-service:
	-sudo systemctl disable --now vnc-monitor-auth.socket
	sudo rm -f \
		/usr/local/libexec/vnc-monitor-auth-helper \
		/etc/pam.d/vnc-monitor \
		/etc/systemd/system/vnc-monitor-auth.socket \
		/etc/systemd/system/vnc-monitor-auth@.service
	sudo systemctl daemon-reload
	@echo 'VNC Monitor PAM/systemd support removed.'

# Since 0.0.19 there is no redraw helper. Runtime support is only the PAM
# authentication helper/socket.
install-support: install-pam-service

status-support: pam-service-status

uninstall-support: uninstall-pam-service

# Remove artifacts left by the 0.0.17/0.0.18 redraw/HW-cursor diagnostics.
# This intentionally keeps vnc-monitor-auth.socket/service because it is part
# of the current authentication path.
cleanup-obsolete-support:
	./tools/cleanup-obsolete-support.sh

clean:
	rm -f $(OBJECTS) $(TARGET) $(PAM_HELPER)
	rm -rf "$(BUILD_DIR)"
