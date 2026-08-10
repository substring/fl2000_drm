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

## Round 3 prep — hunting the missing DDC enable

New fact from the user: dongle + monitor + cable work perfectly on Windows,
including detection by name at native resolution. EDID therefore works
end-to-end on this exact hardware. The 0x50 NAK is not physical; the Windows
driver performs some enable that our probe_only init does not, i.e. the DDC
path to the connector is gated at the chip level.

### Reference driver: every HW touch before its first EDID access

Traced through `fl2000_module.c` → `fl2000_dev_init()` (fl2000_dev.c) →
`fl2000_dongle_card_initialize()` (fl2000_dongle.c) →
`fl2000_monitor_manual_check_connection()` → `fl2000_monitor_vga_status_handler()`
→ `fl2000_monitor_plugin_handler()` → `fl2000_monitor_read_edid_dsub()`.
In order, on the VGA (no ITE chip) path:

| # | Reference action | In our probe_only? |
|---|---|---|
| 1 | `usb_set_interface(if 0, alt 1)` — workaround for FL2000's buggy descriptors; kernels ≥5.x ignore the redundant endpoints unless alt 1 is selected | **MISSING** |
| 2 | `0x0070` set bit 19, set bit 20 (reject U1/U2) | yes — `usb_magic()` `u1_reject`/`u2_reject` |
| 3 | `0x8048` set bit 15 (app reset, self-clear) | yes — `fl2000_reset()` `sw_reset` bit 15, +10 ms |
| 4 | I2C probes at 0x4C (`fl2000_hdmi_find_chip`) — NAK on a VGA dongle | skipped; harmless |
| 5 | `0x8020` set bit 30 **alone**, then `msleep(750)` (`DELAY_FOR_I2C_CONNECTION_ENABLE`) | **PARTIAL** — bit is written, but combined with bit 28 in one write, and no delay |
| 6 | `0x8020` set bit 28 (ext monitor detect enable) | yes — same combined write |
| 7 | `0x8088` clear bit 10 ("turn off hardware reset") | yes — `usb_magic()` `wakeup_clr_en = 0` (klogg's `FL2000_VGA_CTRL_REG_3` *is* 0x8088) |
| 8 | Read `0x8000`; proceed to EDID **only if** bit 0 `connected` is set | **MISSING** — we never read 0x8000 |
| 9 | `0x0078` clear bit 17 | yes — `usb_magic()` `wake_nrdy = 0` |
| 10 | `0x0070` reject U1/U2 again | yes (idempotent) |
| 11 | dword EDID reads at 0x50, offsets 0,4,8..., 10 ms apart | equivalent to our reads |

Two candidates initially suspected are in fact already covered: 0x8088 bit 10
and 0x0078 bit 17 are both cleared by `usb_magic()` (the 0x8088 grep miss was
because klogg computes the offset as `0x8000 + 0x88`).

Genuinely missing, in suspicion order:

- **A. Detect-enable sequencing (row 5):** reference sets the EDID/I2C detect
  bit *by itself*, waits 750 ms for the detection circuit to settle, and only
  then enables external monitor detect. We slam both bits in one write with no
  settle time. Note the elapsed-time argument cuts both ways: our manual
  rescans ran *minutes* after insmod and still NAKed, so a pure "wait 750 ms
  after enable" cannot be the whole story — but "bit 30 must be enabled alone
  before bit 28 joins" would not be cured by elapsed time.
- **B. `usb_set_interface(0, 1)` (row 1):** a USB-level SET_INTERFACE the
  chip's firmware observes, performed before any register work. If the
  firmware keys internal power/routing off the altsetting, no amount of
  register poking fixes it.
- **C. `0x8000` gating (row 8):** the reference never even attempts EDID until
  the chip's own monitor-detect circuit reports `connected` (bit 0). If the
  DDC pads are powered by that circuit, a chip that thinks no monitor is
  present may hold DDC dead. Reading 0x8000 costs nothing and adjudicates:
  bits of interest are 0 `connected`, 28 `edid_connected`, 29
  `ext_mon_connected`, 6 `pll_powered_up`, 7 `dac_powered_up` (reference
  `struct vga_status`, fl2000_interrupt.h).

Also checked and ruled out: the reference does **no** PLL/DAC/output-stage init
before its first EDID read — `fl2000_dongle_set_params()` runs much later. So
"DDC needs the output stage powered" is not supported by the reference code.

### Round 3 mechanism

`ddc_step` module param (int, cumulative), iterated via the probe_only loop:

    ddc_step=0   baseline, current behaviour
    ddc_step=1   A: set 0x8020 bit 30 alone, msleep(750), then usual init
    ddc_step=2   A+B: usb_set_interface(0, 1) first, then step 1

probe_only now always ends by reading `0x8000` and logging the decoded
monitor-detect status (C), so each step's rescan also shows what the chip
thinks is connected.

## Round 3 results — both software candidates eliminated; the chip agrees DDC is dead

Loop run with the ddc_step build (probe_only, rmmod/insmod between steps,
0x50 rescan after each):

| step | init variation | 0x8000 decode | 0x50 |
|---|---|---|---|
| 0 | baseline | `0x600000e1` connected **1** edid_connected **0** ext_mon **1** pll_up 1 dac_up 1 | NAK |
| 1 | + staged enable (bit 30 alone, 750 ms settle) | `0x200000c1` same flags | NAK |
| 2 | + `usb_set_interface(0, 1)` first | `0x200000c1` same flags | NAK |

0x30 control-read ACKed in every round, so the I2C master stayed healthy
throughout. With the restored NAK check, a NAKed read now surfaces as a clean
I/O error instead of returning bus garbage.

The 0x8000 decode is the significant new evidence:

- **`connected = 1`** — the chip's analog monitor-detect circuit (load sensing
  on the RGB lines) sees the monitor. Detection hardware works, dongle-to-
  monitor analog wiring works.
- **`edid_connected = 0`** — the chip's *own* EDID-detect circuit cannot see
  the DDC EEPROM. The chip independently reports exactly what our reads
  experience. This is not a register we forgot; the hardware itself observes
  no device on the DDC lines.

The 750 ms settle and the altsetting workaround made no difference to either
flag or to the reads.

### The device at 0x30

Dumped offsets 0x00-0x1f: `08 05 01 52 92 04 24 09 00 00 00 00 0a 00 ...` —
structured but not an EDID and not obviously monitor-side. 7-bit 0x30 happens
to be the E-DDC segment pointer address (8-bit 0x60), which would place it in
the *monitor*, but a segment pointer does not normally back a 16-byte memory.
More likely a dongle-internal configuration EEPROM. Discriminator (physical):
detach the VGA cable at the dongle and re-read 0x30 — if it stops ACKing it
was the monitor and the DDC wires are electrically alive after all.

### Leading remaining theory: monitor deep-standby DDC power-down

Windows works on this exact chain, but with a crucial difference in dynamics:
Windows lights the display *immediately* on plug-in, so the monitor never
enters deep standby. Under our probe_only testing the monitor has been sitting
for hours with no video signal. Many monitors keep their EDID EEPROM powered
from the DDC +5V line permanently, but some cut DDC power in deep sleep while
the passive 75 ohm terminations that trip the analog load detect stay
present — which would produce precisely the observed signature:
`connected = 1`, `edid_connected = 0`, EEPROM NAK.

This theory fits every observation and explains the Windows result without
requiring any undiscovered register. Test: wake the monitor (power-cycle it so
it is actively showing its "no signal" OSD) and rescan 0x50 within the first
seconds. If it ACKs, the "fix" is behavioural, not register-level: the driver
should retry EDID after enabling video output, and the EDID-less fallback
carries the first modeset.

## Round 4 — cable diffing: the chip does watch the connector, in a register nobody models

Physical tests run with the user's help, probe_only loop throughout.

### VGA cable detached at the dongle

- 0x30 still ACKs (`0x08`) → the device at 0x30 is **dongle-internal** (config
  EEPROM, most likely). The E-DDC segment pointer theory is dead.
- `0x8000` reads **bit-identical** to the attached state (`0x200000c1`,
  connected=1, ext_mon=1) → those bits are static under our init and do NOT
  track the connector. The earlier inference "the chip sees the monitor" was
  wrong; whatever those bits mean, they are not live presence.

### Monitor awake test

Cable re-attached, monitor powered on and actively showing its "Check Cable
Connection" OSD — the most DDC-favourable state possible. 0x50 still NAKs.
**The deep-standby theory is dead.**

### Raw register diff (dump_regs=1), cable attached vs detached

Full raw dump of 0x8000-0x80FF plus 0x0070/0x0078, bypassing the regmap cache
(usb_control_msg backend directly). Two dumps per state to separate counters
from state:

    0x8040  changes between same-state dumps          -> free-running counter, ignore
    0x805C  stable within state, tracks cable exactly:
                detached: 0x00000000
                attached: 0x00ffecbb

**0x805C is a live connector-tracking register modelled by neither driver.**
Byte pattern ff/ec/bb looks like three analog sense readings — plausibly the
raw output of the load-detect block on the RGB lines. So the detection
hardware is alive and the connector is electrically visible to the chip; only
the DDC/EDID side stays dead. (0x8024 = f1f1f1f1 in the dumps also confirms
0xf1 was fill garbage in the I2C read register, as suspected.)

### Where this converges: DDC +5V (VGA pin 9)

Facts now established simultaneously:
- I2C engine works (0x30 ACKs, structured data)
- analog sense path works (0x805C tracks the cable)
- monitor is awake and Windows-proven on this exact chain
- EEPROM at 0x50 NAKs, and the chip's own EDID-detect agrees

The one element that would produce exactly this split is the **DDC +5V supply**
on VGA pin 9: the host powers the monitor's EDID EEPROM through it (that is
why EDID normally works even on an unpowered monitor — and conversely, a
monitor whose EEPROM expects pin-9 power will NAK even when awake if pin 9 is
dead). If the dongle gates +5V through a FET on an FL2000 GPIO which the
Windows driver sets, everything observed fits. Neither Linux driver contains
the string "gpio"; the FL2000's GPIO registers are in space neither driver
touches, so the enable cannot be found by reading their code.

### Next: Windows USB capture (decisive)

All software candidates derivable from the two Linux codebases are exhausted.
The Windows driver demonstrably performs the missing enable, and every
register access it makes is visible on the wire as a vendor control transfer
(bRequest 64 = read, 65 = write, register offset in wIndex, 4-byte value).
A USBPcap/Wireshark capture on the Windows machine from dongle plug-in to
desktop-on-VGA contains, by construction, the complete working init sequence.
Diffing its writes against ours is a mechanical lookup that ends the guessing.

## Debugging notes

- regmap debugfs exists at `/sys/kernel/debug/regmap/2-4/` but is of limited
  use: `registers` is read-only and dumping it would issue ~16384 USB control
  transfers (`max_register = 0xFFFF`, stride 4). There is no register-write
  file. Add `dev_dbg()` to the driver instead.
- The driver is almost entirely silent by default. Enable dynamic debug:
      echo 'module fl2000 +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
- `fl2000.ko` depends on `drm_dma_helper`; `modprobe drm_dma_helper` first when
  using `insmod`.
