CC ?= gcc

CPPFLAGS += -Iinclude
CFLAGS   += -O2 -Wall -Wextra -pthread -MMD -MP

PACKAGES := libvncserver openssl nettle glib-2.0 gio-2.0 \
	gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 libpipewire-0.3

PKG_CFLAGS := $(shell pkg-config --cflags $(PACKAGES))
PKG_LIBS   := $(shell pkg-config --libs $(PACKAGES))
BROKER_LIBS := $(shell pkg-config --libs glib-2.0 gio-2.0)

SOURCES := \
	src/main.c \
	src/broker_protocol.c \
	src/runtime_config.c \
	src/log.c \
	src/shutdown_signal.c \
	src/io.c \
	src/auth_client.c \
	src/ra2.c \
	src/ra2_identity.c \
	src/frame_bridge.c \
	src/frame_diff.c \
	src/adaptive_rfb_transport.c \
	src/pipeline_stats.c \
	src/monitor_layout_cache.c \
	src/mutter_virtual_monitor.c \
	src/pipewire_resolver.c \
	src/pipewire_capture.c \
	src/gstreamer_capture.c \
	src/real_monitor.c \
	src/rfb_backend.c \
	src/rfb_proxy.c \
	src/ra2_stream_coalescer.c

OBJECTS := $(SOURCES:.c=.o)
BROKER_OBJECTS := src/broker.o src/broker_protocol.o src/log.o
DEPS := $(sort $(OBJECTS:.o=.d) $(BROKER_OBJECTS:.o=.d))

TARGET := vnc-monitor
BROKER_TARGET := vnc-monitor-broker

BUILD_DIR := build
PAM_BUILD_DIR := $(BUILD_DIR)/pam-service
PAM_HELPER := auth-helper/vnc-monitor-auth-helper
PAM_SOCKET_TEMPLATE := auth-helper/vnc-monitor-auth.socket.in
PAM_SOCKET_GENERATED := $(PAM_BUILD_DIR)/vnc-monitor-auth.socket
CONFIG_TEMPLATE := config/vnc-monitor.conf

USER_BIN_DIR := $(HOME)/.local/bin
USER_SYSTEMD_DIR := $(HOME)/.config/systemd/user
USER_CONFIG_DIR := $(HOME)/.config/vnc-monitor
LEGACY_LAYOUT_DIR := $(HOME)/.config/vnc-monitor-server
USER_CACHE_DIR := $(HOME)/.cache/vnc-monitor
USER_SERVICE := $(USER_SYSTEMD_DIR)/vnc-monitor.service
USER_CONFIG_FILE := $(USER_CONFIG_DIR)/config.ini
USER_RA2_KEY := $(USER_CONFIG_DIR)/ra2-server-key.pem
LEGACY_RA2_KEY := ./ra2-server-key.pem

SYSTEM_CONFIG_DIR := /etc/vnc-monitor
SYSTEM_CONFIG_FILE := $(SYSTEM_CONFIG_DIR)/config.ini
BROKER_BIN := /usr/local/libexec/vnc-monitor-broker
BROKER_SERVICE := /etc/systemd/system/vnc-monitor-broker.service

# Source installs keep the PAM socket restricted to the invoking desktop user.
# Binary .deb installs use a generic local socket plus SO_PEERCRED validation.
AUTH_SOCKET_USER ?= $(shell id -un)
AUTH_SOCKET_GROUP ?= $(shell id -gn)

.PHONY: all clean \
	auth-helper pam-service install-pam-service pam-service-status uninstall-pam-service \
	install-broker-service broker-service-status uninstall-broker-service \
	install install-service uninstall-service restart-service stop-service status-service logs-service \
	install-support status-support uninstall-support cleanup-obsolete-support purge-config

all: $(TARGET) $(BROKER_TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(PKG_LIBS) -ljpeg -pthread

$(BROKER_TARGET): $(BROKER_OBJECTS)
	$(CC) $(CFLAGS) $(BROKER_OBJECTS) -o $@ $(BROKER_LIBS) -pthread

src/%.o: src/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

-include $(DEPS)

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

pam-service: auth-helper $(PAM_SOCKET_GENERATED)
	@printf '%s\n' \
		'PAM service build ready:' \
		'  helper:  $(PAM_HELPER)' \
		'  PAM:     auth-helper/vnc-monitor.pam' \
		'  socket:  $(PAM_SOCKET_GENERATED)' \
		'  service: auth-helper/vnc-monitor-auth@.service' \
		'  user:    $(AUTH_SOCKET_USER):$(AUTH_SOCKET_GROUP)'

# Do not run "sudo make install-pam-service": sudo is used only for the files
# that need root, preserving the invoking desktop user's uid/gid for the socket.
install-pam-service: pam-service
	@printf '%s\n' 'Installing PAM/systemd authentication support...'
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

uninstall-pam-service:
	-sudo systemctl disable --now vnc-monitor-auth.socket
	sudo rm -f \
		/usr/local/libexec/vnc-monitor-auth-helper \
		/etc/pam.d/vnc-monitor \
		/etc/systemd/system/vnc-monitor-auth.socket \
		/etc/systemd/system/vnc-monitor-auth@.service
	sudo systemctl daemon-reload
	@echo 'PAM/systemd authentication support removed.'

install-broker-service: $(BROKER_TARGET)
	@printf '%s\n' 'Installing system broker...'
	sudo install -Dm0755 "$(BROKER_TARGET)" "$(BROKER_BIN)"
	sudo install -Dm0644 systemd/vnc-monitor-broker.service "$(BROKER_SERVICE)"
	@if [ ! -e "$(SYSTEM_CONFIG_FILE)" ]; then \
		sudo install -Dm0644 "$(CONFIG_TEMPLATE)" "$(SYSTEM_CONFIG_FILE)"; \
		printf '%s\n' 'Created system config: $(SYSTEM_CONFIG_FILE)'; \
	else \
		printf '%s\n' 'Preserving existing system config: $(SYSTEM_CONFIG_FILE)'; \
	fi
	sudo systemctl daemon-reload
	sudo systemctl enable --now vnc-monitor-broker.service
	@$(MAKE) --no-print-directory broker-service-status

broker-service-status:
	@printf '%s\n' '===== BROKER SERVICE ====='
	@systemctl is-enabled vnc-monitor-broker.service 2>/dev/null || true
	@systemctl is-active vnc-monitor-broker.service 2>/dev/null || true
	@systemctl status vnc-monitor-broker.service --no-pager -l 2>/dev/null | sed -n '1,18p' || true

uninstall-broker-service:
	-sudo systemctl disable --now vnc-monitor-broker.service
	sudo rm -f "$(BROKER_BIN)" "$(BROKER_SERVICE)"
	sudo systemctl daemon-reload
	@echo 'System broker removed. /etc/vnc-monitor/config.ini was preserved.'

install: all
	install -Dm0755 "$(TARGET)" "$(USER_BIN_DIR)/vnc-monitor"
	@mkdir -p "$(USER_CONFIG_DIR)" "$(LEGACY_LAYOUT_DIR)" "$(USER_CACHE_DIR)"
	@chmod 700 "$(USER_CONFIG_DIR)" "$(LEGACY_LAYOUT_DIR)" "$(USER_CACHE_DIR)"
	@if [ ! -e "$(USER_CONFIG_FILE)" ]; then \
		install -m0600 "$(CONFIG_TEMPLATE)" "$(USER_CONFIG_FILE)"; \
		printf '%s\n' 'Created default config: $(USER_CONFIG_FILE)'; \
	else \
		printf '%s\n' 'Preserving existing config: $(USER_CONFIG_FILE)'; \
	fi
	@if [ ! -e "$(USER_RA2_KEY)" ] && [ -f "$(LEGACY_RA2_KEY)" ]; then \
		install -m0600 "$(LEGACY_RA2_KEY)" "$(USER_RA2_KEY)"; \
		printf '%s\n' 'Migrated existing RA2 server identity to $(USER_RA2_KEY)'; \
	fi

# Upgrade order is deliberate: replace/restart the old standalone user daemon
# as an agent first so TCP/5901 is released before the system broker starts.
install-service: install install-pam-service
	install -Dm0644 systemd/vnc-monitor.service "$(USER_SERVICE)"
	systemctl --user daemon-reload
	systemctl --user enable vnc-monitor.service
	systemctl --user restart vnc-monitor.service
	@$(MAKE) --no-print-directory install-broker-service
	@$(MAKE) --no-print-directory status-service

uninstall-service:
	-systemctl --user disable --now vnc-monitor.service
	rm -f "$(USER_SERVICE)" "$(USER_BIN_DIR)/vnc-monitor"
	systemctl --user daemon-reload
	@printf '%s\n' 'User agent removed. RA2 identity/config were preserved.'

restart-service:
	systemctl --user restart vnc-monitor.service

stop-service:
	systemctl --user stop vnc-monitor.service

status-service:
	@systemctl --user status vnc-monitor.service --no-pager -l 2>/dev/null || true

logs-service:
	journalctl --user -u vnc-monitor.service -n 100 --no-pager

purge-config:
	rm -rf "$(USER_CONFIG_DIR)" "$(LEGACY_LAYOUT_DIR)" "$(USER_CACHE_DIR)"
	@echo 'User VNC Monitor config/cache removed (including config, RA2 identity and layout cache).'

# Production support is system broker + per-user agent + PAM helper.
install-support: install-service
status-support: broker-service-status status-service pam-service-status
uninstall-support: uninstall-service uninstall-broker-service uninstall-pam-service

# Remove artifacts left by the old redraw/HW-cursor diagnostics. Current PAM,
# broker and user agent services are intentionally preserved.
cleanup-obsolete-support:
	./tools/cleanup-obsolete-support.sh

clean:
	rm -f $(OBJECTS) $(BROKER_OBJECTS) $(DEPS) $(TARGET) $(BROKER_TARGET) $(PAM_HELPER)
	rm -rf "$(BUILD_DIR)"
