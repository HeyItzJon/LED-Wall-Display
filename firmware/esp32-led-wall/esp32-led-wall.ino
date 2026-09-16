// ============================================================
// ESP32-S3 LED WALL — Orange Pi Secretary live display
// ============================================================
// Full rewrite ported from the "HUB75 Twin" browser simulator
// (hub75-twin.html) once its look was settled there. Polls two
// endpoints on the Orange Pi backend:
//   GET http://<PI_HOST>:<PI_PORT>/api/matrix          (real data, every 30s)
//   GET http://<PI_HOST>:<PI_PORT>/api/matrix/command   (live control, every 1.5s)
//
// Screens: portfolio, events, holdings, news, and weather (round 86) render
// real data today. markets doesn't have a real renderer/data on the backend
// yet and always shows a "COMING SOON" card. News and Weather each degrade
// to that same card automatically whenever the backend hasn't sent anything
// usable yet (no headlines, or no weather block at all) — no firmware
// change needed if that ever happens again, they just pick back up the
// moment real data resumes.
//
// Round 91 — clock, dayoverview, commuting, stars, and balls are now real
// entries in the backend's screen catalog (matrixControl.js) with genuine
// rotation checkboxes on the web Wall tab, same as any other screen.
// getActiveScreens() no longer force-injects any of them — enabledScreens
// (from /api/matrix/command) is the one source of truth for what's in
// rotation, falling back to a small DEFAULTS list only if the Pi has never
// answered even once. Sleep & Alarm (id "sleep") and Wake Up Mode (id
// "wakeup") are also in the catalog now as pin/push-only preview screens
// (hasData: false — no real backend source yet); Sleep & Alarm has a real
// renderer here (renderSleepAlarm(), ported from the HUB75 Twin), Wake Up
// Mode's animated sunrise takeover is still Twin-only pending a firmware
// port (it leans on true alpha-blended compositing the direct-to-hardware
// HUB75 driver doesn't support yet).
//
// WiFi credentials and Pi address are filled in below. NOTE: the SSID is
// "Adidas" with a capital A — WiFi.begin() does a case-sensitive exact
// match, and the previous lowercase "adidas" is exactly why this network
// never connected.
//
// Background/design docs (in the "Orange Pi Zero 3" claude.ai project):
//   claude/esp32-led-wall-handoff.md
//   claude/round-49-esp32-led-wall-design.md
//   claude/round-49-esp32-led-wall-implementation.md
//   claude/round-51-esp-command-endpoint-handoff.md
//   claude/round-51-esp-live-control.md
//   claude/round-56-esp32-live-firmware-and-garbling.md
//   claude/round-57-hub75-twin-full-rewrite-handoff.md   (this round — new fields/behavior)
// ============================================================

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <time.h>

// ---- WiFi ----
const char* WIFI_SSID     = "Adidas";   // capital A — case-sensitive, this is the actual fix
const char* WIFI_PASSWORD = "335607199472";

// ---- Orange Pi backend ----
const char* PI_HOST = "192.168.0.130";
const int   PI_PORT = 3001;
String matrixUrl;   // built in setup()
String commandUrl;  // built in setup()

// ---- Local clock (Clock screen) ----
// Finally wired up — was carried over unused in earlier firmware revisions.
const char* NTP_SERVER = "pool.ntp.org";
const char* NTP_SERVER2 = "time.nist.gov";
const char* TZ_INFO = "EST5EDT,M3.2.0/2,M11.1.0/2";  // America/Toronto, incl. DST rules
bool timeSynced = false;

// ---- Panel/chain configuration (matches your working setup) ----
#define PANEL_RES_X 64
#define PANEL_RES_Y 32
#define PANEL_CHAIN 3   // 3 panels chained = 192x32 total

// ---- Pin configuration ----
// SEENGREAT "RGB Matrix Adapter Board (E)" plugged into an
// ESP32-S3-DevKitC-1 — these pins are fixed by the adapter's PCB traces
// now, not a wiring choice, so they must match SEENGREAT's documented
// mapping for this exact board/header combo rather than whatever's
// convenient. Below is their "V2.x" hardware-generation pinout (see
// seengreat.com/wiki/186) — SEENGREAT labels board generations "V1.x"/
// "V2.x", not "2.2", so treat this as the best-match candidate until
// confirmed against the physical board (silkscreen, or their bundled
// demo code lighting the panel up correctly). If colors/rows come out
// wrong or nothing lights up, try the "V1.x" table below instead.
#define R1_PIN  18
#define G1_PIN  8
#define B1_PIN  17
#define R2_PIN  16
#define G2_PIN  1
#define B2_PIN  15
#define A_PIN   7
#define B_PIN   48
#define C_PIN   6
#define D_PIN   47
#define E_PIN   -1   // no E line on this adapter — fine, same as the old hand-wired setup: your panels are 1/16-scan
#define LAT_PIN 21
#define OE_PIN  4
#define CLK_PIN 5

// ---- Fallback pin tables (comment out the block above and use one of
// these instead if the V2.x mapping doesn't match your actual board) ----
//
// Old hand-wired jumper setup (round 56, before the adapter board — keep
// for reference in case you ever go back to bare jumper wires):
//   R1=4  G1=5  B1=6  R2=7  G2=15 B2=16 A=18 B=8 C=3 D=42 E=-1 LAT=40 OE=2  CLK=41
//
// SEENGREAT's "V1.x" generation mapping for the same S3-DevKitC-1 header,
// in case your board is that earlier revision instead of V2.x:
//   R1=37 G1=6 B1=36 R2=35 G2=5 B2=0  A=45 B=1 C=48 D=2  E=-1 LAT=38 OE=21 CLK=47

#define W (PANEL_RES_X * PANEL_CHAIN)  // 192
#define H (PANEL_RES_Y)                // 32

MatrixPanel_I2S_DMA *dma_display = nullptr;

// ---- Timing ----
const unsigned long FRAME_INTERVAL_MS     = 30;
const unsigned long DATA_POLL_MS          = 30000; // /api/matrix
const unsigned long COMMAND_POLL_MS       = 1500;  // /api/matrix/command
const unsigned long ROTATION_MS           = 12000; // per-screen auto-rotate time
const unsigned long STALE_THRESHOLD_MS    = 95000; // ~3 missed data polls = offline
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000;

unsigned long lastFrameTime = 0;
unsigned long lastDataFetchAttempt = 0;
unsigned long lastCommandFetchAttempt = 0;

// Set this to a screen id ("stars", "balls", "alerts", "dayoverview", ...)
// to pin the wall to it permanently for bench testing, ignoring WiFi state,
// data staleness, and the normal rotation entirely. Leave empty for normal
// operation. ("stars"/"balls" have no other way to reach the screen right
// now — the backend's screen catalog doesn't know about them.)
#define FORCE_SCREEN ""

// ---- Data model: /api/matrix ----
#define MAX_EVENTS 12
#define MAX_HOLDINGS 8
#define MAX_NEWS 6
#define MAX_MARKETS 6

struct EventItem {
  String time;
  String title;
  String busyLevel;  // "busy" | "medium" | "light" — always sent today
  String cal;        // "work"/"school"/"personal"/"important"/"cannotmiss"/"tests" — NOT sent yet, see handoff doc
  String desc;        // NOT sent yet, see handoff doc
  int    dur;          // minutes — NOT sent yet, defaults to 30
};
EventItem events[MAX_EVENTS];
int numEvents = 0;

// Round 76 — Jon: "a new separate page with the all day events
// separately... they can scroll." All-day events used to be mixed into
// events[] with an empty time string, which sorted them first and gave
// them a bogus hour-0 timeline sliver — that's the "beginning of the
// timeline" bug. The backend now excludes them from `events` entirely
// and sends this separate, time-free list instead (see server.js).
#define MAX_ALLDAY 6
struct AllDayEventItem { String title; String cal; };
AllDayEventItem allDayEvents[MAX_ALLDAY];
int numAllDay = 0;

struct HoldingItem { String symbol; float value; float dayChangePercent; float weightPercent; };
HoldingItem holdings[MAX_HOLDINGS];
int numHoldings = 0;

String newsHeadlines[MAX_NEWS];
int numNews = 0;

// Market indices (S&P 500 / Nasdaq / TSX / Dow / Russell 2000 today) and
// VIX — round 74. Both ride the same marketPulse blob backend/server.js
// already builds; this is the first time the firmware has anywhere to
// put them (a real Markets screen, see buildMarketsStrip() below,
// instead of the renderComingSoonFwd placeholder).
struct MarketIdx { String symbol; float changePercent; };
MarketIdx markets[MAX_MARKETS];
int numMarkets = 0;
bool hasVix = false;
float vixValue = 0;
String vixBucket = ""; // "calm" | "normal" | "jumpy" | "volatile"

float portfolioTotal = 0, portfolioDayChange = 0, portfolioDayChangePercent = 0;
int dailyBusyPercent = 0;
bool hasMarketOpen = false, marketOpen = false;

// Round 74 — Jon: "so we know that these prices... are not current, and
// they are the last known price." Compact day+time string from
// server.js's formatLastPriceLabel(), e.g. "FRI 4:00PM" — only
// non-empty when the market is closed. Spliced into the Holdings
// ticker's MARKET CLOSED TODAY banner, see buildHoldingsStrip() below.
String lastPriceLabel = "";

// Day Overview extra fields. hoursBusy/hoursFree ARE real backend data as
// of round 72 (server.js computes them off today's actual calendar) —
// commuteMin still has no real ETA source wired up. hasHours/hasCommute
// still gate whether renderDayOverview()/renderCommuting() draw these
// numbers at all, never fabricated either way. See handoff doc.
struct DayOverviewData {
  bool hasHours = false;
  float hoursBusy = 0, hoursFree = 0;
  bool hasCommute = false;
  int commuteMin = 0;
};
DayOverviewData dayOverview;

// Round 91 — Sleep & Alarm, ported from the HUB75 Twin's prototype
// (renderSleepAlarm() below). hasData is false in the backend catalog
// (no real sleep-schedule/alarm source exists yet), so this always shows
// the "coming soon" fallback for now — struct + parsing are here and
// forward-compatible so the real screen lights up the moment a `sleep`
// block ever shows up in /api/matrix, with zero further firmware change.
#define MAX_SLEEP_EVENTS 4
struct SleepEvent {
  String label;
  String time;   // "HH:MM", 24h
  String period; // "late" or "early"
};
struct SleepAlarmData {
  bool hasData = false;
  String bedTime = "23:00";
  String wakeTime = "07:00";
  String nextAlarm = "07:00";
  SleepEvent nearbyEvents[MAX_SLEEP_EVENTS];
  int numNearbyEvents = 0;
};
SleepAlarmData sleepAlarm;

// Weather — round 86. sources/weather.js's current temp/hi-lo/icon/summary
// and the hourly icon timeline, replacing the previous COMING SOON
// placeholder. Ported from the HUB75 Twin (claude/hub75-twin.html, rounds
// 82-85.1) — see renderWeather() further down for the reference this
// matches pixel-for-pixel. MAX_WEATHER_HOURLY=24 gives headroom over the
// real 17-entry (6am-10pm hourly steps) window sources/weather.js's
// buildHourlySlots() sends today.
#define MAX_WEATHER_HOURLY 24
bool hasWeather = false;
int weatherTempC = 0, weatherHighC = 0, weatherLowC = 0;
String weatherIcon = "cloud";
String weatherSummary = "";
String weatherHourly[MAX_WEATHER_HOURLY];
int numWeatherHourly = 0;

bool dataValid = false;
unsigned long lastDataSuccessTime = 0;

// Round 79 — Jon: "error messages that are useful to me with actionable
// steps or progress indicators." Updated every frame in maintainWifi()
// while connected; renderOffline() reads "now - this" as how long WiFi's
// actually been down, instead of just naming the problem with no sense of
// whether it just started or has been stuck for 20 minutes.
unsigned long lastWifiConnectedTime = 0;

// ---- Data model: /api/matrix/command ----
// Round 91 — bumped from 12: the backend's unified screen catalog is now
// 13 entries (portfolio, markets, holdings, events, news, weather, clock,
// dayoverview, commuting, stars, balls, sleep, wakeup), and clock/
// dayoverview/commuting no longer get hardcoded in on top of whatever the
// backend sends (see getActiveScreens() below) — 16 leaves headroom for
// the catalog to keep growing without this needing to change again.
#define MAX_SCREENS 16
String enabledScreens[MAX_SCREENS];
int numEnabledScreens = 0;
String pinnedScreen = "";
// Round 75 — the "push" half of "push a page or pin a page depending":
// a short-lived override that wins over pinnedScreen/rotation without
// touching either. Backend-authoritative like everything else here — no
// local countdown, it's just present or not on each ~1-2s poll (see
// matrixControl.js's pushScreen()/livePushedScreen()).
String pushedScreenId = "";
bool notificationActive = false;
String notificationText = "";
int notificationSecondsRemaining = 0;
int lastTestEventId = -1;

// Alert overlay — NOT sent by the backend yet (no push/alert channel
// exists there today; see claude/round-53-system-health-watchdog.md's
// "no push notifications yet" note). Fully wired and ready: the instant
// /api/matrix/command ever adds an "alert" field, this lights up with no
// firmware change needed. Until then it just never fires. See handoff doc.
bool alertActive = false;
String alertSeverity = "medium";
String alertText = "";

// ---- Screen rotation state ----
String currentScreenId = "";
unsigned long currentScreenStart = 0;
unsigned long forceScreenStart = 0;
unsigned long notifyStart = 0;

// ---- Struct types used across screens ----
// These are declared up here, before any function definitions, on purpose:
// the Arduino build's auto-generated function-prototype step inserts
// forward declarations near the top of the file, and if a struct used as
// a parameter type is defined further down (after the point where its
// function is first used), that auto-prototype fails to compile with
// "X was not declared in this scope" / "X does not name a type" even
// though the actual function definition is in a perfectly valid order.
// Keep every custom struct type up here to avoid that class of error.
struct Spark { float x, y, life, maxLife, peak; };
struct ExcludeRect { int x0, x1, y0, y1; };
struct Ball { float x, y, vx, vy, r; uint16_t color; };
struct BootTiming {
  unsigned long starsFadeIn = 1000, starsHold = 2000, helloFade = 500, helloHold = 400, subFade = 500, finalHold = 6000;
  unsigned long t1, t2, t3, t4, t5, t6;
};
struct EventPlan { String cap, desc; int capOverflow, descOverflow; unsigned long dur; };
// Round 76 — declared here (not next to computeAllDayPlan() below, where
// it's actually used) for the same reason EventPlan above is: Arduino's
// auto-generated function prototypes are inserted near the TOP of the
// sketch, before any type declared further down would be visible yet.
struct AllDayPlan { String cap; int capOverflow; unsigned long dur; };
struct StripCmd { bool isIcon; int x, y, w, h; String text; uint32_t iconCp; uint16_t color; };
struct NotifCmd { bool isIcon; int x, w; String text; uint32_t cp; };
struct AlertLevel { uint8_t ar, ag, ab, br, bg, bb, tr, tg, tb; };

// ============================================================
// Small helpers
// ============================================================

uint16_t hsvToColor565(uint16_t h, uint8_t s, uint8_t v) {
  h = h % 360;
  uint8_t region = h / 60;
  uint8_t remainder = (h % 60) * 255 / 60;
  uint8_t p = (v * (255 - s)) >> 8;
  uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
  uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;
  uint8_t r, g, b;
  switch (region) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
  }
  return dma_display->color565(r, g, b);
}

// Fades any color565 toward black by `alpha` (0..1) — used by the boot
// sequence to fade the star field and titles in, same idea as the
// simulator's scaleColor().
uint16_t scaleColor565(uint8_t r, uint8_t g, uint8_t b, float alpha) {
  if (alpha < 0) alpha = 0;
  if (alpha > 1) alpha = 1;
  return dma_display->color565((uint8_t)(r * alpha), (uint8_t)(g * alpha), (uint8_t)(b * alpha));
}

int centerTextX(const String &s, int charWidthPx) {
  int w = s.length() * charWidthPx;
  int x = (W - w) / 2;
  return x < 0 ? 0 : x;
}

// Round 91 — same idea as centerTextX but within an arbitrary x0..x1
// sub-range instead of the full panel width. Needed for renderSleepAlarm()'s
// left/right split, ported from the Twin's centerXIn().
int centerTextXIn(const String &s, int charWidthPx, int x0, int x1) {
  int w = s.length() * charWidthPx;
  int x = x0 + max(0, (int)round(((x1 - x0) - w) / 2.0));
  return x;
}

String formatMoney(float v) {
  bool neg = v < 0; if (neg) v = -v;
  long centsTotal = (long)round(v * 100.0);
  long whole = centsTotal / 100;
  int cents = centsTotal % 100;
  String wholeStr = String(whole);
  String withCommas = "";
  int len = wholeStr.length();
  for (int i = 0; i < len; i++) {
    if (i > 0 && (len - i) % 3 == 0) withCommas += ",";
    withCommas += wholeStr[i];
  }
  char buf[8];
  snprintf(buf, sizeof(buf), ".%02d", cents);
  return (neg ? "-$" : "$") + withCommas + String(buf);
}

String formatMoneyWhole(float v) {
  bool neg = v < 0; if (neg) v = -v;
  long whole = (long)round(v);
  String wholeStr = String(whole);
  String withCommas = "";
  int len = wholeStr.length();
  for (int i = 0; i < len; i++) {
    if (i > 0 && (len - i) % 3 == 0) withCommas += ",";
    withCommas += wholeStr[i];
  }
  return (neg ? "-$" : "$") + withCommas;
}

String formatSignedMoney(float v) {
  bool neg = v < 0;
  String base = formatMoney(neg ? -v : v);
  return (neg ? "-" : "+") + base;
}

String formatSignedPercent(float v) {
  String s = (v >= 0 ? "+" : "");
  s += String(v, 2);
  s += "%";
  return s;
}

int timeToMinutes(const String &t) {
  int colon = t.indexOf(':');
  if (colon < 0) return 0;
  int hh = t.substring(0, colon).toInt();
  int mm = t.substring(colon + 1).toInt();
  return hh * 60 + mm;
}

// Maps minutes-since-midnight onto a 0..1 position across a 6am-11pm day —
// matches the simulator's minutesToFrac() exactly.
float minutesToFrac(int mins) {
  const int dayStart = 6 * 60, dayEnd = 23 * 60;
  float f = (float)(mins - dayStart) / (float)(dayEnd - dayStart);
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  return f;
}

String minutesToClockStr(int mins) {
  mins = ((mins % 1440) + 1440) % 1440;
  int hh = mins / 60, mm = mins % 60;
  bool pm = hh >= 12;
  hh = hh % 12; if (hh == 0) hh = 12;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d:%02d%s", hh, mm, pm ? "PM" : "AM");
  return String(buf);
}

// Rainbow-cycling 1px border, used for the notification overlay.
void drawAnimatedBorder(unsigned long t) {
  for (int x = 0; x < W; x++) {
    uint16_t hue = ((x * 3) + (t / 5)) % 360;
    uint16_t c = hsvToColor565(hue, 255, 255);
    dma_display->drawPixel(x, 0, c);
    dma_display->drawPixel(x, H - 1, c);
  }
  for (int y = 0; y < H; y++) {
    uint16_t hue = ((y * 6) + (t / 5)) % 360;
    uint16_t c = hsvToColor565(hue, 255, 255);
    dma_display->drawPixel(0, y, c);
    dma_display->drawPixel(W - 1, y, c);
  }
}

void drawRectOutlineColor(int x, int y, int w, int h, uint16_t color) {
  dma_display->drawRect(x, y, w, h, color);
}

// ============================================================
// UTF-8 decoding — the notification text can carry the same emoji
// characters the simulator's icon set understands (star/heart/check/three
// warning-triangle severities/four faces). Arduino String is just raw
// UTF-8 bytes, so this walks it one codepoint at a time.
// ============================================================
uint32_t utf8Decode(const String &s, int i, int *outLen) {
  uint8_t c0 = (uint8_t)s[i];
  if (c0 < 0x80) { *outLen = 1; return c0; }
  if ((c0 & 0xE0) == 0xC0 && i + 1 < (int)s.length()) {
    *outLen = 2;
    return ((c0 & 0x1F) << 6) | ((uint8_t)s[i + 1] & 0x3F);
  }
  if ((c0 & 0xF0) == 0xE0 && i + 2 < (int)s.length()) {
    *outLen = 3;
    return ((c0 & 0x0F) << 12) | (((uint8_t)s[i + 1] & 0x3F) << 6) | ((uint8_t)s[i + 2] & 0x3F);
  }
  if ((c0 & 0xF8) == 0xF0 && i + 3 < (int)s.length()) {
    *outLen = 4;
    return ((c0 & 0x07) << 18) | (((uint8_t)s[i + 1] & 0x3F) << 12) |
           (((uint8_t)s[i + 2] & 0x3F) << 6) | ((uint8_t)s[i + 3] & 0x3F);
  }
  *outLen = 1;
  return c0;
}

// ============================================================
// Icon primitives — real vector shapes (not tiny bitmaps), ported from the
// simulator's fillPolygon/fillCircle/drawThickLine helpers. ICON_INK is an
// explicit dark fill for carved-in details (eyes, mouths, the "!" mark) —
// same reasoning as the simulator: a true unlit pixel inside a bright glow
// reads as washed-out/invisible, not black.
// ============================================================
const uint16_t ICON_INK_R = 12, ICON_INK_G = 9, ICON_INK_B = 6;

bool pointInPolygonIcon(float px, float py, const float *xs, const float *ys, int n) {
  bool inside = false;
  for (int i = 0, j = n - 1; i < n; j = i++) {
    float xi = xs[i], yi = ys[i], xj = xs[j], yj = ys[j];
    if (((yi > py) != (yj > py)) && (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) inside = !inside;
  }
  return inside;
}

void fillPolygonIcon(const float *xs, const float *ys, int n, uint16_t color) {
  float minX = xs[0], maxX = xs[0], minY = ys[0], maxY = ys[0];
  for (int i = 1; i < n; i++) {
    minX = min(minX, xs[i]); maxX = max(maxX, xs[i]);
    minY = min(minY, ys[i]); maxY = max(maxY, ys[i]);
  }
  for (int y = (int)floor(minY); y <= (int)ceil(maxY); y++) {
    for (int x = (int)floor(minX); x <= (int)ceil(maxX); x++) {
      if (pointInPolygonIcon(x + 0.5f, y + 0.5f, xs, ys, n)) dma_display->drawPixel(x, y, color);
    }
  }
}

void drawThickLineIcon(float x1, float y1, float x2, float y2, float thick, uint16_t color) {
  float dist = hypot(x2 - x1, y2 - y1);
  int steps = max(1, (int)ceil(dist * 1.5f));
  int rad = max(1, (int)(thick / 2));
  for (int i = 0; i <= steps; i++) {
    float t = (float)i / steps;
    dma_display->fillCircle((int)round(x1 + (x2 - x1) * t), (int)round(y1 + (y2 - y1) * t), rad, color);
  }
}

void iconStar(int x0, int y0, int w, int h, uint16_t color) {
  float cx = x0 + w / 2.0f, cy = y0 + h / 2.0f;
  float outerR = min(w, h) / 2.0f, innerR = outerR * 0.42f;
  float xs[10], ys[10];
  for (int i = 0; i < 10; i++) {
    float r = (i % 2 == 0) ? outerR : innerR;
    float ang = -HALF_PI + i * PI / 5.0f;
    xs[i] = cx + r * cos(ang);
    ys[i] = cy + r * sin(ang);
  }
  fillPolygonIcon(xs, ys, 10, color);
}

// Sampled parametric heart curve, filled as one polygon — a genuine heart
// silhouette (two round lobes, a real cleft), not circles + a cutout.
void iconHeart(int x0, int y0, int w, int h, uint16_t color) {
  const int N = 32;
  float rawX[N], rawY[N];
  float minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
  for (int i = 0; i < N; i++) {
    float t = (i / (float)N) * TWO_PI;
    float hx = 16.0f * pow(sin(t), 3);
    float hy = -(13.0f * cos(t) - 5.0f * cos(2 * t) - 2.0f * cos(3 * t) - cos(4 * t));
    rawX[i] = hx; rawY[i] = hy;
    minX = min(minX, hx); maxX = max(maxX, hx);
    minY = min(minY, hy); maxY = max(maxY, hy);
  }
  float sx = w / (maxX - minX), sy = h / (maxY - minY);
  float xs[N], ys[N];
  for (int i = 0; i < N; i++) { xs[i] = x0 + (rawX[i] - minX) * sx; ys[i] = y0 + (rawY[i] - minY) * sy; }
  fillPolygonIcon(xs, ys, N, color);
}

// Real straight-edged triangle via polygon fill (not a per-row taper, which
// stair-steps at this resolution).
void iconWarning(int x0, int y0, int w, int h, uint16_t color) {
  float xs[3] = { x0 + w / 2.0f, (float)x0, (float)(x0 + w) };
  float ys[3] = { (float)y0, (float)(y0 + h), (float)(y0 + h) };
  fillPolygonIcon(xs, ys, 3, color);
  uint16_t ink = dma_display->color565(ICON_INK_R, ICON_INK_G, ICON_INK_B);
  int barW = max(2, (int)round(w * 0.1f));
  int barX = (int)round(x0 + w / 2.0f - barW / 2.0f);
  dma_display->fillRect(barX, y0 + (int)round(h * 0.34f), barW, (int)round(h * 0.30f), ink);
  dma_display->fillRect(barX, y0 + (int)round(h * 0.76f), barW, (int)round(h * 0.09f), ink);
}

void iconCheck(int x0, int y0, int w, int h, uint16_t color) {
  int thick = max(2, (int)round(min(w, h) * 0.16f));
  drawThickLineIcon(x0 + w * 0.12f, y0 + h * 0.55f, x0 + w * 0.42f, y0 + h * 0.85f, thick, color);
  drawThickLineIcon(x0 + w * 0.42f, y0 + h * 0.85f, x0 + w * 0.88f, y0 + h * 0.15f, thick, color);
}

// kind: 0=smile 1=frown 2=cry 3=neutral
void iconFace(int x0, int y0, int w, int h, uint16_t color, int kind) {
  float cx = x0 + w / 2.0f, cy = y0 + h / 2.0f, r = min(w, h) / 2.0f;
  dma_display->fillCircle((int)cx, (int)cy, (int)r, color);
  uint16_t ink = dma_display->color565(ICON_INK_R, ICON_INK_G, ICON_INK_B);
  float eyeY = cy - r * 0.22f - 1.5f, eyeDX = r * 0.36f;
  int eyeR = max(1, (int)round(r * 0.15f));
  dma_display->fillCircle((int)round(cx - eyeDX), (int)round(eyeY), eyeR, ink);
  dma_display->fillCircle((int)round(cx + eyeDX), (int)round(eyeY), eyeR, ink);
  float mouthBaseline = cy + r * 0.32f;
  if (kind == 1 || kind == 2) mouthBaseline += 1.5f;
  else if (kind == 3) mouthBaseline += 1.0f;
  float amp = r * 0.28f, mw = r * 0.55f;
  int steps = max(6, (int)round(mw));
  float mouthThick = max(0.9f, r * 0.12f - 1.0f);
  for (int i = -steps; i <= steps; i++) {
    float t = (float)i / steps;
    float arc = amp * (1 - t * t);
    float my = mouthBaseline;
    if (kind == 0) my = mouthBaseline + arc;
    else if (kind == 1 || kind == 2) my = mouthBaseline - arc;
    dma_display->fillCircle((int)round(cx + t * mw), (int)round(my), (int)round(mouthThick), ink);
  }
  if (kind == 2) {
    float tearX = cx + eyeDX, tearTop = eyeY + r * 0.25f;
    uint16_t tearColor = dma_display->color565(70, 150, 255);
    dma_display->fillRect((int)round(tearX - r * 0.06f), (int)round(tearTop), max(1, (int)round(r * 0.12f)), max(1, (int)round(r * 0.32f)), tearColor);
    dma_display->fillCircle((int)round(tearX), (int)round(tearTop + r * 0.34f), max(1, (int)round(r * 0.14f)), tearColor);
  }
}

// Draws whichever icon a codepoint maps to. Returns the icon's pixel width
// (0 if the codepoint isn't one of the recognized icons).
int drawIconForCodepoint(uint32_t cp, int x, int y, int size) {
  int w = size, h = size;
  switch (cp) {
    case 0x2B50: iconStar(x, y, w, h, dma_display->color565(255, 210, 0)); return w;
    case 0x2764: iconHeart(x, y, w, h, dma_display->color565(255, 70, 90)); return w;
    case 0x2705: iconCheck(x, y, w, h, dma_display->color565(60, 220, 110)); return w;
    case 0x26A0: { int ww = (int)round(w * 1.3f); iconWarning(x, y, ww, h, dma_display->color565(255, 210, 0)); return ww; }   // yellow — low
    case 0xE001: { int ww = (int)round(w * 1.3f); iconWarning(x, y, ww, h, dma_display->color565(255, 140, 0)); return ww; }   // orange — medium
    case 0xE002: { int ww = (int)round(w * 1.3f); iconWarning(x, y, ww, h, dma_display->color565(255, 30, 20)); return ww; }   // red — high
    case 0x1F600: iconFace(x, y, w, h, dma_display->color565(255, 205, 40), 0); return w; // smile
    case 0x1F641: iconFace(x, y, w, h, dma_display->color565(255, 205, 40), 1); return w; // frown
    case 0x1F622: iconFace(x, y, w, h, dma_display->color565(255, 205, 40), 2); return w; // cry
    case 0x1F610: iconFace(x, y, w, h, dma_display->color565(255, 205, 40), 3); return w; // neutral
    default: return 0;
  }
}

// ============================================================
// Jeep sprite — real pixel-art G-Wagon, traced pixel-for-pixel from the
// reference art Jon picked (deep red, black wheels, pink spare-tire cover
// on the back), plus a small headlight + light-rays added at the front
// (right side, since it drives left-to-right) per Jon's ask. 62x30, drawn
// as-is via drawPixel — no procedural shapes. Rows are a uniform 62 chars
// (the original art had a few rows one column short, which combined with
// JEEP_W previously being set to 63 — wider than every row — meant
// drawJeepSprite could read one byte past a row's null terminator; fixed
// by padding every row to the same length and matching JEEP_W to it).
// ============================================================
const char *const JEEP_SPRITE[30] PROGMEM = {
  "..............................................................",
  "..........OOOOOOOOOOOOOOOOOOOOOO..............................",
  "......OSSSBBBBBBBBBBBBBBBBBBBBBBBBBDOO........................",
  "......ODDDDDDDDDDDDDDDDDDSSDDDDDDDDSSSO.......................",
  "......OSBOOOOOOOOBBBOOOOOKODBBBOOOOKKBO.......................",
  "......SSOWWWWWWWWKBBKWWWWWWOBBOHHHHHHOBO......................",
  "......SSKWWWWWWWWKBBKWWWWWWKSBOHHHHHHHDOK.....................",
  "......SSOWWWWWWWWKBBKWWWWWWKSBOHHHHHHHKSO.....................",
  "..OOOOSOWWWWWWWWWKBBWWWWWWWKSBOHHHHHHHHSD.....................",
  ".OPPPOBOWWWWWWWWWOBBWWWWWWWKSBOHHHHHHHOOS.....................",
  ".PPPPOBOWWWWWWWWWOBBWWWWWWWOSBOHHHHHHHBBSO....................",
  ".PPPPOBBKOOOOOOOOBSBKOOOOOOBBBBOOOOOOOOOKSSSSSSKOOOOO.........",
  ".PPPPKBBBBBBBBBBBBSBBBBBBBBBBBBBBBBBBBBKBSSSSSSDBBBBKOOOO.....",
  ".OPPPKBBBBBBBBBBBBSBBBBBBBBBBBBBBBBBBBBBBSBBBBBBDDDDSSSKOO....",
  ".PPPPKBBBBBBBBBBBBSBBBBBBBBBBBBBBBBBBBBBBSBBBBBBBBBBBBBBBBO...",
  ".PPPPKSWWWWWWWWWWWDDDDDWWWSSWBSDDDWWWWWWWWWWWWWWWWWDSWWWSPO...",
  ".OPPPKBBBBSBBBBBBBSSBBBBBBBBBBBBBBBBBBBBBSBBBSBBBBBBBBBBBPO...",
  ".OPPPKBBBSSOOOOOOOBSBBBBBBBBBBBBBBBBBBBBBSBBBBOOOOOOOOSBBOO...",
  "..OOOBBBBSKOOKWWKOOBSBBBBBBBBBBBBBBBBBBBBSBBSOOOWWWWOOOSBBO...",
  "....OKBBSSOKWWWWWKOSSBBBBBBBSBBBBBBBBBBBBSBSSOOWWWWWWOOSDKKO..",
  "....OKKKKOOWWOKOKWKOBSBBBBBBSBBBBBBBBBBBBSBSOOWWOWKKWWOBSBBO..",
  "....OBBBBOKKOOKOOOWOOSSSSSSSSSSSSSSSSSSSSSSKOKWOOKKOKWKOBBSO..",
  ".....OOOOOWKKKWKKWWKOOKKKKKKKKKKKKKKKKKKKKOOOWWWKWWKWWWOOO....",
  ".......OOOWOOKWWKOWWOOOOOOOOOOOOOOOOOOOOOOOOOWKKWWWKOWWO......",
  ".........OWOOOWWOOWKO.......................OWKOOWWKOWW.......",
  ".........OKWOWOKWWWO........................OKWOWKWWKWK.......",
  "..........OWWKOOWWKO.........................OWWOOOWWWO.......",
  "..........OOWWWWWKO...........................OWWWWWWO........",
  "............OOOOO..............................OOOOOO.........",
  ".............................................................."
};
const int JEEP_W = 62, JEEP_H = 30;

uint16_t jeepPaletteColor(char c) {
  switch (c) {
    case 'B': return dma_display->color565(150, 45, 72);
    case 'S': return dma_display->color565(100, 35, 48);
    case 'D': return dma_display->color565(72, 20, 28);
    case 'W': return dma_display->color565(48, 50, 54);
    case 'H': return dma_display->color565(110, 100, 108);
    case 'K': return dma_display->color565(18, 17, 18);
    case 'O': return dma_display->color565(6, 5, 5);
    case 'P': return dma_display->color565(205, 110, 150);
    default:  return 0;
  }
}

void drawJeepSprite(int x0, int y0) {
  for (int row = 0; row < JEEP_H; row++) {
    const char *line = JEEP_SPRITE[row];
    for (int col = 0; col < JEEP_W; col++) {
      char ch = line[col];
      if (ch == '.' || ch == '\0') continue;
      dma_display->drawPixel(x0 + col, y0 + row, jeepPaletteColor(ch));
    }
  }
}

// ============================================================
// Sparks ("stars") — each spawns at a random spot, flashes up fast then
// decays, then respawns elsewhere — used by both the Stars screen and the
// boot sequence (which fades the whole field in/out and can carve out
// rectangles so sparks don't clutter text drawn on top).
// ============================================================
#define NUM_SPARKS 70
Spark sparks[NUM_SPARKS];
unsigned long lastSparkT = 0;
bool sparksInited = false;

void newSpark(Spark &s) {
  s.x = random(0, W);
  s.y = random(0, H);
  s.life = 0;
  s.maxLife = 250 + random(0, 900);
  s.peak = 0.55f + (random(0, 450) / 1000.0f);
}

void initSparksIfNeeded() {
  if (sparksInited) return;
  for (int i = 0; i < NUM_SPARKS; i++) {
    newSpark(sparks[i]);
    sparks[i].life = random(0, (long)sparks[i].maxLife);
  }
  sparksInited = true;
}

void renderSparks(unsigned long elapsed, float globalAlpha, const ExcludeRect *excludeRects, int numExclude) {
  initSparksIfNeeded();
  unsigned long dt = (lastSparkT == 0 || elapsed < lastSparkT) ? 16 : (elapsed - lastSparkT);
  lastSparkT = elapsed;
  for (int i = 0; i < NUM_SPARKS; i++) {
    Spark &s = sparks[i];
    s.life += dt;
    if (s.life >= s.maxLife) newSpark(s);
    float frac = s.life / s.maxLife;
    float b = (frac < 0.12f) ? (frac / 0.12f) : pow(1 - ((frac - 0.12f) / 0.88f), 1.6f);
    int v = (int)round(255 * b * s.peak * globalAlpha);
    if (v <= 4) continue;
    int sx = (int)round(s.x), sy = (int)round(s.y);
    bool excluded = false;
    for (int r = 0; r < numExclude; r++) {
      if (sx >= excludeRects[r].x0 && sx < excludeRects[r].x1 && sy >= excludeRects[r].y0 && sy < excludeRects[r].y1) { excluded = true; break; }
    }
    if (excluded) continue;
    dma_display->drawPixel(sx, sy, dma_display->color565(v, v, min(255, v + 20)));
  }
}

void renderStars(unsigned long elapsed) {
  dma_display->clearScreen();
  renderSparks(elapsed, 1.0f, nullptr, 0);
}

// ============================================================
// Bouncing balls — ambient demo effect, ported from hub75-effects-demo.ino.
// ============================================================
#define NUM_BALLS 4
Ball balls[NUM_BALLS];
bool ballsInited = false;
unsigned long lastBallT = 0;

void initBallsIfNeeded() {
  if (ballsInited) return;
  uint16_t colors[NUM_BALLS] = {
    dma_display->color565(255, 80, 80), dma_display->color565(80, 180, 255),
    dma_display->color565(120, 255, 120), dma_display->color565(255, 210, 60)
  };
  for (int i = 0; i < NUM_BALLS; i++) {
    balls[i].x = 10 + random(0, (W - 20) * 100) / 100.0f;
    balls[i].y = 5 + random(0, (H - 10) * 100) / 100.0f;
    balls[i].vx = (i % 2 == 0 ? 1 : -1) * (0.035f + random(0, 20) / 1000.0f);
    balls[i].vy = (i % 2 == 0 ? -1 : 1) * (0.035f + random(0, 20) / 1000.0f);
    balls[i].r = 3;
    balls[i].color = colors[i];
  }
  ballsInited = true;
}

void renderBalls(unsigned long elapsed) {
  dma_display->clearScreen();
  initBallsIfNeeded();
  unsigned long dt = (lastBallT == 0 || elapsed < lastBallT) ? 16 : (elapsed - lastBallT);
  lastBallT = elapsed;
  for (int i = 0; i < NUM_BALLS; i++) {
    Ball &b = balls[i];
    b.x += b.vx * dt; b.y += b.vy * dt;
    if (b.x - b.r < 0) { b.x = b.r; b.vx *= -1; }
    if (b.x + b.r > W) { b.x = W - b.r; b.vx *= -1; }
    if (b.y - b.r < 0) { b.y = b.r; b.vy *= -1; }
    if (b.y + b.r > H) { b.y = H - b.r; b.vy *= -1; }
    dma_display->fillCircle((int)round(b.x), (int)round(b.y), (int)round(b.r), b.color);
  }
}

// ============================================================
// Boot splash — ported from the simulator: stars fade in from black, hold,
// then "HELLO JON"/"WELCOME BACK" fade in on top (with excluded rectangles
// so sparks don't clutter the text), then a long final hold before it
// loops (it only actually loops if setup somehow takes longer than the
// full cycle — normally this runs once, for TOTAL_MS, then setup moves on).
// ============================================================
BootTiming BOOT;

void initBootTiming() {
  BOOT.t1 = BOOT.starsFadeIn;
  BOOT.t2 = BOOT.t1 + BOOT.starsHold;
  BOOT.t3 = BOOT.t2 + BOOT.helloFade;
  BOOT.t4 = BOOT.t3 + BOOT.helloHold;
  BOOT.t5 = BOOT.t4 + BOOT.subFade;
  BOOT.t6 = BOOT.t5 + BOOT.finalHold;
}

void renderBootFrame(unsigned long t) {
  dma_display->clearScreen();
  float starsAlpha = (t < BOOT.t1) ? ((float)t / BOOT.starsFadeIn) : 1.0f;

  String hello = "HELLO JON";
  int helloSize = 2, helloX = centerTextX(hello, 6 * helloSize), helloY = 6;
  String sub = "WELCOME BACK";
  int subSize = 1, subX = centerTextX(sub, 6 * subSize), subY = 22;

  ExcludeRect rects[2];
  int numRects = 0;
  float helloAlpha = 0, subAlpha = 0;
  if (t >= BOOT.t2) {
    helloAlpha = min(1.0f, (float)(t - BOOT.t2) / BOOT.helloFade);
    rects[numRects++] = { helloX - 2, helloX + (int)hello.length() * 6 * helloSize + 2, helloY - 2, helloY + 8 * helloSize + 2 };
  }
  if (t >= BOOT.t4) {
    subAlpha = min(1.0f, (float)(t - BOOT.t4) / BOOT.subFade);
    rects[numRects++] = { subX - 2, subX + (int)sub.length() * 6 * subSize + 2, subY - 2, subY + 8 * subSize + 2 };
  }

  renderSparks(t, starsAlpha, rects, numRects);

  if (helloAlpha > 0) {
    uint16_t helloColor = hsvToColor565((t / 40) % 360, 255, 255);
    uint8_t r8, g8, b8;
    // scale the hue color by helloAlpha — decode back from color565 is
    // wasteful, so just recompute hsv at reduced "value" instead.
    helloColor = hsvToColor565((t / 40) % 360, 255, (uint8_t)(255 * helloAlpha));
    dma_display->setTextSize(helloSize);
    dma_display->setTextColor(helloColor);
    dma_display->setCursor(helloX, helloY);
    dma_display->print(hello);
  }
  if (subAlpha > 0) {
    dma_display->setTextSize(subSize);
    dma_display->setTextColor(scaleColor565(180, 180, 180, subAlpha));
    dma_display->setCursor(subX, subY);
    dma_display->print(sub);
  }

  // Called from runBootSplash()'s own loop, before the main loop() (and
  // its flipDMABuffer() call) ever runs — see the matching note in
  // drawStatusScreen(). Without this, the entire boot animation draws into
  // the back buffer and nothing ever appears on the panel (round 60).
  dma_display->flipDMABuffer();
}

void runBootSplash() {
  initBootTiming();
  unsigned long start = millis();
  while (millis() - start < BOOT.t6) {
    renderBootFrame(millis() - start);
    delay(30);
  }
}

// ============================================================
// Status screens (WiFi connecting / initial data fetch) — simplified to
// match the simulator: title, subtitle, spinner, no border/scan bar.
// ============================================================
// Round 91 — Jon: "I like the Wifi connected page with the network name
// but can we remove the animated spinner once it connected, it looks
// weird and frozen." The spinner only ever reads as alive while its
// caller keeps looping and redrawing with an advancing `t` (true for
// "CONNECTING TO WIFI" and "Fetching data..." above); a screen that's
// drawn once and then held for a few seconds behind a single delay()
// froze it mid-frame, which is exactly what "WIFI CONNECTED" was doing.
// showSpinner defaults true so every existing loop-driven call site is
// unaffected; the two one-shot "we're done, here's the result" screens
// below pass false.
void drawStatusScreen(String title, String subtitle, unsigned long t, uint16_t accentColor, bool showSpinner = true) {
  dma_display->clearScreen();

  dma_display->setTextSize(1);
  dma_display->setTextColor(accentColor);
  dma_display->setCursor(centerTextX(title, 6), 4);
  dma_display->print(title);

  dma_display->setTextColor(dma_display->color565(150, 150, 150));
  dma_display->setCursor(centerTextX(subtitle, 6), 14);
  dma_display->print(subtitle);

  if (showSpinner) {
    const char spin[4] = {'|', '/', '-', '\\'};
    char sc = spin[(t / 150) % 4];
    dma_display->setTextColor(accentColor);
    dma_display->setCursor(W / 2 - 3, 23);
    dma_display->print(sc);
  }

  // This (and renderBootFrame) are the only screens ever drawn outside the
  // main loop() — during setup(), before loop()'s own flipDMABuffer() at
  // the bottom of every iteration ever runs. With double_buff on (round
  // 59), a frame is invisible until it's flipped, so without this line
  // every call site here (WiFi connecting, WiFi connected, initial data
  // fetch) would draw into the back buffer and never actually show up —
  // exactly the "boot animation looks broken/blank" symptom (round 60).
  dma_display->flipDMABuffer();
}

bool connectWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // WiFi modem-sleep power saving causes periodic radio
                          // wake bursts that steal CPU cycles from the HUB75
                          // I2S/DMA refresh — this is the classic cause of
                          // display garbling that only shows up once WiFi is
                          // active. Disabling it trades a little more power
                          // draw for a stable picture.
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    drawStatusScreen("CONNECTING TO WIFI", WIFI_SSID, millis(), dma_display->color565(0, 180, 255));
    delay(30);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
    // Round 91 — no spinner (see drawStatusScreen's comment) and held for
    // a full 3s (was 900ms) so there's actually time to read it.
    drawStatusScreen("WIFI CONNECTED", WIFI_SSID, millis(), dma_display->color565(0, 255, 120), false);
    delay(3000);
    return true;
  }

  Serial.println("WiFi connect timed out — will keep retrying in the background");
  return false;
}

// Called every loop() iteration regardless of WiFi state: while connected
// it just stamps lastWifiConnectedTime (round 79, see comment above that
// global) and returns; while down it's the non-blocking reconnect attempt
// it always was, retried every 10s.
void maintainWifi(unsigned long now) {
  static unsigned long lastAttempt = 0;
  if (WiFi.status() == WL_CONNECTED) {
    lastWifiConnectedTime = now;
    return;
  }
  if (now - lastAttempt > 10000) {
    lastAttempt = now;
    Serial.println("WiFi not connected — attempting reconnect...");
    WiFi.disconnect();
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
}

void setupClock() {
  configTzTime(TZ_INFO, NTP_SERVER, NTP_SERVER2);
  struct tm timeinfo;
  timeSynced = getLocalTime(&timeinfo, 4000);
  Serial.println(timeSynced ? "NTP time synced" : "NTP time sync failed — Clock screen will show SYNCING until it succeeds in the background");
}

// ============================================================
// Networking: /api/matrix (data) and /api/matrix/command (live control)
// ============================================================
void pollData() {
  HTTPClient http;
  http.begin(matrixUrl);
  http.setTimeout(5000);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    // Bumped 6144 -> 10240 for round 74: markets[]/vix/lastPriceLabel are
    // new fields, MAX_EVENTS grew 8->12, and news headlines are no longer
    // truncated server-side (up to ~90 chars each now instead of 60).
    // Bumped 10240 -> 11264 for round 86: weather's summary (up to 200
    // chars) plus its 17-entry hourly icon array add a few hundred bytes.
    DynamicJsonDocument doc(11264);
    DeserializationError err = deserializeJson(doc, payload);

    if (!err) {
      dataValid = true;
      lastDataSuccessTime = millis();

      portfolioTotal            = doc["portfolio"]["total"] | 0.0;
      portfolioDayChange        = doc["portfolio"]["dayChange"] | 0.0;
      portfolioDayChangePercent = doc["portfolio"]["dayChangePercent"] | 0.0;
      dailyBusyPercent          = doc["dailyBusyPercent"] | 0;

      hasMarketOpen = doc.containsKey("marketOpen");
      if (hasMarketOpen) marketOpen = doc["marketOpen"].as<bool>();
      lastPriceLabel = doc["lastPriceLabel"] | "";

      numMarkets = 0;
      if (doc.containsKey("markets")) {
        for (JsonVariant v : doc["markets"].as<JsonArray>()) {
          if (numMarkets >= MAX_MARKETS) break;
          markets[numMarkets].symbol        = v["symbol"] | "";
          markets[numMarkets].changePercent = v["changePercent"] | 0.0;
          numMarkets++;
        }
      }

      hasVix = doc.containsKey("vix") && !doc["vix"].isNull();
      if (hasVix) {
        vixValue  = doc["vix"]["value"] | 0.0;
        vixBucket = doc["vix"]["bucket"] | "";
      }

      numEvents = 0;
      if (doc.containsKey("events")) {
        for (JsonVariant v : doc["events"].as<JsonArray>()) {
          if (numEvents >= MAX_EVENTS) break;
          events[numEvents].time      = v["time"] | "";
          events[numEvents].title     = v["title"] | "";
          events[numEvents].busyLevel = v["busyLevel"] | "";
          events[numEvents].cal       = v["cal"] | "";          // not sent yet — see handoff doc
          events[numEvents].desc      = v["desc"] | "";          // not sent yet — see handoff doc
          events[numEvents].dur       = v["dur"] | 30;
          numEvents++;
        }
      }

      // Round 76 — see AllDayEventItem comment above.
      numAllDay = 0;
      if (doc.containsKey("allDayEvents")) {
        for (JsonVariant v : doc["allDayEvents"].as<JsonArray>()) {
          if (numAllDay >= MAX_ALLDAY) break;
          allDayEvents[numAllDay].title = v["title"] | "";
          allDayEvents[numAllDay].cal   = v["cal"] | "";
          numAllDay++;
        }
      }

      numHoldings = 0;
      if (doc.containsKey("holdings")) {
        for (JsonVariant v : doc["holdings"].as<JsonArray>()) {
          if (numHoldings >= MAX_HOLDINGS) break;
          holdings[numHoldings].symbol            = v["symbol"] | "";
          holdings[numHoldings].value             = v["value"] | 0.0;
          holdings[numHoldings].dayChangePercent  = v["dayChangePercent"] | 0.0;
          holdings[numHoldings].weightPercent     = v["weightPercent"] | 0.0;
          numHoldings++;
        }
      }

      // News: shape isn't nailed down on the backend yet (round-51 added
      // the field but this firmware has never parsed it before) — accept
      // either a plain array of strings, or an array of objects carrying
      // "headline" or "title". Confirm the real shape once deployed and
      // adjust this block if it differs. See handoff doc.
      numNews = 0;
      if (doc.containsKey("news")) {
        JsonVariant nv = doc["news"];
        if (nv.is<JsonArray>()) {
          for (JsonVariant item : nv.as<JsonArray>()) {
            if (numNews >= MAX_NEWS) break;
            String headline;
            if (item.is<const char*>()) headline = item.as<String>();
            else headline = (item["headline"] | (const char*)(item["title"] | ""));
            if (headline.length() > 0) newsHeadlines[numNews++] = headline;
          }
        } else if (nv.is<const char*>()) {
          String s = nv.as<String>();
          if (s.length() > 0) newsHeadlines[numNews++] = s;
        }
      }

      // Day Overview extras — hoursBusy/hoursFree ARE real since round 72
      // (server.js's weekForecast() on today's actual calendar); commuteMin
      // still has no real ETA source, hence hasCommute staying gated.
      dayOverview.hasHours = false;
      dayOverview.hasCommute = false;
      if (doc.containsKey("dayOverview") && !doc["dayOverview"].isNull()) {
        JsonVariant dov = doc["dayOverview"];
        if (dov.containsKey("hoursBusy") && dov.containsKey("hoursFree")) {
          dayOverview.hasHours = true;
          dayOverview.hoursBusy = dov["hoursBusy"] | 0.0;
          dayOverview.hoursFree = dov["hoursFree"] | 0.0;
        }
        if (dov.containsKey("commuteMin")) {
          dayOverview.hasCommute = true;
          dayOverview.commuteMin = dov["commuteMin"] | 0;
        }
      }

      // Round 91 — Sleep & Alarm. Forward-compatible parsing for a `sleep`
      // block that doesn't exist in the backend payload yet (no real
      // source — see matrixControl.js's hasData:false on this screen);
      // this just means it lights up automatically with zero firmware
      // change the day a real source lands, same convention as
      // News/Weather degrading gracefully when their key is absent.
      sleepAlarm.hasData = doc.containsKey("sleep") && !doc["sleep"].isNull();
      if (sleepAlarm.hasData) {
        JsonVariant sv = doc["sleep"];
        sleepAlarm.bedTime = sv["bedTime"] | "23:00";
        sleepAlarm.wakeTime = sv["wakeTime"] | "07:00";
        sleepAlarm.nextAlarm = sv["nextAlarm"] | sleepAlarm.wakeTime;
        sleepAlarm.numNearbyEvents = 0;
        if (sv.containsKey("nearbyEvents")) {
          for (JsonVariant ev : sv["nearbyEvents"].as<JsonArray>()) {
            if (sleepAlarm.numNearbyEvents >= MAX_SLEEP_EVENTS) break;
            SleepEvent &se = sleepAlarm.nearbyEvents[sleepAlarm.numNearbyEvents++];
            se.label = ev["label"] | "";
            se.time = ev["time"] | "00:00";
            se.period = ev["period"] | "early";
          }
        }
      }

      // Weather — round 86. Ported from the HUB75 Twin (rounds 82-85.1);
      // see renderWeather() further down for the reference implementation
      // this was checked against pixel-for-pixel. hasWeather false (no
      // "weather" key at all yet, fresh install/first pull still pending)
      // falls back to renderComingSoonFwd via renderScreen()'s dispatch,
      // same convention as numNews==0 does for the News screen.
      hasWeather = doc.containsKey("weather") && !doc["weather"].isNull();
      if (hasWeather) {
        JsonVariant wv = doc["weather"];
        weatherTempC   = wv["tempC"] | 0;
        weatherHighC   = wv["highC"] | 0;
        weatherLowC    = wv["lowC"]  | 0;
        weatherIcon    = wv["icon"]  | "cloud";
        weatherSummary = wv["summary"] | "";
        numWeatherHourly = 0;
        if (wv.containsKey("hourly")) {
          for (JsonVariant h : wv["hourly"].as<JsonArray>()) {
            if (numWeatherHourly >= MAX_WEATHER_HOURLY) break;
            weatherHourly[numWeatherHourly++] = h.as<String>();
          }
        }
      }

      Serial.printf("Data OK — total $%.2f, %d events, %d all-day, %d holdings, %d news, %d markets, busy=%d\n",
                    portfolioTotal, numEvents, numAllDay, numHoldings, numNews, numMarkets, dailyBusyPercent);
    } else {
      Serial.printf("JSON parse error on /api/matrix: %s\n", err.c_str());
    }
  } else {
    Serial.printf("HTTP error fetching /api/matrix: %d\n", code);
  }
  http.end();
}

int getActiveScreens(String *out) {
  int n = 0;
  if (numEnabledScreens > 0) {
    for (int i = 0; i < numEnabledScreens && n < MAX_SCREENS; i++) out[n++] = enabledScreens[i];
  } else {
    // Round 91 — Jon: "auto rotate should be able to rotate through every
    // single page available, selectable in the checkbox menu." That means
    // enabledScreens (from the backend's matrixControl.js) is now the only
    // source of truth for rotation membership — clock/dayoverview/
    // commuting used to be force-injected below regardless of what the web
    // Wall tab had checked, which is exactly why "Today's Timeline" (the
    // busy-score page) could show up in rotation with no checkbox to find
    // or turn it off. That forced-injection block is gone.
    //
    // This DEFAULTS fallback only fires if /api/matrix/command has never
    // answered even once (e.g. the Pi is unreachable from first boot) — a
    // judgment call, flagged to Jon rather than silently made: it includes
    // clock/dayoverview/commuting (real, useful screens with no dependency
    // on the Pi actually answering) but deliberately leaves out stars/
    // balls, since showing an ambient demo effect as the *fallback* during
    // a can't-reach-the-Pi outage seemed like the wrong first impression.
    // Once a real poll succeeds, enabledScreens takes over completely.
    const char *DEFAULTS[] = { "portfolio", "events", "holdings", "markets", "news", "weather",
                                "clock", "dayoverview", "commuting" };
    for (int i = 0; i < 9 && n < MAX_SCREENS; i++) out[n++] = String(DEFAULTS[i]);
  }
  return n;
}

void pollCommand() {
  HTTPClient http;
  http.begin(commandUrl);
  http.setTimeout(1500);
  int code = http.GET();

  if (code == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(1536);
    DeserializationError err = deserializeJson(doc, payload);

    if (!err) {
      numEnabledScreens = 0;
      if (doc.containsKey("enabledScreens")) {
        for (JsonVariant v : doc["enabledScreens"].as<JsonArray>()) {
          if (numEnabledScreens < MAX_SCREENS) enabledScreens[numEnabledScreens++] = v.as<String>();
        }
      }

      pinnedScreen = (doc.containsKey("pinnedScreen") && !doc["pinnedScreen"].isNull())
                       ? doc["pinnedScreen"].as<String>() : "";

      bool wasActive = notificationActive;
      if (doc.containsKey("notification") && !doc["notification"].isNull()) {
        notificationActive = true;
        notificationText = doc["notification"]["text"] | "";
        notificationSecondsRemaining = doc["notification"]["secondsRemaining"] | 0;
        if (!wasActive) Serial.printf("Notification: %s\n", notificationText.c_str());
      } else {
        notificationActive = false;
      }

      // Alert overlay — not sent by the backend at all yet (see the field's
      // declaration comment above). Parsed defensively so this lights up
      // automatically the day the backend adds it, with zero firmware
      // change needed.
      bool wasAlertActive = alertActive;
      if (doc.containsKey("alert") && !doc["alert"].isNull()) {
        alertActive = true;
        alertSeverity = doc["alert"]["severity"] | "medium";
        alertText = doc["alert"]["text"] | "";
        if (!wasAlertActive) Serial.printf("Alert (%s): %s\n", alertSeverity.c_str(), alertText.c_str());
      } else {
        alertActive = false;
      }

      // Round 75 — "push a page": present only while the backend's own
      // expiresAt window is still open (see livePushedScreen()), so this
      // just tracks whatever the last poll saw with no local timer.
      String prevPushedScreenId = pushedScreenId;
      pushedScreenId = (doc.containsKey("pushedScreen") && !doc["pushedScreen"].isNull())
                         ? doc["pushedScreen"]["id"].as<String>() : "";
      if (pushedScreenId != prevPushedScreenId && pushedScreenId.length() > 0) {
        Serial.printf("Pushed screen: %s\n", pushedScreenId.c_str());
      }

      if (doc.containsKey("testEvent") && !doc["testEvent"].isNull()) {
        int id = doc["testEvent"]["id"] | -1;
        String label = doc["testEvent"]["label"] | "";
        if (id != lastTestEventId) {
          lastTestEventId = id;
          Serial.println(label);
        }
      }
    }
  } else {
    static unsigned long lastWarn = 0;
    if (millis() - lastWarn > 30000) {
      Serial.printf("Command endpoint not reachable (HTTP %d) — using default screen rotation\n", code);
      lastWarn = millis();
    }
  }
  http.end();
}

void runInitialDataFetch() {
  unsigned long start = millis();
  while (millis() - start < 8000 && !dataValid) {
    drawStatusScreen("ORANGE PI SECRETARY", "Fetching data...", millis(), dma_display->color565(255, 180, 0));
    pollData();
    if (!dataValid) delay(400);
  }
  pollCommand();

  // Round 91 — Jon: "same with the pi fetch page, I want to see the
  // status," same treatment as WIFI CONNECTED above: a real one-shot
  // result screen (green when the fetch actually landed, red if it timed
  // out) instead of cutting straight into rotation the instant this
  // function returns. No spinner (see drawStatusScreen's comment) and
  // held 3s so it's actually readable. Subtitle deliberately doesn't show
  // PI_HOST — round 79 already settled that the boot screens show plain
  // status text, never a network address (that round's "SSID not IP" fix
  // on WIFI CONNECTED), so this stays consistent with that.
  if (dataValid) {
    drawStatusScreen("PI CONNECTED", "Data received", millis(), dma_display->color565(0, 255, 120), false);
  } else {
    drawStatusScreen("NO DATA YET", "Will keep retrying", millis(), dma_display->color565(255, 70, 60), false);
  }
  delay(3000);
}

// ============================================================
// Screen renderers
// ============================================================
void renderPortfolio(unsigned long now) {
  dma_display->clearScreen();

  if (hasMarketOpen) {
    if (marketOpen) {
      float pulse = ((sin(now / 260.0) + 1) / 2.0) * 255; // full sweep — goes completely dark at the trough
      dma_display->fillCircle(187, 4, 2, dma_display->color565(0, (uint8_t)pulse, 0));
      String liveLabel = "LIVE";
      dma_display->setTextSize(1);
      dma_display->setTextColor(dma_display->color565(120, 120, 120));
      dma_display->setCursor(187 - 2 - 2 - (int)liveLabel.length() * 6, 1);
      dma_display->print(liveLabel);
    } else {
      // Static — no pulse/blink, unlike LIVE — same muted gray label,
      // mirrored to the left of the dot the same way (round 73, Jon:
      // "I need closed written next to it, similar to the live except
      // without any of the blinking").
      dma_display->fillCircle(187, 4, 2, dma_display->color565(255, 40, 40));
      String closedLabel = "CLOSED";
      dma_display->setTextSize(1);
      dma_display->setTextColor(dma_display->color565(120, 120, 120));
      dma_display->setCursor(187 - 2 - 2 - (int)closedLabel.length() * 6, 1);
      dma_display->print(closedLabel);
    }
  }

  dma_display->setTextSize(2);
  dma_display->setTextColor(dma_display->color565(255, 255, 255));
  String total = formatMoneyWhole(portfolioTotal);
  dma_display->setCursor(centerTextX(total, 12), 2);
  dma_display->print(total);

  bool up = portfolioDayChange >= 0;
  uint16_t changeColor = up ? dma_display->color565(0, 255, 80) : dma_display->color565(255, 60, 60);
  dma_display->setTextSize(1);
  dma_display->setTextColor(changeColor);
  String line = formatSignedMoney(portfolioDayChange) + " (" + formatSignedPercent(portfolioDayChangePercent) + ")";
  dma_display->setCursor(centerTextX(line, 6), 21);
  dma_display->print(line);
}

// Busy score color scale — green/amber/orange/red, dark green = light day,
// red = packed. Matches the website's busy indicators.
uint16_t busyScoreColor(int score) {
  if (score <= 2) return dma_display->color565(40, 200, 100);
  if (score <= 4) return dma_display->color565(110, 230, 120);
  if (score <= 6) return dma_display->color565(255, 210, 50);
  if (score <= 8) return dma_display->color565(255, 150, 30);
  return dma_display->color565(255, 70, 60);
}

// Real calendar colors — ported from the website's actual calendarSwatch()
// categories (frontend/src/Display.css's .d-* rules), not the placeholder
// 6-category set this used to guess at (work/school/personal/important/
// cannotmiss/tests — none of which are Jon's real config.json categories,
// which is exactly why every event rendered in the same default blue;
// round 74 — Jon: "the colors are not correct. They are all showing
// blue... but some of them are on different calendars"). Each hex value
// below is Display.css's real color run through an HSV saturation/value
// boost (round 74 — Jon: "brighten/saturate for the LED") since the
// website's palette is tuned for readability on a light page background,
// not a small low-resolution LED matrix.
uint16_t calColor(const String &cal) {
  if (cal == "critical")    return dma_display->color565(208, 90, 0);   // css #b5560d
  if (cal == "opportunity") return dma_display->color565(0, 191, 156);  // css #12806c
  if (cal == "assessment")  return dma_display->color565(191, 147, 0);  // css #9c7a0a
  if (cal == "important")   return dma_display->color565(110, 30, 199); // css #7440ad
  if (cal == "work")        return dma_display->color565(223, 15, 0);   // css #c22a1f
  // Round 76 — Jon: colors "still wrong... a lot of events are showing up
  // as yellow even though they should be a different color." Family is
  // teal/sea-green in his real calendar (it was wrongly sharing Sydney's
  // Demands' blue before this round); Sydney's Demands and Regular Events
  // never had their own bucket at all, so both silently fell through to
  // "personal" below — which is exactly the stray yellow he was seeing.
  if (cal == "family")      return dma_display->color565(20, 200, 145); // css #1f8f6f, brighter for LED
  if (cal == "sydney")      return dma_display->color565(60, 70, 255);  // css #4a4fc0, brighter for LED
  if (cal == "regular")     return dma_display->color565(190, 196, 204); // css #8b8f95, near-white for LED
  if (cal == "deadline")    return dma_display->color565(193, 44, 0);   // css #a83c1c
  if (cal == "class")       return dma_display->color565(0, 191, 63);   // css #1f7a3d
  if (cal == "admin")       return dma_display->color565(85, 137, 191); // css #40566d
  if (cal == "appointment") return dma_display->color565(10, 152, 191); // css #276f83
  if (cal == "gmail")       return dma_display->color565(0, 113, 193);  // css #1f6fa8
  return dma_display->color565(120, 125, 135); // css #5a5f66 — "personal" fallback, no longer yellow (round 76)
}

// Fallback for events that don't have a "cal" field yet — reuses the
// existing busy/medium/light color convention so nothing regresses
// visually until the backend adds "cal".
uint16_t calColorFallback(const String &cal, const String &busyLevel) {
  if (cal.length() > 0) return calColor(cal);
  if (busyLevel == "busy") return dma_display->color565(255, 60, 60);
  if (busyLevel == "medium") return dma_display->color565(255, 200, 0);
  return dma_display->color565(0, 255, 80);
}

String eventCaption(const EventItem &e) {
  String s = e.time + " " + e.title;
  s.toUpperCase();
  return s;
}

// Event caption/description scrolling — sit still for a beat, then crawl
// left slowly enough to read, then hold briefly before moving on. Every
// event gets at least EVENT_MIN_HOLD even if it never needs to scroll.
const unsigned long EVENT_STATIC_HOLD = 2000, EVENT_END_HOLD = 3000, EVENT_MIN_HOLD = 7000;
const float EVENT_SCROLL_SPEED = 0.018f;

unsigned long textRequiredTime(int overflowPx) {
  return overflowPx > 0 ? (unsigned long)(EVENT_STATIC_HOLD + overflowPx / EVENT_SCROLL_SPEED + EVENT_END_HOLD) : 0;
}

int scrollOffsetPx(unsigned long tMod, int overflowPx) {
  if (overflowPx <= 0) return 0;
  long scrollElapsed = (long)tMod - (long)EVENT_STATIC_HOLD;
  if (scrollElapsed < 0) scrollElapsed = 0;
  float off = scrollElapsed * EVENT_SCROLL_SPEED;
  if (off > overflowPx) off = overflowPx;
  return (int)off;
}

void computeEventPlan(int capMaxW, EventPlan *plan) {
  for (int i = 0; i < numEvents; i++) {
    plan[i].cap = eventCaption(events[i]);
    plan[i].desc = events[i].desc; plan[i].desc.toUpperCase();
    plan[i].capOverflow = max(0, (int)plan[i].cap.length() * 6 - capMaxW);
    plan[i].descOverflow = max(0, (int)plan[i].desc.length() * 6 - capMaxW);
    unsigned long durCap = textRequiredTime(plan[i].capOverflow);
    unsigned long durDesc = textRequiredTime(plan[i].descOverflow);
    plan[i].dur = max((unsigned long)EVENT_MIN_HOLD, max(durCap, durDesc));
  }
}

// Round 76 — same idea as computeEventPlan(), but for the new dedicated
// all-day page: one caption per all-day event ("ALL DAY: <title>"), no
// desc line, no timeline bar (there's no start/end time to plot).
// (AllDayPlan itself is declared up near EventPlan — see the comment
// there for why.)
void computeAllDayPlan(int capMaxW, AllDayPlan *plan) {
  for (int i = 0; i < numAllDay; i++) {
    String s = "ALL DAY: " + allDayEvents[i].title;
    s.toUpperCase();
    plan[i].cap = s;
    plan[i].capOverflow = max(0, (int)plan[i].cap.length() * 6 - capMaxW);
    plan[i].dur = max((unsigned long)EVENT_MIN_HOLD, textRequiredTime(plan[i].capOverflow));
  }
}

unsigned long allDayRequiredTime(int capMaxW) {
  if (numAllDay == 0) return 0;
  AllDayPlan plan[MAX_ALLDAY];
  computeAllDayPlan(capMaxW, plan);
  unsigned long total = 0;
  for (int i = 0; i < numAllDay; i++) total += plan[i].dur;
  return total;
}

// How long the Events screen needs to stay up to cycle through every event
// at least once. renderEvents() below picks the current event from
// `elapsed % cycle` over ALL numEvents events, but until round 74 nothing
// extended the screen's rotation slot to match that full cycle length —
// unlike News/Holdings, which have had this since round 59/61 — so a day
// with several events got cut off partway through instead of showing all
// of them (round 74 — Jon: "If there's ten events, we gotta go through
// all of them, get all of their metadata before this page disappears").
// Recomputes the same plan computeEventPlan() builds so this can never
// drift out of sync with what renderEvents() actually cycles through.
unsigned long eventsRequiredTime() {
  if (numEvents == 0 && numAllDay == 0) return 0;
  const int barX0 = 2; // must match renderEvents()'s margin
  int capMaxW = W - barX0 - 2;
  EventPlan plan[MAX_EVENTS];
  computeEventPlan(capMaxW, plan);
  unsigned long cycle = 0;
  for (int i = 0; i < numEvents; i++) cycle += plan[i].dur;
  cycle += allDayRequiredTime(capMaxW); // round 76 — all-day page's own slot
  return cycle;
}

void renderEvents(unsigned long elapsed) {
  dma_display->clearScreen();
  // Busy score lives on its own Day Overview page now — the timeline gets
  // the full board width, and the description text uses the same width
  // budget as the title above it (nothing left to dodge any more).
  const int barX0 = 2, barX1 = W - 2;

  if (numEvents == 0 && numAllDay == 0) {
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(150, 150, 150));
    dma_display->setCursor(barX0, 13);
    dma_display->print("NO EVENTS TODAY");
    return;
  }

  int capMaxW = W - barX0 - 2;
  EventPlan plan[MAX_EVENTS];
  computeEventPlan(capMaxW, plan);
  AllDayPlan allDayPlan[MAX_ALLDAY];
  computeAllDayPlan(capMaxW, allDayPlan);

  unsigned long cycle = 0;
  for (int i = 0; i < numEvents; i++) cycle += plan[i].dur;
  unsigned long allDayStart = cycle; // all-day page's slot starts where the timed events end
  for (int i = 0; i < numAllDay; i++) cycle += allDayPlan[i].dur;
  if (cycle == 0) cycle = 1;
  unsigned long tMod = elapsed % cycle;

  if (tMod >= allDayStart) {
    // Round 76 — dedicated all-day page: cycles through each all-day
    // event's title on its own (same scroll style as a timed event's
    // caption), with no timeline bar underneath — there's no start/end
    // time to plot for an all-day event, and Jon explicitly did not want
    // them mixed into the timed-events timeline any more.
    unsigned long t = tMod - allDayStart;
    int ai = 0;
    while (ai < numAllDay - 1 && t >= allDayPlan[ai].dur) { t -= allDayPlan[ai].dur; ai++; }
    dma_display->setTextSize(1);
    dma_display->setTextColor(calColorFallback(allDayEvents[ai].cal, ""));
    dma_display->setCursor(barX0 - scrollOffsetPx(t, allDayPlan[ai].capOverflow), 1);
    dma_display->print(allDayPlan[ai].cap);
    dma_display->setTextColor(dma_display->color565(120, 118, 110));
    dma_display->setCursor(barX0, 13);
    dma_display->print(numAllDay <= 1 ? "ALL-DAY EVENT" : ("ALL-DAY EVENT " + String(ai + 1) + "/" + String(numAllDay)));
    return;
  }

  int idx = 0;
  while (idx < numEvents - 1 && tMod >= plan[idx].dur) { tMod -= plan[idx].dur; idx++; }

  dma_display->setTextSize(1);
  dma_display->setTextColor(calColorFallback(events[idx].cal, events[idx].busyLevel));
  dma_display->setCursor(barX0 - scrollOffsetPx(tMod, plan[idx].capOverflow), 1);
  dma_display->print(plan[idx].cap);
  if (plan[idx].desc.length() > 0) {
    dma_display->setTextColor(dma_display->color565(150, 150, 150));
    dma_display->setCursor(barX0 - scrollOffsetPx(tMod, plan[idx].descOverflow), 11);
    dma_display->print(plan[idx].desc);
  }

  dma_display->fillRect(barX0, 25, barX1 - barX0, 4, dma_display->color565(45, 42, 38)); // grey track
  const int barY = 21, barH = 8;
  int hlX0 = 0, hlX1 = 0;
  for (int i = 0; i < numEvents; i++) {
    int startMin = timeToMinutes(events[i].time);
    int endMin = startMin + (events[i].dur > 0 ? events[i].dur : 30);
    int tx0 = barX0 + (int)round(minutesToFrac(startMin) * (barX1 - barX0));
    int tx1raw = barX0 + (int)round(minutesToFrac(endMin) * (barX1 - barX0));
    int tx1 = max(tx0 + 2, tx1raw); // every event gets at least a visible sliver
    int dx0 = max(barX0, tx0), dx1 = min(tx1, barX1);
    dma_display->fillRect(dx0, barY, dx1 - dx0, barH, calColorFallback(events[i].cal, events[i].busyLevel));
    if (i == idx) { hlX0 = dx0; hlX1 = dx1; }
  }
  float pulse = ((sin(elapsed / 220.0) + 1) / 2.0) * 255; // full sweep — goes completely dark at the trough
  uint16_t pulseColor = dma_display->color565((uint8_t)pulse, (uint8_t)pulse, (uint8_t)pulse);
  drawRectOutlineColor(hlX0 - 1, barY - 1, (hlX1 - hlX0) + 2, barH + 2, pulseColor);

  // Moving "now" marker, drawn AFTER the timeline bars so it always sits
  // on top of them — styled like a text cursor (crossbar top/bottom with
  // 1px ears, connected by a thin stem), running through to the bottom edge.
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int nowX = barX0 + (int)round(minutesToFrac(nowMinutes) * (barX1 - barX0));
    float nowPulseAlpha = 0.55f + 0.45f * ((sin(elapsed / 260.0) + 1) / 2.0f);
    uint16_t nowColor = scaleColor565(255, 210, 0, nowPulseAlpha);
    const int nowTop = 19, nowBottom = 30;
    dma_display->fillRect(nowX - 1, nowTop, 3, 1, nowColor);
    dma_display->fillRect(nowX, nowTop + 1, 1, nowBottom - nowTop - 1, nowColor);
    dma_display->fillRect(nowX - 1, nowBottom, 3, 1, nowColor);
  }
}

// Ticker text size. Was 3 (up from an original 2 that never actually
// applied on hardware — drawStripCmd used to gate setTextSize on cmd.h==8,
// but text commands always carry h=0, so real boards rendered at size 1
// the whole time). Size 3 turned out too big/cramped once actually seen on
// hardware, so round 59 dialed it back to 2 — still clearly bigger than
// the original size-1 bug, just not filling most of the panel height.
#define TICKER_TEXT_SIZE 2

int buildHoldingsStrip(StripCmd *cmds, int maxCmds) {
  int n = 0;
  int x = 6;
  const int GAP = 12, size = TICKER_TEXT_SIZE, rowY = (H - 8 * size) / 2; // vertically centered
  const int barH = 8 * size; // matches the text glyph height exactly, whatever size is — keeps
                              // the divider bars vertically centered with the text instead of
                              // drifting out of alignment if TICKER_TEXT_SIZE ever changes again.
  auto addText = [&](String text, uint16_t color) {
    if (n >= maxCmds) return;
    cmds[n++] = { false, x, rowY, 0, 0, text, 0, color };
    x += (int)text.length() * 6 * size + GAP;
  };
  auto addBar = [&]() {
    if (n >= maxCmds) return;
    cmds[n++] = { false, x, rowY, 3, barH, "", 0, dma_display->color565(120, 170, 255) }; // reuse text=="" as "bar" marker via w>0
    x += 3 + GAP;
  };
  addText("HOLDINGS", dma_display->color565(0, 200, 255));
  addBar();
  // Round 73 — Jon: holdings colors stay the same, but on a closed market
  // he wants "a big, bright, red MARKET CLOSED TODAY disclaimer" spliced
  // into the strip right after the header, before the up/down holdings.
  // Reuses the same hasMarketOpen/marketOpen globals renderPortfolio()
  // already parses off /api/matrix, so no new JSON field is needed, and
  // it's plain-text same size as the rest of the ticker — just red.
  if (hasMarketOpen && !marketOpen) {
    // Round 74 — Jon: "next to it in the same size font... LAST PRICE...
    // so we know these prices... are not current." Kept in the same
    // addText() call (no bar between them) so it reads as one red
    // disclaimer instead of two separately-boxed ticker segments.
    // Round 75 — Jon: strip "MARKET"/"TODAY", just "CLOSED" (matches the
    // Markets screen's own bare "CLOSED" segment, added this round too).
    String closedText = "CLOSED";
    if (lastPriceLabel.length() > 0) closedText += "   LAST PRICE: " + lastPriceLabel;
    addText(closedText, dma_display->color565(255, 20, 20));
    addBar();
  }
  if (numHoldings == 0) {
    addText("NO HOLDINGS DATA", dma_display->color565(150, 150, 150));
  } else {
    // Top 3 gainers and top 3 decliners, picked separately — this used to
    // be one combined sort-by-percent-descending taking the first 6
    // overall, which could show 6 gainers and zero decliners (or vice
    // versa) instead of the intended 3-and-3 split. The twin already did
    // this correctly (round 61 fix brings the firmware in line with it).
    bool used[MAX_HOLDINGS] = { false };
    int upIdx[3], upCount = 0, downIdx[3], downCount = 0;
    for (int pick = 0; pick < 3; pick++) {
      int best = -1;
      for (int i = 0; i < numHoldings; i++) {
        if (used[i] || holdings[i].dayChangePercent < 0) continue;
        if (best == -1 || holdings[i].dayChangePercent > holdings[best].dayChangePercent) best = i;
      }
      if (best == -1) break;
      used[best] = true;
      upIdx[upCount++] = best;
    }
    for (int pick = 0; pick < 3; pick++) {
      int worst = -1;
      for (int i = 0; i < numHoldings; i++) {
        if (used[i] || holdings[i].dayChangePercent >= 0) continue;
        if (worst == -1 || holdings[i].dayChangePercent < holdings[worst].dayChangePercent) worst = i;
      }
      if (worst == -1) break;
      used[worst] = true;
      downIdx[downCount++] = worst;
    }
    auto addHolding = [&](int hi) {
      bool up = holdings[hi].dayChangePercent >= 0;
      uint16_t color = up ? dma_display->color565(0, 255, 80) : dma_display->color565(255, 60, 60);
      float dayDollar = holdings[hi].value * holdings[hi].dayChangePercent / 100.0f;
      addText(holdings[hi].symbol, dma_display->color565(255, 255, 255));
      addText(formatSignedPercent(holdings[hi].dayChangePercent), color);
      addText(formatSignedMoney(dayDollar), color);
      addBar();
    };
    for (int i = 0; i < upCount; i++) addHolding(upIdx[i]);
    for (int i = 0; i < downCount; i++) addHolding(downIdx[i]);
  }
  return n;
}

// How long the Holdings screen needs to stay up to scroll through the full
// strip (HOLDINGS header + up to 3 up + 3 down holdings) at least once.
// Reuses buildHoldingsStrip so this can never drift out of sync with what
// actually gets drawn. Same pattern as newsRequiredTime() (round 59) — a
// long strip like a full 6-holding split easily takes 30+ seconds to
// scroll through once at the ticker's speed, far more than the fixed 12s
// rotation slot, which was cutting it off before showing everything
// (round 61).
unsigned long holdingsRequiredTime() {
  StripCmd cmds[40];
  int n = buildHoldingsStrip(cmds, 40);
  int totalWidth = 6;
  for (int i = 0; i < n; i++) {
    int end = cmds[i].x + (cmds[i].w > 0 && cmds[i].text.length() == 0 ? cmds[i].w : (int)cmds[i].text.length() * 6 * TICKER_TEXT_SIZE);
    if (end > totalWidth) totalWidth = end;
  }
  const float speedPxPerMs = 0.05f; // must match renderHoldingsTicker's speed
  // Round 73: renderHoldingsTicker no longer loops the strip back around
  // (Jon: "I already tried to get it to not be an infinite loop... that
  // never happened" — so this makes it actually happen). It's a single
  // pass that starts with HOLDINGS already roughly centered (startShift,
  // must match renderHoldingsTicker's) instead of pinned at its native
  // left edge, and needs to run until the last item has fully cleared the
  // left edge (exitMargin of slack past that, same idea as the old +40).
  // Required time is the whole distance covered from start to that exit.
  const float startShift = (W / 2.0f) - 6; // must match renderHoldingsTicker
  const int exitMargin = 20;
  float totalScrollDistance = startShift + totalWidth + exitMargin;
  return (unsigned long)(totalScrollDistance / speedPxPerMs);
}

void drawStripCmd(const StripCmd &cmd, float sx) {
  int x = (int)round(sx);
  if (cmd.w > 0 && cmd.text.length() == 0) {
    if (x > -10 && x < W + 10) dma_display->fillRect(x, cmd.y, cmd.w, cmd.h, cmd.color);
  } else {
    // Round 75 fix — Jon: "the entire red text line disappears... blank
    // screen until the first ticker scrolled into view." The old fixed
    // "-200" cutoff assumed every segment was short (a symbol, a percent
    // sign) — once the closed-market banner grew a LAST PRICE suffix it
    // could run 400+px wide, and this cutoff stopped the print() call the
    // moment the segment's LEFT edge passed -200, well before the text
    // had actually scrolled clear of the visible 192px panel, so a wide
    // segment vanished mid-scroll instead of sliding smoothly off. Scale
    // the left margin to the segment's own width instead of a constant,
    // so a segment only stops drawing once it's genuinely off-screen.
    int textWidthPx = (int)cmd.text.length() * 6 * TICKER_TEXT_SIZE;
    if (x + textWidthPx > -10 && x < W + 50) {
      dma_display->setTextSize(TICKER_TEXT_SIZE);
      dma_display->setTextColor(cmd.color);
      dma_display->setCursor(x, cmd.y);
      dma_display->print(cmd.text);
    }
  }
}

void renderHoldingsTicker(unsigned long elapsed) {
  dma_display->clearScreen();
  StripCmd cmds[40];
  int n = buildHoldingsStrip(cmds, 40);
  const float speedPxPerMs = 0.05f;
  // Round 73: single pass, no wraparound — the old version drew the strip
  // twice (once at its normal offset, once shifted a full totalW ahead)
  // so it cycled back to the start forever inside its rotation slot; Jon
  // wanted a clean finish instead. Starting position is shifted right so
  // HOLDINGS begins roughly centered rather than pinned at its native
  // x=6 near the panel's left edge ("just make sure to start the word
  // holdings somewhere in the middle of the screen we can probably start
  // the scrolling right away" — no separate static pause needed).
  // startShift must match holdingsRequiredTime()'s, which is what decides
  // how long this screen stays up.
  const float startShift = (W / 2.0f) - 6;
  float offset = -startShift + elapsed * speedPxPerMs;
  for (int i = 0; i < n; i++) drawStripCmd(cmds[i], cmds[i].x - offset);
}

// Round 74 — Markets screen: same single-pass, centered-start ticker as
// Holdings (buildHoldingsStrip/holdingsRequiredTime/renderHoldingsTicker
// above), just a different strip: the tracked indices' % change, then
// VIX colored by its own calm/normal/jumpy/volatile bucket instead of a
// directional up/down color (VIX doesn't have an "up is good" reading).
uint16_t vixColorFor(const String &bucket) {
  if (bucket == "calm") return dma_display->color565(0, 255, 80);
  if (bucket == "jumpy") return dma_display->color565(255, 160, 0);
  if (bucket == "volatile") return dma_display->color565(255, 40, 40);
  return dma_display->color565(255, 255, 255); // normal
}

int buildMarketsStrip(StripCmd *cmds, int maxCmds) {
  int n = 0;
  int x = 6;
  const int GAP = 12, size = TICKER_TEXT_SIZE, rowY = (H - 8 * size) / 2;
  const int barH = 8 * size;
  auto addText = [&](String text, uint16_t color) {
    if (n >= maxCmds) return;
    cmds[n++] = { false, x, rowY, 0, 0, text, 0, color };
    x += (int)text.length() * 6 * size + GAP;
  };
  auto addBar = [&]() {
    if (n >= maxCmds) return;
    cmds[n++] = { false, x, rowY, 3, barH, "", 0, dma_display->color565(120, 170, 255) };
    x += 3 + GAP;
  };
  addText("MARKETS", dma_display->color565(0, 200, 255));
  addBar();
  // Round 75 — Jon: same closed-market indicator as Holdings, "in the
  // same spot, in the same format" — a bare red "CLOSED" right after the
  // header, before the index data. Reuses the same hasMarketOpen/
  // marketOpen globals Holdings/Portfolio already parse off /api/matrix.
  if (hasMarketOpen && !marketOpen) {
    addText("CLOSED", dma_display->color565(255, 20, 20));
    addBar();
  }
  if (numMarkets == 0 && !hasVix) {
    addText("NO MARKET DATA", dma_display->color565(150, 150, 150));
  } else {
    for (int i = 0; i < numMarkets; i++) {
      bool up = markets[i].changePercent >= 0;
      uint16_t color = up ? dma_display->color565(0, 255, 80) : dma_display->color565(255, 60, 60);
      addText(markets[i].symbol, dma_display->color565(255, 255, 255));
      addText(formatSignedPercent(markets[i].changePercent), color);
      addBar();
    }
    if (hasVix) {
      addText("VIX", dma_display->color565(255, 255, 255));
      addText(String(vixValue, 1), vixColorFor(vixBucket));
      addBar();
    }
  }
  return n;
}

// Same math as holdingsRequiredTime() — see its comment for why this
// isn't just the fixed ROTATION_MS slot.
unsigned long marketsRequiredTime() {
  StripCmd cmds[40];
  int n = buildMarketsStrip(cmds, 40);
  int totalWidth = 6;
  for (int i = 0; i < n; i++) {
    int end = cmds[i].x + (cmds[i].w > 0 && cmds[i].text.length() == 0 ? cmds[i].w : (int)cmds[i].text.length() * 6 * TICKER_TEXT_SIZE);
    if (end > totalWidth) totalWidth = end;
  }
  const float speedPxPerMs = 0.05f; // must match renderMarketsTicker's speed
  const float startShift = (W / 2.0f) - 6; // must match renderMarketsTicker
  const int exitMargin = 20;
  float totalScrollDistance = startShift + totalWidth + exitMargin;
  return (unsigned long)(totalScrollDistance / speedPxPerMs);
}

void renderMarketsTicker(unsigned long elapsed) {
  dma_display->clearScreen();
  StripCmd cmds[40];
  int n = buildMarketsStrip(cmds, 40);
  const float speedPxPerMs = 0.05f;
  const float startShift = (W / 2.0f) - 6;
  float offset = -startShift + elapsed * speedPxPerMs;
  for (int i = 0; i < n; i++) drawStripCmd(cmds[i], cmds[i].x - offset);
}

// Forward declaration: defined later in the file (after renderCommuting),
// but renderNews() above needs it and this build has no auto-prototype
// generation, so it must be declared explicitly.
void renderComingSoonFwd(String id, unsigned long now);

const float NEWS_SCROLL_SPEED_PX_MS = 0.035f;

// Shared with newsRequiredTime() below so the rotation timer and the actual
// on-screen scroll always agree on exactly what text is being shown.
String buildNewsJoined() {
  String joined = "";
  for (int i = 0; i < numNews; i++) { joined += newsHeadlines[i]; if (i < numNews - 1) joined += "   /   "; }
  joined.toUpperCase();
  return joined;
}

// How long the News screen needs to stay up to scroll through every
// headline at least once — 0 if it already fits statically (nothing to
// scroll, so the normal fixed rotation slot is plenty). Used by loop() to
// hold News on screen past its usual slot instead of cutting a long
// headline set off mid-scroll (round 59).
unsigned long newsRequiredTime() {
  if (numNews == 0) return 0;
  String joined = buildNewsJoined();
  int textW = joined.length() * 6;
  if (textW <= W - 4) return 0;
  int totalW = textW + 40;
  return (unsigned long)(totalW / NEWS_SCROLL_SPEED_PX_MS);
}

void renderNews(unsigned long elapsed) {
  if (numNews == 0) { renderComingSoonFwd("news", elapsed); return; }
  dma_display->clearScreen();
  String joined = buildNewsJoined();
  int textW = joined.length() * 6;
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(0, 200, 255));
  if (textW <= W - 4) {
    dma_display->setCursor(centerTextX(joined, 6), 13);
    dma_display->print(joined);
  } else {
    int totalW = textW + 40;
    float offset = fmod(elapsed * NEWS_SCROLL_SPEED_PX_MS, (float)totalW);
    dma_display->setCursor((int)round(-offset), 13);
    dma_display->print(joined);
    dma_display->setCursor((int)round(-offset + totalW), 13);
    dma_display->print(joined);
  }
}

void pad2Into(char *buf, int n) { snprintf(buf, 3, "%02d", n); }

const char *WEEKDAY_ABBR[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
const char *MONTH_ABBR[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

void renderClock() {
  dma_display->clearScreen();
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 0)) {
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(150, 150, 150));
    String msg = "SYNCING...";
    dma_display->setCursor(centerTextX(msg, 6), 13);
    dma_display->print(msg);
    return;
  }
  int hh = timeinfo.tm_hour;
  bool pm = hh >= 12;
  hh = hh % 12; if (hh == 0) hh = 12;
  char mb[3], sb[3];
  pad2Into(mb, timeinfo.tm_min); pad2Into(sb, timeinfo.tm_sec);
  String rest = ":" + String(mb) + ":" + String(sb) + " " + (pm ? "PM" : "AM");
  // No leading zero on the hour — but the rest of the string must not shift
  // when the hour drops from 2 digits to 1. Anchor the centering math to a
  // fixed-width "00:MM:SS AM" reference, then nudge the actual (shorter)
  // string right by one char cell for single-digit hours. Intentionally
  // looks a little off-center for 1-9 o'clock; dead-centered at 10/11/12.
  const int size = 2, charW = 6 * size;
  String anchorStr = "00" + rest;
  int anchorX = centerTextX(anchorStr, charW);
  int startX = (hh >= 10) ? anchorX : anchorX + charW;
  String timeStr = String(hh) + rest;
  dma_display->setTextSize(size);
  dma_display->setTextColor(dma_display->color565(230, 35, 35)); // more red, less pink (round 59)
  dma_display->setCursor(startX, 5);
  dma_display->print(timeStr);

  String dateStr = String(WEEKDAY_ABBR[timeinfo.tm_wday]) + " " + String(MONTH_ABBR[timeinfo.tm_mon]) + " " + String(timeinfo.tm_mday);
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(150, 150, 150));
  dma_display->setCursor(centerTextX(dateStr, 6), 23); // round 89: nudged up 1px per Jon
  dma_display->print(dateStr);
}

// Day Overview — round 91 redesign, Jon: "I want some sort of half page
// split, where one half is a bar with the busy side and the free side
// where the colour seperation shifts based on the proportion of each, the
// label of Xh busy and Xh free should be below this display chart line on
// the respective side, the rest of the page should have the x events and
// busy score parts." Top half (y 0-15): the existing score-dot/"BUSY N"/
// event-count row, unchanged. Bottom half (y 16-31): the new proportional
// busy/free bar — the color boundary between the two sides sits exactly at
// hoursBusy/(hoursBusy+hoursFree) along the bar's width, not a fixed
// midpoint — with "X.XH BUSY"/"X.XH FREE" labels below it on their
// respective sides. Still real, still gated: the bar and its labels only
// draw when the backend has actually sent hoursBusy/hoursFree (rules
// decide the split, this just renders it) — same "coming soon" fallback
// as before when it hasn't.
void renderDayOverview() {
  dma_display->clearScreen();

  // Top half — busy score + event count (both real, existing fields).
  // Round 91 — Jon: "a slightly bigger coloured circle with a centered
  // number inside of it in white as we were doing way way back. right
  // now the circle would be barely big enough but one extra pixel all
  // around would make it perfect." dotR 4->5; the score number now lives
  // inside the circle in white, so the label beside it drops the digit
  // and just reads "BUSY SCORE".
  int score = max(0, min(10, (int)round(dailyBusyPercent / 10.0)));
  uint16_t scoreColor = busyScoreColor(score);
  const int dotR = 5, dotX = 2 + dotR, dotY = 5;
  dma_display->fillCircle(dotX, dotY, dotR, scoreColor);
  dma_display->setTextSize(1);
  String scoreStr = String(score);
  dma_display->setTextColor(dma_display->color565(255, 255, 255));
  dma_display->setCursor(dotX - (int)round(scoreStr.length() * 6 / 2.0), dotY - 3);
  dma_display->print(scoreStr);

  String busyText = "BUSY SCORE";
  dma_display->setTextColor(scoreColor);
  dma_display->setCursor(dotX + dotR + 3, 2);
  dma_display->print(busyText);

  String evText = String(numEvents) + (numEvents == 1 ? " EVENT" : " EVENTS");
  dma_display->setTextColor(dma_display->color565(150, 150, 150));
  dma_display->setCursor(W - 2 - (int)evText.length() * 6, 2);
  dma_display->print(evText);

  // Bottom half — proportional busy/free bar + labels below it.
  // Round 91 — Jon: "id like the free portion of the busy free to be
  // grey, that way a less busy day that is also green ... doesnt match
  // the green free part." A light-busy day's scoreColor IS green
  // (busyScoreColor(score<=2) below), which used to collide visually
  // with a green free side — grey never collides with any busy-score
  // color.
  const uint16_t freeColor = dma_display->color565(150, 150, 150);
  if (dayOverview.hasHours) {
    const int barX0 = 2, barX1 = W - 2, barY = 18, barH = 4;
    const float total = dayOverview.hoursBusy + dayOverview.hoursFree;
    const float busyFrac = total > 0 ? (dayOverview.hoursBusy / total) : 0.0f;
    const int splitX = barX0 + (int)round(busyFrac * (barX1 - barX0));

    dma_display->fillRect(barX0, barY, barX1 - barX0, barH, dma_display->color565(35, 32, 28)); // track
    if (splitX > barX0) dma_display->fillRect(barX0, barY, splitX - barX0, barH, scoreColor);
    if (barX1 > splitX) dma_display->fillRect(splitX, barY, barX1 - splitX, barH, freeColor);

    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fH BUSY", dayOverview.hoursBusy);
    String busyHText = buf;
    snprintf(buf, sizeof(buf), "%.1fH FREE", dayOverview.hoursFree);
    String freeHText = buf;
    dma_display->setTextColor(scoreColor);
    dma_display->setCursor(barX0, 24);
    dma_display->print(busyHText);
    dma_display->setTextColor(freeColor);
    dma_display->setCursor(barX1 - (int)freeHText.length() * 6, 24);
    dma_display->print(freeHText);
  } else {
    dma_display->setTextColor(dma_display->color565(90, 85, 75));
    String tbd = "HOURS DATA COMING SOON";
    dma_display->setCursor(centerTextX(tbd, 6), 22);
    dma_display->print(tbd);
  }
  // Commute/drive row intentionally not shown here any more — there's a
  // dedicated Commuting page for that now.
}

// Sleep & Alarm — round 91, ported from the HUB75 Twin's renderSleepAlarm()
// prototype (hub75-twin-publish.html) pixel-for-pixel, same 8PM-9AM sleep
// window mapping and left/right split (left: bed-wake range + sleep-window
// bar + nearest nearby event; right: pulsing next-alarm time). hasData
// gates the real render vs. "coming soon" the same way every other
// no-backend-yet screen does — see sleepAlarm parsing above.
const int SLEEP_MID = W / 2;      // 96
const int SLEEP_L0 = 2, SLEEP_L1 = SLEEP_MID - 4;
const int SLEEP_R0 = SLEEP_MID + 4, SLEEP_R1 = W - 2;

// Maps a clock time (minutes since midnight) into 0..1 across the fixed
// 8:00 PM - 9:00 AM sleep window, wrapping times after midnight forward by
// 24h first so the whole window is one continuous span to place a dot on.
float sleepWindowFrac(int mins) {
  const int WIN_START = 20 * 60, WIN_END = 33 * 60; // 8:00 PM .. 9:00 AM (24:00+9:00)
  int m = mins;
  if (m < 12 * 60) m += 24 * 60;
  float frac = (float)(m - WIN_START) / (float)(WIN_END - WIN_START);
  return max(0.0f, min(1.0f, frac));
}

void renderSleepAlarm(unsigned long now) {
  dma_display->clearScreen();

  if (!sleepAlarm.hasData) {
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(90, 85, 75));
    String tbd = "SLEEP DATA COMING SOON";
    dma_display->setCursor(centerTextX(tbd, 6), 14);
    dma_display->print(tbd);
    return;
  }

  int bedMin = timeToMinutes(sleepAlarm.bedTime), wakeMin = timeToMinutes(sleepAlarm.wakeTime);
  String rangeText = minutesToClockStr(bedMin) + "-" + minutesToClockStr(wakeMin);
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(150, 160, 255));
  dma_display->setCursor(centerTextXIn(rangeText, 6, SLEEP_L0, SLEEP_L1), 1);
  dma_display->print(rangeText);

  const int barX0 = SLEEP_L0 + 2, barX1 = SLEEP_L1 - 2, barY = 12, barH = 4;
  dma_display->fillRect(barX0, barY, barX1 - barX0, barH, dma_display->color565(38, 42, 58));
  int sleepX0 = barX0 + (int)round(sleepWindowFrac(bedMin) * (barX1 - barX0));
  int sleepX1 = barX0 + (int)round(sleepWindowFrac(wakeMin) * (barX1 - barX0));
  dma_display->fillRect(sleepX0, barY, max(1, sleepX1 - sleepX0), barH, dma_display->color565(90, 100, 190));

  const uint16_t lateColor = dma_display->color565(170, 130, 255);
  const uint16_t earlyColor = dma_display->color565(255, 180, 80);
  int lateIdx = -1, earlyIdx = -1;
  for (int i = 0; i < sleepAlarm.numNearbyEvents; i++) {
    int evX = barX0 + (int)round(sleepWindowFrac(timeToMinutes(sleepAlarm.nearbyEvents[i].time)) * (barX1 - barX0));
    dma_display->fillCircle(evX, barY + barH / 2, 1, sleepAlarm.nearbyEvents[i].period == "late" ? lateColor : earlyColor);
    if (sleepAlarm.nearbyEvents[i].period == "late" && lateIdx < 0) lateIdx = i;
    if (sleepAlarm.nearbyEvents[i].period == "early" && earlyIdx < 0) earlyIdx = i;
  }

  int shownIdx = -1;
  if (lateIdx >= 0 && earlyIdx >= 0) shownIdx = ((now / 2600) % 2 == 0) ? lateIdx : earlyIdx;
  else shownIdx = (lateIdx >= 0) ? lateIdx : earlyIdx;

  if (shownIdx >= 0) {
    String label = sleepAlarm.nearbyEvents[shownIdx].label;
    label.toUpperCase();
    const int maxW = (SLEEP_L1 - SLEEP_L0) - 4;
    while ((int)label.length() * 6 > maxW && label.length() > 0) label = label.substring(0, label.length() - 1);
    bool late = sleepAlarm.nearbyEvents[shownIdx].period == "late";
    dma_display->setTextColor(late ? lateColor : earlyColor);
    dma_display->setCursor(centerTextXIn(label, 6, SLEEP_L0, SLEEP_L1), 22);
    dma_display->print(label);
  } else {
    dma_display->setTextColor(dma_display->color565(100, 100, 100));
    String none = "NO EVENTS NEARBY";
    const int maxW = (SLEEP_L1 - SLEEP_L0) - 4;
    while ((int)none.length() * 6 > maxW && none.length() > 0) none = none.substring(0, none.length() - 1);
    dma_display->setCursor(centerTextXIn(none, 6, SLEEP_L0, SLEEP_L1), 22);
    dma_display->print(none);
  }

  for (int y = 2; y < 30; y++) dma_display->drawPixel(SLEEP_MID, y, dma_display->color565(55, 60, 80));

  // Same white->gold pulse convention as the Twin (lerp3([255,255,255],
  // [255,200,110], pulse)) — red channel stays pinned at 255 throughout.
  String alarmStr = minutesToClockStr(timeToMinutes(sleepAlarm.nextAlarm));
  float pulse = ((sin(now / 900.0) + 1) / 2.0) * 0.35;
  uint8_t ag = (uint8_t)round(255 + (200 - 255) * pulse);
  uint8_t ab = (uint8_t)round(255 + (110 - 255) * pulse);
  dma_display->setTextSize(2);
  dma_display->setTextColor(dma_display->color565(255, ag, ab));
  dma_display->setCursor(centerTextXIn(alarmStr, 12, SLEEP_R0, SLEEP_R1), 2);
  dma_display->print(alarmStr);

  dma_display->setTextSize(1);
  String caption = "NEXT ALARM";
  dma_display->setTextColor(dma_display->color565(190, 190, 190));
  dma_display->setCursor(centerTextXIn(caption, 6, SLEEP_R0, SLEEP_R1), 22);
  dma_display->print(caption);
}

// ============================================================
// Wake Up Mode — round 91, ported from the HUB75 Twin's renderWakeUp()
// pixel-for-pixel: same staged sunrise timeline (black -> navy -> orange
// horizon -> warm yellow -> "GOOD MORNING"), same timings and colors.
//
// The Twin blends stars/halo/text against a live framebuffer it can read
// back from; earlier this round that read-back gap was the reason Wake Up
// Mode's port got deferred. On closer look it doesn't actually matter
// here: every single thing this screen blends against is the sky
// gradient, and the sky gradient is a pure function of row y and elapsed
// time t — never something drawn earlier and then sampled back. So each
// blend point below computes its own background analytically (same top/
// bot lerp the sky fill itself uses) instead of reading a buffer, and the
// result is pixel-identical to the Twin's version without needing one.
// ============================================================
struct RGBf { float r, g, b; };
RGBf rgbf(float r, float g, float b) { return { r, g, b }; }
RGBf lerp3f(RGBf a, RGBf b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
float clamp255f(float v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }
uint16_t packRGBf(RGBf c) {
  return dma_display->color565((uint8_t)round(clamp255f(c.r)), (uint8_t)round(clamp255f(c.g)), (uint8_t)round(clamp255f(c.b)));
}
float wakeupSmoothstep(float a, float b, float t) {
  if (t <= a) return 0;
  if (t >= b) return 1;
  float x = (t - a) / (b - a);
  return x * x * (3 - 2 * x);
}
// Same hsv->rgb math as the Twin's hsv(h,s,v), kept in float RGB (not
// packed color565) so it can still be blended precisely below.
RGBf hsvToRGBf(float h, float s, float v) {
  h = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f);
  float c = (v / 255.0f) * (s / 255.0f);
  float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
  float m = (v / 255.0f) - c;
  float r, g, b;
  if (h < 60) { r = c; g = x; b = 0; }
  else if (h < 120) { r = x; g = c; b = 0; }
  else if (h < 180) { r = 0; g = c; b = x; }
  else if (h < 240) { r = 0; g = x; b = c; }
  else if (h < 300) { r = x; g = 0; b = c; }
  else { r = c; g = 0; b = x; }
  return rgbf((r + m) * 255.0f, (g + m) * 255.0f, (b + m) * 255.0f);
}
// Faux-bold: same trick the Twin's drawCharBold used (stretch every lit
// pixel 1 extra column wide) approximated at the whole-glyph level, since
// firmware prints through Adafruit_GFX's built-in font rather than the
// Twin's own per-pixel one — print the string twice, offset 1px in x.
// setTextSize()/current font are whatever the caller already set.
void printBold(int x, int y, const String &s, uint16_t color) {
  dma_display->setTextColor(color);
  dma_display->setCursor(x, y);
  dma_display->print(s);
  dma_display->setCursor(x + 1, y);
  dma_display->print(s);
}

const unsigned long WAKEUP_T_BLACK = 1200;     // pure black, just the corner label
const unsigned long WAKEUP_T_BLUE = 4000;      // navy blues fully faded in, no orange yet
const unsigned long WAKEUP_T_SUNPOKE = 6500;   // sun's top edge starts breaking the horizon
const unsigned long WAKEUP_T_ORANGE = 10000;   // sun fully risen to resting height, orange filled in
const unsigned long WAKEUP_T_YELLOW = 13000;   // whole screen warmed toward yellow, sun at final size
const unsigned long WAKEUP_T_TEXTFADE = 1500;  // "GOOD MORNING" fade-in duration
const unsigned long WAKEUP_T_HOLD = WAKEUP_T_YELLOW + WAKEUP_T_TEXTFADE; // ~14.5s — settled state begins here

// Fixed points (not random) — same 9 the Twin uses, kept out of the sun's
// bottom-right landing spot and the top-left corner label.
struct WakeupStar { int x, y; unsigned long offset, cycle; };
const WakeupStar WAKEUP_STARS[9] = {
  { 22,  5,  0,    2600 }, { 60,  10, 1450, 3100 }, { 96,  4,  2600, 2400 },
  { 128, 14, 500,  3400 }, { 145, 20, 1950, 2900 }, { 40,  18, 950,  2700 },
  { 78,  22, 2100, 3000 }, { 165, 9,  300,  2500 }, { 110, 25, 1600, 3300 },
};
// "Quick rise, slower decay" twinkle curve — same shape as the boot
// sequence's spark field, reads as an actual twinkle rather than a smooth
// symmetric sine.
float wakeupStarTwinkle(unsigned long t, const WakeupStar &star) {
  long cyc = (long)star.cycle;
  long m = ((long)(t + star.offset)) % cyc;
  if (m < 0) m += cyc;
  float frac = (float)m / (float)cyc;
  return frac < 0.15f ? frac / 0.15f : pow(1.0f - (frac - 0.15f) / 0.85f, 1.6f);
}

void renderWakeUp(unsigned long t) {
  dma_display->clearScreen();

  float blueP        = wakeupSmoothstep(WAKEUP_T_BLACK, WAKEUP_T_BLUE, t);
  float sunRiseP     = wakeupSmoothstep(WAKEUP_T_BLUE, WAKEUP_T_ORANGE, t);
  float orangeSpreadP = wakeupSmoothstep(WAKEUP_T_SUNPOKE, WAKEUP_T_ORANGE, t);
  float yellowP      = wakeupSmoothstep(WAKEUP_T_ORANGE, WAKEUP_T_YELLOW, t);
  float textP        = wakeupSmoothstep(WAKEUP_T_YELLOW, WAKEUP_T_YELLOW + WAKEUP_T_TEXTFADE, t);
  // The whole sunrise dims to black over the same window GOOD MORNING
  // fades in, landing on a plain black screen the instant the text is
  // fully in — Jon: "when good morning hits, I just want good morning."
  float bgFade = 1.0f - textP;

  // Once settled (past WAKEUP_T_HOLD), a small continuous breathe keeps
  // the resting screen from looking like a static screenshot.
  unsigned long holdT = (t > WAKEUP_T_HOLD) ? (t - WAKEUP_T_HOLD) : 0;
  float glowWobble = sin(holdT / 4000.0f) * 0.06f;
  float bob = sin(holdT / 1800.0f) * 1.2f;

  // Sky — vertical gradient, staged black -> navy -> orange horizon glow
  // -> warm yellow, exactly like the Twin.
  RGBf BLACKC = rgbf(0, 0, 0);
  RGBf top = lerp3f(BLACKC, rgbf(10, 16, 42), blueP);
  RGBf bot = lerp3f(BLACKC, rgbf(16, 26, 64), blueP);
  bot = lerp3f(bot, rgbf(214, 112, 36), orangeSpreadP);
  top = lerp3f(top, rgbf(120, 108, 74), yellowP);
  bot = lerp3f(bot, rgbf(255, 196, 80), yellowP);
  float glowMod = 1.0f + glowWobble;
  top = rgbf(clamp255f(top.r * glowMod), clamp255f(top.g * glowMod), clamp255f(top.b * glowMod));
  bot = rgbf(clamp255f(bot.r * glowMod), clamp255f(bot.g * glowMod), clamp255f(bot.b * glowMod));
  top = lerp3f(BLACKC, top, bgFade);
  bot = lerp3f(BLACKC, bot, bgFade);

  for (int y = 0; y < H; y++) {
    float f = (float)y / (float)(H - 1);
    dma_display->fillRect(0, y, W, 1, packRGBf(lerp3f(top, bot, f)));
  }

  // Night-sky stars — only during the black/navy opening beat, retired by
  // starEnvelope before the sunrise itself starts.
  float starEnvelope = 1.0f - wakeupSmoothstep(WAKEUP_T_BLUE, WAKEUP_T_SUNPOKE, t);
  if (starEnvelope > 0.01f) {
    for (int i = 0; i < 9; i++) {
      const WakeupStar &s = WAKEUP_STARS[i];
      float alpha = wakeupStarTwinkle(t, s) * starEnvelope;
      if (alpha <= 0.01f) continue;
      float rowF = (float)s.y / (float)(H - 1);
      RGBf bg = lerp3f(top, bot, rowF);
      dma_display->drawPixel(s.x, s.y, packRGBf(lerp3f(bg, rgbf(255, 255, 255), alpha)));
    }
  }

  // Sun — climbs from below the bottom edge to its resting spot in the
  // bottom-right corner as sunRiseP goes 0->1, growing from nothing to
  // full size over the same climb, with a modest extra size bump during
  // the yellow phase. Halo/mid rings are alpha-blended against the sky
  // (computed analytically per row, per the big comment above); core/
  // bright are solid, no blending needed.
  int sunX = W - 26;
  float sunY = (H + 20) + ((H - 5) - (H + 20)) * sunRiseP + bob;
  float sizeScale = sunRiseP * (1.0f + 0.2f * yellowP) * bgFade;
  if (sizeScale > 0.02f) {
    int rHalo = (int)round(14 * sizeScale), rMid = (int)round(9 * sizeScale);
    int rCore = max(1, (int)round(5 * sizeScale)), rBright = max(1, (int)round(3 * sizeScale));
    RGBf haloColor = hsvToRGBf(38, 200, 255);
    for (int pass = 0; pass < 2; pass++) {
      int r = pass == 0 ? rHalo : rMid;
      float alpha = pass == 0 ? 0.10f : 0.22f;
      if (r <= 0) continue;
      float rr = (r + 0.5f) * (r + 0.5f);
      for (int dy = -r; dy <= r; dy++) {
        int py = (int)round(sunY) + dy;
        if (py < 0 || py >= H) continue;
        float rowF = (float)py / (float)(H - 1);
        RGBf bg = lerp3f(top, bot, rowF);
        uint16_t blended = packRGBf(lerp3f(bg, haloColor, alpha));
        for (int dx = -r; dx <= r; dx++) {
          if (dx * dx + dy * dy > rr) continue;
          int px = sunX + dx;
          if (px < 0 || px >= W) continue;
          dma_display->drawPixel(px, py, blended);
        }
      }
    }
    dma_display->fillCircle(sunX, (int)round(sunY), rCore, packRGBf(hsvToRGBf(42, 150, 255)));
    dma_display->fillCircle(sunX, (int)round(sunY), rBright, packRGBf(hsvToRGBf(48, 70, 255)));
  }

  // Corner label during the intro, "GOOD MORNING" full-screen once the
  // sequence lands — one crossfades into the other. Both are color-mixed
  // against the current sky rather than flat-blended, so the fade reads
  // correctly over the gradient.
  dma_display->setTextSize(1);
  if (textP < 0.3f) {
    RGBf labelColor = lerp3f(top, rgbf(255, 255, 255), 1.0f - (textP / 0.3f));
    dma_display->setTextColor(packRGBf(labelColor));
    dma_display->setCursor(4, 2);
    dma_display->print("WAKE UP MODE");
  }
  if (textP > 0.02f) {
    // "GOOD" / "MORNING" at size 2 are each exactly 16px tall, stacking to
    // fill the full 32px panel height with no gap. Faux-bold via
    // printBold(). Once settled, a slow white->gold shimmer keeps it from
    // looking static now that the background behind it is flat black.
    RGBf GOLD = rgbf(255, 200, 110);
    float holdPulse = holdT > 0 ? ((sin(holdT / 1400.0f) + 1) / 2.0f) * 0.18f : 0.0f;
    const char *lines[2] = { "GOOD", "MORNING" };
    const int ys[2] = { 0, 16 };
    dma_display->setTextSize(2);
    for (int i = 0; i < 2; i++) {
      String msg = lines[i];
      float rowF = (float)ys[i] / (float)(H - 1);
      RGBf msgBg = lerp3f(top, bot, rowF);
      RGBf color = lerp3f(msgBg, rgbf(255, 255, 255), textP);
      color = lerp3f(color, GOLD, holdPulse);
      printBold(centerTextX(msg, 12), ys[i], msg, packRGBf(color));
    }
  }
}

// Whichever of today's events is soonest from right now, wrapping back to
// the first event of the day once everything has already passed. Drives
// the Commuting page.
int nextUpcomingEventIdx() {
  if (numEvents == 0) return -1;
  struct tm timeinfo;
  int nowMin = 0;
  if (getLocalTime(&timeinfo, 0)) nowMin = timeinfo.tm_hour * 60 + timeinfo.tm_min;
  int order[MAX_EVENTS];
  for (int i = 0; i < numEvents; i++) order[i] = i;
  for (int i = 0; i < numEvents; i++)
    for (int j = i + 1; j < numEvents; j++)
      if (timeToMinutes(events[order[j]].time) < timeToMinutes(events[order[i]].time)) { int t = order[i]; order[i] = order[j]; order[j] = t; }
  for (int i = 0; i < numEvents; i++) if (timeToMinutes(events[order[i]].time) >= nowMin) return order[i];
  return order[0];
}

// Commuting: a big G-Wagon-style SUV takes up nearly the full height of
// the board and drives across it on a loop. The car is the opening beat —
// at the start of each pass it's the only thing on screen, and the
// commute text only becomes visible in the swath the car has already
// driven over (behind it, to its left, since it drives left-to-right),
// like it's dragging a curtain open. Ahead of the car nothing is revealed
// yet; by the time it exits the right edge the whole line is visible,
// then it loops and re-opens. Real Google Maps ETA is a documented future
// Pi feature (integration-roadmap.md item 8) — commuteMin is a
// placeholder until then.
void renderCommuting(unsigned long elapsed) {
  dma_display->clearScreen();
  const float speedPxPerMs = 0.045f;
  float jeepX = -JEEP_W + elapsed * speedPxPerMs;  // no modulo/wraparound — plays once per
                                                     // screen-visit, then holds off-screen
                                                     // until elapsed resets on re-entry

  int idx = nextUpcomingEventIdx();
  dma_display->setTextSize(1);
  if (idx >= 0) {
    if (dayOverview.hasCommute) {
      String leaveStr = "LEAVE BY " + minutesToClockStr(timeToMinutes(events[idx].time) - dayOverview.commuteMin);
      dma_display->setTextColor(dma_display->color565(255, 255, 255));
      dma_display->setCursor(centerTextX(leaveStr, 6), 12);
      dma_display->print(leaveStr);
      String sub = String(dayOverview.commuteMin) + " MIN TO " + events[idx].title;
      sub.toUpperCase();
      while ((int)sub.length() * 6 > W - 4 && sub.length() > 0) sub = sub.substring(0, sub.length() - 1);
      dma_display->setTextColor(dma_display->color565(150, 150, 150));
      dma_display->setCursor(centerTextX(sub, 6), 20);
      dma_display->print(sub);
    } else {
      String title = events[idx].title; title.toUpperCase();
      String line1 = "NEXT: " + title;
      while ((int)line1.length() * 6 > W - 4 && line1.length() > 0) line1 = line1.substring(0, line1.length() - 1);
      dma_display->setTextColor(dma_display->color565(255, 255, 255));
      dma_display->setCursor(centerTextX(line1, 6), 12);
      dma_display->print(line1);
      String sub = "COMMUTE ETA COMING SOON";
      dma_display->setTextColor(dma_display->color565(150, 150, 150));
      dma_display->setCursor(centerTextX(sub, 6), 20);
      dma_display->print(sub);
    }
  } else {
    String msg = "NO UPCOMING EVENTS";
    dma_display->setTextColor(dma_display->color565(150, 150, 150));
    dma_display->setCursor(centerTextX(msg, 6), 12);
    dma_display->print(msg);
  }

  // Curtain: black out everything the car hasn't driven over yet (from
  // its current trailing/left edge to the right side of the board). Only
  // the swath to the left of the car stays revealed.
  int revealX = (int)round(jeepX);
  if (revealX < 0) revealX = 0;
  if (revealX > W) revealX = W;
  if (revealX < W) dma_display->fillRect(revealX, 0, W - revealX, H, dma_display->color565(0, 0, 0));

  // jeep sprite drawn last, on top of both the revealed text and the
  // curtain — nearly full board height, 1px margin top and bottom.
  drawJeepSprite((int)round(jeepX), 1);
}

// ============================================================
// Weather — round 86. Real backend data (sources/weather.js's icon
// buckets/current temp/hi-lo/summary/hourly timeline), ported
// pixel-for-pixel from the HUB75 Twin browser simulator
// (claude/hub75-twin.html, rounds 82-85.1) where this whole look was
// designed and approved before ever touching real firmware — every sprite
// row, palette value, layout constant, and timing/pulse number below
// matches that file's renderWeather()/drawWeatherTimeline() exactly.
// ============================================================

// ---- weather icon sprites — 22px wide, height varies by icon ----
// Traced from the Twin's WEATHER_SPRITES tables. Each row is a PROGMEM
// string, one character per pixel column; '.' is transparent (skipped),
// every other character is a palette key resolved by weatherPaletteColor().
const char *const WEATHER_SPRITE_SUN[22] PROGMEM = {
  "......................",
  "......................",
  "......................",
  "..........ll..........",
  "..........ll..........",
  ".....ll..llll..ll.....",
  ".....lllmmmmmmlll.....",
  "......lmmmmmmmml......",
  "......mmmmmmmmmm......",
  ".....lmmmmmmmmmml.....",
  "...lllmmmmmmmmmmlll...",
  "...lllmmmmmmmmddlll...",
  ".....lmmmmmmmdddl.....",
  "......mmmmmmdddd......",
  "......lmmmmddddl......",
  ".....lllmmmdddlll.....",
  ".....ll..llll..ll.....",
  "..........ll..........",
  "..........ll..........",
  "......................",
  "......................",
  "......................",
};
const int WEATHER_SUN_H = 22;

const char *const WEATHER_SPRITE_PARTLY_SUNNY[19] PROGMEM = {
  "......................",
  "...m.....m............",
  "...mm...mm............",
  "....mm.mm.............",
  "m...mllmm...m.........",
  ".mmmllllmmmm..........",
  "..mmllllmmm...........",
  "...mmllddm............",
  "..mmmmdddcccc.........",
  ".mmmmmccccccccc.......",
  "m...mcccccccccccc.....",
  "....mcccccccccccc.....",
  "...ccccccccccccccc....",
  "..cccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  "..ssssssssssssssssss..",
  "...sssssssssssssss....",
  "...sssssssssssssss....",
};
const int WEATHER_PARTLY_SUNNY_H = 19;

const char *const WEATHER_SPRITE_CLOUD[13] PROGMEM = {
  ".........cccc.........",
  "......ccccccccc.......",
  ".....cccccccccccc.....",
  ".....cccccccccccc.....",
  "...ccccccccccccccc....",
  "..cccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  "..ssssssssssssssssss..",
  "...sssssssssssssss....",
  "...sssssssssssssss....",
  "......................",
  "......................",
};
const int WEATHER_CLOUD_H = 13;

const char *const WEATHER_SPRITE_RAIN[18] PROGMEM = {
  ".........cccc.........",
  "......ccccccccc.......",
  ".....cccccccccccc.....",
  ".....cccccccccccc.....",
  "...ccccccccccccccc....",
  "..cccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  "..ssssssssssssssssss..",
  "...sssssssssssssss....",
  "...sssssssssssssss....",
  "......................",
  "....r.........r.......",
  "...r.....r...r....r...",
  "........r........r....",
  "...r.........r........",
  "..r.....r...r....r....",
  ".......r........r.....",
};
const int WEATHER_RAIN_H = 18;

const char *const WEATHER_SPRITE_SNOW[20] PROGMEM = {
  ".........cccc.........",
  "......ccccccccc.......",
  ".....cccccccccccc.....",
  ".....cccccccccccc.....",
  "...ccccccccccccccc....",
  "..cccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  "..ssssssssssssssssss..",
  "...sssssssssssssss....",
  "...sssssssssssssss....",
  "......................",
  "....w.....w.....w.....",
  "...www...www...www....",
  "....w.....w.....w.....",
  "......................",
  ".......n.....n.....n..",
  "......nnn...nnn...nnn.",
  ".......n.....n.....n..",
  "......................",
};
const int WEATHER_SNOW_H = 20;

const char *const WEATHER_SPRITE_LIGHTNING[18] PROGMEM = {
  ".........cccc.........",
  "......ccccccccc.......",
  ".....cccccccccccc.....",
  ".....cccccccccccc.....",
  "...ccccccccccccccc....",
  "..cccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  ".ccccccccccccccccccc..",
  "..ssssssssssssssssss..",
  "...sssssssssssssss....",
  "...sssssssssssssss....",
  "..........ggg.........",
  "...r.....gg........r..",
  "..r.....gg........r...",
  ".........gggg.........",
  ".....r......gg...r....",
  "....r......gg...r.....",
  "..........gg..........",
};
const int WEATHER_LIGHTNING_H = 18;

const int WEATHER_ICON_W = 22; // every icon sprite is 22px wide

// Same nine-key sprite palette as the Twin's WEATHER_PALETTE.
uint16_t weatherPaletteColor(char c) {
  switch (c) {
    case 'd': return dma_display->color565(255, 150, 20);  // sun accent, darker orange
    case 'm': return dma_display->color565(255, 201, 60);  // sun mid tone
    case 'l': return dma_display->color565(255, 205, 70);  // sun core, lightest
    case 's': return dma_display->color565(159, 176, 196); // cloud shadow/underside
    case 'c': return dma_display->color565(223, 235, 244); // cloud body, light
    case 'r': return dma_display->color565(66, 173, 244);  // rain drop, blue
    case 'n': return dma_display->color565(196, 222, 245); // snowflake, pale blue-white
    case 'w': return dma_display->color565(255, 255, 255); // snowflake, white
    case 'g': return dma_display->color565(255, 210, 0);   // lightning bolt, gold
    default:  return 0;
  }
}

void drawWeatherSpriteRows(const char *const *rows, int rowCount, int x0, int y0) {
  for (int row = 0; row < rowCount; row++) {
    // Same PROGMEM convention as JEEP_SPRITE/drawJeepSprite above: on the
    // ESP32's unified address space PROGMEM is just a placement hint, not
    // a separate access method the way it is on AVR, so this is a plain,
    // direct read — no pgm_read_ptr() needed (and this file doesn't use it
    // anywhere else).
    const char *line = rows[row];
    for (int col = 0; col < WEATHER_ICON_W; col++) {
      char ch = line[col];
      if (ch == '.' || ch == '\0') continue;
      dma_display->drawPixel(x0 + col, y0 + row, weatherPaletteColor(ch));
    }
  }
}

// icon bucket -> sprite. Unknown/missing buckets fall back to "cloud",
// same default sources/weather.js's own iconForWmoCode() uses.
int weatherIconHeight(const String &icon) {
  if (icon == "sun") return WEATHER_SUN_H;
  if (icon == "partly_sunny") return WEATHER_PARTLY_SUNNY_H;
  if (icon == "rain") return WEATHER_RAIN_H;
  if (icon == "snow") return WEATHER_SNOW_H;
  if (icon == "lightning") return WEATHER_LIGHTNING_H;
  return WEATHER_CLOUD_H; // cloud, and anything unrecognized
}

void drawWeatherIcon(const String &icon, int x0, int y0) {
  if (icon == "sun") drawWeatherSpriteRows(WEATHER_SPRITE_SUN, WEATHER_SUN_H, x0, y0);
  else if (icon == "partly_sunny") drawWeatherSpriteRows(WEATHER_SPRITE_PARTLY_SUNNY, WEATHER_PARTLY_SUNNY_H, x0, y0);
  else if (icon == "rain") drawWeatherSpriteRows(WEATHER_SPRITE_RAIN, WEATHER_RAIN_H, x0, y0);
  else if (icon == "snow") drawWeatherSpriteRows(WEATHER_SPRITE_SNOW, WEATHER_SNOW_H, x0, y0);
  else if (icon == "lightning") drawWeatherSpriteRows(WEATHER_SPRITE_LIGHTNING, WEATHER_LIGHTNING_H, x0, y0);
  else drawWeatherSpriteRows(WEATHER_SPRITE_CLOUD, WEATHER_CLOUD_H, x0, y0);
}

// A small drawn ring for the degree mark — same "control every pixel"
// approach as the icons above, rather than trusting the default font's
// '°' glyph. fillCircle(r=1) draws a solid 3x3 block; punching the center
// pixel back to black turns it into a hollow ring.
void drawWeatherDegreeMark(int x, int y, uint16_t color) {
  dma_display->fillCircle(x, y, 1, color);
  dma_display->drawPixel(x, y, dma_display->color565(0, 0, 0));
}

// ---- hourly timeline bar (rounds 84-85.1, 88) ----
// Round 88 — Jon: the grey/cloud columns were "way too bright... hard to
// see the other colours," and he wanted the timeline (not the cloud icon
// elsewhere on this screen — that sprite is untouched) raised and made 1px
// taller. TIMELINE_BAR_H going 6->7 does both at once: barY0 below is
// `H - 2 - TIMELINE_BAR_H`, so growing this by 1 raises the bar's top row
// by 1 while its bottom row stays exactly where it always was.
const int TIMELINE_BAR_H = 7;

// One flat color per row (top row first), by icon bucket — ported from
// the Twin's TIMELINE_PATTERNS. row is 0..TIMELINE_BAR_H-1.
uint16_t weatherTimelineRowColor(const String &icon, int row) {
  // Round 88 — Jon: grey nearly unreadable against the other colors, wanted
  // it "almost the dimmest white we can make," and the yellow/blue/red
  // pushed to maximum brightness/saturation so they pop against that much
  // darker grey. GREY keeps its original cool-blue hue ratio, just scaled
  // way down (was 159,176,196 — same proportions, ~18% of the brightness).
  const uint16_t YELLOW = dma_display->color565(255, 220, 0);
  const uint16_t GREY   = dma_display->color565(28, 31, 35);
  const uint16_t BLUE   = dma_display->color565(20, 140, 255);
  const uint16_t WHITE  = dma_display->color565(255, 255, 255);
  const uint16_t RED    = dma_display->color565(255, 0, 0);
  if (icon == "sun") return YELLOW;
  if (icon == "partly_sunny") return row < 3 ? YELLOW : GREY; // half yellow, half grey — round 85.1
  if (icon == "rain") return row < 2 ? GREY : BLUE;
  if (icon == "snow") return row < 2 ? GREY : WHITE;
  if (icon == "lightning") {
    if (row < 2) return GREY;
    if (row == 2) return BLUE;
    return RED; // bottom 3 rows — round 85: "too similar to sunny... not clear its significant"
  }
  return GREY; // cloud, and anything unrecognized
}

// scaleColor565() (see the small-helpers section up top) takes separate
// r/g/b — this unpacks them back out of an already-built color565 value so
// weatherTimelineRowColor() above can stay a single palette function
// instead of two parallel r/g/b-and-color565 versions of the same colors.
uint16_t scaleColor565Packed(uint16_t c, float alpha) {
  uint8_t r = ((c >> 11) & 0x1F) << 3;
  uint8_t g = ((c >> 5) & 0x3F) << 2;
  uint8_t b = (c & 0x1F) << 3;
  return scaleColor565(r, g, b, alpha);
}

// Draws the hourly bars across [x0,x1) at row y0, one flat-color column
// per hour with no gaps, plus a real-time "now" cursor through the bar —
// same ears+stem shape and partial pulse (0.55-1.0, never fully dark) as
// the Events screen's own day-timeline now marker (see renderEvents()
// above), since Jon wants this bar to read as that same kind of timeline.
// alpha<1 fades bar AND cursor together toward black, for the cross-fade
// in/out when this swaps places with the ticker text.
void drawWeatherTimeline(int x0, int x1, int y0, float alpha, unsigned long elapsed) {
  int n = numWeatherHourly > 0 ? numWeatherHourly : 1;
  for (int i = 0; i < n; i++) {
    int bx0 = x0 + (int)round((float)i / n * (x1 - x0));
    int bx1 = x0 + (int)round((float)(i + 1) / n * (x1 - x0));
    String icon = numWeatherHourly > 0 ? weatherHourly[i] : "cloud";
    uint16_t colW = max(1, bx1 - bx0);
    for (int r = 0; r < TIMELINE_BAR_H; r++) {
      uint16_t rowColor = scaleColor565Packed(weatherTimelineRowColor(icon, r), alpha);
      dma_display->fillRect(bx0, y0 + r, colW, 1, rowColor);
    }
  }

  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    int nowMinutes = timeinfo.tm_hour * 60 + timeinfo.tm_min;
    int cursorX = x0 + (int)round(minutesToFrac(nowMinutes) * (x1 - x0));
    float nowPulseAlpha = 0.55f + 0.45f * ((sin(elapsed / 260.0) + 1) / 2.0f);
    // Near-white, slight YELLOW tinge (round 88 — Jon switched this from the
    // round 85.1 blue tinge to yellow; still clearly brighter than the new,
    // much darker TIMELINE_GREY and still distinct from solid-white snow
    // columns so it doesn't blend into either).
    uint16_t cursorColor = scaleColor565(255, 248, 200, nowPulseAlpha * alpha);
    // Round 88 — Jon wanted the bottom overhang to match the top overhang
    // (was 2px above / 1px below; both 2px now) and the cursor tall enough
    // to still clear the raised, now-taller bar by the same 1px it always
    // has. earTop unchanged at 2px above the bar's (new, higher) top row;
    // earBottom now 2px below the bar's bottom row (y0+TIMELINE_BAR_H-1),
    // same distance as earTop's 2px gap above the bar's top row.
    int earTop = y0 - 2, earBottom = y0 + TIMELINE_BAR_H + 1;
    dma_display->fillRect(cursorX - 1, earTop, 3, 1, cursorColor);
    dma_display->fillRect(cursorX, earTop + 1, 1, earBottom - earTop - 1, cursorColor);
    dma_display->fillRect(cursorX - 1, earBottom, 3, 1, cursorColor);
  }
}

// ---- bottom-band timing (round 85.1) ----
// Text holds 3s, scrolls (if it needs to) to exactly flush against the
// right edge — no extra travel past full visibility — holds another 3s,
// fades to the timeline (10s hold), fades back to text. weatherTextPhaseMs
// ()/weatherTotalCycleMs() are shared between renderWeather() and
// weatherRequiredTime() below so the rotation timer and the actual
// on-screen animation can never drift out of sync — same discipline
// newsRequiredTime()/buildNewsJoined() use above.
const float WEATHER_SCROLL_SPEED_PX_MS = 0.02f;
const unsigned long WEATHER_HOLD_MS = 3000, WEATHER_FADE_MS = 500, WEATHER_TIMELINE_HOLD_MS = 10000;

unsigned long weatherTextPhaseMs(const String &summaryUpper) {
  int sumW = summaryUpper.length() * 6;
  int availW = (W - 2) - 2; // matches renderWeather()'s WX_TICKER_X0=2, WX_TICKER_X1=W-2
  if (sumW <= availW) return WEATHER_HOLD_MS; // sits still, no scroll needed
  int scrollPx = sumW - availW;
  return (unsigned long)(WEATHER_HOLD_MS * 2 + scrollPx / WEATHER_SCROLL_SPEED_PX_MS);
}

unsigned long weatherTotalCycleMs(const String &summaryUpper) {
  return weatherTextPhaseMs(summaryUpper) + WEATHER_FADE_MS + WEATHER_TIMELINE_HOLD_MS + WEATHER_FADE_MS;
}

// How long the Weather screen needs to stay up to show one full cycle —
// the AI summary AND the hourly timeline bar — before rotating away. Same
// "don't cut it off mid-cycle" reasoning as newsRequiredTime()/
// holdingsRequiredTime()/eventsRequiredTime() above.
unsigned long weatherRequiredTime() {
  if (!hasWeather) return 0;
  String summaryUpper = weatherSummary; summaryUpper.toUpperCase();
  return weatherTotalCycleMs(summaryUpper);
}

// ---- temperature-to-color gradient (round 89) ----
// Jon: "-30 is the most frigid, darkest blue you can think of... zero is
// white... progressively more yellowish, then orange, then darker red at
// +30." Two gradients meeting at white, each a straight RGB interpolation
// between control points (matches this file's "rules decide" convention —
// no HSV math, just a lookup a person could sanity-check by eye): cold
// half blends a deep navy up to white as tempC rises from -30 to 0; warm
// half runs white -> pale yellow -> orange -> a deeper (not neon) red
// across three sub-segments from 0 to +30. Clamped at both ends so
// anything colder/hotter than +/-30 still reads as the most extreme color
// instead of extrapolating past it.
uint16_t lerpColor565(uint8_t r0, uint8_t g0, uint8_t b0, uint8_t r1, uint8_t g1, uint8_t b1, float t) {
  if (t < 0) t = 0; if (t > 1) t = 1;
  uint8_t r = r0 + (int)round((r1 - r0) * t);
  uint8_t g = g0 + (int)round((g1 - g0) * t);
  uint8_t b = b0 + (int)round((b1 - b0) * t);
  return dma_display->color565(r, g, b);
}

uint16_t tempToColor565(int tempC) {
  if (tempC <= 0) {
    float f = (tempC + 30.0f) / 30.0f; // -30 -> 0.0, 0 -> 1.0
    return lerpColor565(20, 40, 160, 255, 255, 255, f); // deep navy -> white
  }
  float f = tempC / 30.0f; // 0 -> 0.0, +30 -> 1.0
  if (f <= 0.5f) return lerpColor565(255, 255, 255, 255, 210, 90, f / 0.5f);        // white -> pale yellow
  if (f <= 0.8f) return lerpColor565(255, 210, 90, 255, 120, 20, (f - 0.5f) / 0.3f); // pale yellow -> orange
  return lerpColor565(255, 120, 20, 170, 20, 20, (f - 0.8f) / 0.2f);                 // orange -> darker red
}

void renderWeather(unsigned long elapsed) {
  dma_display->clearScreen();

  const int WX_TOP_Y0 = 0, WX_TOP_Y1 = 22;                             // top band: icon + temp/hi-lo
  // WX_TICKER_Y nudged up 1px (round 87 — Jon: the ticker text's bottom row
  // was landing exactly on the panel's last row, flush against the physical
  // edge; this leaves 1px of margin below it, same reasoning as the other
  // "give it room to breathe" nudges already in this file).
  const int WX_TICKER_X0 = 2, WX_TICKER_X1 = W - 2, WX_TICKER_Y = 23;  // ticker: full width, bottom band

  String icon = weatherIcon;
  int iconH = weatherIconHeight(icon);
  const int iconW = WEATHER_ICON_W;

  int tempSize = 2, tempH = 7 * tempSize;
  String tempText = String(weatherTempC);
  String hiText = "H:" + String(weatherHighC), loText = "L:" + String(weatherLowC);
  const int hiloGapRows = 2; // Jon: nudge L down 1px further from H
  const int hiloStackH = 7 + hiloGapRows + 7;
  int tempBlockW = (int)tempText.length() * 6 * tempSize + 6; // text + gap + degree dot
  const int iconGapPx = 8, hiloGapPx = 6; // icon->temp, temp->hi-lo
  int hiloW = max((int)hiText.length(), (int)loText.length()) * 6;

  // ---- the whole trio (icon, temp, hi-lo) centered as one block ----
  int groupW = iconW + iconGapPx + tempBlockW + hiloGapPx + hiloW;
  float groupHalfGap = ((WX_TICKER_X1 - WX_TICKER_X0) - groupW) / 2.0f;
  int groupX0 = WX_TICKER_X0 + max(0, (int)round(groupHalfGap));
  float topCenterY = (WX_TOP_Y0 + WX_TOP_Y1) / 2.0f;

  // icon, nudged up 1px for breathing room from the ticker below — only
  // where it actually has that pixel of headroom to give (sun, the
  // tallest icon at h=22, already fills the band exactly).
  int iconX = groupX0;
  int iconYCentered = (int)round(topCenterY - iconH / 2.0f);
  int iconY = max(WX_TOP_Y0, iconYCentered - 1);
  drawWeatherIcon(icon, iconX, iconY);

  // temp, vertically centered on the same axis as the icon
  int tempX = iconX + iconW + iconGapPx;
  int tempY = (int)round(topCenterY - tempH / 2.0f);
  dma_display->setTextSize(tempSize);
  dma_display->setTextColor(tempToColor565(weatherTempC)); // round 89: temp-gradient color
  dma_display->setCursor(tempX, tempY);
  dma_display->print(tempText);
  // Degree mark: pulses at the portfolio LIVE dot's own rate (elapsed/260),
  // but — round 87 — never truly off. It used to be a complete 0-1 sweep
  // (same style as the LIVE dot), which read as "fading out completely";
  // Jon wanted it to keep breathing without going fully dark, so this uses
  // the same 0.55-floor sweep as the timeline's own now-cursor pulse
  // instead of a 0-to-1 one.
  float degreePulse = 0.55f + 0.45f * ((sin(elapsed / 260.0) + 1) / 2.0f);
  drawWeatherDegreeMark(tempX + (int)tempText.length() * 6 * tempSize, tempY + 2,
                        scaleColor565(255, 255, 255, degreePulse));

  // hi-lo, stacked H over L, also centered on that same axis. Round 89 —
  // Jon: "H:" and "L:" stay white always; only the number after them takes
  // the temp-gradient color (e.g. a high of 18 prints "H:" white then "18"
  // in that gradient's orange) — so each line is now two print() calls
  // instead of one, with the label's fixed 2-char width ("H:"/"L:" at text
  // size 1) used to place the number right after it.
  int hiloX = tempX + tempBlockW + hiloGapPx;
  int hiloY0 = (int)round(topCenterY - hiloStackH / 2.0f);
  const int hiloLabelW = 2 * 6; // "H:" / "L:" at text size 1
  String hiNumStr = String(weatherHighC), loNumStr = String(weatherLowC);
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(255, 255, 255));
  dma_display->setCursor(hiloX, hiloY0);
  dma_display->print("H:");
  dma_display->setTextColor(tempToColor565(weatherHighC));
  dma_display->setCursor(hiloX + hiloLabelW, hiloY0);
  dma_display->print(hiNumStr);
  dma_display->setTextColor(dma_display->color565(255, 255, 255));
  dma_display->setCursor(hiloX, hiloY0 + 7 + hiloGapRows);
  dma_display->print("L:");
  dma_display->setTextColor(tempToColor565(weatherLowC));
  dma_display->setCursor(hiloX + hiloLabelW, hiloY0 + 7 + hiloGapRows);
  dma_display->print(loNumStr);

  // ---- bottom band: summary ticker alternating with the hourly timeline bar ----
  String summary = weatherSummary; summary.toUpperCase();
  int availW = WX_TICKER_X1 - WX_TICKER_X0;
  int sumW = (int)summary.length() * 6;
  bool needsScroll = sumW > availW;
  int scrollPx = sumW - availW;
  unsigned long textPhaseMs = weatherTextPhaseMs(summary);
  unsigned long totalCycle = weatherTotalCycleMs(summary);
  unsigned long t = elapsed % totalCycle;
  int barY0 = H - 2 - TIMELINE_BAR_H; // bar moved up off the very bottom edge so the cursor's ears fit

  if (t < textPhaseMs) {
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(200, 200, 200));
    if (!needsScroll) {
      dma_display->setCursor(centerTextX(summary, 6), WX_TICKER_Y);
      dma_display->print(summary);
    } else {
      float x;
      if (t < WEATHER_HOLD_MS) x = WX_TICKER_X0;
      else if (t < WEATHER_HOLD_MS + scrollPx / WEATHER_SCROLL_SPEED_PX_MS) x = WX_TICKER_X0 - (t - WEATHER_HOLD_MS) * WEATHER_SCROLL_SPEED_PX_MS;
      else x = WX_TICKER_X0 - scrollPx;
      dma_display->setCursor((int)round(x), WX_TICKER_Y);
      dma_display->print(summary);
    }
  } else if (t < textPhaseMs + WEATHER_FADE_MS) {
    drawWeatherTimeline(WX_TICKER_X0, WX_TICKER_X1, barY0, (float)(t - textPhaseMs) / WEATHER_FADE_MS, elapsed);
  } else if (t < textPhaseMs + WEATHER_FADE_MS + WEATHER_TIMELINE_HOLD_MS) {
    drawWeatherTimeline(WX_TICKER_X0, WX_TICKER_X1, barY0, 1.0f, elapsed);
  } else {
    unsigned long fadeOutT = t - (textPhaseMs + WEATHER_FADE_MS + WEATHER_TIMELINE_HOLD_MS);
    drawWeatherTimeline(WX_TICKER_X0, WX_TICKER_X1, barY0, 1.0f - (float)fadeOutT / WEATHER_FADE_MS, elapsed);
  }
}

void renderComingSoonFwd(String id, unsigned long now) {
  dma_display->clearScreen();
  String label = id; label.toUpperCase();
  uint16_t hue = (now / 10) % 360;
  dma_display->setTextSize(2);
  dma_display->setTextColor(hsvToColor565(hue, 255, 255));
  dma_display->setCursor(centerTextX(label, 12), 4);
  dma_display->print(label);
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(180, 180, 180));
  String sub = "COMING SOON";
  dma_display->setCursor(centerTextX(sub, 6), 22);
  dma_display->print(sub);
}

// Derives a real reason from actual state (never a hardcoded guess) —
// WiFi down, no data ever received, or data gone stale, each get their own
// message, same idea as the simulator's editable offlineReason but backed
// by what's actually true.
// Round 79 — mm:ss (or h:mm once it's been an hour) for renderOffline()'s
// "how long has this actually been going on" indicator. Plain ASCII only.
String formatOfflineDuration(unsigned long ms) {
  unsigned long s = ms / 1000;
  if (s < 60) return String(s) + "S";
  unsigned long m = s / 60;
  s %= 60;
  if (m < 60) return String(m) + "M" + String(s) + "S";
  unsigned long h = m / 60;
  m %= 60;
  return String(h) + "H" + String(m) + "M";
}

void renderOffline(unsigned long now) {
  dma_display->clearScreen();
  uint8_t pulse = (uint8_t)(128 + 127 * sin(now / 300.0));
  drawRectOutlineColor(0, 0, W, H, dma_display->color565(pulse, 0, 0));

  // Round 79 — Jon: "error messages that are useful to me with
  // actionable steps or progress indicators." Naming the problem alone
  // didn't say whether it just started or has been stuck for 20 minutes
  // — appended here as "REASON - Xm Ys", reusing lastWifiConnectedTime /
  // lastDataSuccessTime (both already tracked for other reasons) rather
  // than adding new bookkeeping.
  String reason;
  unsigned long since;
  if (WiFi.status() != WL_CONNECTED) {
    reason = "WIFI DISCONNECTED";
    since = now - lastWifiConnectedTime;
  } else if (!dataValid) {
    reason = "NO DATA YET";
    since = now - lastDataSuccessTime;
  } else {
    reason = "CAN'T REACH PI";
    since = now - lastDataSuccessTime;
  }
  reason += " - " + formatOfflineDuration(since);

  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(255, 150, 150));
  dma_display->setCursor(centerTextX(reason, 6), 3);
  dma_display->print(reason);

  String label = "RETRYING " + String("|/-\\"[(now / 200) % 4]);
  dma_display->setTextSize(2);
  dma_display->setTextColor(dma_display->color565(255, 80, 80));
  dma_display->setCursor(centerTextX(label, 12), 13);
  dma_display->print(label);
}

// Notification: a rainbow-bordered overlay, text built as a single-line
// strip (plain text runs + big emoji icons) — if it fits, center it
// statically, otherwise scroll. Text never shrinks or wraps.
const int NOTIF_ICON_SIZE = 26, NOTIF_ICON_Y = (H - 26) / 2, NOTIF_TEXT_Y = 13, NOTIF_GAP = 8;
#define MAX_NOTIF_CMDS 40
int buildNotifStrip(const String &text, NotifCmd *cmds, int maxCmds) {
  int n = 0, x = 0;
  String buffer = "";
  auto flush = [&]() {
    if (buffer.length() > 0 && n < maxCmds) {
      cmds[n++] = { false, x, 0, buffer, 0 };
      x += (int)buffer.length() * 6 + NOTIF_GAP;
      buffer = "";
    }
  };
  int i = 0;
  int len = text.length();
  while (i < len) {
    int cpLen;
    uint32_t cp = utf8Decode(text, i, &cpLen);
    int iconW = drawIconForCodepoint(cp, -1000, -1000, NOTIF_ICON_SIZE); // probe width without drawing visibly... see note below
    // NOTE: drawIconForCodepoint always draws when it recognizes an icon —
    // there's no pure "measure" path. Instead, recognize icons by
    // codepoint match here (same switch), and let the actual render pass
    // call drawIconForCodepoint for real at the right x.
    bool isIcon = (cp == 0x2B50 || cp == 0x2764 || cp == 0x2705 || cp == 0x26A0 ||
                   cp == 0xE001 || cp == 0xE002 || cp == 0x1F600 || cp == 0x1F641 ||
                   cp == 0x1F622 || cp == 0x1F610);
    if (isIcon) {
      flush();
      int w = (cp == 0x26A0 || cp == 0xE001 || cp == 0xE002) ? (int)round(NOTIF_ICON_SIZE * 1.3f) : NOTIF_ICON_SIZE;
      if (n < maxCmds) { cmds[n++] = { true, x, w, "", cp }; x += w + NOTIF_GAP; }
    } else {
      buffer += text[i];
    }
    i += cpLen;
  }
  flush();
  return n;
}

void drawNotifCmd(const NotifCmd &cmd, float sx) {
  int x = (int)round(sx);
  if (cmd.isIcon) {
    if (x > -60 && x < W + 10) drawIconForCodepoint(cmd.cp, x, NOTIF_ICON_Y, NOTIF_ICON_SIZE);
  } else {
    if (x > -200 && x < W + 50) {
      dma_display->setTextSize(1);
      dma_display->setTextColor(dma_display->color565(255, 255, 255));
      dma_display->setCursor(x, NOTIF_TEXT_Y);
      dma_display->print(cmd.text);
    }
  }
}

void renderNotification(unsigned long now) {
  dma_display->clearScreen();
  drawAnimatedBorder(now);
  unsigned long elapsedNotif = now - notifyStart;
  NotifCmd cmds[MAX_NOTIF_CMDS];
  int n = buildNotifStrip(notificationText, cmds, MAX_NOTIF_CMDS);
  int totalWidth = 0;
  for (int i = 0; i < n; i++) totalWidth = max(totalWidth, cmds[i].x + cmds[i].w);
  totalWidth = max(0, totalWidth - NOTIF_GAP);

  if (totalWidth <= W - 6) {
    int offsetX = (W - totalWidth) / 2;
    for (int i = 0; i < n; i++) drawNotifCmd(cmds[i], cmds[i].x + offsetX);
  } else {
    const float speedPxPerMs = 0.045f;
    const int loopGap = 40;
    int totalW = totalWidth + loopGap;
    float offset = fmod(elapsedNotif * speedPxPerMs, (float)totalW);
    for (int i = 0; i < n; i++) drawNotifCmd(cmds[i], cmds[i].x - offset);
    for (int i = 0; i < n; i++) drawNotifCmd(cmds[i], cmds[i].x - offset + totalW);
  }
}

// ---- Alerts page — a thick hazard-stripe border that marches like a
// construction barrier, color-coded by severity, plus a flashing warning
// triangle and a short message. Fully implemented and wired to
// /api/matrix/command's optional "alert" field — dormant until the backend
// adds it (see the field declaration above and the handoff doc).
AlertLevel alertLevelFor(const String &severity) {
  if (severity == "low")  return { 255, 220, 0,  255, 255, 255,  255, 210, 0 };   // yellow & white
  if (severity == "high") return { 255, 30, 20,  255, 140, 0,    255, 30, 20 };   // red & orange
  return { 255, 255, 255, 255, 140, 0, 255, 140, 0 };                             // medium: white & orange
}

void drawHazardBorder(unsigned long t, int thick, uint16_t colorA, uint16_t colorB, int stripeSize) {
  long shift = t / 40;
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) {
      if (x >= thick && x < W - thick && y >= thick && y < H - thick) continue;
      long stripe = ((x + y + shift) / stripeSize) % 2;
      dma_display->drawPixel(x, y, stripe == 0 ? colorA : colorB);
    }
  }
}

void wrapTwoLines(const String &text, int maxChars, String *l1, String *l2) {
  String upper = text; upper.toUpperCase();
  *l1 = ""; *l2 = "";
  int start = 0;
  while (start < (int)upper.length()) {
    int sp = upper.indexOf(' ', start);
    String word = (sp < 0) ? upper.substring(start) : upper.substring(start, sp);
    String *target = (l2->length() == 0 && (l1->length() == 0 || (int)(*l1 + " " + word).length() <= maxChars)) ? l1 : l2;
    if (target == l1) *l1 = (l1->length() == 0) ? word : (*l1 + " " + word);
    else if ((int)(l2->length() == 0 ? word.length() : (*l2 + " " + word).length()) <= maxChars) *l2 = (l2->length() == 0) ? word : (*l2 + " " + word);
    if (sp < 0) break;
    start = sp + 1;
  }
}

void renderAlert(unsigned long t) {
  dma_display->clearScreen();
  AlertLevel lvl = alertLevelFor(alertSeverity);
  uint16_t colorA = dma_display->color565(lvl.ar, lvl.ag, lvl.ab);
  uint16_t colorB = dma_display->color565(lvl.br, lvl.bg, lvl.bb);
  drawHazardBorder(t, 2, colorA, colorB, 6);

  int triW = 30, triH = 26;
  int triX = W - triW - 6, triY = (H - triH) / 2;
  bool blinkOn = ((t / 350) % 2) == 0;
  if (blinkOn) iconWarning(triX, triY, triW, triH, dma_display->color565(lvl.tr, lvl.tg, lvl.tb));

  int regionX0 = 5, regionX1 = triX - 4;
  int regionW = regionX1 - regionX0;
  int maxChars = max(6, regionW / 6);
  String l1, l2;
  wrapTwoLines(alertText, maxChars, &l1, &l2);
  int lineY1 = l2.length() > 0 ? 8 : 12;
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(255, 255, 255));
  int l1x = regionX0 + max(0, (regionW - (int)l1.length() * 6) / 2);
  dma_display->setCursor(l1x, lineY1);
  dma_display->print(l1);
  if (l2.length() > 0) {
    int l2x = regionX0 + max(0, (regionW - (int)l2.length() * 6) / 2);
    dma_display->setCursor(l2x, lineY1 + 9);
    dma_display->print(l2);
  }
}

void renderScreen(String id, unsigned long screenElapsed, unsigned long now) {
  if (id == "portfolio") renderPortfolio(now);
  else if (id == "events") renderEvents(screenElapsed);
  else if (id == "holdings") renderHoldingsTicker(screenElapsed);
  else if (id == "markets") renderMarketsTicker(screenElapsed);
  else if (id == "news") renderNews(screenElapsed);
  else if (id == "clock") renderClock();
  else if (id == "dayoverview") renderDayOverview();
  // Round 91 — Jon: "can we also hide the comuting behind a coming soon
  // page. that car annoys me without real information behind it." Same
  // hasX-gated pattern as weather/wakeup: the animated jeep only ever had
  // a real ETA to show once dayOverview.hasCommute is true (there's still
  // no real commuteMin source today — see the DayOverviewData comment
  // above), so until then this is just the plain "COMING SOON" card, same
  // as any other no-data-yet screen. Nothing about renderCommuting() itself
  // changed — it picks back up automatically the day commuteMin is real.
  else if (id == "commuting") { if (dayOverview.hasCommute) renderCommuting(screenElapsed); else renderComingSoonFwd(id, now); }
  else if (id == "stars") renderStars(screenElapsed);
  else if (id == "balls") renderBalls(screenElapsed);
  else if (id == "alerts") renderAlert(now);
  else if (id == "offline") renderOffline(now); // preview only — see loop()'s own offline check for the real thing
  else if (id == "weather") { if (hasWeather) renderWeather(screenElapsed); else renderComingSoonFwd(id, now); }
  // Round 91 — Sleep & Alarm renderer is self-gating (renderSleepAlarm()
  // shows its own "coming soon" card when sleepAlarm.hasData is false), so
  // no hasWeather-style check needed here.
  else if (id == "sleep") renderSleepAlarm(now);
  // Round 91 — Wake Up Mode is a real, fully animated renderer now (Jon:
  // "Im confident you can code the animation, id really like to see it on
  // the pi"). screenElapsed drives it (not `now`/millis()) so the sunrise
  // intro always restarts from black the moment this screen is entered
  // (pinned or pushed), the same way every other elapsed-driven screen
  // here already works.
  else if (id == "wakeup") renderWakeUp(screenElapsed);
  else renderComingSoonFwd(id, now); // markets, and anything unrecognized
}

// ============================================================
// Setup / loop
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("=== ESP32 LED WALL — Orange Pi Secretary ===");

  matrixUrl  = String("http://") + PI_HOST + ":" + String(PI_PORT) + "/api/matrix";
  commandUrl = String("http://") + PI_HOST + ":" + String(PI_PORT) + "/api/matrix/command";
  Serial.println("Data endpoint:    " + matrixUrl);
  Serial.println("Command endpoint: " + commandUrl);

  HUB75_I2S_CFG::i2s_pins pins = {
    R1_PIN, G1_PIN, B1_PIN, R2_PIN, G2_PIN, B2_PIN,
    A_PIN, B_PIN, C_PIN, D_PIN, E_PIN,
    LAT_PIN, OE_PIN, CLK_PIN
  };
  HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN, pins);

  // Round 59 pushed this to HZ_15M + min_refresh_rate 120, reasoning that
  // the SEENGREAT adapter's 74HCT245 level shifting removed the signal-
  // integrity problem that justified the slow clock. That was wrong scope:
  // round 56's note was specifically that garbling "gets worse further
  // down a jumper-wired panel chain" — i.e. the panel-to-panel wiring
  // between the 3 chained panels, not the ESP-to-adapter connection. The
  // SEENGREAT board only upgrades the latter; the inter-panel chain is
  // exactly the same wiring it always was, and it's shared by the same
  // I2S clock. Pushing that clock to 15M (and demanding 120Hz worth of
  // bit-plane throughput on top of it) re-triggered the original
  // bottleneck — reported round 60 as everything loading slow, animations
  // glitching. Reverted both back to the round-56 confirmed-good values.
  // Real "fast refresh" is available once the inter-panel cabling gets the
  // same level-shifting/quality upgrade the ESP-to-panel-1 link just did —
  // worth revisiting i2sspeed then, one step at a time (HZ_10M first).
  mxconfig.i2sspeed = HUB75_I2S_CFG::HZ_8M;
  mxconfig.min_refresh_rate = 60;

  // Double buffering — draw the next frame into an off-screen buffer and
  // flip it into place atomically once fully drawn (see flipDMABuffer() at
  // the end of loop(), and inside renderBootFrame()/drawStatusScreen() for
  // the two screens drawn outside the main loop). Fixes the tearing/
  // "seeping through" artifact on the Commuting page's reveal curtain.
  // This is a different lever from i2sspeed above (it's about frame
  // atomicity, not clock speed/wiring), so it stayed on through the round
  // 60 clock-speed revert. If the round-60 fix doesn't fully clear up the
  // slowness/glitching, this is the next thing to try setting back to
  // false — just remember the flipDMABuffer() calls become harmless no-ops
  // if you do, no need to remove them too.
  mxconfig.double_buff = true;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);

  // Round 79 — see the comment above this section in the round-79
  // handoff doc for the full story. 4 attempts, ~1.2s worst case, before
  // giving up and restarting the board.
  bool displayReady = dma_display->begin();
  for (int attempt = 1; !displayReady && attempt < 4; attempt++) {
    Serial.printf("Display init failed (attempt %d/4) — retrying...\n", attempt);
    delay(400);
    displayReady = dma_display->begin();
  }
  if (!displayReady) {
    Serial.println("Display init failed after 4 attempts — restarting board");
    delay(500);
    ESP.restart();
  }

  dma_display->setBrightness8(90);
  dma_display->clearScreen();
  dma_display->setTextWrap(false);
  randomSeed(analogRead(0));

  runBootSplash();

  bool wifiOk = connectWiFi();
  if (wifiOk) {
    setupClock();
    runInitialDataFetch();
  } else {
    Serial.println("Proceeding without WiFi for now — will keep retrying in the background");
  }

  lastDataFetchAttempt = millis();
  lastCommandFetchAttempt = millis();
  lastFrameTime = millis();
  forceScreenStart = millis();
  Serial.println("Setup complete — entering main loop");
}

void loop() {
  unsigned long now = millis();

  maintainWifi(now);

  // Re-attempt the NTP sync once WiFi comes up, if it hasn't landed yet
  // (e.g. WiFi wasn't connected at boot, or the first attempt timed out).
  static bool clockAttempted = false;
  if (WiFi.status() == WL_CONNECTED && !timeSynced && !clockAttempted) {
    clockAttempted = true;
    setupClock();
  }
  if (WiFi.status() != WL_CONNECTED) clockAttempted = false;

  if (now - lastDataFetchAttempt > DATA_POLL_MS) {
    lastDataFetchAttempt = now;
    if (WiFi.status() == WL_CONNECTED) pollData();
  }

  if (now - lastCommandFetchAttempt > COMMAND_POLL_MS) {
    lastCommandFetchAttempt = now;
    if (WiFi.status() == WL_CONNECTED) pollCommand();
  }

  if (now - lastFrameTime < FRAME_INTERVAL_MS) return;
  lastFrameTime = now;

  if (strlen(FORCE_SCREEN) > 0) {
    renderScreen(String(FORCE_SCREEN), now - forceScreenStart, now);
    dma_display->flipDMABuffer();
    return;
  }

  bool offline = !dataValid || (now - lastDataSuccessTime > STALE_THRESHOLD_MS);
  if (offline) {
    renderOffline(now);
    dma_display->flipDMABuffer();
    return;
  }

  if (alertActive) {
    renderAlert(now);
    dma_display->flipDMABuffer();
    return;
  }

  if (notificationActive) {
    renderNotification(now);
    dma_display->flipDMABuffer();
    return;
  }

  String activeScreens[MAX_SCREENS];
  int activeCount = getActiveScreens(activeScreens);

  String targetId;
  if (pushedScreenId.length() > 0) {
    // Round 75 — "push a page": jump here for the backend's short window
    // without disturbing pinnedScreen or rotation underneath it.
    targetId = pushedScreenId;
  } else if (pinnedScreen.length() > 0) {
    targetId = pinnedScreen;
  } else {
    int idx = (now / ROTATION_MS) % activeCount;
    targetId = activeScreens[idx];
    // Keep "news" up past its normal rotation slot until the headline
    // ticker has scrolled through at least once — otherwise a long
    // headline set gets cut off mid-scroll by the fixed rotation timer
    // (round 59). newsRequiredTime() returns 0 if the current headlines
    // already fit statically (nothing to scroll), so short news is
    // unaffected.
    if (currentScreenId == "news" && targetId != "news") {
      unsigned long needed = max((unsigned long)ROTATION_MS, newsRequiredTime());
      if (now - currentScreenStart < needed) targetId = "news";
    }
    // Same idea for Holdings — a full 3-up/3-down strip takes well over
    // the fixed 12s rotation slot to scroll through once (round 61).
    if (currentScreenId == "holdings" && targetId != "holdings") {
      unsigned long needed = max((unsigned long)ROTATION_MS, holdingsRequiredTime());
      if (now - currentScreenStart < needed) targetId = "holdings";
    }
    // Markets ticker — same extension as Holdings above (round 74).
    if (currentScreenId == "markets" && targetId != "markets") {
      unsigned long needed = max((unsigned long)ROTATION_MS, marketsRequiredTime());
      if (now - currentScreenStart < needed) targetId = "markets";
    }
    // Events: round 73 added a bare ROTATION_MS floor after it was observed
    // flashing for under a second ("lasts for about one second on
    // screen"). Round 74 replaces that floor with a real extension, same
    // shape as News/Holdings above — eventsRequiredTime() is the actual
    // time needed to cycle through every event at least once (Jon: "if
    // there's ten events, we gotta go through all of them... before this
    // page disappears"), not just a fixed one-slot minimum. max() with
    // ROTATION_MS keeps round 73's floor behavior intact for 0/1-event
    // days where eventsRequiredTime() is shorter than a full slot.
    if (currentScreenId == "events" && targetId != "events") {
      unsigned long needed = max((unsigned long)ROTATION_MS, eventsRequiredTime());
      if (now - currentScreenStart < needed) targetId = "events";
    }
    // Weather — round 86, same "don't cut it off mid-cycle" extension as
    // News/Holdings/Markets/Events above: the Weather cycle covers both
    // the AI summary (which can itself need to scroll) and the hourly
    // timeline bar, and easily runs longer than one fixed rotation slot.
    if (currentScreenId == "weather" && targetId != "weather") {
      unsigned long needed = max((unsigned long)ROTATION_MS, weatherRequiredTime());
      if (now - currentScreenStart < needed) targetId = "weather";
    }
  }

  if (targetId != currentScreenId) {
    Serial.printf("Now showing: %s\n", targetId.c_str());
    currentScreenId = targetId;
    currentScreenStart = now;
  }

  renderScreen(currentScreenId, now - currentScreenStart, now);
  // Double-buffered above (mxconfig.double_buff, see setup()) — this flip
  // is what makes every frame atomic. Without it the DMA engine can scan
  // out a frame while the CPU is mid-draw (e.g. text already drawn but the
  // Commuting curtain/jeep not yet painted over it), which is exactly the
  // "seeping through" tearing seen once refresh rate went up (round 59).
  dma_display->flipDMABuffer();
}
