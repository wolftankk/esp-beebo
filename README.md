# esp-beebo

A desk creature for a Waveshare ESP32-S3-Touch-LCD-3.49. Hold the button, talk,
let go, hear the answer. It fronts an agent that already lives somewhere else —
in your chat apps, on your own machine — and gives it a face, a voice, and a
place to sit.

```
firmware/    the ESP-IDF project that runs on the board
services/    the proxy that runs on your computer
```

## What it does

- **Push to talk.** Hold BOOT, speak, release. Speech-to-text, the agent, and
  text-to-speech all happen on the proxy; the board carries audio and nothing else.
- **A face that means something.** Eyes, brows and a mouth on a 172×640 strip.
  The brow angle carries the mood, the mouth is driven by the amplitude actually
  leaving the codec, and a band below it shows the microphone level while you
  talk, a working indicator while the agent thinks, and the clock the rest of the time.
- **It notices being handled.** Tilt it and the head leans. Shake it and it goes
  cross-eyed and squawks. Set it face down and the screen, amplifier and radio
  go quiet until you pick it up.
- **It knows what time it is before it knows whether it has a network.** The
  PCF85063 is read into the system clock about two seconds into boot; NTP and
  the proxy only ever correct it. The timezone comes from whichever machine
  runs the proxy, so there is nothing to configure and nothing to get wrong.
- **It dozes.** After a configurable idle timeout the backlight, amplifier and
  radio wind down on their own.
- **Set up on the device.** WiFi scanner, the proxy address, brightness, volume
  and nap timeout, all on an on-screen keyboard laid out for a panel 23 mm wide.
  Networks you have joined before are remembered, so picking one again does not
  ask for the password.
- **It can speak first.** `POST /v1/notify` and the robot says it out loud.

## The shape

```
board ──── one WebSocket ───▶ beebo-proxy ──┬── speech-to-text → DashScope
                                your machine├── the agent      → OpenClaw
                                     :18797 └── text-to-speech → DashScope
```

One connection, and it runs on your computer rather than on the agent's host.
Both of those were deliberate:

- **One connection** means one address to configure and nothing to forward.
- **A socket, not HTTP**, because a socket goes both ways. A device that can
  only ask can never be told anything.
- **Only text crosses to the agent.** Speech is the proxy's business at both
  ends, so the board carries no agent credential and the agent needs no
  knowledge of the board. Swapping OpenClaw for something else is one file.

The reply is spoken as it is written: each finished clause is synthesised and
sent while the agent is still writing the rest.

## Getting it running

```sh
# the proxy
cd services
cp beebo-proxy/.env.example beebo-proxy/.env     # DashScope key, gateway URL
./run.sh

# the board
cd ../firmware
cp build-env.example.sh build-env.sh             # point it at the proxy
source build-env.sh
idf.py set-target esp32s3
idf.py build flash monitor -p /dev/cu.usbmodem2101
```

WiFi and the proxy address are both best left to the on-device screens — what
you type there goes to NVS rather than into the binary, and the machine running
the proxy is usually on DHCP, so its address moves. Settings → the second row
under `network` takes a bare address: `192.168.1.42`, or `192.168.1.42:9000` if
you moved the port. Clearing it falls back to whatever was built in.

The build-time value is the fallback, and it comes from the environment, so the repository carries
placeholders and never a real address. `firmware/main/beebo_config.h` documents
the precedence: **environment > menuconfig > default**. CMake reads the
environment at configure time, so after changing a variable run
`idf.py reconfigure`.

## Using it

| | |
|---|---|
| hold BOOT | talk; the band tracks your voice, release to send |
| double-tap BOOT | back to the robot from any settings screen |
| tap PWR | screen off / on |
| hold PWR 5 s | power off |
| touch the face | the eyes follow your finger |
| tilt the board | the head leans with it |
| shake it | goes cross-eyed and squawks, then carries on |
| set it face down | quiet until you lift it |
| gear, top right | wifi, proxy address, brightness, volume, nap timeout |

## The board

Waveshare ESP32-S3-Touch-LCD-3.49: ESP32-S3R8, 16 MB flash, 8 MB octal PSRAM,
172×640 AXS15231B panel with integrated touch, ES8311 out through an NS4150B,
ES7210 four-channel input with a dual mic array, TCA9554 expander, QMI8658 IMU,
PCF85063 RTC, microSD on SDMMC.

### Time

The clock used to read `--:--` from boot until NTP landed - measured on one
desk, eight to thirty-four seconds - and forever on a board that never got a
network. That is backwards: the board has a real-time clock on it.

So the RTC is the clock and everything else is a correction to it. It is read
into the system clock before the network is even started, NTP writes back to it
on every successful sync, and the proxy sends the time on connect and hourly
after that.

The timezone comes from the proxy too, as a POSIX TZ string derived from the
machine it runs on, and is kept in NVS so the first second of the next boot is
already right. `BEEBO_TZ` in the build is only the fallback for a board that
has never connected to anything. POSIX inverts the sign of a UTC offset, which
is why UTC+8 is written `-8`; the proxy emits the unambiguous `<+08>-8` form.

The one thing this does not carry is a daylight-saving rule - only the offset in
force at the moment it was sent. That is why the proxy re-sends hourly rather
than once.

`firmware/main/probe.c` logs a census at every boot — I2C addresses, the IMU,
the RTC, the card slot, PSRAM, die temperature — so a part that is absent reads
as "no answer at 0x6B" rather than as a feature that quietly never worked.

`firmware/components/board_bsp/include/board_pins.h` holds the verified pin map.
Two things there are not obvious and cost real time to find:

- The speaker amplifier is switched by **EXIO7** on the expander and comes up
  muted. Every layer above can report success — codec configured, I2S clocking
  at the right rate, writes accepted — and still produce silence.
- On battery, power only holds once firmware raises **SYS_EN** on EXIO6. Until
  then, letting go of the power button turns the board off.

A third, found later and just as invisible: **WiFi modem sleep must not be
enabled while the link is down.** Associating with the radio asleep fails over
and over with reason 2, so a nap that turned power save on could make the board
look permanently offline. `net.c` now defers the request until there is an address.

## Notes worth keeping

`services/README.md` documents the board protocol and why a turn takes about
three seconds to first sound rather than twenty.

`firmware/docs/gateway-protocol.md` documents OpenClaw's gateway handshake as
verified against a live gateway: the signed string, the encodings, the closed
enums, and the fact that `chat.send` answers asynchronously with the reply
arriving *after* the run's own lifecycle-end event.

## Licence

Apache 2.0. The Chinese pixel font is [Ark Pixel Font](https://github.com/TakWolf/ark-pixel-font),
SIL Open Font License 1.1 — see `firmware/main/fonts/OFL-ark-pixel.txt`.
