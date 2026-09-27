# Enable all features when running 'make clean' so everything gets cleaned
ifneq ($(filter clean,$(MAKECMDGOALS)),)
WITH_ALL = YES
endif

ifdef WITH_ALL
WITH_XWAYLAND = YES
WITH_SCREENCOPY = YES
WITH_GAMMACONTROL = YES
WITH_LAYERSHELL = YES
WITH_VIRTUAL_INPUT = YES
endif

OS := $(shell uname)
VERSION ?= "CURRENT"
PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
ETC_PREFIX ?= $(PREFIX)

OBJS = \
	action.o \
	action_config.o \
	binding_config.o \
	binding_group.o \
	border.o \
	command.o \
	completion.o \
	configuration.o \
	cursor.o \
	decoration.o \
	dnd_mode.o \
	exec.o \
	font.o \
	geometry.o \
	group.o \
	group_assign_mode.o \
	indicator.o \
	indicator_bar.o \
	indicator_frame.o \
	input_buffer.o \
	input_grab_mode.o \
	input_method_relay.o \
	keyboard.o \
	keyboard_config.o \
	layer_shell.o \
	layout.o \
	layout_config.o \
	layout_select_mode.o \
	lock_indicator.o \
	lock_mode.o \
	log.o \
	main.o \
	mark.o \
	mark_assign_mode.o \
	mark_select_mode.o \
	maximized_state.o \
	memory.o \
	move_mode.o \
	normal_mode.o \
	output.o \
	output_config.o \
	pointer.o \
	pointer_config.o \
	position_config.o \
	renderer.o \
	resize_mode.o \
	server.o \
	sheet.o \
	sheet_assign_mode.o \
	split.o \
	switch.o \
	switch_config.o \
	tile.o \
	view.o \
	view_config.o \
	workspace.o \
	xdg_view.o

ifeq ($(WITH_XWAYLAND),YES)
OBJS += \
	xwayland_unmanaged_view.o \
	xwayland_view.o
endif

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable pkgdatadir wayland-protocols)

.PHONY: distclean clean clean-doc doc dist install uninstall all test smoke

VPATH = src

# Allow specification of /extra/ CFLAGS and LDFLAGS
CFLAGS += $(CFLAGS_EXTRA)
LDFLAGS += $(LDFLAGS_EXTRA)

ifdef DEBUG
CFLAGS += -g -O0 -fsanitize=address
else
CFLAGS += -DNDEBUG
endif

ifdef WITH_POSIX_C_SOURCE
CFLAGS += -D_POSIX_C_SOURCE=200809L
endif

ifeq ($(WITH_XWAYLAND),YES)
CFLAGS += -DHAVE_XWAYLAND=1
endif

ifdef WITH_GAMMACONTROL
CFLAGS += -DHAVE_GAMMACONTROL=1
endif

ifdef WITH_SCREENCOPY
CFLAGS += -DHAVE_SCREENCOPY=1
endif

ifdef WITH_LAYERSHELL
CFLAGS += -DHAVE_LAYERSHELL=1
endif

ifdef WITH_SUID
PERMS = 4555
else
PERMS = 555
endif

ifdef WITH_VIRTUAL_INPUT
CFLAGS += -DHAVE_VIRTUAL_INPUT=1
endif

CFLAGS += -Wall -I. -Iinclude -DHIKARI_ETC_PREFIX=$(ETC_PREFIX)
CFLAGS += -MMD -MP

WLROOTS_CFLAGS := $(shell $(PKG_CONFIG) --cflags wlroots-0.21)
WLROOTS_LIBS := $(shell $(PKG_CONFIG) --libs wlroots-0.21)

WLROOTS_CFLAGS += -DWLR_USE_UNSTABLE=1

PANGO_CFLAGS := $(shell $(PKG_CONFIG) --cflags pangocairo)
PANGO_LIBS := $(shell $(PKG_CONFIG) --libs pangocairo)

PIXMAN_LIBS := $(shell $(PKG_CONFIG) --libs pixman-1)

XKBCOMMON_LIBS := $(shell $(PKG_CONFIG) --libs xkbcommon)

WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-server)

LIBINPUT_LIBS := $(shell $(PKG_CONFIG) --libs libinput)

UCL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libucl)
UCL_LIBS := $(shell $(PKG_CONFIG) --libs libucl)

CFLAGS += \
	$(WLROOTS_CFLAGS) \
	$(PANGO_CFLAGS) \
	$(UCL_CFLAGS)

LIBS = \
	$(WLROOTS_LIBS) \
	$(PANGO_LIBS) \
	$(PIXMAN_LIBS) \
	$(XKBCOMMON_LIBS) \
	$(WAYLAND_LIBS) \
	$(LIBINPUT_LIBS) \
	$(UCL_LIBS)

PROTOCOL_HEADERS = xdg-shell-protocol.h

ifdef WITH_LAYERSHELL
PROTOCOL_HEADERS += wlr-layer-shell-unstable-v1-protocol.h
endif

DEPS = $(OBJS:.o=.d)

# Configuration stamp -- see the long comment further down for the mechanism.
# It must be defined HERE, before any rule references it: make expands a rule's
# prerequisites when it parses the rule, so a rule written above this line would
# silently see an empty string and lose the dependency entirely. (That is
# exactly what happened to tests/popup_placement when this lived further down.)
CFLAGS_STAMP = .build-flags

all: hikari hikari-unlocker

# Unit tests that need no compositor. tests/popup_placement.c includes the real
# hikari_input_popup_place() from include/hikari/input_method_relay.h, so this
# exercises the shipped code, not a copy.
# ASAN_OPTIONS: the test binary inherits whatever sanitizer flags the tree was
# configured with, and a DEBUG build turns it into an ASan binary. Leak
# detection then runs at exit and fails outright in environments where LSan
# cannot work (e.g. under ptrace), so the assertions all pass and make still
# reports an error. This test exercises a pure function and allocates nothing,
# so the leak check buys nothing -- disable it rather than let the target go
# red for environmental reasons, which is how a check stops being run at all.
test: tests/popup_placement
	@ASAN_OPTIONS=detect_leaks=0 ./tests/popup_placement

tests/popup_placement: tests/popup_placement.c include/hikari/input_method_relay.h $(CFLAGS_STAMP)
	$(CC) $(CFLAGS) -o $@ $<

# Pre-flight check: boot under the headless backend and make sure startup
# survives. Exists because mixing build configurations (e.g. rebuilding only
# some objects after toggling WITH_LAYERSHELL) produces a binary whose struct
# layouts disagree -- hikari then segfaults during init and takes the whole
# session down with it. The compiler cannot catch this; booting it can.
# Exit 124 means it was still alive when timeout fired, which is the pass case.
# XDG_RUNTIME_DIR is created here rather than inherited so the check does not
# depend on the caller's environment (CI/containers often lack a writable one,
# and a spurious failure would train people to ignore the check). The log is
# printed on failure so a real startup crash can be told apart from an
# environment problem at a glance.
smoke: hikari
	@rt=$$(mktemp -d) && chmod 700 $$rt; \
	log=$$(mktemp); \
	timeout 5 env XDG_RUNTIME_DIR=$$rt WLR_BACKENDS=headless WLR_RENDERER=pixman \
	  WLR_LIBINPUT_NO_DEVICES=1 ./hikari -c tests/smoke.conf -a /bin/true \
	  >$$log 2>&1; \
	rc=$$?; \
	rm -rf $$rt; \
	if [ $$rc -eq 124 ]; then \
	  rm -f $$log; \
	  echo "smoke: OK (startup survived)"; \
	else \
	  echo "smoke: FAILED (exit $$rc; 139 = SIGSEGV)"; \
	  echo "--- last 20 log lines ---"; \
	  tail -n 20 $$log; \
	  rm -f $$log; \
	  exit 1; \
	fi

-include $(DEPS)

# --- Configuration stamp -----------------------------------------------------
# Every object must be built with the same configuration. struct hikari_server
# has HAVE_LAYERSHELL-conditional members, so mixing objects built with
# different feature flags yields a binary whose struct layouts disagree -- it
# segfaults during init (wl_signal_add <- hikari_input_method_relay_init) and
# takes the whole session down. Neither the compiler nor the linker can catch
# this: each .o is individually valid, and make only rebuilds objects whose
# *source* is newer, not whose *flags* changed.
#
# This has happened three times. The stamp is rewritten only when the compile
# command changes, and every object depends on it, so a configuration change
# rebuilds everything while an unchanged configuration rebuilds nothing.
#
# The stamp covers the whole built-in %.o: %.c rule, which is
#   COMPILE.c = $(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH) -c
# -- not just CFLAGS. Stamping CFLAGS alone would still let a changed CC
# (gcc -> clang) or CPPFLAGS mix objects silently.
#
# Consequence to be aware of: the WITH_* flags are command-line variables, so
# `make test` / `make smoke` invoked *without* them are a different
# configuration and will trigger a full rebuild. Pass the same flags (or run
# ./build.sh) when using those targets.

.PHONY: FORCE
FORCE:

$(CFLAGS_STAMP): FORCE
	@if [ -f $@ ] && \
	    printf '%s\n' '$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH)' | cmp -s - $@; then \
	  :; \
	else \
	  [ -f $@ ] && echo "build: configuration changed -- rebuilding all objects"; \
	  printf '%s\n' '$(CC) $(CFLAGS) $(CPPFLAGS) $(TARGET_ARCH)' > $@; \
	fi

$(OBJS): $(CFLAGS_STAMP)

version.h:
	echo "#define HIKARI_VERSION \"$(VERSION)\"" > version.h

hikari: version.h $(PROTOCOL_HEADERS) $(OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJS) $(LIBS)

xdg-shell-protocol.h:
	wayland-scanner server-header $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

wlr-layer-shell-unstable-v1-protocol.h:
	wayland-scanner server-header protocol/wlr-layer-shell-unstable-v1.xml $@

# Deliberately exempt from the configuration stamp: hikari_unlocker.c includes
# only system headers (pwd.h, security/pam_appl.h, ...) and no hikari header, so
# it has no struct whose layout depends on a WITH_* flag. It also needs none of
# the wlroots/pango includes CFLAGS carries, which is why it uses CFLAGS_EXTRA.
# Nothing here can be mixed up by a configuration change.
hikari-unlocker: hikari_unlocker.c
	$(CC) $(CFLAGS_EXTRA) $(LDFLAGS_EXTRA) -o hikari-unlocker hikari_unlocker.c -lpam

clean-doc:
	@test -e _darcs && echo "cleaning manpage" ||:
	@test -e _darcs && rm share/man/man1/hikari.1 2> /dev/null ||:

clean: clean-doc
	@echo "cleaning headers"
	@test -e _darcs && rm version.h 2> /dev/null ||:
	@echo "cleaning object files"
	@rm -f $(OBJS) $(DEPS)
	@echo "cleaning executables"
	@rm hikari 2> /dev/null ||:
	@rm hikari-unlocker 2> /dev/null ||:
	@rm -f tests/popup_placement tests/popup_placement.d 2> /dev/null ||:
	@rm -f $(CFLAGS_STAMP) 2> /dev/null ||:

share/man/man1/hikari.1:
	pandoc -M title:"HIKARI(1) $(VERSION) | hikari - Wayland Compositor" -s \
		--to man -o share/man/man1/hikari.1 share/man/man1/hikari.md

doc: share/man/man1/hikari.1

hikari-$(VERSION).tar.gz: version.h share/man/man1/hikari.1
	@darcs revert
	@tar -s "#^#hikari-$(VERSION)/#" -czf hikari-$(VERSION).tar.gz \
		version.h \
		main.c \
		hikari_unlocker.c \
		include/hikari/*.h \
		src/*.c \
		protocol/*.xml \
		Makefile \
		LICENSE \
		README.md \
		CoC.md \
		CHANGELOG.md \
		share/man/man1/hikari.md \
		share/man/man1/hikari.1 \
		share/backgrounds/hikari/hikari_wallpaper.png \
		share/wayland-sessions/hikari.desktop \
		etc/hikari/hikari.conf \
		etc/pam.d/hikari-unlocker.*

distclean: clean-doc
	@test -e _darcs && echo "cleaning version.h" ||:
	@test -e _darcs && rm version.h ||:

dist: distclean hikari-$(VERSION).tar.gz

install: hikari hikari-unlocker share/man/man1/hikari.1
	mkdir -p $(DESTDIR)/$(PREFIX)/bin
	mkdir -p $(DESTDIR)/$(PREFIX)/share/man/man1
	mkdir -p $(DESTDIR)/$(PREFIX)/share/backgrounds/hikari
	mkdir -p $(DESTDIR)/$(PREFIX)/share/wayland-sessions
	mkdir -p $(DESTDIR)/$(ETC_PREFIX)/etc/hikari
	mkdir -p $(DESTDIR)/$(ETC_PREFIX)/etc/pam.d
	sed "s,PREFIX,$(PREFIX)," etc/hikari/hikari.conf > $(DESTDIR)/$(ETC_PREFIX)/etc/hikari/hikari.conf
	chmod 644 $(DESTDIR)/$(ETC_PREFIX)/etc/hikari/hikari.conf
	install -m $(PERMS) hikari $(DESTDIR)/$(PREFIX)/bin
	install -m 4555 hikari-unlocker $(DESTDIR)/$(PREFIX)/bin
	install -m 644 share/man/man1/hikari.1 $(DESTDIR)/$(PREFIX)/share/man/man1
	install -m 644 share/backgrounds/hikari/hikari_wallpaper.png $(DESTDIR)/$(PREFIX)/share/backgrounds/hikari/hikari_wallpaper.png
	install -m 644 share/wayland-sessions/hikari.desktop $(DESTDIR)/$(PREFIX)/share/wayland-sessions/hikari.desktop
	install -m 644 etc/pam.d/hikari-unlocker.$(OS) $(DESTDIR)/$(ETC_PREFIX)/etc/pam.d/hikari-unlocker

uninstall:
	-rm $(DESTDIR)/$(PREFIX)/bin/hikari
	-rm $(DESTDIR)/$(PREFIX)/bin/hikari-unlocker
	-rm $(DESTDIR)/$(PREFIX)/share/man/man1/hikari.1
	-rm $(DESTDIR)/$(PREFIX)/share/backgrounds/hikari/hikari_wallpaper.png
	-rm $(DESTDIR)/$(PREFIX)/share/wayland-sessions/hikari.desktop
	-rm $(DESTDIR)/$(ETC_PREFIX)/etc/pam.d/hikari-unlocker
	-rm $(DESTDIR)/$(ETC_PREFIX)/etc/hikari/hikari.conf
	-rmdir $(DESTDIR)/$(ETC_PREFIX)/etc/hikari
	-rmdir $(DESTDIR)/$(PREFIX)/share/backgrounds/hikari
