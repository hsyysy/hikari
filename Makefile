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
PREFIX ?= /usr
PKG_CONFIG ?= pkg-config

# /usr/etc is not a thing: with the system prefix the configuration belongs in
# /etc, any other prefix keeps it under the prefix so that a $HOME install
# never writes to /etc.
ETC_PREFIX ?= $(if $(filter /usr,$(PREFIX)),/,$(PREFIX))

# The release build always defined this one; it is not worth remembering.
WITH_POSIX_C_SOURCE ?= YES

WLROOTS_CFLAGS := $(shell $(PKG_CONFIG) --cflags wlroots-0.21)
WLROOTS_LIBS := $(shell $(PKG_CONFIG) --libs wlroots-0.21)

WLROOTS_CFLAGS += -DWLR_USE_UNSTABLE=1

# wlroots installs its headers under a versioned directory (wlroots-0.21/), so
# the -I flag pkg-config reports is the only reliable way to find them. The
# first one is the wlroots include root.
WLROOTS_INCLUDE := $(patsubst -I%,%,$(firstword $(filter -I%,$(WLROOTS_CFLAGS))))

ifeq ($(WLROOTS_INCLUDE),)
$(warning pkg-config found no wlroots-0.21 include directory; all optional features are off)
endif

# --- Optional features -------------------------------------------------------
#
# A feature is enabled when the wlroots in use can provide it, so a plain
# `make` builds everything that is available. Nothing to remember, and no way
# to end up with objects from two different configurations (see the
# configuration stamp below).
#
# The test is the presence of the header the feature includes, which is what
# really decides whether the code compiles: wlroots installs a header only when
# it was built with the matching feature, and drops one again when a protocol it
# deprecated is removed. A version comparison would miss both.
#
# ?=, so a value given on the command line still wins: WITH_XWAYLAND=NO.
wlroots-has = $(wildcard $(WLROOTS_INCLUDE)/$(1))

WITH_XWAYLAND ?= $(if $(call wlroots-has,wlr/xwayland.h),YES,NO)
WITH_GAMMACONTROL ?= $(if $(call wlroots-has,wlr/types/wlr_gamma_control_v1.h),YES,NO)
WITH_SCREENCOPY ?= $(if $(call wlroots-has,wlr/types/wlr_screencopy_v1.h),YES,NO)
WITH_LAYERSHELL ?= $(if $(call wlroots-has,wlr/types/wlr_layer_shell_v1.h),YES,NO)
WITH_VIRTUAL_INPUT ?= $(if $(and $(call wlroots-has,wlr/types/wlr_virtual_keyboard_v1.h),$(call wlroots-has,wlr/types/wlr_virtual_pointer_v1.h)),YES,NO)

# All of them, for the summary and the flag check the release target prints.
FEATURES = XWAYLAND GAMMACONTROL SCREENCOPY LAYERSHELL VIRTUAL_INPUT

# $(call feature-on,XWAYLAND) -> XWAYLAND if that feature is enabled
feature-on = $(if $(filter YES,$(WITH_$(1))),$(1))

# hikari-unlocker is a separate binary that needs PAM, so it is built when the
# PAM development files are installed and skipped otherwise. Its own flag and
# not a HAVE_* one: it does not change how hikari itself is compiled.
#
# Not every PAM ships a pkg-config file, so the fallback is to ask the compiler
# for the header the unlocker includes; the link flags fall back to plain -lpam
# the same way, or such a system would lose the unlocker for no good reason.
# -include rather than a pipe: a '#' cannot be written literally here without
# turning into a make comment, and the escaped form reaches the shell with its
# backslash intact, which makes the preprocessor ignore the line and report
# success for every header.
PAM_HEADER := $(shell $(CC) -E -include security/pam_appl.h - < /dev/null > /dev/null 2>&1 && echo YES)

WITH_UNLOCKER ?= $(if $(or $(shell $(PKG_CONFIG) --exists pam && echo YES),$(PAM_HEADER)),YES,NO)
UNLOCKER = $(if $(filter YES,$(WITH_UNLOCKER)),hikari-unlocker)

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

.PHONY: distclean clean clean-doc doc dist install uninstall all test smoke release

VPATH = src

# Allow specification of /extra/ CFLAGS and LDFLAGS
CFLAGS += $(CFLAGS_EXTRA)
LDFLAGS += $(LDFLAGS_EXTRA)

ifdef DEBUG
CFLAGS += -g -O0 -fsanitize=address
else
CFLAGS += -DNDEBUG
endif

ifeq ($(WITH_POSIX_C_SOURCE),YES)
CFLAGS += -D_POSIX_C_SOURCE=200809L
endif

ifeq ($(WITH_XWAYLAND),YES)
CFLAGS += -DHAVE_XWAYLAND=1
endif

ifeq ($(WITH_GAMMACONTROL),YES)
CFLAGS += -DHAVE_GAMMACONTROL=1
endif

ifeq ($(WITH_SCREENCOPY),YES)
CFLAGS += -DHAVE_SCREENCOPY=1
endif

ifeq ($(WITH_LAYERSHELL),YES)
CFLAGS += -DHAVE_LAYERSHELL=1
endif

ifdef WITH_SUID
PERMS = 4555
else
PERMS = 555
endif

ifeq ($(WITH_VIRTUAL_INPUT),YES)
CFLAGS += -DHAVE_VIRTUAL_INPUT=1
endif

CFLAGS += -Wall -I. -Iinclude -DHIKARI_ETC_PREFIX=$(ETC_PREFIX)
CFLAGS += -MMD -MP

PANGO_CFLAGS := $(shell $(PKG_CONFIG) --cflags pangocairo)
PANGO_LIBS := $(shell $(PKG_CONFIG) --libs pangocairo)

PIXMAN_LIBS := $(shell $(PKG_CONFIG) --libs pixman-1)

XKBCOMMON_LIBS := $(shell $(PKG_CONFIG) --libs xkbcommon)

WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-server)

LIBINPUT_LIBS := $(shell $(PKG_CONFIG) --libs libinput)

UCL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libucl)
UCL_LIBS := $(shell $(PKG_CONFIG) --libs libucl)

PAM_CFLAGS := $(shell $(PKG_CONFIG) --cflags pam)
PAM_LIBS := $(shell $(PKG_CONFIG) --libs pam)
PAM_LIBS := $(if $(PAM_LIBS),$(PAM_LIBS),-lpam)

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

# wlroots ships wlr/types/wlr_layer_shell_v1.h with
#     #include "wlr-layer-shell-unstable-v1-protocol.h"
# but never installs that generated header, so every compositor using layer
# shell has to generate its own copy where the quoted include can find it --
# that is what the -I. in CFLAGS is for. It is generated from protocol/*.xml
# and not committed: the XML is the tracked source of truth, and a committed
# copy silently wins over regeneration, which is exactly how this header
# drifted to protocol version 4 while wlroots 0.21 already spoke version 5.
#
# wayland-scanner ships with wayland itself, which wlroots already depends on.
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner 2> /dev/null)
WAYLAND_SCANNER := $(if $(WAYLAND_SCANNER),$(WAYLAND_SCANNER),wayland-scanner)

ifeq ($(WITH_LAYERSHELL),YES)
PROTOCOL_HEADERS = wlr-layer-shell-unstable-v1-protocol.h
endif

DEPS = $(OBJS:.o=.d)

# Configuration stamp -- see the long comment further down for the mechanism.
# It must be defined HERE, before any rule references it: make expands a rule's
# prerequisites when it parses the rule, so a rule written above this line would
# silently see an empty string and lose the dependency entirely. (That is
# exactly what happened to tests/popup_placement when this lived further down.)
CFLAGS_STAMP = .build-flags

all: hikari $(UNLOCKER)

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

# The release build: everything the detection above found, verified end to end.
# `make` already builds that binary; this adds the checks. They are not
# paranoia -- a feature that is detected as available but whose -DHAVE_* never
# reaches the compiler yields a compositor that silently lacks it: without
# HAVE_VIRTUAL_INPUT there is no zwp_virtual_keyboard_manager_v1 global and the
# input method cannot start at all, while make, smoke and the binary itself all
# look fine.
#
# The recursive $(MAKE) calls keep the steps in order even under -j: as plain
# prerequisites, `clean` and `all` could run at the same time.
release:
	$(MAKE) clean
	$(MAKE) all
	@echo "release: $(foreach f,$(FEATURES),$(f)=$(WITH_$(f))) UNLOCKER=$(WITH_UNLOCKER)"
	@for f in $(foreach f,$(FEATURES),$(call feature-on,$(f))); do \
	  grep -q -- "-DHAVE_$$f=1" .build-flags || { \
	    echo "release: HAVE_$$f never reached the compiler" >&2; \
	    exit 1; \
	  }; \
	done
	$(MAKE) test
	$(MAKE) smoke
	@echo "release: OK"

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
# The WITH_* flags are no longer something the caller has to remember: they are
# detected above and are the same for every target, so `make test` and
# `make smoke` reuse the objects `make` just built instead of being a different
# configuration. Overriding one on the command line still changes the flags,
# and then the stamp rebuilds everything -- which is the point.

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

wlr-layer-shell-unstable-v1-protocol.h: protocol/wlr-layer-shell-unstable-v1.xml
	$(WAYLAND_SCANNER) server-header $< $@

# Deliberately exempt from the configuration stamp: hikari_unlocker.c includes
# only system headers (pwd.h, security/pam_appl.h, ...) and no hikari header, so
# it has no struct whose layout depends on a WITH_* flag. It also needs none of
# the wlroots/pango includes CFLAGS carries, which is why it uses CFLAGS_EXTRA.
# Nothing here can be mixed up by a configuration change.
#
# Not part of `all` unless PAM was found (see WITH_UNLOCKER above); asking for
# it explicitly still works and fails at the compiler if PAM is missing.
hikari-unlocker: hikari_unlocker.c
	$(CC) $(CFLAGS_EXTRA) $(PAM_CFLAGS) $(LDFLAGS_EXTRA) -o hikari-unlocker hikari_unlocker.c $(PAM_LIBS)

clean-doc:
	@test -e _darcs && echo "cleaning manpage" ||:
	@test -e _darcs && rm share/man/man1/hikari.1 2> /dev/null ||:

clean: clean-doc
	@echo "cleaning headers"
	@test -e _darcs && rm version.h 2> /dev/null ||:
	@rm -f $(PROTOCOL_HEADERS) 2> /dev/null ||:
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

install: hikari $(UNLOCKER) share/man/man1/hikari.1
	mkdir -p $(DESTDIR)/$(PREFIX)/bin
	mkdir -p $(DESTDIR)/$(PREFIX)/share/man/man1
	mkdir -p $(DESTDIR)/$(PREFIX)/share/backgrounds/hikari
	mkdir -p $(DESTDIR)/$(PREFIX)/share/wayland-sessions
	mkdir -p $(DESTDIR)/$(ETC_PREFIX)/etc/hikari
	$(if $(UNLOCKER),mkdir -p $(DESTDIR)/$(ETC_PREFIX)/etc/pam.d)
	sed "s,PREFIX,$(PREFIX)," etc/hikari/hikari.conf > $(DESTDIR)/$(ETC_PREFIX)/etc/hikari/hikari.conf
	chmod 644 $(DESTDIR)/$(ETC_PREFIX)/etc/hikari/hikari.conf
	install -m $(PERMS) hikari $(DESTDIR)/$(PREFIX)/bin
	$(if $(UNLOCKER),install -m 4555 hikari-unlocker $(DESTDIR)/$(PREFIX)/bin)
	install -m 644 share/man/man1/hikari.1 $(DESTDIR)/$(PREFIX)/share/man/man1
	install -m 644 share/backgrounds/hikari/hikari_wallpaper.png $(DESTDIR)/$(PREFIX)/share/backgrounds/hikari/hikari_wallpaper.png
	install -m 644 share/wayland-sessions/hikari.desktop $(DESTDIR)/$(PREFIX)/share/wayland-sessions/hikari.desktop
	$(if $(UNLOCKER),install -m 644 etc/pam.d/hikari-unlocker.$(OS) $(DESTDIR)/$(ETC_PREFIX)/etc/pam.d/hikari-unlocker)

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
