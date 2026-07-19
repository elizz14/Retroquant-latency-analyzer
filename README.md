# RetroQuant — Tick-to-Trade Latency Analyzer

**RetroQuant** is a distributed embedded Linux latency analyzer built on two NXP FRDM-IMX93 boards during the NXP Linux Kernel Summer School hackathon.

The project is inspired by **tick-to-trade latency** in electronic trading: the time between receiving a market update, or “tick”, and reacting to it with a decision or order, or “trade”.

RetroQuant does not implement a real trading system. Instead, it uses the trading theme to build a concrete embedded Linux demo: one board behaves like an **Exchange**, generating ticks, while the other behaves like a **Trader**, receiving and echoing them back. The Exchange measures the round-trip latency and displays it live on a physical LCD.

The goal was to make a small event travel through real Linux and hardware layers:

```text
userspace C application
        ↓
UDP socket
        ↓
Linux networking stack
        ↓
Ethernet driver + physical cable
        ↓
second embedded Linux board
        ↓
UDP receive/echo path
        ↓
return packet
        ↓
latency calculation
        ↓
LVGL framebuffer display
```

## Demo

### Exchange node — latency chart

![Exchange node latency chart](docs/images/exchange-node.png)

The Exchange node sends UDP ticks to the Trader node, receives the echoed packets, computes round-trip time, and plots the latency on a scrolling LVGL chart.

The display shows:

```text
peer IP
tx / rx / pending packets
RTT
MIN
AVG
MAX
chart scale
LIVE / PAUSED state
```

### Trader node — poll mode

![Trader node poll mode](docs/images/trader-poll-live.png)

The Trader node receives packets from the Exchange, echoes them back, and displays system and network telemetry in a retro terminal-style interface.

In normal `POLL` mode, the network thread waits for incoming packets instead of continuously spinning. During the demo, this kept CPU usage around 9–10%.

### Trader node — paused Exchange

![Trader node while Exchange is paused](docs/images/trader-poll-paused.png)

When the Exchange node is paused, the Trader stops receiving packets and the message rate drops to `0/s`.

### Trader node — busy mode

![Trader node busy mode](docs/images/trader-busy-mode.png)

The Trader node can switch from `POLL` mode to `BUSY` mode using the physical button `SW3`. In busy mode, the network thread repeatedly checks the UDP socket using non-blocking receive. This increases CPU load visibly; in the hardware demo, CPU usage rose from around 9–10% to over 60%.

## Hardware Setup

RetroQuant runs on two separate embedded Linux boards connected directly through an Ethernet cable.

```text
+-----------------------------+          Ethernet          +-----------------------------+
| Board A: Exchange Node      |  <---------------------->  | Board B: Trader Node        |
| FRDM-IMX93 + LKSS board     |                            | FRDM-IMX93 + LKSS board     |
| ST7789V 240x240 LCD         |                            | ST7789V 240x240 LCD         |
| Hackpad buttons + LEDs      |                            | Hackpad buttons + LEDs      |
| Runs chart.c                |                            | Runs sysmon.c               |
+-----------------------------+                            +-----------------------------+
```

Hardware used:

- 2 × NXP FRDM-IMX93 boards;
- 2 × LKSS daughter boards;
- 2 × ST7789V 240x240 SPI LCD modules;
- 4 physical buttons per daughter board;
- 3 user-controllable LEDs per daughter board;
- direct Ethernet cable between the two boards.

The LKSS daughter board exposes the LCD, buttons, and LEDs through Linux kernel drivers developed/provided during the labs. The final application uses these drivers from userspace through the HAL.

## Network Setup

The two boards were connected directly through Ethernet and configured with static IP addresses.

Example setup used during the demo:

```text
Exchange node: 192.168.1.100
Trader node:   192.168.1.101
UDP port:      5000
```

The exact IP addresses can be changed, but the Exchange node must send packets to the actual IP address of the Trader node.

Before running the LVGL applications, the raw Ethernet link was tested with `ping`:

```bash
ping 192.168.1.101
```

This was an important step because UDP and LVGL debugging only makes sense after the physical link, Ethernet driver, static IP configuration, and routing are already working.

## Network Protocol

The two applications communicate using a minimal UDP protocol.

Both sides use the same packet format:

```c
struct tick_packet {
    uint32_t seq;
} __attribute__((packed));
```

The `seq` field identifies each tick.

The protocol flow is:

```text
Exchange sends: tick_packet { seq = N }
Trader receives packet N
Trader immediately echoes the same packet back
Exchange receives packet N
Exchange computes current_time - send_time[N]
```

The Trader node always echoes received packets, independently of the simulated trading state. This keeps the latency stream continuous and prevents gaps in the Exchange chart.

## Timing Strategy

The boards do not need synchronized clocks.

The Exchange node measures round-trip time using only its own monotonic clock:

1. it stores a timestamp before sending a packet;
2. it receives the echoed packet;
3. it takes a second timestamp;
4. it subtracts the original send timestamp.

Because both timestamps are taken on the Exchange board, the measurement avoids cross-device clock drift.

The measured value is UDP round-trip latency from the Exchange node’s point of view.

## Exchange Node

The Exchange node is implemented in:

```text
src/chart.c
```

It started from the original LKSS LVGL `chart` demo. The original demo plotted a synthetic sine wave. In RetroQuant, the synthetic signal was replaced with real network latency measurements.

Main responsibilities:

- initialize LVGL and the HAL;
- create a green-on-black latency dashboard;
- open a UDP socket;
- periodically send `tick_packet` messages to the Trader node;
- store send timestamps in a circular sequence table;
- receive echoed packets;
- match received packets by sequence number;
- compute RTT latency in microseconds;
- update live latency statistics;
- render a scrolling LVGL chart.

Displayed values:

```text
peer    IP address of the Trader node
tx      packets sent
rx      packets received back
loss    pending/unanswered packets
RTT     latest round-trip time
MIN     minimum measured RTT
AVG     average measured RTT
MAX     maximum measured RTT
SCALE   current chart scale
LIVE    automatic tick generation active
PAUSED  automatic tick generation stopped
```

Host A button controls:

```text
SW1  reset statistics and clear the chart
SW2  change chart scale
SW3  pause/resume automatic tick generation
SW4  send one manual tick
```

Host A LED feedback:

```text
Green  echo received
Red    warning/error/pending packets
Blue   paused mode
```

## Trader Node

The Trader node is implemented in:

```text
src/sysmon.c
```

It started from the original LKSS `sysmon` demo. The original demo was a retro system monitor using Linux `/proc` data. In RetroQuant, it became the UDP responder and trading-themed telemetry dashboard.

Main responsibilities:

- initialize LVGL and the HAL;
- open a UDP socket on port `5000`;
- start a network thread;
- receive packets from the Exchange node;
- echo received packets back immediately;
- count received messages per second;
- track the latest received sequence number;
- measure local echo time around the response path;
- parse system telemetry from `/proc`;
- display a terminal-style dashboard;
- switch between `POLL` and `BUSY` receive modes.

Real values shown on the Trader display:

```text
UP       system uptime
CPU      CPU usage parsed from /proc/stat
MEM      memory usage parsed from /proc/meminfo
load     load average parsed from /proc/loadavg
net      received UDP packets per second
ECHO     local echo time on the Trader node
MODE     POLL or BUSY
SEQ      latest sequence number received from Exchange
```

Trading-themed simulated values shown on the display:

```text
PRICE    simulated price
POS      simulated position
PNL      simulated profit/loss
SIG      simulated signal
STRAT    simulated strategy state
```

These fields are part of the RetroQuant theme. They make the board look like a small trading terminal, while the actual technical measurements are the packet rate, sequence number, echo timing, CPU usage, memory usage, and polling mode.

Host B button controls:

```text
SW3  toggle between POLL and BUSY mode
```

Modes:

```text
POLL  uses poll(), allowing the network thread to sleep while waiting for packets
BUSY  repeatedly checks the socket using non-blocking receive
```

This difference is visible on the display: `BUSY` mode increases CPU usage because the thread spends more time actively checking the socket.

## Linux Stack Used

```text
userspace:
    src/chart.c
    src/sysmon.c
    src/common/hal.c
    LVGL
    UDP sockets
    /proc parsing

device nodes:
    /dev/fb0
    /dev/hackpad

kernel:
    st7789fb.ko
    hackpad.ko
    Ethernet/FEC support

hardware:
    ST7789V SPI LCD
    GPIO buttons
    user LEDs
    Ethernet PHY
    FRDM-IMX93 board
```

## What Was Modified from the Lab Skeleton

The project was built by extending the LKSS Lab 5 userspace demos instead of writing the UI and hardware access from scratch.

### `src/chart.c`

Original role:

- simple LVGL chart demo;
- plotted a generated sine wave.

Changed into:

- Exchange node;
- UDP tick generator;
- latency measurement node.

Added:

- UDP socket setup;
- `tick_packet` protocol;
- periodic tick generation;
- sequence tracking;
- send timestamp table;
- RTT calculation;
- live latency chart;
- min/avg/max statistics;
- button controls;
- LED feedback.

### `src/sysmon.c`

Original role:

- retro Linux system monitor skeleton;
- intended to parse `/proc` and display CPU/memory/load.

Changed into:

- Trader node;
- UDP echo responder;
- terminal-style trading dashboard.

Added:

- CPU usage parsing from `/proc/stat`;
- memory usage parsing from `/proc/meminfo`;
- load and uptime display;
- UDP receive/echo thread;
- messages-per-second counter;
- latest sequence tracking;
- local echo timing;
- `POLL` vs `BUSY` runtime mode;
- simulated trading fields;
- physical button control for mode switching.

### `src/common/`

The common HAL was reused from the LKSS Lab 5 framework.

It provides:

- LVGL initialization;
- framebuffer binding to `/dev/fb0`;
- button input through `/dev/hackpad`;
- LED output through `/dev/hackpad`;
- the main LVGL event loop through `hal_run()`.

### `kernel-drivers/`

The repository includes the kernel-side support files used by the project:

- `hackpad.c` exposes the physical buttons and LEDs as `/dev/hackpad`;
- `st7789fb.c` exposes the ST7789V LCD as a Linux framebuffer device.

### `dts/`

The Device Tree configuration is included to document the board setup required for the peripherals used by the demo.

## Build Notes

This project is intended to be built inside or alongside the LKSS Linux tree used during the hackathon. It is hardware-specific and is not a standalone desktop Linux application.

The kernel configuration needs support for:

```text
framebuffer support
LKSS Lab 5 drivers
st7789fb module
hackpad module
Ethernet/FEC support
```

In practice, the setup required enabling the Lab 5 display and hackpad drivers in `menuconfig`, making sure the relevant Device Tree nodes were enabled, and rebuilding the kernel/modules.

Typical build/deploy flow:

```bash
python3 scripts/lkss.py compile --install-modules
python3 scripts/lkss.py boot
```

Userspace applications are cross-compiled for ARM64:

```bash
cd src
make
```

Depending on the local Makefile naming, this builds the Exchange and Trader applications from `chart.c` and `sysmon.c`.

## Running the Demo

### 1. Load the required kernel modules

On both boards:

```bash
modprobe st7789fb
modprobe hackpad
```

The BMP280 sensor module is not required for this project.

### 2. Configure Ethernet

Example static IP configuration:

On the Exchange node:

```bash
ip link set eth0 up
ip addr add 192.168.1.100/24 dev eth0
```

On the Trader node:

```bash
ip link set eth0 up
ip addr add 192.168.1.101/24 dev eth0
```

The important part is that the `TRADER_IP` value used by the Exchange application points to the actual IP address of the Trader board.

### 3. Test the physical network link

From the Exchange node:

```bash
ping 192.168.1.101
```

If `ping` fails, fix the Ethernet link, IP addresses, or Device Tree/network driver setup before running the applications.

### 4. Start the Trader node

On the Trader board:

```bash
./trader
```

or, if using the original lab binary name:

```bash
./sysmon
```

### 5. Start the Exchange node

On the Exchange board:

```bash
./exchange
```

or, if using the original lab binary name:

```bash
./chart
```

Expected behavior:

- the Trader display shows increasing `SEQ`;
- the Trader display shows around 19–20 received messages per second when the Exchange is live;
- the Exchange display shows RTT values and a scrolling latency chart;
- pausing the Exchange drops the Trader packet rate to `0/s`;
- switching the Trader to `BUSY` mode increases CPU usage.

## Debugging Notes

Common issues:

```text
No eth0:
    Ethernet/FEC support or Device Tree configuration is missing.

eth0 exists but no link:
    cable is disconnected, the other board is off, or the interface is not up.

ping fails:
    static IPs are wrong, subnet mismatch, or duplicate MAC/IP configuration.

Trader display shows 0 msg/s:
    Exchange is paused, not running, or sending to the wrong IP/port.

Exchange chart is empty:
    Trader is not echoing packets back, or packet format/port mismatch exists.

RTT values look wrong:
    returned packet does not contain the original seq, or send timestamp lookup failed.

BUSY mode does not increase CPU:
    the mode flag is not reaching the network thread or the socket is not non-blocking.
```

## Limitations

- The trading data is simulated and used for visual context.
- The measured latency is UDP round-trip latency from the Exchange node’s point of view.
- The automatic tick source is an LVGL timer.
- Physical buttons are used for runtime control and hardware feedback.
- The project depends on the LKSS FRDM-IMX93 hardware setup.

## Team

- **Ana-Maria Ghica** — Exchange node, UDP tick generation, latency chart, RTT statistics, Host A UI.
- **Eliza-Maria Niculae** — Trader node, UDP echo responder, system telemetry parsing, `POLL`/`BUSY` mode, terminal-style Host B dashboard, GitHub project structuring.

## Why This Project Matters

RetroQuant turns a low-level embedded Linux stack into a visible latency experiment.

It combines:

- C programming;
- Linux userspace APIs;
- UDP sockets;
- physical Ethernet networking;
- procfs parsing;
- LVGL graphics;
- framebuffer rendering;
- kernel driver interfaces;
- hardware buttons and LEDs;
- embedded Linux debugging.

The result is a compact distributed embedded system that shows how quickly one Linux node can react to another over a real network link, and how implementation choices such as polling versus busy-polling affect system behavior.
