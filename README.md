# Hikari - Wayland Compositor

## Description

_hikari_ is a stacking Wayland compositor with additional tiling capabilities,
it is heavily inspired by the Calm Window manager (cwm(1)). Its core concepts
are *views*, *groups*, *sheets* and the *workspace*.

The workspace is the set of views that are currently visible.

A sheet is a collection of views, each view can only be a member of a single
sheet. Switching between sheets will replace the current content of the
workspace with all the views that are a member of the selected sheet. _hikari_
has 9 general purpose sheets that correspond to the numbers **1** to **9** and a
special purpose sheet **0**. Views that are a member of sheet **0** will
always be visible but stacked below the views of the selected sheet.

Groups are a bit more fine grained than sheets. Like sheets, groups are a
collection of views. Unlike sheets you can have a arbitrary number of groups
and each group can have an arbitrary name. Views from one group can be spread
among all available sheets. Some operations act on entire groups rather than
individual views.

## Setup

### Setting up PAM

Setting up PAM is needed to give `hikari` the ability to unlock the screen when
using the screen locker. Copy the appropriate `hikari-unlocker` file from the
`pam.d` folder to `/etc/pam.d`.

### Setting up the keyboard layout

`hikari` gets its keyboard settings from `xkb` environment variables. To select
a layout set the `XKB_DEFAULT_LAYOUT` to the desired value before staring
`hikari`.

```
XKB_DEFAULT_LAYOUT "de(nodeadkeys),de"
```

## Building

`hikari` currently only works on Linux. When building directly from the
repository, breaking changes might be encountered. These are documented in the
`UPDATING` file which should be consulted before updating to a newer build.

### Dependencies

* wlroots-0.21
* pango
* cairo
* libinput
* xkbcommon
* pixman
* libucl
* evdev-proto
* PAM (optional, for `hikari-unlocker`)
* XWayland (optional, runtime dependency)

### Compiling and Installing

The build process will produce two binaries `hikari` and `hikari-unlocker`; the
latter is only built when the PAM development files are installed (see
"Optional features"). It is used to check credentials for unlocking the screen,
which needs to be installed with root setuid.  `hikari` can rely on `seatd`,
`(e)logind` or other mechanisms to gain root privileges when required; however,
if needed it can also be installed with root setuid - see "Installing with
SUID" below.  Both binaries need to be located in your `PATH`.

`hikari` can be configured via `$XDG_CONFIG_HOME/hikari/hikari.conf`, the
default configuration can be found under `$PREFIX/etc/hikari/hikari.conf`
(depending on the value of `PREFIX` that was specified during the installation).

The default configuration expects your default terminal emulator to be specified
in the `$TERMINAL` environment variable.

The installation destination can be configured by setting `PREFIX` (default is
`/usr` and does not need to be given explicitly). If you want to install
`hikari` into a directory other than `/usr` you always should state the
`PREFIX` when issuing `make`, since this information is also used to specify
where `hikari` can find the default configuration on your system and is needed
during the compilation process. To override installation paths for `etc` specify
`ETC_PREFIX`; it defaults to `/` for `PREFIX=/usr` and to `PREFIX` otherwise.

Simply run `make`:

```
make
```

and install it afterwards:

```
make PREFIX=/usr/local install
```

`uninstall` requires the same values for prefixes.

### Optional features

Optional features are detected automatically: `make` enables every feature the
wlroots in use can provide, so a plain build is a full build. A feature stays
off when wlroots does not ship the header it needs -- a wlroots built without
XWayland, for instance, or one that has dropped a deprecated protocol.

Each one can still be forced with the matching `WITH_*` flag, which is mostly
useful to turn a feature *off*:

```
make WITH_XWAYLAND=NO
```

#### XWayland support

XWayland support lets X11 applications run under `hikari`.

#### Screencopy support

Screencopy support allows tools like `grim` to work with `hikari`, it also
allows applications to copy the desktop content.

#### Gammacontrol support

Gamma control is needed for tools like `redshift`.

#### Layer-shell support

Some applications that are used to build desktop components require
`layer-shell`. Examples for this are `waybar`, `wofi` and `slurp`.

#### Virtual input support

Virtual input support is needed for applications like `wayvnc`.

#### Building the manpage

Building the `hikari` manpage requires [`pandoc`](http://pandoc.org/). To build
the manpage just run `make VERSION=1.0.0 doc`, where `VERSION` is the version
number that will be spliced into the manpage. The distribution tarball of
`hikari` comes with a precompiled manpage removing the need for a `pandoc`
installation.

#### Installing with SUID

If `hikari` should require root privileges for startup, state `WITH_SUID=YES`
during installation.

```
make WITH_SUID=YES install
```

#### Building a DEBUG build

In the case of a crash or a bug you should build a debug version of `hikari` and
try to reproduce the issue. This builds `hikari` with debug symbols and
sanitizers enabled. Extracting a stack trace for debugging purposes is also very
helpful if you are planning to submit a bug report.

```
make DEBUG=YES
```

## Community

The `hikari` community gears to be inclusive and welcoming to everyone, this is
why we chose to adhere to the [Geekfeminism Code of
Conduct](https://hikari.acmelabs.space/coc.html).

If you care to be a part of our community, please join our Matrix chat at
`#hikari:acmelabs.space` and/or subscribe to our mailing list by sending a mail
to `hikari+subscribe@acmelabs.space`.

## Contributing

Please make sure you use `clang-format` with the accompanying `.clang-format`
configuration before submitting any patches.
