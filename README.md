# LED Wall Display for pi-secretary

An ESP32-S3 system that displays live portfolio, calendar, and system data from the Orange Pi Secretary backend on a 192×32 LED wall (3× chained Lyson 32×64 HUB75 panels).

**Status (round 72, 2026-09-13):** deployed and running — confirmed by pulling the actual firmware off the live device. Not the design in this README's older revisions; see [Current design](#current-design-round-72) below.

---

## Current design (round 72)

This repo went through a full architecture rewrite (rounds 49-62) that this README, `docs/DESIGN.md`, `docs/IMPLEMENTATION.md`, and `docs/TESTING.md` never caught up to — they still describe the original 3-page scrolling-ticker design. That older content is kept for history (each file now has a banner at the top), but **`docs/JSON-CONTRACT.md` is the current, actively-maintained source of truth** for the JSON contract, screen list, and render-priority rules. Read that first for anything beyond the quick start below.

The short version: instead of one continuous ticker, the wall now **rotates through named screens** (portfolio, events, holdings, markets, news, weather, clock, day overview, commuting — plus overlay/takeover screens for offline, notifications, alerts, and a wake-up mode), driven by two endpoints polled at different rates:

| Endpoint | Poll interval | Purpose |
|---|---|---|
| `GET /api/matrix` | 30s | Real data — portfolio, events, holdings, day overview, news |
| `GET /api/matrix/command` | 1.5s | Live control — which screens are enabled, a pinned screen, notifications, alerts |

## Quick Start

### 1. Backend Setup (Orange Pi)
The real `/api/matrix` and `/api/matrix/command` routes live in `pi-secretary/backend/server.js` and `pi-secretary/backend/lib/matrixControl.js` — this repo's `backend/api-matrix-endpoint.js` is a **reference copy** of the `/api/matrix` route only, kept in sync for anyone who needs to diff or restore it. Edit `server.js` directly, then update the reference copy to match, not the other way around.

Test the real endpoint:
```bash
curl http://192.168.0.130:3001/api/matrix | jq .
curl http://192.168.0.130:3001/api/matrix/command | jq .
```

### 2. Arduino Setup (Your Computer)

**Install libraries:**
- Arduino IDE → Sketch → Include Library → Manage Libraries
- Install: `ESP32-HUB75-MatrixPanel-I2S-DMA` (search "HUB75")
- Install: `ArduinoJson`

**Load firmware:**
- Open `firmware/esp32-led-wall/esp32-led-wall.ino`
- Edit WiFi/Pi settings near the top:
  ```cpp
  const char* WIFI_SSID     = "Adidas";   // capital A — case-sensitive
  const char* WIFI_PASSWORD = "...";
  const char* PI_URL        = "http://192.168.0.130:3001/api/matrix";
  const char* COMMAND_URL   = "http://192.168.0.130:3001/api/matrix/command";
  ```
- Upload to ESP32-S3

**Known gaps on the currently-flashed firmware** (confirmed round 72 by diffing the live device's `.ino` against this repo — see `docs/JSON-CONTRACT.md`'s round-72 addendum for the full story):
- `wakeMode` isn't parsed at all yet — needs porting down before wake-up mode can fire for real, even once the backend sends it.
- The round-63 boot-panel-init retry fix (`dma_display->begin()` retried with backoff instead of called once, unchecked) isn't on the device yet — the intermittent garbled-boot issue it fixed may still show up.

### 3. Testing Without Panels

- Open `firmware/esp32-led-wall-SIMULATION/esp32-led-wall-SIMULATION.ino`
- Edit WiFi/Pi settings (same as above)
- Upload to ESP32-S3
- Open Serial Monitor (115200 baud) and watch it poll/parse/log without a physical panel

See `docs/TESTING.md` for the original detailed testing guide (still broadly applicable to the polling/parsing mechanics, even though the page list it describes is outdated).

### 4. Panel Wiring

See `docs/IMPLEMENTATION.md` Part 3 and `PINOUT_V1.html` for the HUB75 pinout, power supply setup, and chaining 3 panels horizontally. Wiring/hardware content in that doc is still accurate — only the software/page-list sections are stale.

---

## Documentation

- **[docs/JSON-CONTRACT.md](docs/JSON-CONTRACT.md)** — **current source of truth.** Every field the firmware reads, per-screen behavior, render-priority tiers, and the punch list of what the backend still needs to send.
- [docs/DESIGN.md](docs/DESIGN.md), [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md), [docs/TESTING.md](docs/TESTING.md) — original design docs from the first (pre-round-49) build. Historical; each has a banner pointing back here.

---

## Project Structure

```
.
├── firmware/
│   ├── esp32-led-wall/esp32-led-wall.ino                    # Production firmware
│   └── esp32-led-wall-SIMULATION/esp32-led-wall-SIMULATION.ino  # Testing firmware (no hardware needed)
├── backend/
│   └── api-matrix-endpoint.js          # Reference copy of /api/matrix — edit server.js first, sync here after
├── docs/
│   ├── JSON-CONTRACT.md                # Current source of truth
│   ├── DESIGN.md                       # Historical — pre-round-49 design
│   ├── IMPLEMENTATION.md               # Historical — wiring/setup content still accurate
│   └── TESTING.md                      # Historical — polling/parsing mechanics still broadly accurate
├── PINOUT_V1.html                       # HUB75 pinout reference
├── _to_delete/                          # Retired prototype files, kept for reference rather than deleted
├── README.md                           # This file
└── .gitignore
```

---

## Hardware Required

- **ESP32-S3** (recommend ESP32-S3 DevKit)
- **3× Lyson 32×64 LED panels** (HUB75 protocol)
- **5V Power Supply** (10A+ for panels)
- **WiFi Connection** (same network as Orange Pi, 2.4GHz)

Optional: battery bank (5V, 10Ah) for wireless operation; buck converter if using a higher-voltage battery.

---

## Troubleshooting

### WiFi Not Connecting
- Verify SSID/password in the sketch (case-sensitive — `"Adidas"` needs the capital A; this was a real bug, round 56)
- Ensure WiFi is 2.4GHz (not 5GHz)

### API Not Responding
- Test manually: `curl http://192.168.0.130:3001/api/matrix` and `curl http://192.168.0.130:3001/api/matrix/command`
- Verify the Orange Pi's IP hasn't changed
- Ensure the firewall allows port 3001

### JSON / screen issues
See `docs/JSON-CONTRACT.md` — it documents exactly what happens (fallback text, default values) when any given field is absent, which is usually the actual symptom rather than a real error.

---

## License

Built for the pi-secretary project. Integrate with your Orange Pi backend.
