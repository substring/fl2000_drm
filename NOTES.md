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

## CRITICAL — devres lifetime bug makes rmmod/insmod unsafe

**This must be fixed before any further bring-up work. It breaks the debugging
loop itself.** This is almost certainly what is behind klogg issues #12 / #33.

Observed: after one `rmmod fl2000` + `insmod fl2000.ko` cycle, opening the
FL2000 i2c bus from userspace oopsed the kernel:

    BUG: unable to handle page fault for address: ffffffffc2b72af0
    #PF: supervisor write access in kernel mode
    Oops: 0003 [#1] SMP NOPTI
    RIP: 0010:try_module_get+0x28/0xe0
    Call Trace:
     ? i2c_get_adapter+0x37/0x70
     i2cdev_open+0x1f/0xd0
     chrdev_open+0xba/0x250
    Modules linked in: fl2000(OE) ... [last unloaded: fl2000(OE)]

The faulting address is in module space, and `i2c_get_adapter()` was doing
`try_module_get(adapter->owner)` where `adapter->owner` is `THIS_MODULE` of the
*already unloaded* module.

### Root cause

Both the i2c adapter and the regmap are attached to the USB **device**, not to
the USB **interface** the driver actually binds to:

    fl2000_i2c.c:    devres_add(&usb_dev->dev, adapter);
    fl2000_registers.c: devm_regmap_init(&usb_dev->dev, ...)
    fl2000_drv.c:    devm_kzalloc(&usb_dev->dev, ...) and
                     dev_set_drvdata(&usb_dev->dev, devs)

devres attached to `usb_dev->dev` is released when the *usb_device* goes away —
i.e. on physical unplug — **not** when the driver's interfaces disconnect on
`rmmod`. So on module unload `fl2000_i2c_adapter_release()` never runs,
`i2c_del_adapter()` is never called, and the adapter stays registered in the
i2c core with `owner` and `algo` pointing into freed module text.

Two consequences, both observed:

1. The stale i2c bus (`i2c-8`) outlives the module. Any access to it —
   `i2cdetect`, `i2ctransfer`, or a DDC read from another driver — dereferences
   freed module memory and oopses. It is a landmine left behind by `rmmod`.
2. On reload, `dev_get_drvdata(&usb_dev->dev)` still returns the **old** `devs`
   pointer, so `fl2000_probe()` skips `fl2000_get_devices()` entirely and
   silently reuses the stale adapter and stale regmap. No new i2c bus is
   created (confirmed: still only `i2c-0`..`i2c-8`, no `i2c-9` after reload).
   Every register access then calls through freed function pointers.

This also means the "clean" first `rmmod`/`insmod` cycle in this session was not
clean at all — it had already left the stale adapter behind. The oops merely
made it visible.

### Fix direction

Move the per-device resources off `usb_dev->dev`. Options, roughly in order of
preference:

- Anchor devres to the interface the driver actually binds (the AVCONTROL
  interface, `&interface->dev`), so unbind/rmmod tears it down properly.
- Or drop devres for the adapter and regmap and manage them explicitly:
  allocate on first probe, `i2c_del_adapter()` in `fl2000_disconnect()` when the
  last interface goes away.

Either way `dev_set_drvdata()` must be cleared on disconnect so a reload does
not pick up a stale `devs`.

Until this is fixed: **do not rmmod.** Reboot between test cycles, or physically
unplug the dongle before unloading (unplug does release the usb_dev devres).

## Phase 2c — VGA DAC init needs no porting

The plan assumed DAC init and VGA mode-setting registers would have to be
lifted from the old FrescoLogic driver. Comparing the two, they do not.

The FL2000's output stage is natively VGA — the register block is named
`FL2000_VGA_*` throughout and carries `dac_output_en`, `vga565_mode`,
`vga332_mode`, `vga_color_palette_en`, `force_vga_connect`. On an HDMI dongle
the IT66121 sits *downstream* of that same parallel output and converts it.
So klogg's driver was already programming the VGA path all along; the bridge
was the only VGA-unaware part.

Concretely, the old driver's DAC enable is:

    fl2000_monitor.c:410   // Set bit 7, Enable external DAC.
                           fl2000_reg_bit_set(dev_ctx, REG_OFFSET_8004, 7);

`REG_OFFSET_8004` is `FL2000_VGA_CTRL_REG_PXCLK` (`0x8000 + 0x04`), and bit 7
of that register is `dac_output_en` in klogg's `union
fl2000_vga_cntrl_reg_pxclk`. `fl2000_set_pixfmt()` already sets it. Likewise
`fl2000_set_pll()` already sets `force_vga_connect`, and `fl2000_set_timings()`
already writes the H/V sync registers (`0x8008`/`0x800C`/`0x8010`/`0x8014`) that
the old driver writes at `REG_OFFSET_8008` onward.

Every bit the old driver touches in `0x8004` (0, 6, 7, 24, 25, 26, 27, 28, 31)
is either already handled by `fl2000_set_pixfmt()` or belongs to a colour mode
this driver does not offer. Notably `vga_timing_en` (bit 8) is set by *neither*
driver, so it is not required.

Conclusion: no register porting needed. The remaining risk for first picture is
in PLL/timing selection and the streaming path, not DAC bring-up.

## Round 1 — devres lifetime fix validated on hardware

Run after a clean reboot, module built from commit "Fix devres lifetime...".
Two full unload/reload cycles, no `vga_mode`. **Pass.**

Baseline was `i2c-0`..`i2c-7` (amdgpu + i801), no fl2000.

    insmod  -> i2c-8 = FL2000 bridge I2C bus     appears
    rmmod   -> i2c-8 GONE                         <- the fix
    insmod  -> i2c-8 recreated, full probe
    rmmod   -> i2c-8 gone
    insmod  -> i2c-8 recreated
    rmmod   -> i2c-8 gone

dmesg for the sequence (loaded with `dyndbg=+p`):

    [ 254.848457] usb 2-4: Created FL2000 bridge I2C bus 8 at interface usb-0000:00:14.0-4
    [ 254.861339] usbcore: registered new interface driver fl2000_usb
    [ 273.006128] usbcore: deregistering interface driver fl2000_usb
    [ 274.048528] usb 2-4: Created FL2000 bridge I2C bus 8 at interface usb-0000:00:14.0-4
    [ 274.060010] usbcore: registered new interface driver fl2000_usb
    [ 276.070107] usbcore: deregistering interface driver fl2000_usb

Two things worth calling out as evidence, beyond "no crash":

1. **The adapter actually disappears on rmmod.** Before the fix it stayed
   registered with `->owner` pointing into freed module text, which is what
   oopsed `try_module_get()` from `i2cdev_open()`.
2. **`Created FL2000 bridge I2C bus 8` is printed again on every reload.** That
   line lives in `fl2000_i2c_init()`, reachable only via
   `fl2000_get_devices()`. Under the old bug the reload found a stale `devs` in
   drvdata and skipped device creation entirely, so this line was absent and
   the driver silently reused the dead adapter. Its reappearance is positive
   proof of a genuine fresh probe rather than reuse.

State afterwards: `fl2000` unloaded, no oops/BUG/Call Trace in dmesg (count 0),
`/proc/sys/kernel/tainted` = 12288, i.e. exactly `TAINT_OOT_MODULE (4096)` +
`TAINT_UNSIGNED_MODULE (8192)` and no crash bits.

Note on D-state kworkers: a `kworker/u112:* events_unbound` flickers into D
throughout, including at baseline *before* fl2000 was ever loaded, and it
reports as `gfx_0.0.0` when idle. That is amdgpu background work, unrelated to
this driver. Do not mistake it for the symptom seen in the oops session.

Tip: load with `insmod ./fl2000.ko dyndbg=+p` to get dev_dbg output from the
very start of probe. Enabling dynamic debug afterwards via
`/sys/kernel/debug/dynamic_debug/control` is too late to catch probe.

## Round 2 — raw DDC over the chip's I2C master (probe_only)

Built from "Add probe_only, force EEPROM ops, stop failing on i2c_status".
`insmod ./fl2000.ko probe_only=1 dyndbg=+p` gives i2c-8 live, refcount 0, no
DRM device — so rmmod/insmod iterates freely with no compositor involvement.

### Bit 16 verdict: NOT set at power-up. The SPI theory is wrong.

First transfer after load, address 0x50 offset 0x00:

    usb 2-4: i2c rd addr 0x50 off 0x00: pre 0xcf0000d0 mask 0xdf03ffff val 0x500000d0
    usb 2-4: i2c rd addr 0x50 off 0x00: status 0xf, continuing
    usb 2-4: i2c rd addr 0x50 off 0x00 ok: post 0xcf0000d0

Decoding `pre = 0xcf0000d0`, bits 16-23 are `0x00`, so **`i2c_spi_op` (bit 16)
and `i2c_spi_erase` (bit 17) were already clear**. Transfers were never being
diverted to SPI. Clearing them explicitly is still correct (they are control
bits that were in no write mask, so the old code depended on luck), but it was
not the cause of the DDC failure.

`mask = 0xdf03ffff` confirms bits 16/17 are now covered, and `val` has them
clear, so the fix does what it claims.

Other fields of `pre`/`post = 0xcf0000d0`:

    i2c_status     bits 24-27 = 0xF   all four bytes failed (1 = failed per byte)
    monitor_detect bit 28     = 0     reads back 0 even though we just wrote 1
    i2c_ready      bit 29     = 0
    edid_detect    bit 30     = 1
    i2c_done       bit 31     = 1     transfer completed

`monitor_detect` reading back as 0 is expected, not a fault: the Fresco Logic
driver has the same behaviour and comments on it at `fl2000_i2c.c`, forcing the
bit on every read with `read_back_data |= 0x10000000` under "BUG: Bit 28 always
return zero."

### The I2C master works. 0x50 is a genuine NAK.

Full bus scan, addresses 0x08-0x77 (112 addresses), one single-byte read each:

    111 addresses NAK   post = 0xcf0000xx, i2c_status = 0xF
      1 address  ACKs   0x30, post = 0xc00000b0, i2c_status = 0x0

0x30 is not a fluke. It answers at multiple offsets, always with status 0, and
returns varying data rather than a constant:

    0x30 off 0x00 -> 0x08    post 0xc00000b0
    0x30 off 0x04 -> 0x92    post 0xc00004b0
    0x30 off 0x08 -> 0x00    post 0xc00008b0
    0x30 off 0x10 -> 0x00    post 0xc00010b0

So the controller can address a device, clock a transfer and collect an ACK.
Register programming, address handling and offset handling are all correct.
The failure at 0x50 is the monitor's DDC EEPROM not answering, not a driver
bug. Reads at 0x50 return a constant `0xf1`, which is stale/garbage from
`FL2000_VGA_I2C_RD_REG` rather than sink data.

Note 0x30 sits on the dongle's own I2C, ahead of the VGA connector — probably a
configuration EEPROM. It is not the sink.

### Consequence: i2c_status is meaningful after all

The relaxation of the `i2c_status` check in this commit was justified by the
reference driver, which waits only on the done bit and never inspects the
status nibble. The scan shows that was the wrong lesson to draw: `i2c_status`
is a clean, reliable ACK/NAK signal on this hardware — 0x0 on the one device
that answers, 0xF on all 111 that do not. klogg's original hard check was
right and the reference driver is the sloppy one here.

The check should be restored once DDC works, because `detect()` needs exactly
this signal to tell connected from disconnected. Leaving it relaxed means EDID
reads return garbage that only gets caught later by header/checksum validation.

### Where this leaves us

Nothing in the driver explains a NAK at 0x50 while 0x30 ACKs on the same bus
with the same code path. The remaining suspects are all outside the driver:

- VGA cable with no DDC wiring. Pins 12 (SDA) and 15 (SCL) are omitted on many
  cheap or older VGA cables, and on most VGA-to-DVI adapters.
- Monitor in a standby state deep enough to unpower its EDID EEPROM.
- A dongle that does not wire the VGA connector's DDC pins to the chip's I2C
  master at all. Some low-cost FL2000 VGA dongles skip this.

## Debugging notes

- regmap debugfs exists at `/sys/kernel/debug/regmap/2-4/` but is of limited
  use: `registers` is read-only and dumping it would issue ~16384 USB control
  transfers (`max_register = 0xFFFF`, stride 4). There is no register-write
  file. Add `dev_dbg()` to the driver instead.
- The driver is almost entirely silent by default. Enable dynamic debug:
      echo 'module fl2000 +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
- `fl2000.ko` depends on `drm_dma_helper`; `modprobe drm_dma_helper` first when
  using `insmod`.
