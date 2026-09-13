> **Historical (pre-round-49).** This doc describes the original 3-page scrolling-ticker design, superseded by a full screen-rotation rewrite in rounds 49-62. Kept for history — see [`docs/JSON-CONTRACT.md`](JSON-CONTRACT.md) for the current source of truth, and the main [README](../README.md) for what's still accurate here (wiring/hardware content generally is; page-list/software content generally isn't).

---

# ESP32 LED Wall — Implementation Guide

This guide walks you through building and deploying the LED wall display system.

---

## Part 1: Backend Setup (Orange Pi)

### Step 1: Add the `/api/matrix` Endpoint

1. Open `backend/server.js` on your Orange Pi (or your local dev machine)
2. Find the section marked `// ----- reading` (around line 40)
3. After the `app.get("/api/display", ...)` route, add the `/api/matrix` endpoint code from `backend/api-matrix-endpoint.js`

The endpoint will:
- Fetch today's portfolio stats
- Collect today's events
- Grab top 5 holdings
- Calculate daily busy score
- Return a small (~1-2KB) JSON payload

### Step 2: Restart the Backend

```bash
# On the Orange Pi (or via SSH)
cd ~/Orange-Pi-Secretary
git add backend/server.js
git commit -m "Add /api/matrix endpoint for LED wall"
npm start
```

Test the endpoint:
```bash
curl http://192.168.0.130:3001/api/matrix | jq .
```

You should see JSON with portfolio, events, holdings, and dailyBusyPercent.

---

## Part 2: Arduino Setup (Your Computer)

### Step 1: Install Arduino IDE & Libraries

1. Download Arduino IDE (or use VS Code with Arduino extension)
2. Go to **Sketch → Include Library → Manage Libraries**
3. Install these libraries:
   - `ESP32-HUB75-MatrixPanel-DMA` (search for "HUB75")
   - `ArduinoJson` (for JSON parsing)

### Step 2: Set Up the Sketch

1. Create a new sketch in Arduino IDE
2. Copy the entire code from `firmware/esp32-led-wall.ino`
3. Edit the configuration at the top:
   ```cpp
   const char* SSID = "YOUR_SSID";           // Your WiFi network
   const char* PASSWORD = "YOUR_PASSWORD";   // WiFi password
   const char* PI_URL = "http://192.168.0.130:3001/api/matrix"; // Orange Pi LAN address
   ```

4. Verify the HUB75 GPIO pin assignments match your ESP32-S3 wiring. If you're using a standard ESP32-S3 DevKit, the defaults should work, but check your panel's pinout docs.

### Step 3: Connect ESP32 to Your Computer

1. Plug the ESP32-S3 into your computer via USB-C
2. In Arduino IDE: **Tools → Board → esp32 → ESP32S3 Dev Module**
3. **Tools → Port → COM[X]** (select your USB port)

### Step 4: Flash the Firmware

1. Click **Upload** (or Ctrl+U)
2. Watch the serial monitor (**Tools → Serial Monitor**) at 115200 baud
3. You should see:
   ```
   === ESP32 LED Wall ===
   Connecting to WiFi: [YOUR_SSID]
   ...
   WiFi connected! IP: 192.168.x.x
   Polling http://192.168.0.130:3001/api/matrix...
   Data fetched successfully
   Setup complete!
   ```

---

## Part 3: Panel Wiring & Testing

### Step 1: Connect the Panels

**HUB75 Pinout (standard):**
- Data: R1, G1, B1, R2, G2, B2 (data lines)
- Address: A, B, C, D (row selection)
- Control: LAT (latch), OE (output enable), CLK (clock)
- Power: GND, 5V

**ESP32-S3 to Panel (default config):**
```
ESP32-S3    → Panel
GPIO 25     → R1
GPIO 26     → G1
GPIO 27     → B1
GPIO 14     → R2
GPIO 12     → G2
GPIO 13     → B2
GPIO 23     → A
GPIO 19     → B
GPIO 5      → C
GPIO 17     → D
GPIO 18     → E (if using 1/32 scan, not needed for 32×64)
GPIO 4      → LAT
GPIO 15     → OE
GPIO 16     → CLK

GND         → GND (multiple)
5V from PSU → 5V (multiple)
```

**Chaining the 3 panels:**
- Connect panels **horizontally** (left to right)
- Output of first panel's HUB75 connector → Input of second panel
- Output of second panel → Input of third panel
- All panels share the same data/address/control lines from ESP32

### Step 2: Power Supply

- Use a **separate 5V power supply** for the panels (at least 10A, depending on brightness)
- Connect panel 5V/GND to PSU 5V/GND
- Optionally connect ESP32 5V rail to PSU for shared ground reference (use a buck converter if PSU is higher than 5V)

### Step 3: First Boot

1. Plug in the 5V PSU for the panels
2. Plug in the ESP32 via USB (or via 5V from PSU if using a barrel jack)
3. Watch the serial monitor
4. The display should light up with "Initializing..."
5. Once WiFi connects and data is fetched, you should see:
   - Page 1 (30s): Portfolio Overview
   - Page 2 (30s): Events Timeline
   - Page 3 (30s): Holdings (cycling)
   - Then repeat

---

## Part 4: Troubleshooting

### "WiFi not connected"
- Double-check SSID and password in the sketch
- Verify ESP32 can see your WiFi network (scan networks if needed)
- Check WiFi signal strength (move closer to router if weak)

### "HTTP error: 0 or 404"
- Verify Orange Pi is running (`curl http://192.168.0.130:3001/api/health`)
- Check the Pi's LAN IP address (may have changed)
- Ensure firewall allows port 3001

### "JSON parse error"
- Manually fetch the API response: `curl http://192.168.0.130:3001/api/matrix`
- Verify it's valid JSON
- Check ArduinoJson library is installed

### Panels not displaying anything
- Verify all GPIO connections (recheck pin numbers)
- Try a simple test sketch that just fills the display with a solid color
- Ensure 5V PSU is plugged in and power cables are secure

### Only some panels light up
- Check chaining connectors between panels
- Verify PANEL_COUNT is set to 3 in the sketch

---

## Part 5: Next Steps

### Phase 2: Dashboard Control Page (Optional)

Once the wall is working, you can add a control page to your Orange Pi dashboard:

1. Create a new endpoint: `POST /api/matrix/settings`
   ```json
   {
     "currentPage": 0,
     "enabledPages": [true, true, true],
     "refreshInterval": 30
   }
   ```

2. Modify the sketch to fetch and apply these settings

3. Add UI buttons on your dashboard to control page selection

### Phase 3: Battery Power

When you're ready to go wireless:
1. Use a 5V Li-Po battery bank with at least 10Ah (for ~4-5 hours of runtime)
2. Add a battery management circuit (optional: low-battery alert)
3. Modify the code to sleep/dim during inactivity to extend battery life

### Phase 4: Data Customization

- Adjust which holdings to show (currently top 5)
- Add more events filtering (filter by calendar, by category)
- Customize fonts and colors
- Add a real dashboard control panel for instant page switching

---

## Files to Keep

After implementation:
- `backend/server.js` (with `/api/matrix` endpoint added)
- `firmware/esp32-led-wall.ino` (your Arduino sketch)
- Wiring diagram or photo of your panel setup

Save these to your project repo for future reference.
