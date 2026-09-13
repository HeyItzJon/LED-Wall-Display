# Round 64 — ESP32 LED Wall: complete JSON contract handoff

**Purpose.** This is the single source of truth for exactly what JSON `esp32-led-wall.ino` expects from the Orange Pi backend, verified line-by-line against the current firmware (2000 lines, read in full for this doc) rather than against older partial docs. It **supersedes** `round-51-esp-command-endpoint-handoff.md`, `round-57-hub75-twin-full-rewrite-handoff.md`, and `round-62-events-cal-dur-backend-gap.md` on anything they conflict with — those docs were accurate when written but the firmware has moved since (true top-3/top-3 holdings selection, dynamic dwell timers, and the new `wakeMode` field). Where this doc doesn't call out a change, the older docs' descriptions still hold. The core contract below reflects the firmware as of round 64 (2026-09-10); **a round-68 addendum at the bottom** documents a brand-new screen (Sleep & Alarm) that exists only as a design prototype in the twin so far — nothing below that addendum is in the firmware or backend yet. Where a claim is about the *backend* rather than the firmware (e.g. "is this field sent today"), that's carried over from the task notes and the older docs' gap lists, not independently re-verified against `backend/server.js` in this pass — spot-check there if in doubt.

---

## Round 72 addendum — ground truth, verified directly against the real repo, backend, and device

Everything above this point was written and revised across rounds 64-71 **without ever checking it against Jon's actual `pi-secretary`/`led-wall-display` git repos or the live ESP32** — all of that work happened in a sandboxed session with its own local copy of `esp32-led-wall.ino` and no access to Jon's machine. Round 72 is the first time this doc has been reconciled against reality, from three sources: `pi-secretary/backend/server.js` + `lib/matrixControl.js` read directly off Jon's laptop, `led-wall-display`'s git history, and the actual `.ino` Jon pulled off the running ESP32. Two real surprises came out of this, both corrected below and in the Quick status table:

1. **The `led-wall-display` git repo was frozen at an Aug 29 snapshot** — a completely different, pre-round-49 design (a single scrolling ticker mixing `markets`/`gainers`/`losers`, no screen rotation at all) that never got updated as rounds 49-71 happened. It's been replaced now (see below) with the actual current firmware.
2. **The live backend already sends more than this doc claimed, and also sends fields the firmware ignores entirely.** Specifically: `news` is actually sent today (`server.js` builds it from `marketPulse.headlines`, shaped as `{title, source}`) and the News screen genuinely works — the "**No** — not sent" claim in the table below (round 64) was never true against the real backend, it was an assumption. Separately, `server.js` also sends top-level `markets` (index % changes), `gainers`, and `losers` arrays — leftovers from the pre-round-49 ticker design that the current screen-rotation firmware doesn't read *at all* (the Markets screen is still a coming-soon placeholder; Holdings does its own top-3-up/top-3-down selection straight from `holdings[]`, not from `gainers`/`losers`). These three fields are pure dead weight in every `/api/matrix` response right now — safe to strip once the backend gets its round-64-contract update, or safe to leave if that's easier for now.

**What's actually running on the ESP32 today** (confirmed by diffing Jon's pulled `.ino` against this Project's copy — only 92 lines different, both additive): the full round-49-62 screen-rotation rewrite, live-control polling, the round-56 SSID case-sensitivity fix, and complete parsing of `events[].cal/.dur/.desc`, `dayOverview.hoursBusy/.hoursFree/.commuteMin`, `alert`, and `news` — all for real, all matching this doc's per-field descriptions below. The device is missing exactly two things relative to this Project's `esp32-led-wall.ino`, both purely additive (nothing to reconcile, just not ported down yet):
   - **`wakeMode`** (round 64) — not parsed at all on the device yet; the field, the render-priority check, and `renderWakeUp()` all need to be added before wake-up mode can ever fire for real.
   - **The round-63 boot-panel-init retry fix** — the device still calls `dma_display->begin()` once, unchecked, instead of the retry-with-backoff loop that was added to fix the intermittent garbled-boot issue.

This firmware file has been committed to `led-wall-display` (`firmware/esp32-led-wall/esp32-led-wall.ino`, replacing the stale Aug-29 version) as the confirmed ground truth. `README.md` and `docs/DESIGN.md` in that repo still describe the old ticker design and are now known-stale — not rewritten yet, flagged for a follow-up pass.

**Net effect on the punch list below:** items 1 (events `cal`/`dur`/`desc`) and 3 (`dayOverview`) are unchanged as open backend work — the firmware side has been ready and waiting since before round 64, confirmed now running on real hardware. Item 2 (news) is **done** — drop it from the punch list. Items 4 (alert) and 5 (wakeMode) are unchanged as open work, with wakeMode now also needing the two device-side additions above before it can do anything even once the backend sends it.

---

Two endpoints, polled independently and on different cadences:

| Endpoint | Poll interval | Purpose |
|---|---|---|
| `GET /api/matrix` | 30s (`DATA_POLL_MS`) | Real data: portfolio, events, holdings, news, dayOverview |
| `GET /api/matrix/command` | 1.5s (`COMMAND_POLL_MS`) | Live control: rotation set, pin, notification, alert, wake mode, test button |

---

## Quick status table

| Field | Endpoint | Screen(s) fed | Sent by backend today? | What happens if absent |
|---|---|---|---|---|
| `portfolio.total` / `.dayChange` / `.dayChangePercent` | `/api/matrix` | portfolio | Yes | Defaults to `0.0` each — shows `$0.00` and `+$0.00 (+0.00%)` |
| `marketOpen` | `/api/matrix` | portfolio | Presumed yes (not listed as a current gap; not independently re-verified against `server.js` here) | Key entirely absent → **no dot drawn at all** (not green, not red) — this is a presence check (`doc.containsKey("marketOpen")`), not a `|` default |
| `dailyBusyPercent` | `/api/matrix` | dayoverview | Yes | Defaults to `0` → "BUSY 0" in dark green |
| `events[].time` / `.title` / `.busyLevel` | `/api/matrix` | events, commuting, dayoverview (count) | Yes | Each defaults to `""`; a blank `busyLevel` falls into the "light" (green) bucket of `calColorFallback` |
| `events[].cal` | `/api/matrix` | events | **Yes — round 72**, closes the round-62 gap. `server.js` now sends the event's already-computed `swatch` (from `sources/calendar.js`'s `calendarSwatch()` at ingestion) | Was: falls back to `busyLevel`-derived color (red/amber/green) instead of the real calendar color. Committed to Jon's dev copy (`pi-secretary` `fd9985a`), **not yet deployed to the Orange Pi** |
| `events[].desc` | `/api/matrix` | events | **Yes — round 72.** Reuses the existing concise note/time/location/attendee one-liner (`e.detail`) already built for the Tasks/Day list rows, truncated to 60 chars | Was: no second scrolling line drawn under the title. Same commit/deploy status as `cal` above |
| `events[].dur` | `/api/matrix` | events | **Yes — round 72.** Real end-minus-start minutes from `meta.end`, clamped 5-600min, skipped (falls back to 30) for all-day events | Was: defaults to `30` — every event rendered as a uniform 30-minute sliver regardless of real length. Same commit/deploy status as `cal` above |
| `holdings[].symbol` / `.value` / `.dayChangePercent` | `/api/matrix` | holdings | Yes | Each defaults to `0.0`/`""`; if the `holdings` array itself is absent, ticker shows "NO HOLDINGS DATA" |
| `holdings[].weightPercent` | `/api/matrix` | holdings | Yes (parsed) | **Parsed into the struct but never read by the current ticker renderer** — has no visual effect whether sent or not; see note in Holdings section |
| `news` | `/api/matrix` | news | **Yes** — corrected round 72, was wrongly marked "No" through round 71 (see round-72 addendum above); `server.js` sends `[{title, source}]` from `marketPulse.headlines`, which the `item["title"]` fallback in the parser below handles correctly | Confirmed working on the real device today; only shows the placeholder if `marketPulse.headlines` itself comes back empty |
| `markets` (top-level array), `gainers`, `losers` | `/api/matrix` | none — not read by this firmware at all | Yes — sent, but **dead weight** (round 72 finding, see addendum above) | No effect either way; leftover from the pre-round-49 ticker design this firmware replaced |
| `dayOverview.hoursBusy` + `.hoursFree` | `/api/matrix` | dayoverview | **Yes — round 72.** Same `weekForecast()` math the Week page already uses (`filterLive` → calendar-only → `weekForecast`), asked for a single day instead of 7 | Was: second row showed dim "HOURS DATA COMING SOON". Committed to Jon's dev copy (`pi-secretary` `fd9985a`), **not yet deployed to the Orange Pi** |
| `dayOverview.commuteMin` | `/api/matrix` | commuting | **No** — still open, deliberately not built in round 72 ("I haven't figured out the logic for that" — Jon) | Shows "NEXT: `<title>`" + "COMMUTE ETA COMING SOON" instead of "LEAVE BY ..." (independent of hoursBusy/hoursFree — gated on its own key) |
| `enabledScreens` | `/api/matrix/command` | rotation set | Yes | Falls back to hardcoded default `portfolio, events, holdings, markets, news, weather`, plus `clock`/`dayoverview`/`commuting` always appended |
| `pinnedScreen` | `/api/matrix/command` | rotation override | Yes | `null`/absent → normal rotation |
| `notification.text` / `.secondsRemaining` | `/api/matrix/command` | notification overlay | Yes | `null`/absent → overlay never shows |
| `alert.severity` / `.text` | `/api/matrix/command` | alerts overlay | **No** — open gap (round 57) | Never fires; no visual at all, no fallback screen |
| `wakeMode` | `/api/matrix/command` | wake-up overlay | **No** — brand new field, open gap (round 64) | Defaults `false` via `doc["wakeMode"] \| false`; never fires |
| `testEvent.id` / `.label` | `/api/matrix/command` | none visual — Serial log only | Yes | `null`/absent → nothing logged |

---

## Render-priority tiers (in `loop()`)

Highest wins; each check does `render...(); flipDMABuffer(); return;` before falling through to the next:

```
1. FORCE_SCREEN (compile-time constant, bench test only — currently "")
2. offline screen        — !dataValid || (millis - lastDataSuccessTime > STALE_THRESHOLD_MS)
3. alertActive            — /api/matrix/command "alert" key present and non-null
4. wakeModeActive         — /api/matrix/command "wakeMode" == true          [NEW round 64]
5. notificationActive     — /api/matrix/command "notification" key present and non-null
6. pinnedScreen           — /api/matrix/command "pinnedScreen" non-empty
7. normal auto-rotation   — getActiveScreens() + ROTATION_MS, with dynamic dwell extension
```

`STALE_THRESHOLD_MS = 95000` (~3 missed 30s data polls). The exact code, in order:

```cpp
bool offline = !dataValid || (now - lastDataSuccessTime > STALE_THRESHOLD_MS);
if (offline) { renderOffline(now); ...; return; }
if (alertActive) { renderAlert(now); ...; return; }
// Below alert in priority on purpose — a real system alert ... should still
// win over a friendly good-morning screen. Above notification/pinned/rotation,
// since the whole point of wake-up mode is that it takes over the display
// for its window rather than waiting its turn in the normal rotation.
if (wakeModeActive) { renderWakeUp(now); ...; return; }
if (notificationActive) { renderNotification(now); ...; return; }
```

### Auto-rotation

`ROTATION_MS = 12000` — each active screen id normally gets a flat 12-second slot, selected by `(now / ROTATION_MS) % activeCount`. Two screens can hold their slot **longer** than 12s if their content hasn't finished scrolling once:

```cpp
if (currentScreenId == "news" && targetId != "news") {
  unsigned long needed = max((unsigned long)ROTATION_MS, newsRequiredTime());
  if (now - currentScreenStart < needed) targetId = "news";
}
if (currentScreenId == "holdings" && targetId != "holdings") {
  unsigned long needed = max((unsigned long)ROTATION_MS, holdingsRequiredTime());
  if (now - currentScreenStart < needed) targetId = "holdings";
}
```

- `newsRequiredTime()` returns `0` (no extension) if the joined headline string already fits statically in 192px; otherwise it computes exactly how long the ticker needs to scroll through once at `NEWS_SCROLL_SPEED_PX_MS = 0.035`.
- `holdingsRequiredTime()` calls `buildHoldingsStrip()` itself (so it can never drift from what's actually drawn) and computes how long the full HOLDINGS strip (header + up to 3 up + 3 down entries) needs to scroll once at `speedPxPerMs = 0.05`. A full 3-and-3 split easily needs 30+ seconds — well past the fixed 12s slot.

**Screen set** (`getActiveScreens()`):
- If `/api/matrix/command` has ever returned `enabledScreens`, that list is used verbatim.
- Otherwise (never polled successfully yet), the hardcoded default is `portfolio, events, holdings, markets, news, weather`.
- **Always appended**, regardless of what `enabledScreens` says, if not already present: `clock`, `dayoverview`, `commuting` — these are firmware-local screens with no id in the backend's `SCREENS` catalog, so the web Wall tab cannot currently toggle them off.
- `stars`, `balls`, `alerts`, `wakeup` are **never** in the rotation set (not in the default list, not appended) — reachable only via `FORCE_SCREEN` (compile-time) or, for alerts/wakeup, via their own priority-tier trigger.

---

## Per-screen detail

### portfolio
```json
"portfolio": { "total": 48213.55, "dayChange": 312.40, "dayChangePercent": 0.65 },
"marketOpen": true
```
```cpp
portfolioTotal            = doc["portfolio"]["total"] | 0.0;
portfolioDayChange        = doc["portfolio"]["dayChange"] | 0.0;
portfolioDayChangePercent = doc["portfolio"]["dayChangePercent"] | 0.0;
hasMarketOpen = doc.containsKey("marketOpen");
if (hasMarketOpen) marketOpen = doc["marketOpen"].as<bool>();
```
Big centered dollar total, signed day-change line below it. Top-right dot: pulsing green "LIVE" when `marketOpen == true`, static red when `false`, **no dot at all** if the `marketOpen` key is missing entirely (this is a `containsKey` gate, not a `| false` default — closed and "field never sent" look different only in that one is silent).

### events
```json
"events": [
  { "time": "2:00 PM", "title": "Team sync", "busyLevel": "busy",
    "cal": "work", "desc": "Weekly standup", "dur": 30 }
]
```
```cpp
events[numEvents].time      = v["time"] | "";
events[numEvents].title     = v["title"] | "";
events[numEvents].busyLevel = v["busyLevel"] | "";
events[numEvents].cal       = v["cal"] | "";          // not sent yet
events[numEvents].desc      = v["desc"] | "";          // not sent yet
events[numEvents].dur       = v["dur"] | 30;
```
Full-width scrolling caption/description at top, a horizontal timeline bar below with a live pulsing "now" cursor, colored per-event via:
```cpp
uint16_t calColorFallback(const String &cal, const String &busyLevel) {
  if (cal.length() > 0) return calColor(cal);   // real calendar color — not reached today
  if (busyLevel == "busy") return ...red...;
  if (busyLevel == "medium") return ...amber...;
  return ...green...;                            // what actually shows today
}
```
`calColor()` maps `work/school/personal/important/cannotmiss/tests` to specific colors matching the intended website `calendarSwatch()` palette (see round-62 for the exact hex-equivalent RGB triples) — dormant until `cal` is sent. `desc` only draws a second line if non-empty. `dur` sizes each event's timeline bar (`startMin` to `startMin + dur`); with `dur` defaulting to 30 for every event, all bars are currently the same width regardless of real length. `numEvents == 0` shows "NO EVENTS TODAY". Each event also gets a minimum 4s on-screen hold (`EVENT_MIN_HOLD`) even if its caption doesn't need to scroll.

### holdings
```json
"holdings": [
  { "symbol": "AAPL", "value": 4210.50, "dayChangePercent": 1.85, "weightPercent": 8.7 }
]
```
```cpp
holdings[numHoldings].symbol            = v["symbol"] | "";
holdings[numHoldings].value             = v["value"] | 0.0;
holdings[numHoldings].dayChangePercent  = v["dayChangePercent"] | 0.0;
holdings[numHoldings].weightPercent     = v["weightPercent"] | 0.0;
```
`weightPercent` is parsed and stored but **`buildHoldingsStrip()` never reads it** — it has no current visual effect either way. Not a bug to fix on the backend; just noted so nobody spends effort tuning a number the firmware doesn't display yet.

**Selection is now a true two-pass top-3-up + top-3-down**, not a single top-6-by-percent sort (this is the round-61 fix that the older round-51/57 docs predate):
```cpp
bool used[MAX_HOLDINGS] = { false };
for (int pick = 0; pick < 3; pick++) {           // pass 1: best 3 with pct >= 0
  ...
  if (used[i] || holdings[i].dayChangePercent < 0) continue;
  if (best == -1 || holdings[i].dayChangePercent > holdings[best].dayChangePercent) best = i;
  ...
}
for (int pick = 0; pick < 3; pick++) {           // pass 2: worst 3 with pct < 0, excluding pass 1's picks via used[]
  ...
}
```
Each selected holding renders as symbol + signed % + signed day-$ (`value * dayChangePercent / 100`), separated by vertical bars, scrolling continuously. `numHoldings == 0` → "NO HOLDINGS DATA". Dynamic dwell (`holdingsRequiredTime()`, described above) holds the screen up long enough to scroll through the whole built strip at least once, extending past the fixed 12s rotation slot when needed.

### news
Accepts three shapes — pick whichever is easiest on the backend:
```json
"news": ["Headline one", "Headline two"]
```
```json
"news": [{ "headline": "Headline one" }, { "title": "Headline two" }]
```
```json
"news": "Single headline string"
```
```cpp
if (item.is<const char*>()) headline = item.as<String>();
else headline = (item["headline"] | (const char*)(item["title"] | ""));
```
Comment in the firmware flags this shape as unconfirmed against real backend output — "Confirm the real shape once deployed and adjust this block if it differs." **Corrected round 72: this is sent today and confirmed working.** `server.js` sends `news` as `[{title, source}]`; the `item["title"]` half of the fallback line above picks it up correctly (the `headline` key it also checks for is just never populated). The News screen is live on the real device — through round 71 this doc wrongly claimed it fell back to `numNews == 0` and the cycling-hue "NEWS / COMING SOON" placeholder; that placeholder only shows if `marketPulse.headlines` itself is empty, not because the field is unsent. If it fits in 192px statically it's centered; otherwise it scrolls, with `newsRequiredTime()` extending the rotation slot to let it scroll through once (see rotation section).

### clock
Firmware-local, no `/api/matrix` fields at all. NTP via `pool.ntp.org`/`time.nist.gov`, `TZ_INFO = "EST5EDT,M3.2.0/2,M11.1.0/2"` (America/Toronto incl. DST). Shows "SYNCING..." until `getLocalTime()` first succeeds. **Not in the backend's `SCREENS` catalog** — always rides along in rotation via the `LOCAL_ONLY` append in `getActiveScreens()`, regardless of `enabledScreens`.

### dayoverview
```cpp
int score = max(0, min(10, (int)round(dailyBusyPercent / 10.0)));   // from /api/matrix, sent today
String evText = String(numEvents) + (numEvents == 1 ? " EVENT" : " EVENTS");  // derived from events[] count
```
Busy-score dot + "BUSY N" (0–10 scale, colored via `busyScoreColor()`) and event count always render — both already real today. Second row (`X.XH BUSY` / `X.XH FREE`) only draws if **both** `dayOverview.hoursBusy` and `dayOverview.hoursFree` are present together:
```cpp
if (dov.containsKey("hoursBusy") && dov.containsKey("hoursFree")) {
  dayOverview.hasHours = true;
  dayOverview.hoursBusy = dov["hoursBusy"] | 0.0;
  dayOverview.hoursFree = dov["hoursFree"] | 0.0;
}
```
Otherwise: dim "HOURS DATA COMING SOON" centered text. Note the commute number is **intentionally not shown here** any more — comment: "there's a dedicated Commuting page for that now." Not in the backend `SCREENS` catalog — always in rotation, same as clock.

### commuting
```json
"dayOverview": { "commuteMin": 22 }
```
```cpp
if (dov.containsKey("commuteMin")) {
  dayOverview.hasCommute = true;
  dayOverview.commuteMin = dov["commuteMin"] | 0;
}
```
Independently gated from `hoursBusy`/`hoursFree` — `commuteMin` alone is enough. Picks the next upcoming event via `nextUpcomingEventIdx()` (soonest event ≥ now, wrapping to the first event of the day if everything's already passed). If `hasCommute`: "LEAVE BY `<time>`" + "`N` MIN TO `<title>`". If not: "NEXT: `<title>`" + "COMMUTE ETA COMING SOON" (never fabricates a number). If `numEvents == 0`: "NO UPCOMING EVENTS". A full-height G-Wagon sprite always drives left-to-right regardless of data state, revealing the text behind it like a curtain (real Google Maps ETA is a separate documented future Pi feature per `integration-roadmap.md` #8 — `commuteMin` is explicitly a placeholder concept until then). Not in the backend `SCREENS` catalog — always in rotation.

### markets / weather
No fields parsed for either — both always render the generic `renderComingSoonFwd(id, now)` placeholder (screen id capitalized as the title, cycling-hue color, "COMING SOON" subtitle). They're in the firmware's hardcoded default rotation list and (per round-51) exist as ids in the backend's `SCREENS` catalog already — this doc doesn't add new backend work for them since no renderer exists yet regardless of what's sent.

### alerts
```json
"alert": { "severity": "high", "text": "Furnace filter overdue" }
```
```cpp
if (doc.containsKey("alert") && !doc["alert"].isNull()) {
  alertActive = true;
  alertSeverity = doc["alert"]["severity"] | "medium";
  alertText = doc["alert"]["text"] | "";
} else {
  alertActive = false;
}
```
`severity` is `low | medium | high`, controlling border color pair and warning-triangle tint (`alertLevelFor()`); defaults to `"medium"` if the key is present but `severity` isn't. Full-screen marching hazard-stripe border + blinking warning triangle + up to two wrapped lines of text. **Not sent by the backend at all today** — no push/alert channel exists (confirmed per round-53's "no push notifications yet" note) — so this never fires. Fully wired; the instant `/api/matrix/command` adds a non-null `alert`, it lights up with zero firmware change. Priority: second-highest tier, above wake mode/notification/pin/rotation, below only the offline screen and `FORCE_SCREEN`.

### wakeup — NEW round 64
```json
"wakeMode": true
```
```cpp
wakeModeActive = doc["wakeMode"] | false;
```
Plain top-level boolean, no nested object, no message/severity fields. When `true`: full-screen size-2 "WAKE UP MODE" text, centered, color breathing orange↔gold (`hsvToColor565(30 + 20*pulse, 255, 255)` on a slow sine). **Not sent by the backend today** — this is a brand-new field with no producer yet. Priority: third-highest tier — below `alert`, above `notification`/`pinnedScreen`/rotation, i.e. it takes over the display for its whole active window rather than waiting a turn in rotation (explicit in the code comment). This is intentionally a placeholder: "the only requirement so far is 'the screen says WAKE UP MODE'; can grow a custom message field later without any compatibility break, since the firmware just ignores JSON fields it doesn't parse." Also reachable via `FORCE_SCREEN "wakeup"` for bench testing without any backend involvement.

### stars / balls
Ambient demo effects (sparkle field / bouncing balls), no data fields, **never in the normal auto-rotation** (not in the hardcoded default list, not in `LOCAL_ONLY`, no backend catalog entry). Fully implemented, reachable only by setting `#define FORCE_SCREEN "stars"` (or `"balls"`) and reflashing. Not something the backend can currently trigger at all.

### notification
```json
"notification": { "text": "Markets are closed — see you Monday", "secondsRemaining": 12 }
```
```cpp
if (doc.containsKey("notification") && !doc["notification"].isNull()) {
  notificationActive = true;
  notificationText = doc["notification"]["text"] | "";
  notificationSecondsRemaining = doc["notification"]["secondsRemaining"] | 0;
} else {
  notificationActive = false;
}
```
Rainbow-bordered full-screen overlay; text (and recognized emoji: star/heart/check/three warning-triangle severities/four faces) is built into a single-line strip, centered if it fits in 192px, otherwise scrolling. This channel is already sent by the backend today and working (per round-51, live-tested against a running backend). Priority: fourth tier — below alert/wakeMode, above pinned/rotation.

### offline / connecting
No backend fields — entirely derived from local firmware state:
```cpp
if (WiFi.status() != WL_CONNECTED) reason = "WIFI DISCONNECTED";
else if (!dataValid) reason = "NO DATA YET";
else reason = "CAN'T REACH PI";
```
Triggered whenever `!dataValid || (now - lastDataSuccessTime > STALE_THRESHOLD_MS)` (95s ≈ 3 missed 30s polls). Highest-priority screen after the compile-time `FORCE_SCREEN` override — beats even `alert`.

### boot / status screens (setup-time only, not part of the rotation catalog)
`drawStatusScreen()` renders "CONNECTING TO WIFI", "WIFI CONNECTED", and "ORANGE PI SECRETARY / Fetching data..." once during `setup()`, before `loop()` (and its rotation logic) ever runs. `renderBootFrame()` plays the "HELLO JON" / "WELCOME BACK" star-field splash, also setup-only. Neither reads any backend JSON.

---

## `/api/matrix` — full expected shape (every field)

```json
{
  "portfolio": {
    "total": 48213.55,
    "dayChange": 312.40,
    "dayChangePercent": 0.65
  },
  "marketOpen": true,
  "dailyBusyPercent": 62,
  "events": [
    { "time": "2:00 PM", "title": "Team sync", "busyLevel": "busy",
      "cal": "work", "desc": "Weekly standup", "dur": 30 }
  ],
  "holdings": [
    { "symbol": "AAPL", "value": 4210.50, "dayChangePercent": 1.85, "weightPercent": 8.7 }
  ],
  "news": ["Headline one", "Headline two"],
  "dayOverview": {
    "hoursBusy": 4.5,
    "hoursFree": 11.5,
    "commuteMin": 22
  }
}
```
Parsed into a `DynamicJsonDocument(6144)`. Every field except `portfolio.*`/`dailyBusyPercent`/`events[].time,title,busyLevel`/`holdings[].*` is currently either absent from real backend responses or unconfirmed — see the Quick Status table above for exactly which. `marketOpen` uses presence (`containsKey`), not a JSON-null-safe default, so omit the key entirely (don't send `false`) only if you actually want no dot at all — sending `"marketOpen": false` is how you get the static red "closed" dot.

## `/api/matrix/command` — full expected shape (every field)

```json
{
  "enabledScreens": ["portfolio", "events", "holdings", "markets", "news", "weather"],
  "pinnedScreen": null,
  "notification": { "text": "Markets are closed — see you Monday", "secondsRemaining": 12 },
  "alert": { "severity": "high", "text": "Furnace filter overdue" },
  "wakeMode": false,
  "testEvent": { "id": 4, "label": "Button 1 Pressed" }
}
```
Parsed into a `DynamicJsonDocument(1536)` — this is a small, cheap, frequently-polled document; keep it lean if adding fields (1536 bytes is not a lot of headroom once `alert`/`wakeMode`/a longer `enabledScreens` are all populated at once — worth sanity-checking actual payload size once several of these gaps are filled simultaneously). `alert` and `wakeMode` are shown here with real shapes/values for completeness even though neither is sent today — `alert: null` (or the key omitted) and `wakeMode: false` (or omitted) are both safe/current. `testEvent` is serial-log-only, no visual effect; the firmware tracks the last-seen `id` and only logs on change:
```cpp
if (id != lastTestEventId) { lastTestEventId = id; Serial.println(label); }
```

---

## What to build this weekend — prioritized punch list

~~1. Events `cal` + `dur` + `desc`.~~ **Done — round 72**, committed to `pi-secretary` (`fd9985a`). `cal` reuses each event's `swatch` (already computed at ingestion by `calendarSwatch()`), `dur` is real end-minus-start minutes from `meta.end` (clamped 5-600min, all-day events fall back to 30), `desc` reuses the existing `e.detail` one-liner. Fixes both symptoms Jon reported in round 62 (wrong colors, uniform-width bars). **Still needs**: `git push` + a Pi-side `./deploy.sh` run — this is only on Jon's Windows dev copy so far, not live.

~~2. News.~~ **Done — verified round 72.** `server.js` already sends `[{title, source}]` and the real device renders it correctly; nothing left to build here. (This doc wrongly listed it as an open item through round 71 — see the round-72 addendum.)

3. ~~`dayOverview.hoursBusy`/`.hoursFree`.~~ **Done — round 72**, committed to `pi-secretary` (`fd9985a`), same `weekForecast()` reuse this doc always recommended. Lights up the Day Overview screen's second row once deployed (same not-yet-deployed caveat as item 1 above). `commuteMin` stays **explicitly out of scope** — Jon hasn't settled on a real ETA source (OC Transpo/Maps integration, `integration-roadmap.md` #8) — the Commuting screen keeps degrading gracefully to "COMMUTE ETA COMING SOON" without it, exactly as designed.

4. **Alert channel on `/api/matrix/command`.** No producer exists yet anywhere in the backend (confirmed per round-53 — push/ping was explicitly deferred). This is genuinely new backend work, not a tweak: needs whatever decides *when* to alert (a watchdog condition, a manual trigger from the web UI, etc.) plus the field itself: `{"alert": {"severity": "low|medium|high", "text": "..."}}`, cleared by sending `null`/omitting the key.

5. **`wakeMode` trigger endpoint.** Jon is building the actual trigger mechanism himself via iPhone Shortcuts — the backend's job is narrower than it sounds: just an endpoint that flips `/api/matrix/command`'s `wakeMode` to `true`, plus some way to clear it back to `false` (a timeout, a second endpoint call, a clear button on the Wall tab — whichever fits the existing `notification`/`pinnedScreen` clearing pattern best). No message/severity payload needed yet — the firmware only reads a bare boolean. This is the smallest of the open items in terms of firmware-visible surface area, but needs a design decision on *what clears it* before it's safe to wire up (an alert-style "present until cleared" contract, same as `alert` and `notification` today).

6. **`SCREENS` catalog additions in `matrixControl.js`** for `clock`, `dayoverview`, `commuting`, and now `wakeup` too (if it should ever be pinnable/testable from the Wall tab rather than purely Shortcut-triggered). Not required for anything to work — all four already render correctly and (except `wakeup`) already ride along in rotation unconditionally — this only affects whether Jon can toggle/pin them individually from the web UI instead of them being permanently on.

---

## Addendum (round 68) — NEW: Sleep & Alarm screen, design-only, not in firmware or backend

**Status: prototype only.** This screen exists in `hub75-twin.html` (the browser simulator) as of round 68 — nothing has been ported to `esp32-led-wall.ino`, and there is no Orange Pi backend support for it in any form. This section exists so the eventual backend work and the firmware port start from one agreed target instead of two people (or two sessions) designing it twice. **This is explicitly not part of the "what to build this weekend" list above** — it needs a new backend subsystem that doesn't exist yet (see below), not just exposing a value the backend already computes.

### Why this screen exists

It ties together two things from recent rounds: wake-up mode (round 64's sunrise/"GOOD MORNING" screen) and the iPhone Shortcuts-driven alarm/sleep-schedule idea from round 64's brainstorm (a Shortcut on the sleep-focus automation that would eventually trigger wake-up mode ~2 minutes after the real alarm). This screen is the piece that shows *why* — the next alarm time and the specific event that's forcing that wake time — so there's context leading into wake-up mode rather than the alarm just appearing to fire out of nowhere.

### Screen layout

Split straight down the middle at the panel's horizontal center (x=96 of 192):

- **Left half — sleep context.** The sleep window as a compact range ("11:15PM-6:30AM"), a mini horizontal timeline spanning 8:00 PM to 9:00 AM with the actual bed-to-wake block highlighted, and small colored dots marking the nearest late-night and early-morning events on that timeline (violet = late, amber = early). The nearer event's name is shown as text underneath, alternating between the late and early event every ~2.6s when both exist (there isn't room for both names on screen at once).
- **Right half — the alarm.** Deliberately given the full right half, per Jon's explicit "at least half the screen is just gonna be about the alarm": the next alarm time in large bold text (a slow white→gold breathing pulse to keep the eye on it), captioned with a plain "NEXT ALARM" underneath (round 70 — originally showed the causing event's name here too, but that was redundant with the left half already showing it, so this half now just identifies what the big number *is*).

A thin vertical divider separates the two halves.

### JSON shape this is designed against

```json
"sleep": {
  "bedTime": "23:15",
  "wakeTime": "06:30",
  "nextAlarm": "06:30",
  "nearbyEvents": [
    { "label": "Study Group", "time": "21:30", "period": "late" },
    { "label": "DND Interview", "time": "08:00", "period": "early" }
  ]
}
```

- **All time fields are 24-hour `"HH:MM"` strings** — the same convention `events[].time` already uses in the core contract above, deliberately, so the eventual firmware port can reuse `timeToMinutes()`/`minutesToClockStr()` exactly as they exist today for both parsing and 12-hour display, with zero new string-parsing code.
- `nearbyEvents` — send as many as are relevant; the twin only ever displays the single nearest `period: "late"` entry and the single nearest `period: "early"` entry (whichever the backend lists first for each). `period` is sent explicitly rather than inferred from the clock time, since "late" and "early" are relative to that day's actual bedtime/wake time, not a fixed hour cutoff — that classification belongs on the backend, not the display. This is also now doing double duty as "the reason for the alarm" — see the next point.
- There is **no `alarmReason` field anymore** (round 70). The first design had a separate free-text reason shown under the alarm time, but that duplicated the event name already shown on the left half via `nearbyEvents`, so it was cut — the right half now just captions itself "NEXT ALARM" (a fixed label, not data-driven). The `period: "early"` entry in `nearbyEvents` is what carries "the event causing this alarm" now — if the backend needs the alarm tied to a *specific* event rather than just whichever one the timeline shows as nearest, flag that when this gets built for real, since the current design assumes those are the same event.

### A correction surfaced while designing this

Building this screen's timeline required understanding exactly how `timeToMinutes()` parses a time string — and it does **not** handle AM/PM at all:

```cpp
int timeToMinutes(const String &t) {
  int colon = t.indexOf(':');
  if (colon < 0) return 0;
  int hh = t.substring(0, colon).toInt();
  int mm = t.substring(colon + 1).toInt();
  return hh * 60 + mm;
}
```

`"2:00 PM"` parses as **2:00 AM** (hour digits taken literally, no PM adjustment) — meaning `events[].time` must actually be sent as 24-hour `"HH:MM"` (`"14:00"`, not `"2:00 PM"`) for the existing Events-screen timeline bar positions (`startMin`/`endMin`, used by the events section above) to land correctly. The `"2:00 PM"` example used in this doc's own Events section — and in the original round-49 design doc it was copied from — was never actually valid against the real parser. The twin's own mock data has always used 24-hour strings (`'10:00'`, `'14:30'`), which is presumably why this never got caught visually. **Worth a quick check on whatever the backend currently sends for `events[].time`** — if it's 12-hour format, on-screen event positions have been silently wrong the whole time, independent of anything to do with this new screen.

### What's still needed before this can go anywhere

- **A new backend subsystem** — reading the iPhone Shortcuts alarm/sleep-schedule data (once that side exists at all — see round 64's notes on the Shortcuts idea), deciding which events count as "nearby" the sleep window, and picking which upcoming event is actually driving the next alarm time. None of this exists today in any form; it's a bigger lift than anything else in this document, which is why it's called out separately instead of folded into the punch list above.
- **A firmware port** — no `renderSleepAlarm()`-equivalent exists in `esp32-led-wall.ino`, no `sleep` screen id anywhere in `getActiveScreens()` or the render-priority chain, and `pollData()` doesn't parse a `sleep` key at all yet. The port itself should be mechanical once the design and backend are settled (same pattern wake-up mode followed: reuse the existing time helpers, add one render function, add the screen id to the rotation/catalog).

This addendum exists purely so the shape above is the one thing to build against whenever both of those are ready — not so either gets rushed this weekend.

---

## Addendum (round 69-71) — screen grouping plan, and where the transition animations attach

**Status: dashboard organization + design recommendation, no rotation-logic or firmware changes.** Round 69 reorganized the twin's own control panel (`hub75-twin.html`) into labeled groups purely for dev convenience, round 70 added a recommendation for how those groups should eventually rotate and where the animation ideas from round 64's brainstorm (falling money/growing cash for finance, a ticking-clock/calendar/emails-opening lead-in for events) should attach, and **round 71 revises both the Day group's membership/order and that animation recommendation** per Jon's direct feedback on the round-70 layout. None of this is implemented as real rotation behavior yet — `AUTO_SCREENS`/`ROTATION_MS` in the twin, and the equivalent in the firmware, are untouched and still only cover `portfolio, events, holdings, dayoverview` exactly as before.

### The groups (as of round 71)

- **Finance** (Portfolio, Holdings, Markets, News) and **Day** (Day Overview, Events, Commuting, Weather) — the two real content pools, meant to blend into ambient rotation the way `portfolio`/`events`/`holdings`/`dayoverview` already do today.
- **Clock — its own group (new, round 71).** Previously grouped inside Day; pulled out because it's "more like a screensaver slash generic info screen versus an actual insight" (Jon's words) rather than something that belongs in the same rotation pool as content that's actually telling you something. It's still meant to appear in ambient rotation, just not blended in with Day's content screens — structurally parallel to Morning/Night in the chip panel (its own labeled group), though it isn't a takeover screen the way those two are; it doesn't override anything, it's just standalone.
- **Morning** (Wake Up Mode) and **Night** (Sleep & Alarm) — takeover groups. Sleep & Alarm was explicitly reclassified in round 70 to match Wake Up Mode: both are meant to override normal rotation for a window (an alarm firing, the hour before bed) rather than share a 12s slot with Finance/Day content. Neither is actually wired as a takeover in the firmware yet — Wake Up Mode already has the `wakeMode` priority tier (see the render-priority section above); Sleep & Alarm has no equivalent trigger/field at all.
- **System/Boot** (Boot, Connecting WiFi, Offline) and **Overlays & test tools** (Notification, Alerts, Stars, Balls) — dev-testing clusters only, not real rotation groups, left unchanged in round 71 per Jon's explicit "all the overlays and test stuff we just leave separate for now." These are either one-shot setup-time screens or trigger-based overlays that already sit outside normal rotation in the firmware; grouping them here is just for finding them faster in the twin's chip panel.

### Recommended order within each content group, and why

**Finance: Portfolio → Holdings → Markets → News.** Unchanged since round 70. A zoom-out: Portfolio is the headline number, Holdings explains what specifically is driving it, Markets gives the broader index context, News explains why any of it moved. Each screen answers the question the previous one raises.

**Day: Day Overview → Events → Commuting → Weather** *(changed in round 71 — was Clock → Day Overview → Events → Weather → Commuting)*. Jon's correction: Commuting should come right after Events "so I know when I have to leave for the events" — seeing what's on the day and then immediately seeing when you need to leave for it reads better than burying that behind Weather. Weather moves to the very end instead, as a closing/ambient note rather than a lead-in to Commuting. Clock is no longer part of this group at all (see above). The group now reads: how's today shaping up overall → what's actually on it → when do I need to leave for it → what's it like outside.

### Where the brainstormed animations attach

Round 64's brainstorm floated a falling-dollar-signs/growing-cash-pile animation leading into the money content, and a ticking-clock/calendar-flip/emails-opening animation leading into events.

- **Finance — unchanged.** The falling-money/growing-cash animation attaches as a **group-entry animation**: it plays once, as the opening beat of Portfolio (the first screen in the group), rather than as a transition between every pair of screens within the group.
- **Day/Events — revised in round 71.** Round 70 had recommended the ticking-clock/calendar animation as Day's group-entry animation, landing on whichever screen entered the group first (at the time, Clock). With Clock now pulled out of the Day pool entirely, there's no longer a natural "first screen" to hang a generic group-entry animation on that still makes sense — Day Overview and Events are both real content, not a good fit for what was really a *transition* metaphor (a clock ticking forward, a calendar page flipping) rather than a Day-Overview- or Events-specific beat. The animation reads better attached directly to **Events** as that screen's own signature entry animation — the same way Commuting already has its own one-shot jeep-drive-across animation (elapsed resets whenever the screen is freshly entered, per round 66's fix). So: Day Overview plays first with no special intro, then Events gets the ticking-clock/calendar/emails-opening beat as its own opening moment, then Commuting keeps its jeep, then Weather closes the group plainly.

Screens changing within a group where no per-screen animation is defined should still get something lighter and shared — a quick wipe or fade, not a full animated sequence each time — since adjacent screens are topically continuous and a heavy transition on every 12-second beat would fight the "ambient" feel the rotation is otherwise going for.

### Not built yet

Neither animation (falling money, ticking clock/calendar) exists in any form — this is a recommendation for where they'd attach once built, not a claim they're implemented. Also unbuilt: the actual grouped-rotation logic itself (today, entering "the Finance group" isn't a real state — `AUTO_SCREENS` is still one flat list), any time-of-day awareness for when Morning/Night/Clock groups would take over or surface automatically, and the lighter within-group transition treatment mentioned above. All of this is future work; this addendum just settles the shape of it.
