# 15 kHz CRT support (branch `15khz`)

This branch turns an FL2000DX USB-to-VGA dongle into a **15 kHz RGB output for
a CRT TV or arcade monitor**, for use with GroovyMAME, Switchres and
GroovyArcade. It builds on the VGA support described further down.

### What works

- **Progressive 15 kHz modes**, any width: 240p/288p at native resolutions,
  super resolutions such as 2560x240, all timings as Switchres computes them.
  On a CRT TV the picture is stable, horizontally centred to the pixel.
- **GPU rendering under X**: the main GPU renders and the dongle displays
  (reverse PRIME), so Attract-Mode Plus, GroovyMAME and the desktop run
  accelerated on the dongle.
- **About one frame of latency**, with page flip events completed when the
  frame starts on screen (60.005-60.01 Hz, 0.1 ms jitter).
- **A protected screen**: in CRT mode the driver refuses anything outside
  14.5-16.8 kHz, whatever asks for it.

### What does not

- **Interlace (480i/576i).** The chip's timing generator always starts vsync
  on a line boundary. Every writable reserved bit was scanned, live and across
  a chip reset, without finding an interlace mode; see the 15 kHz chapter of
  [NOTES.md](NOTES.md).
- **Accelerated rendering in KMS mode.** The dongle has no GPU, and
  applications that render with OpenGL on the display device itself (SFML or
  SDL in KMS/DRM mode) only get software rendering (llvmpipe). Use X.

### Setup

1. **Build and install** with DKMS (a `dkms.conf` is in the repository):
   copy the tree to `/usr/src/fl2000_drm-<version>`, then
   `dkms add fl2000_drm/<version>` and `dkms install fl2000_drm/<version>`.

2. **Module options**, in `/etc/modprobe.d/fl2000.conf`:

   ```
   options fl2000 vga_mode=1 crt15k=1
   # The dongle also exposes a driver CD. usb-storage's SCSI error handling
   # resets the whole USB device, display included: keep it off the dongle
   options usb-storage quirks=1d5c:2000:i
   ```

   When `usb_storage` is in the initramfs (it usually is), regenerate it so
   the quirk applies at boot (`mkinitcpio -P` on Arch).

3. **Modes calibrated for the screen** (recommended). The built-in mode list
   has generic timings that are off-centre on a given TV. Let Switchres
   generate an EDID from the screen's calibrated `crt_range`, install it, and
   give it to the connector on the kernel command line:

   ```
   switchres 640 240 60 -e        # writes custom.bin with the user's switchres.ini
   install -m 644 custom.bin /usr/lib/firmware/edid/fl2000_640x240.bin
   ```

   ```
   video=VGA-1:e drm.edid_firmware=VGA-1:edid/fl2000_640x240.bin
   ```

   `drm.edid_firmware` takes one comma-separated list for all connectors (a
   second occurrence replaces the first), while `video=` is repeated, one per
   connector. The EDID must contain a progressive mode: an interlaced one is
   dropped and the built-in list is used instead.

4. **X**: the dongle appears as a "Sink Output" provider associated with the
   main GPU. Its output starts disabled, so turn it on before starting
   anything fullscreen, and make it primary: SFML (Attract-Mode) takes its
   fullscreen mode list from the primary output and aborts on an empty one.
   For example in `~/.xinitrc`:

   ```
   primary=--primary
   for out in $(xrandr | awk '/ connected/ {print $1}') ; do
     xrandr --output "$out" --auto $primary
     primary=
   done
   ```

   On GroovyArcade, also set `video.backend=X` in `ga.conf`.

### Module parameters

| Parameter | Default | Meaning |
|---|---|---|
| `vga_mode` | 0 | Drive the integrated VGA DAC (needed for VGA dongles) |
| `crt15k` | 0 | 15 kHz CRT mode: 15 kHz modes only, from the EDID when it has a progressive 15 kHz one, else built in; everything outside 14.5-16.8 kHz refused |
| `queue_depth` | 1 | Transfers queued to the chip (1-3). Each one beyond the first is a frame of latency; raise it only if frames ever go black |
| `debug_frames` | 0 | Hash every frame and log what is sent (debugfs `sent`) |
| `test_fill` | 0 | Stream solid white instead of the framebuffer, to test the data path |

### Diagnostics

A debugfs directory per dongle, `/sys/kernel/debug/fl2000-<usb device>/`,
gives raw register access (`reg`, `dump`), frame pacing measured from the
chip (`frames`, `ring`), the frames actually sent (`sent`, with
`debug_frames=1`) and bit overrides applied around each modeset (`pokes`).
The chip has no frame memory, so its transfers complete at the real refresh
rate and serve as a timing instrument. Tools in `tools/` use them:

- `fl2k_mode`: set any modeline with a test pattern; `-a FPS` animates
  through DirtyFB like X does, `-f` page flips with events and reports their
  timing.
- `fl2k_measure.py`: measured frame period against the programmed timings.
- `fl2k_sent.py`: new, repeated, out-of-order and lost frames, and waits.
- `fl2k_scan.py`, `fl2k_fsync.py`: the register bit scans behind the
  interlace search.
- `fl2k_primetest`: checks that GPU buffers import into the dongle.

### Tested on

One FL2000DX VGA dongle (`1d5c:2000`) on an HP EliteDesk 705 G4 (AMD Vega 11),
Arch Linux / GroovyArcade, kernels 7.2.9 and 6.18 LTS, with a 15 kHz CRT TV.

---

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
sudo modprobe drm_shmem_helper
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
