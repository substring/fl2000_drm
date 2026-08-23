# FL2000DX VGA dongle support (fork)

This is a fork of [klogg/fl2000_drm](https://github.com/klogg/fl2000_drm) that
makes **FL2000DX USB3-to-VGA dongles** work as proper DRM/KMS displays on
current kernels. The upstream driver targets HDMI dongles built around the
IT66121 bridge chip; on VGA dongles there is no bridge, the component match
never completes, and the display never comes up. This fork adds a direct path
for the chip's integrated VGA DAC.

### What this fork adds

- **VGA dongle support** (`vga_mode=1`): a `DRM_MODE_CONNECTOR_VGA` connector
  driving the FL2000DX's integrated DAC, no bridge chip required.
- **EDID-less operation with VESA fallback modes.** These VGA dongles have no
  usable DDC path to the monitor — established by a lengthy investigation
  culminating in a USB capture of the working Windows driver, which performs
  no DDC traffic either (it serves EDID from the OS cache). Modes up to
  1920x1200 are offered with 1024x768@60 preferred.
- **Hotplug detect via the undocumented `0x805C` connector-sense register**,
  found by raw-diffing chip state against physical cable changes. It reads
  zero with no cable and a stable analog-sense pattern with a monitor
  attached, which lets `detect()` report real `connected`/`disconnected`
  states — desktops auto-enable the display on plug-in.
- **Port to current kernels** (developed and tested on 7.0; upstream baseline
  was 6.5): fbdev client API, `struct drm_edid` API, bridge attach signature.
- **Five teardown/lifetime fixes**, several of which likely correspond to
  long-standing upstream issues
  [#12](https://github.com/klogg/fl2000_drm/issues/12) and
  [#33](https://github.com/klogg/fl2000_drm/issues/33): a devres lifetime bug
  that left a registered i2c adapter pointing into freed module text after
  rmmod; a disconnect deadlock between `i2c_del_adapter()` and the DRM
  device's devres parenting; a stream-worker semaphore deadlock that froze
  every display in the system on hot-unplug; an anchored-URB leak that could
  wedge a USB port for good on a flaky unplug; and an atomic shutdown called
  from the DRM release callback after the refcount already hit zero.

The full investigation record — every dead end included — is in
[NOTES.md](NOTES.md).

### Quick start

Requirements: kernel headers and build tools (`build-essential`,
`linux-headers-$(uname -r)`).

```
make
sudo modprobe drm_dma_helper
sudo insmod fl2000.ko vga_mode=1
```

A connected VGA monitor should appear as a normal display and be enabled by
your desktop automatically.

**USB 3 ports only.** The FL2000's descriptors declare duplicate endpoints,
which modern kernels reject when the device enumerates at USB 2 speed
(`can't set config #1, error -71`). On USB 3 the affected interfaces use
alternate settings and enumerate fine.

Without `vga_mode=1` the driver behaves like upstream (HDMI/IT66121 path).
Debugging aids from the investigation are also available: `probe_only=1`
brings up only the USB/register/I2C layer, and `dump_regs=1` dumps raw chip
state at probe.

### Tested on

One FL2000DX-based VGA dongle with one VGA monitor, on Pop!_OS 24.04
(kernel 7.0.11, COSMIC desktop, AMD primary GPU). Hot-unplug and replug
while the desktop is extended have been exercised deliberately and survive
cleanly. That is the honest extent of the testing — reports from other
dongles, monitors, kernels and desktops are very welcome in the issues.

### Attribution and license

Fork of [klogg/fl2000_drm](https://github.com/klogg/fl2000_drm) by Artem
Mygaiev — the clean DRM implementation this work builds on. Register-level
reference material from the abandoned official
[FrescoLogic/FL2000](https://github.com/FrescoLogic/FL2000) driver. Same
license as upstream: **GPL-2.0**.

---

## Original upstream README

[![Gitter](https://badges.gitter.im/fl2000_drm/community.svg)](https://gitter.im/fl2000_drm/community?utm_source=badge&utm_medium=badge&utm_campaign=pr-badge)
[![Build](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/makefile.yml)
[![Stylecheck](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml/badge.svg)](https://github.com/klogg/fl2000_drm/actions/workflows/codingstyle.yaml)
[![Quality Gate Status](https://sonarcloud.io/api/project_badges/measure?project=klogg_fl2000_drm&metric=alert_status)](https://sonarcloud.io/dashboard?id=klogg_fl2000_drm)

# Linux kernel FL2000DX/IT66121FN dongle DRM driver

Clean re-implementation of FrescoLogic FL2000DX DRM driver and ITE Tech IT66121F driver, allowing to enable full display controller capabilities for [USB-to-HDMI dongles](https://www.aliexpress.com/item/32821739801.html?spm=a2g0o.productlist.0.0.14ee52fb8rFfu5) based on such chips in Linux

### Building driver

Check out the code and type
```
make
```
Use
```
insmod fl2000.ko && insmod it66121.ko
```
with sudo or in root shell to start the driver. If you are running on a system with secure boot enabled, you may need to sign kernel modules. Try using provided script for this:
```
./scritps/sign.sh
```
ensure that DRM components are loaded in your system, if not - please use
```
modprobe drm
modprobe drm_kms_helper
```
**NOTE:** proper kernel headers and build tools (e.g. "build-essential" package) must be installed on the system. Driver is developed and tested on Ubuntu 22.04 with **Linux kernel 6.5.0**, so better to test it this way. Please use gcc-8 or newer to build the driver

For more information check project's [Wiki](https://github.com/klogg/fl2000_drm/wiki)

### Install via DKMS (survives kernel updates)

The out-of-tree module silently stops loading after a kernel update unless it is
managed by DKMS. Package it once and it rebuilds automatically on every new kernel:

```sh
VER=1.0
sudo cp -r . /usr/src/fl2000-$VER          # includes dkms.conf
sudo dkms add    -m fl2000 -v $VER
sudo dkms build  -m fl2000 -v $VER
sudo dkms install -m fl2000 -v $VER
```

Force VGA mode and keep the crashing HDMI bridge out of the way:

```sh
# /etc/modprobe.d/fl2000.conf
options fl2000 vga_mode=1
install it66121 /bin/true

# /etc/modules-load.d/fl2000.conf
fl2000
```

**Why block it66121:** on recent kernels (observed on 7.0.11) `it66121_probe()`
oopses and wedges the module (only a reboot clears it). VGA output does not use
the IT66121 HDMI bridge, so `vga_mode=1` + not loading it66121 is the stable path.
