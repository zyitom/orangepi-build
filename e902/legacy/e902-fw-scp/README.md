# A733 E902 firmware

Custom bare-metal firmware for the RISC-V E902 core in the A733's CPUS
domain, replacing the vendor `scp.fex`. First version: S_UART0 console plus
bidirectional MSGBOX messaging with the ARM cores.

## Status

Builds clean with the Xuantie toolchain: 2896 bytes, no relocations, six
sections, and the first 16 bytes are byte-identical to the vendor `scp.fex`
entry sequence.

**The firmware itself has still not been started on hardware.** What *has* been
verified on a live board (2026-07-29, Orange Pi Zero 3W, stock 6.6.98-sun60iw2):

- the vendor SCP's real load address, `0x40004000` — see "Resolved" below
- the E902↔ARM SRAM_A2 address translation, and that the old scripts would have
  corrupted kernel memory instead of loading anything
- `awdevmem.py` read, write (with read-back verify) and range refusal, exercised
  against an idle region of SRAM_A2 above the vendor image and then restored
- `check-e902.sh` end to end

The load/start sequence in "Verify on hardware" is still untested: running it
overwrites the middle of the live vendor SCP, so it needs a console on PL2/PL3
attached first.

Design record and the full set of findings: `../doc/e902/DESIGN-NOTES.md`.

## Layout

```
src/a733.h     SoC register definitions (E902 view)
src/fw.h       cross-module prototypes
src/start.S    reset entry, .data/.bss init, mtvec/mtvt, CLIC vector table
src/fw.ld      link script, image at 0x40014000 in SRAM_A2
src/soc.c      clock/reset bring-up; runtime APBS1 rate detection
src/pinmux.c   PL2/PL3 -> S_UART0 mux (with a race warning, read it)
src/uart.c     S_UART0 driver (16550-compatible)
src/msgbox.c   MSGBOX transport to ARM
src/clic.c     CLIC interrupt setup
src/main.c     banner, UART echo, message protocol

scripts/get-toolchain.sh    download the Xuantie bare-metal toolchain
scripts/awdevmem.py         SRAM/register access with E902->ARM translation
scripts/e902-load.sh        load+start firmware from Linux, no reboot
scripts/e902-restore.sh     put the vendor scp.fex back
scripts/arm-msgbox-test.c   ARM-side poker via /dev/mem
```

`awdevmem.py` must be copied to the board alongside the two shell scripts —
they call it, and it is what keeps SRAM writes off DRAM. If you only have a
serial console, `doc/e902/serial-put.py` copies files over it.

## Build

Needs the T-Head Xuantie **elf-newlib** (bare-metal) GNU toolchain. The
`linux-glibc` builds cannot target RV32E.

```sh
./scripts/get-toolchain.sh          # ~1 GB, into ../toolchains/
make CROSS=<toolchain>/bin/riscv64-unknown-elf-
```

Produces `build/fw.bin`, loads at `0x40014000`.

The A733's E902 is **RV32EMC** — the vendor `scp.fex` contains 87
`mul`/`div`/`rem` instructions, so the M extension is present. Hence
`-march=rv32emc_zicsr -mabi=ilp32e` (the `_zicsr` suffix is required since
binutils 2.36 — `start.S` uses `csrw`/`csrr`).

## How this fits together

```
Linux (ARM)                          E902
    |                                  |
    |  MBOX_CPUS 0x07094000  ------->  | RX, CLIC irq 48
    |  <------- MBOX_CPUX 0x03004000   | TX
    |                                  |
    |                          S_UART0 0x07080000 -> PL2 (TX) / PL3 (RX)
```

Message format, 32 bits (all the hardware carries):

| bits | meaning |
|------|---------|
| 31:24 | command |
| 23:0  | payload |

| cmd  | direction | meaning |
|------|-----------|---------|
| 0x01 | ARM -> E902 | PING, answered with PONG |
| 0x02 | E902 -> ARM | PONG |
| 0x10 | E902 -> ARM | HELLO, sent once at startup |
| 0x11 | E902 -> ARM | key pressed on the E902's UART |
| 0x20 | both | ECHO, payload returned unchanged |

## Prerequisites in the build tree

**Only one change is required**, and u-boot does not need to be touched:

1. `external/config/kernel/linux-sun60iw2-current-a733.config`
   - `CONFIG_AW_MSGBOX=y`, `CONFIG_AW_HWSPINLOCK=y` — the DTS nodes were
     already `okay` but no matching driver was compiled in.
   - `CONFIG_AW_DMC_DEVFREQ` off — see "What you give up".
   Original saved as `.config.bak-e902`.
   `AW_MSGBOX=y` is the load-bearing one: the DTS node was already `okay`
   but the only compiled-in mailbox driver was mainline `sun6i-msgbox.c`,
   which matches a different compatible and never bound. Enabling it also
   opens the `MBOX_CPUX` clock gate for us (see "Open questions").

2. `userpatches/kernel/sun60iw2-current/0001-*.patch` — comments only,
   recording that `uart7` *is* S_UART0. Safe to delete.

3. `userpatches/u-boot/u-boot-sunxi/0001-*.patch` — **optional**, and not
   needed for testing. It turns off `CONFIG_SUNXI_ARISC_EXIST` and
   `CONFIG_ARISC_DEASSERT_BEFORE_KERNEL` so bl31 never loads `scp.fex` at
   all. Use it only if you want your firmware to own the core permanently;
   it permanently gives up DFS and suspend, and rolling back means
   reflashing raw sectors, so keep a bootable backup image first.

## Borrow the core instead of taking it

Nothing in the device tree references `CLK_RISCV` or `RST_BUS_RISCV`, so no
Linux driver holds a reference to the E902's clocks or reset. That means the
core can be stopped, reloaded and restarted from a running system, behind
Linux's back — no reboot, no u-boot change:

```sh
sudo ./scripts/e902-load.sh build/fw.bin      # borrow
sudo ./scripts/e902-restore.sh /path/scp.fex  # give it back
```

The vendor `scp.fex` keeps loading normally at boot; you only lose DFS,
suspend and PMIC supervision for as long as your firmware is running. Copy
`u-boot/v2018.05-sun60iw2/scp.fex` onto the board so you can restore it.

Caveat: on restore the SCP starts cold, without the parameter block bl31
normally hands it (DRAM timings, DVFS voltage table, PMIC config — see
`u-boot/.../drivers/arisc/arisc_i.h`, `struct dts_cfg_64`). Basic services
should return; reboot for a clean vendor-managed start if they do not.

**Do not suspend while your firmware is running.**

## What you give up

The stock `scp.fex` runs DRAM DFS, suspend support, PMIC supervision and
clock management. Replacing it loses all of it. Each failure is soft —
error returns, not crashes — because Linux reaches the SCP through SMC
calls that simply fail:

| lost | mechanism | severity |
|------|-----------|----------|
| DRAM frequency scaling | `ccu-ddr.c:77` `ARM_SVC_SUNXI_DDRFREQ` | performance only; disabled `AW_DMC_DEVFREQ` so it stops retrying every 100 ms |
| suspend-to-RAM | `irq-sunxi-wakeupgen.c:49` `SET_WAKEUP_SRC`; E902 is the only core awake across STR | `echo mem > /sys/power/state` will not resume |
| extra PMIC watching | E902 polls AXP515/AXP8191 over `s_twi0` | Linux's own AXP drivers still manage regulators |

**CPU** cpufreq is unaffected: `ccu-sun60iw2-cpupll.c` programs the PLLs
directly, no SMC involved.

## Verify on hardware

Untested — this is the intended sequence.

Wire up the console first. On the 40-pin header:

| header pin | net | signal |
|-----|-----|--------|
| 16 | PL2 | `SCPU-TX` — connect to your adapter's RX |
| 18 | PL3 | `SCPU-RX` — connect to your adapter's TX |
| 6/9/14/20/25/30 | GND | any |

115200 8N1, 3.3 V logic. These are the pins the board vendor reserved for the
SCP console (schematic sheet 18, `18_EX_GPIO`) — separate from the CPU debug
UART on PB4/PB5.

```sh
# 1. baseline, before changing anything
sudo sh ../doc/e902/check-e902.sh | tee before.txt

# 2. build+install the kernel package (u-boot untouched), reboot
#    ./build.sh BOARD=orangepizero3w BRANCH=current BUILD_OPT=kernel

# 3. confirm the vendor SCP is running, and keep a copy to restore later
sudo python3 scripts/awdevmem.py read 0x0701021C  # expect 0x00010003
sudo python3 scripts/awdevmem.py read 0x07032204  # expect 0x40004000 (vendor entry)

# 4. load our firmware -- wait until Linux has finished booting first,
#    so its pinctrl probing cannot race our PL_CFG0 write
sudo ./scripts/e902-load.sh build/fw.bin

# 5. watch pin 16 at 115200 8N1 for the banner

# 6. from the ARM side
gcc -O2 -o /tmp/amt scripts/arm-msgbox-test.c
sudo /tmp/amt ping        # expect PONG back
sudo /tmp/amt             # listen; press 's' on the E902 UART

# 7. hand the core back when done
sudo ./scripts/e902-restore.sh /path/to/scp.fex
```

### If the banner does not appear

- **`PL_CFG0` shows `MISMATCH`** — something else is writing the register.
  Load the firmware later in boot, after Linux has finished probing.
- **Garbled output** — the APBS1 rate detection guessed wrong. `soc.c`
  reads `R_APBS1_CLK_REG` (`0x07010010`) and decodes mux + divider; compare
  the printed value against that register's actual contents.
- **Nothing at all** — check `E902_RST_START_ADDR` (`0x07032204`) reads
  back `0x40014000` and `RISCV_BGR` (`0x0701021C`) reads `0x00010003`.
  Order matters: the manual (5.2.4.8) requires the start address be set
  *before* reset is released. If it still reads `0x40004000`, the vendor
  firmware is what's running — your load did not take effect.

## Open questions

- `MBOX_CPUX` at `0x03004000` is in the CPUX power domain; its clock gate
  is in the main CCU (`0x02002000 + 0x0744`). `soc.c` tries to enable it,
  but the main CCU may be secured against E902 writes. This should not
  matter: `sunxi_msgbox_hw_init()` (`sunxi-msgbox.c:825`, called from probe
  at line 884) unconditionally deasserts the reset and enables the clock,
  with no mailbox client required — so with `CONFIG_AW_MSGBOX=y` the gate is
  already open. If TX fails while RX works, check that the driver actually
  bound to `msgbox@3004000`.

  **Resolved.** On the stock image nothing was bound: `msgbox@3004000`
  advertises `allwinner,sun60iw2-msgbox` and `hwspinlock@3005000` advertises
  `allwinner,sunxi-hwspinlock`, but the shipped kernel had
  `CONFIG_AW_MSGBOX`/`CONFIG_AW_HWSPINLOCK` unset and only registered
  `sun6i-msgbox`, which matches neither. After building and installing this
  tree's kernel (`BUILD_OPT=kernel`, both symbols `=y`), both bind:

  ```
  3004000.msgbox      -> sunxi-msgbox
  3005000.hwspinlock  -> sunxi-hwspinlock
  dmesg: sunxi-msgbox 3004000.msgbox: ... sunxi msgbox probe success
  ```

  The driver lives in `bsp/drivers/msgbox/`, not `drivers/mailbox/` — don't
  look for `drivers/mailbox/sunxi-msgbox.o` to confirm the build.

  Note: installing the kernel deb over the stock one reuses version `1.0.0`
  and overwrites the only `/boot/uImage`, with no fallback entry. Back up
  `/boot/uImage`, `/boot/dtb-*`, and `/lib/modules/*` first. Also, the board
  did **not** come back from `reboot` after the install — it needed a cold
  power cycle. Budget physical access for that.

## Verified: the ARM side of MSGBOX works

Measured with `scripts/arm-msgbox-test.c` while the vendor `scp.fex` was
running (so nothing implements our protocol on the far side):

- Both mailbox blocks map and read cleanly through `/dev/mem`:
  `MBOX_CPUX 0x03004000` (RX) and `MBOX_CPUS 0x07094000` (TX).
- Writing a message raises TX `msg_status` from `0x0` to `0x1`, and it
  **stays** there across repeated polls. Sending more increments the count.
  At 8 queued messages `fifo` reads `0x1` (full), so the FIFO is 8 deep.

That is exactly the signature of a working transmit path with nobody
consuming: the hardware accepts and holds our messages. It also independently
confirms the E902 is not executing — a running responder would drain the FIFO.

`amt` takes an optional `-t <seconds>` to bound the listen loop for scripting;
it exits 0 if any message arrived, 2 on an empty timeout.

Caution when reading its output over `ssh -c`: the first line can be lost to
the session. Check via the serial console if a line seems missing.

## Resolved: where the vendor firmware actually lives

Measured on hardware (2026-07-29). bl31 loads `scp.fex` at **`0x40004000`**,
not `0x40014000`:

- `E902_RST_START_ADDR` (`0x07032204`) reads `0x40004000` on a running system.
- The 105912 bytes of SRAM_A2 at that address match
  `u-boot/v2018.05-sun60iw2/scp.fex` byte for byte, except 10 bytes of runtime
  scratch in `0x40010738..0x40010767`. The first `0xC738` bytes hash identically
  to the file.

This retires the old "unresolved contradiction" about the vendor stack being at
`0x4003f000`, past the end of SRAM_A2. That figure came from disassembling at
the wrong vma. At the correct base the prologue reads:

```
40004026: auipc sp,0x2b      ┐  sp = 0x4002F000 — inside SRAM_A2,
4000402a: addi  sp,sp,-38    ┘  and above the image end at 0x4001DDB8
```

The manual's memory map was right all along.

Consequence for this firmware: `0x40014000` is **inside** the vendor image
(offset `0x10000` of `0x19DB8`), so loading here overwrites the middle of the
running vendor SCP. Restoring means writing the *whole* `scp.fex` back to
`0x40004000` — which is what `e902-restore.sh` now does.

## Two addresses for one SRAM

`0x40000000` means different things depending on which core is asking:

| view | SRAM_A2 | what `0x40000000` is |
|------|---------|----------------------|
| E902 (manual ch.2, `fw.ld`) | `0x40000000..0x40033FFF` | SRAM_A2 |
| ARM (`SUNXI_SRAM_A2_BASE`, u-boot `cpu_autogen.h:5`) | `0x00040000` | base of DRAM |

`ARM = 0x00040000 + (E902 - 0x40000000)`, so E902 `0x40014000` is ARM
`0x00054000`.

Earlier versions of these scripts used the E902 address as an ARM-side
`/dev/mem` offset. On this board that lands in unreserved kernel memory
(`/proc/iomem`: `40000000-13fffffff : System RAM`; kernel code starts at
`0x41010000`), and `CONFIG_STRICT_DEVMEM` is off, so the kernel permits the
write: `dd` returns 0, the E902 receives nothing, and 128K of live pages get
overwritten — with delayed, misleading symptoms. `scripts/awdevmem.py` does the
translation, refuses addresses outside SRAM_A2, and reads back what it wrote.

Two more constraints it handles, both measured: `/dev/mem` `lseek`+`read`/`write`
fails with `Bad address` on non-System-RAM, so SRAM access must go through
`mmap`; and byte-wise `memcpy` on that mapping raises `SIGBUS` unless the length
is 16-byte aligned (`0x19DB8` and `0x19DB4` fault, `0x19DB0` and `0x19DC0` do
not), so it copies 32 bits at a time.

Neither `busybox` nor `devmem2` is installed on the stock image, and neither
could do this safely anyway. `python3` is present, so `awdevmem.py` needs no
new packages.

## Reverting

If you only used `e902-load.sh`, nothing persistent changed — restore the
vendor firmware and you are done:

```sh
sudo ./scripts/e902-restore.sh /path/to/scp.fex   # or just reboot
```

To undo the build-tree changes as well:

```sh
cp ../external/config/kernel/linux-sun60iw2-current-a733.config.bak-e902 \
   ../external/config/kernel/linux-sun60iw2-current-a733.config
rm -f ../userpatches/kernel/sun60iw2-current/0001-a733-zero3w-*.patch
rm -f ../userpatches/u-boot/u-boot-sunxi/0001-sun60iw2-do-not-start-E902-SCP.patch
```

Then rebuild. If you had applied the u-boot patch, you must reflash u-boot
(`BUILD_OPT=u-boot` then `platform_install.sh`) for the stock `scp.fex` to
load again.
