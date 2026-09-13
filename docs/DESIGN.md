> **Historical (pre-round-49).** This doc describes the original 3-page scrolling-ticker design, superseded by a full screen-rotation rewrite in rounds 49-62. Kept for history — see [`docs/JSON-CONTRACT.md`](JSON-CONTRACT.md) for the current source of truth, and the main [README](../README.md) for what's still accurate here (wiring/hardware content generally is; page-list/software content generally isn't).

---

# ESP32 LED Wall Display — Design & Implementation Plan

**Hardware:** 3x Lyson 32×64 LED panels, chained horizontally → **192×32 total resolution**  
**Controller:** ESP32-S3, Arduino IDE  
**Network:** WiFi polling from Orange Pi backend (30s interval)  
**Pages:** 3 rotatable display modes (auto-rotate every 30 seconds)

---

## Display Layout & Mockups

### Page 1: Market Ticker (scrolling)
```
┌────────────────────────────────────────────┬─────────┐
│ +0.4% TSX | -0.1% NASDAQ | +0.3% S&P  |    │   ^     │
│  +2.3% AAPL | -1.4% META | +0.8% MSFT ...  │  HOLD   │
│  ←—— continuously scrolling, seamless loop  │ +1.0%   │
│                                              │  $+825  │
└────────────────────────────────────────────┴─────────┘
```
- **Left (150px):** Continuous scrolling ticker — percent first, then symbol
  (`+2.5% TSX`). Markets (TSX/NASDAQ/S&P) first, then a gap, then the top 3
  holdings gainers and top 3 losers sprinkled together (not sequential
  blocks). Loops seamlessly — a second copy is drawn right behind the first.
- **Right (40px):** Portfolio side panel — up/down indicator (^ or v,
  green/red), "HOLD" label, day %, day $.
- **Font:** Small (textSize 1) for the ticker, size 2 for the indicator.
- **Dynamic:** Scrolls 2px every 50ms, independent of the 30s page rotation
  and the 30s data poll — the scroll never stalls waiting on either.

### Page 2: Events Timeline
```
┌──────────────────────────────────────────────────────┐
│  TODAY'S EVENTS                                      │
│                                                      │
│  10:00 [●busy#] Team standup                        │
│  14:30 [●med#]  1:1 with manager                    │
│  16:00 [●light#] Personal time                      │
│  18:00 [●busy#]  Project review                     │
│                                                      │
│  Busy: ████░ (68%)                                  │
└──────────────────────────────────────────────────────┘
```
- **Data:** Today's events (time, title), busy score (dot with %), busy% bar
- **Busy dot colors:** Red (busy), Yellow (medium), Green (light)
- **Font:** Small to fit multiple events
- **Static:** No scrolling

### Page 3: Portfolio Detail (Scrolling)
```
┌──────────────────────────────────────────────────────┐
│  TOP HOLDINGS                                        │
│                                                      │
│  [Holding 1]  $[Value]  [+/-]%  [Weight]%          │
│                                                      │
│  [Holding 2]  $[Value]  [+/-]%  [Weight]%          │
│                                                      │
│  [Scrolls through top N holdings]                   │
└──────────────────────────────────────────────────────┘
```
- **Data:** Top 5 holdings, value, day change %, portfolio weight %
- **Behavior:** Cycle through holdings (show each for ~4-5s)
- **Font:** Small
- **Dynamic:** Rotating display

### Page 4: Offline / Fallback
```
┌──────────────────────────────────────────────────────┐
│  Could not connect to server                        │
│                                                      │
│  Last refresh: 3 minutes ago                        │
│                                                      │
│  [Retrying...]                                      │
└──────────────────────────────────────────────────────┘
```
- Shows when Pi is unreachable
- Updates "last refresh" counter every minute
- Continues polling in background

---

## API Endpoint Design

### New Endpoint: `GET /api/matrix`

**Purpose:** Slim, real-time data for the LED wall (no full page payload, just the numbers)

**Zero external API calls.** Every field is read from meta blobs the regular
15-minute pull cycle already wrote — `moneySummary` (money.js) and
`marketPulse` (marketNews.js). See the header comment in
`backend/api-matrix-endpoint.js` for exactly which cached field feeds which
JSON field. Want fresher numbers? Raise `config.schedule.pullEveryMinutes` —
don't add a fetch to this route.

**Response:**
```json
{
  "timestamp": 1693478400000,
  "lastRefresh": 1693478350000,
  "portfolio": {
    "total": 125450.50,
    "dayChange": 1250.75,
    "dayChangePercent": 1.01
  },
  "markets": [
    { "symbol": "TSX", "changePercent": 0.42 },
    { "symbol": "NASDAQ", "changePercent": -0.18 },
    { "symbol": "S&P", "changePercent": 0.31 }
  ],
  "gainers": [
    { "symbol": "AAPL", "changePercent": 2.3 }
  ],
  "losers": [
    { "symbol": "META", "changePercent": -1.4 }
  ],
  "events": [
    {
      "time": "10:00",
      "title": "Team standup",
      "busyLevel": "busy"
    },
    {
      "time": "14:30",
      "title": "1:1 with manager",
      "busyLevel": "medium"
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

**Payload size:** ~2-3KB (vs. full `/api/display` which is much larger)

---

## ESP32 Firmware Architecture

### Main Components:

1. **WiFi Manager**
   - Connect to Orange Pi LAN
   - Graceful offline handling

2. **HTTP Poller**
   - Fetch from `/api/matrix` every 30 seconds
   - Update last-refresh timestamp
   - Handle network errors

3. **Page Manager**
   - Rotate through 3 pages every 30 seconds
   - Render current page data

4. **Renderers** (one per page)
   - Market Ticker → scroll ticker text + render portfolio side panel
   - Events Timeline → render events + busy bar
   - Portfolio Detail → render rotating holdings
   - Offline → render error message

5. **Display Driver**
   - HUB75 library initialization (ESP32-HUB75-MatrixPanel-DMA)
   - Basic graphics functions (text, rectangles, circles)

### Polling & Timing:
```
Main loop:
  - Every 30s: Fetch data from /api/matrix
  - Every 30s: Rotate to next page
  - Continuous: Render current page (update scrolling/animations each frame)
  - On error: Flip to offline page, keep retrying
```

---

## Font & Graphics Constraints

**192×32 pixels** means small text. Recommendations:
- **4×6 pixel font:** ~30 chars per line (title/headers)
- **3×5 pixel font:** ~40 chars per line (content)
- **2×4 pixel font:** ~60 chars per line (tight fit, last resort)

For scrolling text: Move 1-2 pixels per frame at 15-30 FPS for smooth animation.

---

## Future Enhancements (Dashboard Control Page)

- Add toggles to enable/disable specific pages
- Add button to jump to a specific page instantly
- Control refresh interval
- Display battery status (once wireless)

---

## Hardware Notes

- **Lyson 32×64 panels:** HUB75 protocol, standard pinout
- **ESP32-S3:** Plenty of RAM (8MB) and storage (16MB) for this task
- **Library:** `ESP32-HUB75-MatrixPanel-DMA` (Arduino Library Manager)
- **Power:** 5V for panels (separate supply), 5V USB for ESP32 (for testing; battery later)
