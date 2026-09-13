> **Historical (pre-round-49).** This doc describes the original 3-page scrolling-ticker design, superseded by a full screen-rotation rewrite in rounds 49-62. Kept for history — see [`docs/JSON-CONTRACT.md`](JSON-CONTRACT.md) for the current source of truth, and the main [README](../README.md) for what's still accurate here (wiring/hardware content generally is; page-list/software content generally isn't).

---

# Testing Without Physical Panels

This guide walks you through validating your entire LED wall system **before the physical panels arrive**.

---

## Phase 1: Backend Verification

### Step 1: Verify `/api/matrix` Endpoint

Make sure your Orange Pi backend is running and the endpoint is accessible:

```bash
# From your computer (same WiFi as Orange Pi)
curl http://192.168.0.130:3001/api/matrix | jq .
```

Expected response (sample data):
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

**Checklist:**
- [ ] Endpoint responds with 200 OK
- [ ] JSON is valid
- [ ] Portfolio data is present
- [ ] Events array contains today's events (or empty array)
- [ ] dailyBusyPercent is a number (0-100)
- [ ] Holdings array contains top 5 stocks (or empty)

---

## Phase 2: Simulation Mode Setup

This is the key test: **validate WiFi, HTTP polling, JSON parsing, and page logic WITHOUT hardware**.

### Step 1: Install ArduinoJson Library Only

1. Open Arduino IDE
2. Go to **Sketch → Include Library → Manage Libraries**
3. Search for `ArduinoJson` and install it
4. **DO NOT install ESP32-HUB75-MatrixPanel-DMA** yet (we'll use simulation mode instead)

### Step 2: Load the Simulation Sketch

1. Open `firmware/esp32-led-wall-SIMULATION.ino` in Arduino IDE
2. Edit the WiFi credentials:
   ```cpp
   const char* SSID = "YOUR_SSID";           // Your WiFi network name
   const char* PASSWORD = "YOUR_PASSWORD";   // Your WiFi password
   const char* PI_URL = "http://192.168.0.130:3001/api/matrix"; // Orange Pi address
   ```

3. Save and compile (Ctrl+Shift+R to verify)

### Step 3: Connect ESP32 and Upload

1. Plug ESP32-S3 into your computer via USB-C
2. In Arduino IDE: **Tools → Board → esp32 → ESP32S3 Dev Module**
3. **Tools → Port → COM[X]** (select your USB port)
4. Click **Upload** (Ctrl+U)

### Step 4: Watch the Serial Monitor

1. Open **Tools → Serial Monitor** (or Ctrl+Shift+M)
2. Set baud rate to **115200**
3. You should see:

```
=== ESP32 LED Wall (SIMULATION MODE) ===
Testing WiFi, API polling, JSON parsing, and page rendering.
This sketch does NOT require physical LED panels.

Connecting to WiFi: YOUR_SSID
.................
WiFi connected! IP: 192.168.1.100
Polling http://192.168.0.130:3001/api/matrix...
Data fetched successfully

╔════════════════════════════════════════════════════════╗
║                    PORTFOLIO                           ║
╠════════════════════════════════════════════════════════╣
║ Total Value:  $125450.50
║ Day Change:   +1250.75 (+1.01%)
║                                                        ║
║ Status:       📈 POSITIVE DAY                          ║
╚════════════════════════════════════════════════════════╝

╔════════════════════════════════════════════════════════╗
║                   TODAY'S EVENTS                       ║
╠════════════════════════════════════════════════════════╣
║ 🔴 10:00 Team standup
║ 🟠 14:30 1:1 with manager
║ 🟢 16:00 Personal time
╠════════════════════════════════════════════════════════╣
║ Daily Busy: 68% █████░░░░ ║
╚════════════════════════════════════════════════════════╝

╔════════════════════════════════════════════════════════╗
║                   TOP HOLDINGS                         ║
╠════════════════════════════════════════════════════════╣
║ Symbol: AAPL
║ Value: $45000
║ Day Change: +2.3%
║ Weight: 35.8% of portfolio
║                                                        ║
║ Showing holding 1 of 5
╚════════════════════════════════════════════════════════╝
```

---

## Phase 3: Validation Checklist

### WiFi & Network
- [ ] ESP32 connects to WiFi
- [ ] Serial monitor shows "WiFi connected!"
- [ ] IP address is displayed

### API Polling
- [ ] Serial monitor shows "Polling http://192.168.0.130:3001/api/matrix..."
- [ ] Response shows "Data fetched successfully"
- [ ] Polling repeats every ~30 seconds

### JSON Parsing
- [ ] Portfolio data is displayed (total value, day change)
- [ ] Events are parsed and displayed with times/titles
- [ ] Busy level indicators (🔴 🟠 🟢) show correctly
- [ ] Daily busy score displays as a number

### Page Rotation
- [ ] Page 1 (Portfolio) displays correctly
- [ ] Page 2 (Events) displays correctly with at least 1 event
- [ ] Page 3 (Holdings) displays and cycles through holdings
- [ ] Pages rotate automatically every ~30 seconds

### Offline Handling
- [ ] Unplug the ESP32 from power
- [ ] After a few seconds, serial monitor should show "OFFLINE" page
- [ ] Last refresh time is displayed
- [ ] Sketch continues running (waiting for reconnect)

---

## Phase 4: Troubleshooting Simulation Mode

### "WiFi not connected"
**Problem:** ESP32 can't connect to your network.

**Solutions:**
- Verify SSID and password are correct (case-sensitive)
- Check that ESP32 is in range of WiFi router
- Try moving closer to router
- Verify your WiFi is 2.4GHz (ESP32-S3 doesn't support 5GHz)

### "HTTP error: 404 or timeout"
**Problem:** ESP32 can't reach the Orange Pi backend.

**Solutions:**
- Verify Orange Pi is running and accessible from your computer: `curl http://192.168.0.130:3001/api/health`
- Check Orange Pi's IP address (may have changed): `hostname -I` on the Pi
- Update PI_URL in the sketch with the correct IP
- Ensure both ESP32 and Orange Pi are on the same WiFi network

### "JSON parse error"
**Problem:** ESP32 receives data but can't parse it.

**Solutions:**
- Manually test the endpoint: `curl http://192.168.0.130:3001/api/matrix | jq .`
- Verify JSON is valid and complete
- Check that ArduinoJson library is installed

### "No events displaying"
**Problem:** Events page shows "No events today" even though you have calendar events.

**Solutions:**
- The `/api/matrix` endpoint only returns today's events
- Create a test event in your calendar for today
- Wait for the next poll (30s) to see it update

---

## Phase 5: Production Firmware Testing

Once simulation mode validates everything, it's time to move to production:

### Step 1: Install HUB75 Library

1. Arduino IDE → **Sketch → Include Library → Manage Libraries**
2. Search for `ESP32-HUB75-MatrixPanel-DMA`
3. Install it

### Step 2: Load Production Firmware

1. Open `firmware/esp32-led-wall.ino`
2. Configure WiFi credentials (same as simulation)
3. Verify GPIO pin assignments match your panel wiring (see IMPLEMENTATION.md)

### Step 3: Compile & Upload

1. Click **Verify** (Ctrl+R) to check for errors
2. Once verified, click **Upload** (Ctrl+U)
3. Watch serial monitor for startup messages

### Step 4: Physical Panel Connection

When your panels arrive:
1. Connect panels to ESP32 via HUB75 (see wiring guide)
2. Connect power supply to panels
3. Load production firmware (`esp32-led-wall.ino`)
4. Enjoy your LED wall! 📺

---

## Testing Success Criteria

✅ **All tests pass when:**
- [ ] WiFi connects on first attempt
- [ ] API endpoint responds with valid JSON
- [ ] Portfolio/Events/Holdings data is parsed correctly
- [ ] Pages rotate every 30 seconds
- [ ] Serial output shows smooth cycling through 3 pages
- [ ] Offline mode triggers when WiFi is disconnected
- [ ] No JSON parse errors or HTTP errors

✅ **Ready for production when:**
- [ ] Simulation mode runs without errors for 5+ minutes
- [ ] All 3 pages display correctly
- [ ] Pages rotate on schedule
- [ ] WiFi reconnects automatically if temporarily lost

---

## Next: Production Hardware

Once all tests pass, your LED wall is ready for physical panels!

Refer to **IMPLEMENTATION.md Part 3** for wiring and first boot with real panels.
