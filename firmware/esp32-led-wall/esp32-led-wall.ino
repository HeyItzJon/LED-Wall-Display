// ============================================================
// ESP32-S3 LED WALL — Orange Pi Secretary live display
// ============================================================
// Full rewrite ported from the "HUB75 Twin" browser simulator
// (hub75-twin.html) once its look was settled there. Polls two
// endpoints on the Orange Pi backend:
//   GET http://<PI_HOST>:<PI_PORT>/api/matrix          (real data, every 30s)
//   GET http://<PI_HOST>:<PI_PORT>/api/matrix/command   (live control, every 1.5s)
//
// Screens: portfolio, events, holdings render real data today. markets,
// news and weather don't have real renderers/data on the backend yet —
// they show a "COMING SOON" card (news upgrades itself automatically to a
// real scrolling ticker the moment /api/matrix ever starts sending a
// non-empty "news" field — no firmware change needed for that). clock,
// dayoverview and commuting are firmware-local screens the backend's
// screen catalog doesn't know about yet, so they always ride along in the
// rotation regardless of what the web Wall tab has enabled/disabled — see
// getActiveScreens(). Auto-rotation includes every one of the above (per
// Jon's explicit ask: "every menu is included, including the ones coming
// soon"). "stars"/"balls" are ambient demo effects, not data screens —
// they're fully implemented but intentionally left out of normal rotation;
// see FORCE_SCREEN below to bench-test them.
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

// Day Overview extra fields — NOT sent by the backend yet (only
// dailyBusyPercent and the event count are real today). Never fabricated:
// hasHours/hasCommute gate whether renderDayOverview()/renderCommuting()
// draw these numbers at all. See handoff doc.
struct DayOverviewData {
  bool hasHours = false;
  float hoursBusy = 0, hoursFree = 0;
  bool hasCommute = false;
  int commuteMin = 0;
};
DayOverviewData dayOverview;

bool dataValid = false;
unsigned long lastDataSuccessTime = 0;

// ---- Data model: /api/matrix/command ----
#define MAX_SCREENS 12
String enabledScreens[MAX_SCREENS];
int numEnabledScreens = 0;
String pinnedScreen = "";
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
void drawStatusScreen(String title, String subtitle, unsigned long t, uint16_t accentColor) {
  dma_display->clearScreen();

  dma_display->setTextSize(1);
  dma_display->setTextColor(accentColor);
  dma_display->setCursor(centerTextX(title, 6), 4);
  dma_display->print(title);

  dma_display->setTextColor(dma_display->color565(150, 150, 150));
  dma_display->setCursor(centerTextX(subtitle, 6), 14);
  dma_display->print(subtitle);

  const char spin[4] = {'|', '/', '-', '\\'};
  char sc = spin[(t / 150) % 4];
  dma_display->setTextColor(accentColor);
  dma_display->setCursor(W / 2 - 3, 23);
  dma_display->print(sc);

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
    drawStatusScreen("WIFI CONNECTED", WiFi.localIP().toString(), millis(), dma_display->color565(0, 255, 120));
    delay(900);
    return true;
  }

  Serial.println("WiFi connect timed out — will keep retrying in the background");
  return false;
}

// Non-blocking reconnect attempt, called every loop() while WiFi is down.
void maintainWifi(unsigned long now) {
  static unsigned long lastAttempt = 0;
  if (WiFi.status() != WL_CONNECTED && now - lastAttempt > 10000) {
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
    DynamicJsonDocument doc(10240);
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

      // Day Overview extras — not sent by the backend yet at all.
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

      Serial.printf("Data OK — total $%.2f, %d events, %d holdings, %d news, %d markets\n",
                    portfolioTotal, numEvents, numHoldings, numNews, numMarkets);
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
    // Default rotation if /api/matrix/command has never answered yet.
    // Includes every screen this firmware knows how to render, per Jon's
    // explicit ask — "every menu is included, including the ones coming
    // soon" — not just the ones with real data.
    const char *DEFAULTS[] = { "portfolio", "events", "holdings", "markets", "news", "weather" };
    for (int i = 0; i < 6 && n < MAX_SCREENS; i++) out[n++] = String(DEFAULTS[i]);
  }
  // clock/dayoverview/commuting are firmware-local screens the backend's
  // screen catalog has no id for yet, so they aren't toggleable from the
  // web Wall tab — they always ride along in the rotation regardless of
  // what enabledScreens says. See handoff doc if/when the backend adds ids
  // for these and this should become a real toggle instead.
  const char *LOCAL_ONLY[] = { "clock", "dayoverview", "commuting" };
  for (int i = 0; i < 3; i++) {
    bool already = false;
    for (int j = 0; j < n; j++) if (out[j] == LOCAL_ONLY[i]) { already = true; break; }
    if (!already && n < MAX_SCREENS) out[n++] = String(LOCAL_ONLY[i]);
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
  if (cal == "family")      return dma_display->color565(38, 45, 221);  // css #4a4fc0
  if (cal == "deadline")    return dma_display->color565(193, 44, 0);   // css #a83c1c
  if (cal == "class")       return dma_display->color565(0, 191, 63);   // css #1f7a3d
  if (cal == "admin")       return dma_display->color565(85, 137, 191); // css #40566d
  if (cal == "appointment") return dma_display->color565(10, 152, 191); // css #276f83
  if (cal == "gmail")       return dma_display->color565(0, 113, 193);  // css #1f6fa8
  return dma_display->color565(191, 185, 7); // css #767322 — "personal", also the fallback category
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
const unsigned long EVENT_STATIC_HOLD = 2000, EVENT_END_HOLD = 600, EVENT_MIN_HOLD = 4000;
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
  if (numEvents == 0) return 0;
  const int barX0 = 2; // must match renderEvents()'s margin
  int capMaxW = W - barX0 - 2;
  EventPlan plan[MAX_EVENTS];
  computeEventPlan(capMaxW, plan);
  unsigned long cycle = 0;
  for (int i = 0; i < numEvents; i++) cycle += plan[i].dur;
  return cycle;
}

void renderEvents(unsigned long elapsed) {
  dma_display->clearScreen();
  // Busy score lives on its own Day Overview page now — the timeline gets
  // the full board width, and the description text uses the same width
  // budget as the title above it (nothing left to dodge any more).
  const int barX0 = 2, barX1 = W - 2;

  if (numEvents == 0) {
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(150, 150, 150));
    dma_display->setCursor(barX0, 13);
    dma_display->print("NO EVENTS TODAY");
    return;
  }

  int capMaxW = W - barX0 - 2;
  EventPlan plan[MAX_EVENTS];
  computeEventPlan(capMaxW, plan);

  unsigned long cycle = 0;
  for (int i = 0; i < numEvents; i++) cycle += plan[i].dur;
  if (cycle == 0) cycle = 1;
  unsigned long tMod = elapsed % cycle;
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
    String closedText = "MARKET CLOSED TODAY";
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
    if (x > -200 && x < W + 50) {
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
  dma_display->setCursor(centerTextX(dateStr, 6), 24);
  dma_display->print(dateStr);
}

// Day Overview: busy score + event count (both real, existing /api/matrix
// fields) always show. The hours-busy/hours-free row only draws if the
// backend has actually sent dayOverview.hoursBusy/hoursFree — never
// fabricated. See handoff doc for the backend field this needs.
void renderDayOverview() {
  dma_display->clearScreen();
  int score = max(0, min(10, (int)round(dailyBusyPercent / 10.0)));
  uint16_t scoreColor = busyScoreColor(score);
  const int dotR = 4, dotX = 2 + dotR, dotY = 5;
  dma_display->fillCircle(dotX, dotY, dotR, scoreColor);
  String busyText = "BUSY " + String(score);
  dma_display->setTextSize(1);
  dma_display->setTextColor(scoreColor);
  dma_display->setCursor(dotX + dotR + 3, 2);
  dma_display->print(busyText);

  String evText = String(numEvents) + (numEvents == 1 ? " EVENT" : " EVENTS");
  dma_display->setTextColor(dma_display->color565(150, 150, 150));
  dma_display->setCursor(W - 2 - (int)evText.length() * 6, 2);
  dma_display->print(evText);

  if (dayOverview.hasHours) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fH BUSY", dayOverview.hoursBusy);
    String busyHText = buf;
    snprintf(buf, sizeof(buf), "%.1fH FREE", dayOverview.hoursFree);
    String freeHText = buf;
    dma_display->setTextColor(dma_display->color565(150, 150, 150));
    dma_display->setCursor(2, 13);
    dma_display->print(busyHText);
    dma_display->setTextColor(dma_display->color565(0, 255, 80));
    dma_display->setCursor(W - 2 - (int)freeHText.length() * 6, 13);
    dma_display->print(freeHText);
  } else {
    dma_display->setTextColor(dma_display->color565(90, 85, 75));
    String tbd = "HOURS DATA COMING SOON";
    dma_display->setCursor(centerTextX(tbd, 6), 14);
    dma_display->print(tbd);
  }
  // Commute/drive row intentionally not shown here any more — there's a
  // dedicated Commuting page for that now.
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
void renderOffline(unsigned long now) {
  dma_display->clearScreen();
  uint8_t pulse = (uint8_t)(128 + 127 * sin(now / 300.0));
  drawRectOutlineColor(0, 0, W, H, dma_display->color565(pulse, 0, 0));

  String reason;
  if (WiFi.status() != WL_CONNECTED) reason = "WIFI DISCONNECTED";
  else if (!dataValid) reason = "NO DATA YET";
  else reason = "CAN'T REACH PI";

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
  else if (id == "commuting") renderCommuting(screenElapsed);
  else if (id == "stars") renderStars(screenElapsed);
  else if (id == "balls") renderBalls(screenElapsed);
  else if (id == "alerts") renderAlert(now);
  else renderComingSoonFwd(id, now); // weather, and anything unrecognized
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
  dma_display->begin();
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
  if (pinnedScreen.length() > 0) {
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
