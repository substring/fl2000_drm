# FL2000DX VGA dongle — porting notes

Target hardware: `1d5c:2000` Fresco Logic FL2000DX, **VGA** dongle (no IT66121
bridge chip), VGA monitor attached and powered on.

Host: Pop!_OS 24.04, kernel `7.0.11-76070011-generic`, gcc 13.3.0, Secure Boot
disabled (no module signing needed).

## Phase 1 — build baseline

See commit "Port to kernel 7.0". Driver baseline was 6.5; the DRM API moved on
in three places that mattered (fbdev client API, EDID API, bridge attach
signature). No functional changes were made.

## Phase 2 diagnosis — where the VGA dongle stops

With `fl2000.ko` loaded alone (`it66121.ko` deliberately not loaded):

    $ sudo insmod ./fl2000.ko
    $ dmesg
    usbcore: registered new interface driver fl2000_usb

That single line is the *only* output. It is misleading — probe **does** run and
**does** succeed. Evidence:

    $ ls /sys/bus/usb/devices/2-4:1.*/driver
    2-4:1.0 -> fl2000_usb
    2-4:1.1 -> fl2000_usb
    2-4:1.2 -> fl2000_usb

All three interfaces (AVCONTROL=0, STREAMING=1, INTERRUPT=2) bind, so
`devs->active_if == FL2000_ALL_IFS` and `component_master_add_with_match()` is
reached. The regmap and I2C adapter are both created successfully:

    $ cat /sys/bus/i2c/devices/i2c-8/name
    FL2000 bridge I2C bus

So USB vendor control transfers to the chip work.

**The stall point is the component framework.** `fl2000_compare()` only ever
matches an `i2c_client` named `it66121`. On a VGA dongle that chip does not
exist, so the match list is never satisfied, `fl2000_master_ops.bind` is never
called, `fl2000_drm_bind()` never runs, and no DRM device is registered:

    $ ls /sys/class/drm/ | grep -E '^card[0-9]+$'
    card1          # amdgpu only; no fl2000 card

There is no error and no timeout — the master simply waits forever. This is
exactly the failure predicted for VGA dongles.

## Two obstacles the plan did not anticipate

### 1. EDID cannot be read with `drm_get_edid()` / `drm_edid_read_ddc()`

The FL2000 I2C master is not a general-purpose I2C controller. It is a
dword-oriented register interface (`fl2000_i2c_dword()`), and the adapter
advertises quirks to match (`fl2000_i2c.c`):

    max_num_msgs         = 2
    max_write_len        = 2
    max_read_len         = 1      <-- one byte per transaction
    max_comb_1st_msg_len = 1
    flags = I2C_AQ_COMB | I2C_AQ_COMB_WRITE_FIRST | I2C_AQ_COMB_SAME_ADDR

A DDC EDID fetch is a 128-byte block read, which `i2c_check_quirks()` rejects
outright before any hardware access:

    $ sudo i2ctransfer -y -f 8 w1@0x50 0x00 r128
    Error: Sending messages failed: Operation not supported

So `drm_edid_read_ddc(connector, adapter)` cannot work here. The EDID reader
must issue its own per-offset reads and be handed to `drm_edid_read_custom()`,
the same way `it66121_get_edid_block()` does for the HDMI bridge.

Worth noting: going through the *i2c_adapter* is doubly wasteful, because
`fl2000_i2c_xfer()` reads a full dword from the hardware and then discards
three of the four bytes. Calling `fl2000_i2c_dword()` directly gets 4 bytes per
transaction — 32 transactions per 128-byte EDID block instead of 128.

Only EDID blocks 0 and 1 are reachable: `i2c_offset` in `FL2000_VGA_I2C_SC_REG`
is 8 bits, so offsets 0..255. There is no E-DDC segment pointer support.

### 2. The I2C master appears to need chip init first

Single-byte reads pass the quirk check but fail against the hardware:

    $ sudo i2ctransfer -y -f 8 w1@0x50 0x00 r1
    Error: Sending messages failed: Input/output error

`fl2000_i2c_dword()` returns `-EIO` on either a poll timeout or a nonzero
`i2c_status` (a NAK). Most likely cause: `fl2000_reset()` and
`fl2000_usb_magic()` are only called at the *end* of `fl2000_drm_bind()`, which
never runs on a VGA dongle. The chip is therefore sitting in its power-on state
and its I2C master / monitor-detect circuit was never enabled.

`fl2000_usb_magic()` is what sets `monitor_detect` and `edid_detect` in
`FL2000_VGA_I2C_SC_REG`. Chip init must be moved ahead of any EDID attempt.

This is a hypothesis, not yet confirmed — it is the first thing to verify in the
VGA bring-up work.

## Useful register offsets

    FL2000_VGA_STATUS_REG    0x8000
    FL2000_VGA_I2C_SC_REG    0x8020   addr:7 cmd:1 offset:8 vga_status:8
                                      i2c_status:4 monitor_detect:1 i2c_ready:1
                                      edid_detect:1 i2c_done:1
    FL2000_VGA_I2C_RD_REG    0x8024
    FL2000_VGA_I2C_WR_REG    0x8028
    FL2000_RST_CTRL_REG      0x8048
    FL2000_USB_LPM_REG       0x0070
    FL2000_USB_CTRL_REG      0x0078

## Debugging notes

- regmap debugfs exists at `/sys/kernel/debug/regmap/2-4/` but is of limited
  use: `registers` is read-only and dumping it would issue ~16384 USB control
  transfers (`max_register = 0xFFFF`, stride 4). There is no register-write
  file. Add `dev_dbg()` to the driver instead.
- The driver is almost entirely silent by default. Enable dynamic debug:
      echo 'module fl2000 +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
- `fl2000.ko` depends on `drm_dma_helper`; `modprobe drm_dma_helper` first when
  using `insmod`.
