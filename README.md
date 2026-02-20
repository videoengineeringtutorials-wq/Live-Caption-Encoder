
# Live Caption Encoder (CEA‑608 → MPEG‑TS H.264)

A lightweight C++17 tool that injects **CEA‑608 (A/53)** captions into a **live MPEG‑TS** stream in real time.  
Captions are accepted over **UDP** (plain text) and embedded as **A/53 CC data** on the video frames before re-encoding and muxing to TS.

- **Input:** MPEG‑TS (UDP or file)  
- **Output:** MPEG‑TS with 608 captions (H.264 by default, MPEG‑2 optional)  
- **Caption Ingest:** UDP (e.g., from Whisper‑based STT)

> Default behavior: decodes video, re-encodes with **H.264** (`libx264`, with `a53cc=1`) so 608 CC are carried as **user data SEI (GA94)** in the H.264 stream.

---

## Features

- **Real‑time 608 injection** (A/53 cc_data side data on frames).
- **UDP text input** (plain ASCII, up to **32 chars** per line).
- **RU2 (two‑line roll‑up)** with duplicate suppression:
  - Rolls only when a new caption is **distinct** from the current bottom line.
  - Repaints when the same caption repeats (prevents duplicate two-line stack).
- **Bootstrap caption** (“CC ONLINE”) helps players expose CC track quickly.
- **Linger window** preserves last caption briefly for stability.
- Audio passthrough via **decode → AAC encode → TS** (if audio present).

---

## Requirements

**Tested on:** Ubuntu 24.04+

- g++ (C++17)
- FFmpeg dev libraries:  
  `libavformat`, `libavcodec`, `libavutil`, `libswresample`
- `pkg-config`
- `netcat` (recommended)
- `ffmpeg` (for test stream generation)

Install (Ubuntu):

```bash
sudo apt update
sudo apt install -y \
  g++ pkg-config \
  libavformat-dev libavcodec-dev libavutil-dev libswresample-dev \
  ffmpeg netcat
```

> **Note:** Uses POSIX sockets. Windows support would require Winsock changes.

---

## Build

```bash
g++ -std=c++17 cc_injector.cpp \
  $(pkg-config --cflags --libs libavformat libavcodec libavutil libswresample) \
  -o cc_injector
```

Produces:

```
./cc_injector
```

---

## Quickstart

### 1) Start a test **H.264** TS source → UDP:5000

```bash
ffmpeg -re \
  -f lavfi -i smptebars=size=1280x720:rate=30000/1001 \
  -f lavfi -i sine=frequency=440:sample_rate=48000 \
  -map 0:v -map 1:a \
  -c:v libx264 -preset veryfast -pix_fmt yuv420p -g 60 \
  -c:a aac -b:a 128k -ar 48000 -ac 2 \
  -f mpegts "udp://127.0.0.1:5000?pkt_size=1316"
```

---

### 2) Run the caption injector

```bash
./cc_injector \
  "udp://127.0.0.1:5000?timeout=5000000&fifo_size=1000000&overrun_nonfatal=1" \
  "udp://127.0.0.1:5004?pkt_size=1316" \
  --cc-udp=127.0.0.1:54001
```

- Input TS: **5000**
- Output TS: **5004**
- Caption ingest: **54001/UDP**

**Optional flags:**
- `--venc=libx264` (default)
- `--venc=mpeg2video`
- `--bootstrap=1|0`
- `--linger_ms=N` (default 750)

---

### 3) Send a caption via UDP

```bash
printf "Hello captions\n" | nc -u -w0 127.0.0.1 54001
```

---

### 4) View output in VLC (important: watch the output port)

```
udp://@127.0.0.1:5004
```

Then enable:

**Subtitle → Sub Track → Track 1 (CC1)**

> VLC may not show a CC track until the first caption arrives; bootstrap caption helps.

---

## How it works (overview)

- Reads MPEG‑TS input using FFmpeg.
- Decodes video → attaches A/53 CC triplets as frame side-data.
- Re-encodes using **libx264** or **MPEG‑2**.
- Writes CC via **GA94 SEI (H.264)** or **user data 0xB2 (MPEG‑2)** depending on encoder.
- Audio is decoded and re‑encoded to **AAC** if present.
- Caption logic:
  - **RU2 (roll‑up 2 lines)**
  - Duplicate suppression
  - Smart repaint vs roll selection
  - Bootstrap + linger for player compatibility

---

## Command-Line Usage

```bash
# Basic
./cc_injector input.ts output.ts --cc-udp=127.0.0.1:54001

# Specify encoder
./cc_injector in.ts out.ts --venc=libx264
./cc_injector in.ts out.ts --venc=mpeg2video

# Customize bootstrap / linger
./cc_injector in.ts out.ts --bootstrap=0 --linger_ms=1500
```

**Defaults when no args supplied:**

```
Input      udp://127.0.0.1:5000
Output     output.ts
Encoder    libx264
Bootstrap  enabled
Linger     750 ms
```

---

## audioWhisper Integration (STT.py)

Setup:

1. Clone audioWhisper:
   ```bash
   git clone https://github.com/awexandrr/audioWhisper
   cd audioWhisper
   python3 -m venv .venv && source .venv/bin/activate
   pip install -r requirements.txt
   ```

2. Copy your modified STT.py into the audioWhisper repo:
   ```bash
   cp /path/to/Live-Caption-Encoder/STT.py /path/to/audioWhisper/STT.py
   ```

3. Run STT.py  
   It will automatically send caption text to your injector over UDP  
   (default in code: `127.0.0.1:54001`).

> If using a different host/port, pass the same to `--cc-udp` on `cc_injector`.

---

## Troubleshooting

- **No captions showing:**
  - Make sure VLC is connected to **output port (5004)**, not input.
  - Enable **Subtitle → Sub Track → Track 1**.
  - Wait for the first caption or use bootstrap.

- **Long caption delays or missing lines:**
  - Reduce network buffering on input.
  - Ensure sender sends only ASCII 0x20–0x7E.

- **Duplicate lines in roll‑up:**
  - Injector suppresses duplicates; check STT sender isn’t adding spaces or CRs.

- **Windows build issues:**
  - Replace POSIX sockets with Winsock equivalents.

---

## Known Limitations

- CEA‑608 **Field 1 only** (0xFC header).
- Max **32 characters** per caption.
- ASCII only (non‑ASCII filtered).
- RU2 only (roll‑up 2‑line).
- No CEA‑708 yet.
- Basic PAC attributes (white text, no underline, bottom row).
