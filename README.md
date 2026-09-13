# What's new — key differences from upstream sd2psXtd

This firmware is a fork of [sd2psXtd](https://github.com/sd2psXtd/firmware) 1.4.0, focused on
PS1 use. If you already know sd2psXtd, this page is the only one you need: it covers what
behaves **differently**, not what stayed the same.

Everything below is about PS1. PS2 support is still compiled in and untouched.
This has been tested on a Bitfunx Psxmemcard PS1 Memory Card - for Sony PlayStation 1/PS One, RP2040 Based.
Built firmware to download are in the `firmware-built` directory, check the pinout section to know what to download (for the above card use `psxmemcardmsc.uf2`)

---

## At a glance

| | What changed | In one line |
|---|---|---|
| 1 | [microSD passthrough over USB](#1-microsd-passthrough-over-usb) | Plug the card into a PC and the microSD shows up as a USB drive — no card reader, no disassembly. **Steady white LED** while the drive is mounted. |
| 2 | [New pad shortcut](#2-new-pad-shortcut-select--l1--l2) | `SELECT + L1 + L2` opens a **mode** the d-pad drives — reachable **with one hand** — and a **steady magenta LED** reminds you you're in it. |
| 3 | [RGB LED colour scheme](#3-rgb-led-colour-scheme) | Seven states, seven colours, dark when idle. **Yellow blinks tell you which card and channel** you landed on. |
| 4 | [BootCard is read-only and self-exiting](#4-bootcard-read-only-out-of-the-card-list-self-exiting) | A **special slot reserved for booting**: **steady green LED** whenever it's mounted, off the browse list, write-protected, and it leaves on its own once boot is done. |
| 5 | [New serial commands](#5-new-serial-commands) | `status`, `card boot`, `start emulation` — and the serial port stays alive while the PC has the drive mounted. |
| 6 | [Folder tree created automatically](#6-folder-tree-created-automatically) | A blank microSD comes back from first boot already laid out, with the settings file written — and **deleting `settings.ini` is now a real factory reset**, not a rewrite of the values you already had. |

---

## 1. microSD passthrough over USB

Plug the memory card into a PC with a USB cable and **the microSD appears as a normal USB drive**.
No card reader, nothing to take apart — drag your saves on and off. The device is composite, so
the PC also gets a serial port (`COMx`, `/dev/ttyACM0`) at the same time; VID `CAFE`, PID `4003`.

Two things are worth knowing:

**The LED is steady white the whole time the drive is mounted** (§3), so passthrough is never
something you have to guess at. Host reads and writes blink cyan and blue on top of it.

**Ejecting does not start card emulation.** At power-up the firmware looks for a USB host for
3 seconds: if the cable is connected it goes into drive mode and emulation never starts. Getting
out of that depends on how you do it:

| | Emulation |
|---|---|
| **Unplug the cable** (which powers the board off) | starts by itself **3 seconds** after the console powers it back up — that is the window it spends looking for a USB host, and it is the normal, everyday case |
| **Eject the volume, cable still attached** | **does not start** — the firmware waits for the `start emulation` command on the serial port (§5) |

(If the board is sitting in a powered-on console *and* on USB at the same time, pulling the cable
doesn't cut the power: there the firmware notices after about half a second and starts emulating
without a restart.)

That is deliberate. An eject is usually just an eject, and running the card while still tethered
to a PC **is useful only for debugging** — which is exactly what `start emulation` is for: it lets
you run the card in the console with the cable attached and read the log live. Doing it
automatically would be wrong; with `Autoboot` on it would even mount the boot card, which without
a console has no way out. No cable, on the other hand, means there is no PC left to serve, so
there emulation has to start by itself.

After an eject the LED goes off — no reboot, and **no red**, because ejecting is not an error.
To get back into drive mode, unplug and replug the cable.

> **Known limitation on SD2PSX hardware:** while the drive is mounted the OLED keeps showing its
> last frame, because drawing the menu would mean reading the microSD, which at that moment
> belongs to the PC.

---

## 2. New pad shortcut: `SELECT + L1 + L2`

**The upstream combo requires two hands.** This one is a **mode**, and its three buttons were
chosen so the whole thing can be done **with one hand**: SELECT, L1 and L2 all sit under the left
hand, which then keeps one of them held while the thumb works the d-pad.

**To enter the combo mode:** press **SELECT + L1 + L2** together.

**To stay in the combo mode:** keep **at least one** of the three held. You can let go of SELECT and keep just
the two triggers, or even just one. You leave the combo mode when you release **all three**.

**While you're in the combo mode**, the d-pad drives the card:

| Button | Action |
|---|---|
| **Up** | next card — `Card1` → `Card2` → `Card3`, up to `MaxCardIdx` |
| **Down** | previous card — `Card3` → `Card2` → `Card1`, and below that out of the numbered cards |
| **Right** | next channel |
| **Left** | previous channel |
| **START** | switch to the **BootCard**, if one exists |

While you stay in the combo mode you can chain several changes — up, up, right — without re-pressing
SELECT each time.

**The LED stays steady magenta the whole time you are in the combo mode**, and that is the other half
of the design: a mode you hold with one hand needs something to remind you it is open. It is not
a blink when you enter, it is a colour that *stays on*, so at any moment you can tell whether the
d-pad is driving the memory card or the game. Card changes blink **yellow** on top of the magenta
— those blinks tell you which card and channel you landed on (§3) — and then it returns to
magenta; when you release all three buttons the magenta goes out.

The shortcut needs `EnableControllerCombo=ON` in `settings.ini` (it is on by default). With it
off, the pad does nothing at all.

**Up/Down walk one single list**, not just the numbered cards:

```
named folders  →  Game ID card  →  Card1  →  Card2  →  Card3  →  …
   ←—— DOWN                                                UP ——→
```

Any subfolder of `MemoryCards/PS1/` that isn't `BOOT` or `CardN`, with a name under 16
characters, counts as a "named folder" — useful for themed cards (`Platform`, `RPG`, `Import`).
The BootCard is deliberately **not** in this list (see §4).

Left/Right stay **inside the current card** and change channel, from 1 to `MaxChannels`.

Which buttons do what is **not** configurable: they are fixed `#define`s and change only by
recompiling.

> Inside **UniROM**, the "Switch MCPro Channel / card" entry is a completely separate path: the
> console reads the pad itself and sends MMCE commands over the memory-card bus. There you don't
> hold SELECT, and `EnableControllerCombo` has nothing to do with it. Unchanged from upstream.

---

## 3. RGB LED colour scheme

For boards with an RGB LED. The whole scheme follows one rule, and it is the opposite of what
the original PicoMemcard firmware did.

**Before: an ugly red LED sitting on, permanently.** It was on when the card was idle, on when it
was working, on when something was wrong. A light that is always on tells you nothing.

**Now: everything off by default, and the LED lights up only when something actually happens.**
Nothing in progress means no light at all. So any light you see is a real event, and it is worth
looking at. That is the entire philosophy, and everything below follows from it: the colour tells
you *what* is happening, the rhythm tells you *how much* or *which one*, and darkness means the
card is simply doing its job.

The corollary is that the three states you might need to recognise *without* having seen them
start — pad mode, boot card, USB — get a colour that **stays on** rather than a blink, because a
blink you missed is a blink that never happened. Everything else blinks, briefly, and goes away.

There are seven states in all, one colour each.

| Colour | Meaning | Pattern |
|---|---|---|
| **off** | idle, all well — including waiting for `start emulation` after an eject (§1) | — |
| red → green → blue | **power-on**: the firmware started | sweep, ~0.4 s total |
| **white** | **ready**: microSD mounted, entering emulation | one short flash |
| **cyan** | **reading** from the memory card | slow blink (~2 Hz) |
| **blue** | **writing** to the memory card | fast blink (~8 Hz) |
| **magenta** | **pad mode active** (§2) | **steady** while you're in it |
| **yellow** | **card or channel change** | N blinks, see below |
| **green** | **boot card mounted** (§4) | **steady** while it's in use |
| **white** | **USB MSC mode**, connected to a PC | **steady** |
| **red** | **error** | the rhythm tells you which |

The startup sweep doubles as a self-test: if one of the three colours is missing, that LED
channel or its wiring has a problem. The sweep and the white "ready" flash are separated by the
microSD init and the USB host wait, so **if you see the sweep but never the white flash**, the
firmware started but stopped before being ready — look at the red.

**Yellow — which card you landed on.** Useful because these boards have no display:

| Selected card | Pattern |
|---|---|
| numbered `CardN` | **N short blinks** (Card3 → 3 blinks) |
| Game ID card | **2 long blinks** |
| named folder, position N | **3 long blinks + N short** |
| **channel change** | **N short blinks** = channel number |
| BootCard | **2 long green blinks**, then steady green |

**Red — errors**, told apart by rhythm rather than by counting:

| Situation | Pattern |
|---|---|
| **microSD missing** or not responding | **slow, regular** blink (1 s on / 1 s off) |
| **microSD unreadable** (unsupported filesystem) | **fast, regular** blink (~3 Hz) |
| **runtime or fatal error** | **N short blinks + a 1 s pause**, N = error code |

Error codes: 2 = `ERR_CARDMAN` (can't create card folders or files), 3 = `ERR_PSRAM`,
4 = `ERR_SETTINGS`, 5 = `ERR_CIV`, 6 = `ERR_MC_DATA`, 7 = `ERR_MC_AUTH_UNK` (PS2 only).
With a latched error, **activity still shows through**: between red blinks you still see the
cyan of reads and the blue of writes, because the card may well still be usable.

**The three steady colours** — magenta, green and white — are the states that must be
recognisable *at any moment*, not just when you enter them. A blink on entry is lost if you're
looking elsewhere, and at power-on there wouldn't be one at all. Card activity keeps blinking
**on top of** them: on the boot card, a read shows as cyan flashes over a green background.

**Priority**, highest first, because there is only one LED: blink sequence in progress → pad
mode (magenta) → the lit phase of an error pattern (red) → write (blue) → read (cyan) →
background colour (white in MSC, green on the boot card, otherwise off).

**If a microSD goes missing**, two different situations with different outcomes:

| | What happens |
|---|---|
| **Missing at power-on** | slow red; the firmware **retries the mount every second**. Insert it and it starts on its own with the RGB sweep — no need to unplug USB. |
| **Removed while the console is playing** | the firmware **can't tell** (these boards have no card-detect pin): writes fail, red signals the error, saves are lost. Re-inserting does **not** recover — restart it by hand. |

On PMC Zero the same scheme drives the WS2812 addressable LED, where colours are full 8-bit per
channel rather than on/off.

---

## 4. BootCard: read-only, out of the card list, self-exiting

`Autoboot` is **on by default**. At power-up, *if a BootCard exists*, the firmware serves the
console the card in `BOOT/` instead of the normal one. This is the FreePSXBoot path: the console
reads the BootCard, the exploit runs, and the payload talks to the firmware over the memory-card
bus to switch away from the boot image — no power cycling.

**The BOOT slot is not a memory card. It is a special slot, reserved for booting.** That is the
idea behind everything in this section, and it is worth stating plainly because upstream treats
`BOOT/` as just another folder in the rotation. Here it is not one of your cards: it is the
launcher, it has one job, and when that job is finished it gets out of the way. It is not a place
to keep saves and you should never end up in it by accident.

Every difference below is that one decision applied consistently — half of it for safety, half of
it simply so there is nothing to be confused about:

| The slot is special, so… | …which means |
|---|---|
| it is **not reachable with the d-pad** | scrolling Up and Down through your cards never lands on it. You get in on purpose or not at all, so you can never wonder "why is this card empty?" |
| it has a **dedicated colour, steady green** | at any moment you know whether the console is looking at the launcher or at a real card — and when the green goes out, the exploit has handed over |
| it is **read-only** | the console writes to it, as it writes to any card, and those writes are discarded. The payload cannot be eaten away by use |
| it **leaves on its own once boot is done** | the payload switches away when the game starts, and a timeout catches the case where it doesn't. You are not left sitting on the launcher |
| it is **not created automatically** | an empty boot card boots nothing, so the firmware never invents one — a missing file falls back to a normal card instead of silently pretending |

Point by point.

**It is not in the browse list.** Up and Down never land on it — it's a special card with an
exploit inside, and scrolling onto it by accident makes no sense. You get in on purpose only:
**START** inside the combo mode (§2), `card boot` on the serial port, or `Autoboot` at power-up.
Getting *out* with Up or Down works fine, and is the quickest way back to a normal card.

**It is write-protected, on two fronts.** While the boot card is mounted the console treats it
like any memory card and writes to it early, as soon as the BIOS probes it. Writes are dropped
both towards the microSD **and** in the RAM copy. The RAM copy is the one the console actually
reads from, and it is only reloaded from file when you change card: a failed boot attempt used
to scribble over it, and since the firmware is USB-powered and **a PS1 reset does not restart
it**, that damaged image was then served to every later boot attempt. From the console's point
of view the write still *succeeds* — it gets its `47h` acknowledgement — it just reads back
unchanged, which is exactly how a write-protected card behaves.

Without this, **a failed exploit attempt damaged the boot image itself.** The console would write
over it, the file was left corrupted, and from then on the exploit failed for a reason that had
nothing to do with the reason it failed the first time. Keep a copy of your boot image on the PC
anyway — from the PC side the file is writable like any other.

**It is not created automatically.** Normal cards start at `Card1`, and each new one is created
as you reach it with the pad, folder and image both (§6). The BootCard is deliberately **not** treated that
way, because it is not a normal card — it has one special job, booting an exploit, and an empty
one cannot do that job. Upstream generated an empty boot card and served it to the console,
which achieves nothing except confusion: the console sees a blank memory card, no exploit runs,
and there is no hint that the file you meant to put there is missing.

So with `Autoboot=ON` and no `MemoryCards/PS1/BOOT/BootCard-1.mcd` on the microSD, the firmware
**falls back to the normal card** and boots as if autoboot were off. This is also why the default
can safely be ON: on a microSD without an exploit, nothing changes.

Copy your payload image in by hand — with the USB passthrough (§1) that's a drag-and-drop. The
firmware does create the folder, a `LEGGIMI.txt` (readme) explaining what to put in it, and the
`BootCard.ini` with `MaxChannels=1`.

**The old fallback filenames are gone**, for the same reason: fewer ways to be confused. The boot
image must be `BOOT/BootCard-1.mcd`. The old `BOOT/BootCard.mcd`, without a channel number, is
**no longer accepted** — it was a leftover from when the boot card had no channels, and a file
can only have one name. If your boot card stopped being picked up after upgrading, this is why:
rename it. Note that `BOOT/` is the one place where the file is *not* named after its folder
(it's `BootCard-N.mcd`, not `BOOT-N.mcd`) — that exception is upstream's and has not changed.

**It can exit on its own.** Normally the exit is driven by the game: when a **Game ID** arrives
(MMCE command `0x21`), a recognised ID switches to that game's card, and an unrecognised
non-empty one switches to the default card. The payload can also send the MMCE reset (`0x27`).
So if you start a normal game that announces no Game ID, you **stay on the BootCard** until you
do something. For the case where the exploit simply doesn't fire, there is now a timeout:

```ini
[PS1]
BootCardTimeout=-1    ; never — you only leave when the payload says so (default)
BootCardTimeout=30    ; after 30 seconds, back to the default card
BootCardTimeout=0     ; leave immediately (no practical use)
```

The countdown restarts every time you enter the BootCard, so it applies to manual entry with
START too. Positive values are 1 to 254 seconds.

**How long the green stays on** is a separate setting, because how long boot mode *lasts* and
how long it is *signalled* are different questions:

```ini
[PS1]
LedBootCardTimeout=-1    ; follows BootCardTimeout (default)
LedBootCardTimeout=0     ; no steady green
LedBootCardTimeout=5     ; green for 5 seconds, then off
```

With `0` you still get the **two long green blinks** on entry — those come from the blink
sequence, not from the background colour. The value is automatically capped at
`BootCardTimeout`: signalling a mode that has already ended would make no sense.

> **The steady green is also your exploit indicator.** It goes out when you move to a normal
> card — which is what the FreePSXBoot payload does when it succeeds. **Green disappearing is
> the signal that the exploit ran.**

---

## 5. New serial commands

Three commands are new compared to upstream: **`status`**, **`card boot`** and
**`start emulation`**. The serial port is 115200 8N1 (the speed is irrelevant over USB CDC) and
carries both the firmware's debug log — including the exact error when the microSD won't mount,
with the SdFat code and the card CID — and the command interface.

```text
help                                  show the command list
status                                configured mode, running mode, USB state, current card
reset dev                             restart the firmware
reset bl                              restart into BOOTSEL
card up | card down                   next / previous card
card boot                             mount the BootCard  (new — START on the pad)
set card <idx> [channel <idx>]        jump straight to a card
channel up | channel down             next / previous channel
set channel <idx>                     jump straight to a channel
set game <id> [channel <idx>]         set the Game ID by hand
set mode <ps1|ps2>
set variant <retail|proto|conquest|arcade>    PS2 only
start emulation                       start emulation after an eject  (new)
```

`status` is the one worth knowing about: it prints the configured mode *and* the mode actually
running (they differ while a mode switch waits for the card to go idle), the USB state, and the
current card kind, index and channel. Without it you cannot tell "PC mode" from "emulation
running" over the serial port.

**The serial port stays alive in every state**, including while the PC has the drive mounted.
That is new: upstream's passthrough loop never serviced the serial port, so the board looked
frozen. What changes between states is *which* commands are allowed to run:

| State | Serial | Commands |
|---|---|---|
| emulation running | yes | all of them |
| **volume mounted on the PC** (passthrough) | **yes** | only the ones that don't touch the microSD: `help`, `status`, `reset dev`, `reset bl`, plus `start emulation`, which answers by telling you what to do. Anything else replies `volume montato, smonta il volume per continuare` |
| **ejected, cable still attached** (on hold) | yes | **all of them**, `start emulation` included |

The gate exists because in passthrough the microSD belongs to the host: a command that read or
rewrote it would be doing so behind Windows' back. So `start emulation` is not something you can
use to grab the card away from the PC — eject the volume first, then send it.

`reset bl`, or opening the port at **1200 baud** and closing it, reboots into BOOTSEL without
opening the case.

### Instrumentation commands (separate build only)

There is a second set of serial commands for looking at what the PS1 bus is actually doing, and
for changing the bus timing without recompiling. `help` lists them when they are present.

**Reading what happened:**

| Command | What it gives you |
|---|---|
| `mcstat` | the *effective* state: every knob as the firmware is really applying it, plus counters and the program counters of the three state machines. Worth reading before every test — the settings survive a PS1 reset, so the risk is always the leftovers of the previous test |
| `mcring` | the event ring: the last bus events with a microsecond timestamp each, so you can see exactly where a transfer stopped and which side stopped it |
| `mcwrites` | writes the console made to the boot card, and what came back on read-back — this is how the write protection was confirmed |
| `mcget` | the same state as `mcstat` but as `key=value` lines, for scripting |
| `mcclear` | zero the counters, histograms and event ring |
| `mmcelog` | the MMCE commands the console sent (separate build flag again) |

**Changing the timing**, none of it saved — everything goes back to the compiled defaults on
restart:

| Command | What it moves |
|---|---|
| `set ackdelay <us>` | when the ACK pulse starts |
| `set ackwidth <cycles>` | how long it lasts; `0` computes the 2 µs minimum |
| `set ackhead <cycles>` | the delay before the ACK is requested |
| `set ackwait 0\|1` | whether to wait for the 8th clock edge before the next byte |
| `set datpio\|rxpio\|ctrlpio <div>` | the PIO clock dividers, per state machine |
| `set txdepth 0..8` | how far ahead the TX FIFO is filled on long reads; `0` is upstream's lock-step |
| `set profile 0\|1\|2` | auto, force the standard profile, or force the boot profile — **on any card**, which is what lets you separate what depends on the timing from what depends on the image |
| `set mmcenodelay`, `set delayall` | whether the ACK delay applies to MMCE commands and to non-boot profiles |
| `set boottimeout <s>` | the BootCard inactivity timeout, overriding the settings file |
| `set tracestop 0\|1` | freeze the event ring at the first long silence, so the interesting moment isn't overwritten by what came after |

**They exist only in a separate instrumented build.** The firmware images shipped here do not
have them, and typing those commands does nothing. They are left out on purpose: the trace buffer
costs several KB of RAM and the knobs add branches in the timing-critical path, so a build meant
to be used should not carry either.

---

## 6. Folder tree created automatically

On the first boot with a blank microSD the firmware creates the minimum tree and the default
settings — and it does so right after mounting the card, **before** deciding between USB
passthrough and emulation. So you see all of it the first time you plug the card into a PC,
without ever having put it in a console:

```
.sd2psx/settings.ini               default settings
.sd2psx/LEGGIMI.txt                (readme) quick reference, next to the settings
MemoryCards/PS1/Card1/             empty
MemoryCards/PS1/BOOT/LEGGIMI.txt   (readme) explains what to copy here
MemoryCards/PS1/BOOT/BootCard.ini  MaxChannels=1
```

The rest appears with use:

- **`Card1-1.mcd`** is created — empty and already formatted as a PS1 memory card — the first
  time the card actually runs in a console. It isn't pre-generated because writing 128 KB would
  make that first PC connection slower.
- **Other folders** (`Card2/`, Game ID folders, named folders) appear when you navigate to them
  with the pad.
- **`BootCard-1.mcd` does not**: the boot card is copied in by hand (§4).

Naming rules, worth repeating because they're easy to get wrong: the file must be named after the
folder that holds it plus `-<channel>` (`Card3/Card3-1.mcd`), a file with any other name is not
seen, and channels start at 1. The boot folder is exactly `BOOT` and its files are
`BootCard-N.mcd` — the one exception to the rule, and the **old un-numbered
`BOOT/BootCard.mcd` is no longer accepted** (§4).

### Deleting `settings.ini` is now a factory reset

`.sd2psx/settings.ini` is a readable mirror of the settings, which actually live in the RP2040's
flash. The file is written by the firmware; you edit it from the PC and the changes take effect
at the next restart.

**What changed is what happens when the file is missing.**

| | Missing `settings.ini` at boot |
|---|---|
| **Upstream** | The file is rewritten **from what is stored in flash**. You get your own settings back, character for character. Deleting it achieves nothing. |
| **Here** | The stored settings are **reset to the firmware's defaults first**, and *then* the file is written from those. You get a clean default file. |

So on this firmware, deleting `.sd2psx/settings.ini` and restarting is the way back to factory
settings — and it is the *only* way to pick up the new defaults of a firmware version you just
installed. That is the practical consequence worth remembering: **upgrading the firmware does not
give you its new defaults.** Your saved values survive the upgrade and keep winning, because the
file is regenerated from flash, not from the code. If you want the defaults a new version ships
with, delete the file.

Concretely, the file you get back is not a copy of the one you deleted. It is this:

```ini
[General]
Mode=PS1
FlippedScreen=OFF
[PS1]
Autoboot=ON
GameID=ON
EnableControllerCombo=ON
RememberLastCard=0
FastMode_Enable=1
FastMode_AckDelay=0
MaxCardIdx=10
MaxChannels=3
BootCardTimeout=-1
LedBootCardTimeout=-1
```

(plus a `[PS2]` section that has no effect while `Mode=PS1`). Current card and channel, game id
and display settings all go back to their initial values too — this is a full reset, not just a
tidy-up of the text file. If you have settings you care about, copy the file somewhere before
deleting it.

> **Leave a blank line at the end of the file.** This goes for `settings.ini` and for every
> per-card `.ini`: the parser requires it.

---

## Which file to flash

Three firmware images are built, and they are **not interchangeable**: each one hard-codes a
different pinout. Flashing the wrong one gives you a board that enumerates nothing and lights
nothing, with no error message anywhere.

| File | Board | Build variant |
|---|---|---|
| `psxmemcardmsc.uf2` | the custom RP2040 board this fork was written for | `PSXMemCardMSC` |
| `pmc+.uf2` | PicoMemcard+ | `PMC+` |
| `pmczero.uf2` | PicoMemcard on an RP2040-Zero | `PMCZero` |
| `sd2psx.uf2` | SD2PSX / PSxMemCard Gen2 | `SD2PSX` |

All four contain everything described on this page. What differs is the pinout and what the
board physically has on it.

To flash: enter BOOTSEL, mount the `RPI-RP2` volume, copy the `.uf2` onto it.

### Pinout of each build

Every GPIO number below is compiled into the binary. This is the table to check against your
board before flashing anything.

| Signal | `psxmemcardmsc.uf2` | `pmc+.uf2` | `pmczero.uf2` | `sd2psx.uf2` |
|---|---|---|---|---|
| PSX DAT | **5** | 5 | 9 | 20 |
| PSX CMD | **6** | 6 | 10 | 19 |
| PSX SEL | **7** | 7 | 11 | 17 |
| PSX CLK | **8** | 8 | 12 | 18 |
| PSX ACK | **9** | 9 | 13 | 16 |
| SD MISO | **16** | 16 | 0 | 24 |
| SD CS | **17** | 17 | 1 | 29 |
| SD SCK | **18** | 18 | 2 | 26 |
| SD MOSI | **19** | 19 | 3 | 27 |
| SPI peripheral | **spi0** | spi0 | spi0 | spi1 |
| LED | **RGB on 25 / 24 / 23** | none | WS2812 on 16 | none (it has the OLED) |
| User buttons | **none** (BOOTSEL only) | 26 / 27 / 28 | 26 / 27 / 28 | 21 / 23 |
| UART TX / RX | **0 / 1** | 0 / 1 | 7 / 8 | 8 / 9 |
| UART baud | **115200** | 115200 | 115200 | 3000000 |
| Display | none | none | none | SSD1306 OLED |
| PSRAM | no | no | no | yes |
| Flash | **2 MB** | 2 MB | 2 MB | 16 MB |

The custom board has no schematic, so its pinout was recovered by reverse-engineering the dumped
original firmware. It turned out to match PicoMemcard's `PICO` board profile exactly, RGB LED on
25/24/23 included. The only difference from a PicoMemcard+ is that it has **no user buttons**.

On the boards that do have them (`pmc+.uf2`, `pmczero.uf2`) a short press changes channel and a
long press changes card, with the third button switching to the BootCard.

That is also the only reason `pmc+.uf2` exists as a separate build: the PSX and microSD pins are
identical, so `psxmemcardmsc.uf2` runs on a PicoMemcard+ — it just looks for the buttons on the
wrong pins, so they do nothing.

`pmczero.uf2` matches PicoMemcard's `RP2040ZERO` profile exactly, and `pmc+.uf2`, `pmczero.uf2`
and `sd2psx.uf2` all use upstream sd2psXtd's own pin assignments unchanged.

> Two upstream variants are deliberately **not** built. `PSXMemCard` has exactly the same pins
> as `PSXMemCardMSC`, so that board is already covered — flash `psxmemcardmsc.uf2` and you get
> everything. `SD2PSXlite` has a stray `PARENT_DIRECTORY` inside `add_compile_definitions()` in
> upstream's own file, and copying that verbatim would be a mistake.

> **A wrong pinout fails silently.** The firmware compiles, flashes and runs; the board simply
> does nothing recognisable — no USB, LED and microSD driven on pins that aren't connected to
> them, and not one error message anywhere. If a freshly flashed board shows no red-green-blue
> sweep at power-up, suspect the file before you suspect the hardware.

> **The first boot after upgrading resets your saved settings once** (current card and channel,
> game id, display). This is sd2psXtd's own mechanism — the settings *version magic* was raised,
> so the new defaults take effect and `/.sd2psx/settings.ini` is rewritten with them.
>
> That one-off reset only happens when the settings *layout* changes. If a later version merely
> changes a default value, your saved settings keep winning: to pick the new defaults up, delete
> `/.sd2psx/settings.ini` and restart. On this firmware that is a genuine factory reset —
> [see §6](#deleting-settingsini-is-now-a-factory-reset) — whereas upstream would simply write
> your old values back into the file.

---

## What has actually been tested

- **The custom board (`psxmemcardmsc.uf2`)**: verified on hardware — the three LED channels, the
  boot card path, and FreePSXBoot "superfast" starting. The pad shortcut, MSC mode, the wait for
  `start emulation` and the LED states are verified by reading the code, not on a bench.
- **`pmc+.uf2`, `pmczero.uf2` and `sd2psx.uf2`**: they **compile, and that is all**. None of
  those three boards was available to test on. Their pin assignments are upstream sd2psXtd's own,
  taken unchanged and cross-checked against both the 1.4.0 release and the current upstream tree,
  and the values that ended up in each binary were read back out of the build afterwards — but
  nobody has watched any of those boards boot. Treat them as a starting point, keep a backup of
  whatever firmware you're replacing, and know how to get back into BOOTSEL before you flash.
