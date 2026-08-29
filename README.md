# LED Wall Display for pi-secretary

A complete ESP32-S3 system for displaying portfolio stats, calendar events, and stock holdings on a 192×32 LED wall (3x chained Lyson 32×64 panels).

**Status:** Ready for deployment ✅

---

## Quick Start

### 1. Backend Setup (Orange Pi)
Add the `/api/matrix` endpoint to your pi-secretary backend:

```bash
# Copy code from backend/api-matrix-endpoint.js
# Add it to your backend/server.js (in the "reading" section)
# Restart your backend
npm start
```

Test it:
```bash
curl http://192.168.0.130:3001/api/matrix | jq .
```

### 2. Arduino Setup (Your Computer)

**Install libraries:**
- Arduino IDE → Sketch → Include Library → Manage Libraries
- Install: `ESP32-HUB75-MatrixPanel-DMA` (search "HUB75")
- Install: `ArduinoJson`

**Load firmware:**
- Open `firmware/esp32-led-wall.ino`
- Edit WiFi settings at the top:
  ```cpp
  const char* SSID = "YOUR_SSID";
  const char* PASSWORD = "YOUR_PASSWORD";
  const char* PI_URL = "http://192.168.0.130:3001/api/matrix";
  ```
- Upload to ESP32-S3

### 3. Testing Without Panels ⭐

**Don't have panels yet?** Use simulation mode:

- Open `firmware/esp32-led-wall-SIMULATION.ino`
- Edit WiFi settings (same as above)
- Upload to ESP32-S3
- Open Serial Monitor (115200 baud)
- Watch ASCII art pages display and rotate automatically
- Validates WiFi, API polling, JSON parsing, and page logic

See `docs/TESTING.md` for detailed testing guide.

### 4. Panel Wiring (When You Get Them)

See `docs/IMPLEMENTATION.md` Part 3 for complete wiring guide:
- HUB75 pinout (all 16 GPIO pins)
- Power supply setup (5V, 10A+)
- Chaining 3 panels horizontally

---

## Documentation

- **[DESIGN.md](docs/DESIGN.md)** — System architecture, page layouts, API spec, fonts & graphics
- **[IMPLEMENTATION.md](docs/IMPLEMENTATION.md)** — Step-by-step setup, wiring, troubleshooting
- **[TESTING.md](docs/TESTING.md)** — Complete testing guide without physical panels

---

## Project Structure

```
.
├── firmware/
│   ├── esp32-led-wall.ino              # Production firmware (for physical panels)
│   └── esp32-led-wall-SIMULATION.ino   # Testing firmware (no hardware needed)
├── backend/
│   └── api-matrix-endpoint.js          # Add this to your Orange Pi backend
├── docs/
│   ├── DESIGN.md                       # System design & specs
│   ├── IMPLEMENTATION.md               # Setup & wiring guide
│   └── TESTING.md                      # Testing without hardware
├── README.md                           # This file
└── .gitignore                          # Git ignore rules
```

---

## Features

✅ **3 Display Pages (auto-rotate every 30s):**
- Portfolio Overview (total value, day change)
- Today's Events (with busy-level indicators)
- Top Holdings (cycles through top 5)

✅ **Smart Offline Mode:**
- Shows "OFFLINE" if backend unreachable
- Displays last refresh time
- Continues polling automatically

✅ **Efficient Design:**
- Small ~1-2KB JSON payloads
- Polls every 30 seconds
- Graceful network error handling
- No stale data shown

✅ **Testing-First:**
- Simulation mode for pre-panel validation
- Proves WiFi, JSON parsing, page logic work
- ASCII art display in Serial Monitor

---

## Hardware Required

- **ESP32-S3** (recommend ESP32-S3 DevKit)
- **3x Lyson 32×64 LED panels** (HUB75 protocol)
- **5V Power Supply** (10A+ for panels)
- **WiFi Connection** (same network as Orange Pi)

Optional:
- Battery bank (5V, 10Ah) for wireless operation
- Buck converter (if using higher voltage battery)

---

## Next Steps

### Before Panels Arrive
1. ✅ Add `/api/matrix` endpoint to backend
2. ✅ Load simulation firmware on ESP32
3. ✅ Validate WiFi, API, JSON parsing with simulation mode
4. ✅ Test all 3 pages display correctly in Serial Monitor

### When Panels Arrive
1. Connect panels to ESP32 via HUB75 (see wiring guide)
2. Connect power supply to panels
3. Load production firmware (`esp32-led-wall.ino`)
4. Enjoy your LED wall! 📺

### Future Enhancements
- Dashboard control page to switch pages on demand
- Battery status indicator
- Custom page order/timing
- More holdings or filtered events
- Brightness control

---

## Troubleshooting

### General
- See **IMPLEMENTATION.md Part 4** for wiring/panel issues
- See **TESTING.md Phase 4** for simulation mode issues

### WiFi Not Connecting
- Verify SSID/password in sketch (case-sensitive)
- Check ESP32 is in WiFi range
- Ensure WiFi is 2.4GHz (not 5GHz)

### API Not Responding
- Test manually: `curl http://192.168.0.130:3001/api/matrix`
- Verify Orange Pi IP (may have changed)
- Ensure firewall allows port 3001

### JSON Errors
- Manually test endpoint and validate JSON response
- Verify ArduinoJson library is installed

---

## API Response Format

**Endpoint:** `GET /api/matrix`

**Response (sample):**
```json
{
  "timestamp": 1693478400000,
  "lastRefresh": 1693478350000,
  "portfolio": {
    "total": 125450.50,
    "dayChange": 1250.75,
    "dayChangePercent": 1.01
  },
  "events": [
    {
      "time": "10:00",
      "title": "Team standup",
      "busyLevel": "busy"
    }
  ],
  "dailyBusyPercent": 68,
  "holdings": [
    {
      "symbol": "AAPL",
      "value": 45000,
      "dayChangePercent": 2.3,
      "weightPercent": 35.8
    }
  ]
}
```

---

## License

Built for pi-secretary project. Integrate with your Orange Pi backend.

---

## Files Summary

| File | Purpose |
|------|---------|
| `esp32-led-wall.ino` | Production firmware (requires HUB75 library & physical panels) |
| `esp32-led-wall-SIMULATION.ino` | Test firmware (no hardware, just ArduinoJson library) |
| `api-matrix-endpoint.js` | Backend endpoint code (add to Orange Pi server.js) |
| `DESIGN.md` | System architecture, page layouts, API spec |
| `IMPLEMENTATION.md` | Step-by-step setup, wiring, troubleshooting |
| `TESTING.md` | Complete testing guide without panels |
