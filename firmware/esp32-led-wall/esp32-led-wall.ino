/**
 * ESP32-S3 LED Wall Display
 *
 * Controls 3x Lyson 32×64 LED panels chained horizontally (192×32 total)
 * via HUB75 protocol. Displays rotating pages of pi-secretary data:
 * a scrolling market/holdings ticker, today's events, and top holdings.
 *
 * Hardware:
 * - ESP32-S3
 * - 3x 32×64 HUB75 LED panels (Lyson)
 * - 5V power supply for panels
 * - WiFi connection to Orange Pi
 *
 * Library: ESP32-HUB75-MatrixPanel-DMA
 * Install via Arduino IDE Library Manager
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ===== CONFIGURATION =====
const char* SSID = "adidas";           // WiFi network name
const char* PASSWORD = "335607199472";   // WiFi password
const char* PI_URL = "http://192.168.0.130:3001/api/matrix"; // Orange Pi address
const char* COMMAND_URL = "http://192.168.0.130:3001/api/matrix/command"; // Live control poll (round 51)
const char* TIMEZONE = "America/Toronto";

// HUB75 Panel configuration
#define R1_PIN  25
#define G1_PIN  26
#define B1_PIN  27
#define R2_PIN  14
#define G2_PIN  12
#define B2_PIN  13
#define A_PIN   23
#define B_PIN   19
#define C_PIN   5
#define D_PIN   17
#define E_PIN   18
#define LAT_PIN 4
#define OE_PIN  15
#define CLK_PIN 16

// Panel dimensions
#define PANEL_WIDTH 64
#define PANEL_HEIGHT 32
#define PANEL_COUNT 3  // 3 panels chained = 192×32
#define DISPLAY_WIDTH (PANEL_WIDTH * PANEL_COUNT)
#define DISPLAY_HEIGHT PANEL_HEIGHT

// Display timing
#define POLL_INTERVAL_MS 30000      // Fetch data every 30s
#define PAGE_DURATION_MS 30000      // Show each page for 30s
#define REFRESH_TIMEOUT_MS 300000   // Consider data stale after 5 min
#define COMMAND_POLL_INTERVAL_MS 1500 // Live control poll, 1-2s per round-51 handoff

// ===== GLOBALS =====
MatrixPanel_I2S_DMA* matrix = nullptr;
WiFiClient wifiClient;
HTTPClient http;
HTTPClient commandHttp;

// Page enumeration
enum Page { PAGE_TICKER = 0, PAGE_EVENTS = 1, PAGE_HOLDINGS = 2, PAGE_COUNT = 3 };

// Maps a backend screen id (see pi-secretary/backend/lib/matrixControl.js's
// SCREENS catalog) to a Page this firmware actually renders. -1 = no
// renderer yet — the round-51 handoff says to skip those in rotation
// rather than crash or show garbage.
//
// NOTE on "portfolio" vs "markets": the catalog's "portfolio" screen was
// originally a simple total-value/day-change static page. That page got
// replaced by PAGE_TICKER (the scrolling markets+movers ticker) in the
// prior round — PAGE_TICKER's actual content (TSX/NASDAQ/S&P + top movers)
// is a match for the catalog's "markets" screen, not "portfolio". So
// "markets" maps to PAGE_TICKER below, and "portfolio" currently has no
// renderer (same bucket as "news") until/unless a distinct simple
// total-value page gets built back in.
int pageForScreenId(const String& id) {
  if (id == "markets") return PAGE_TICKER;
  if (id == "holdings") return PAGE_HOLDINGS;
  if (id == "events") return PAGE_EVENTS;
  return -1; // "portfolio", "news", "weather" — no renderer yet
}

// Last API response
struct MatrixData {
  long timestamp;
  long lastRefresh;
  struct {
    float total;
    float dayChange;
    float dayChangePercent;
  } portfolio;
  struct {
    String symbol;
    float changePercent;
  } markets[3];
  int marketCount;
  struct {
    String symbol;
    float changePercent;
  } gainers[3];
  int gainerCount;
  struct {
    String symbol;
    float changePercent;
  } losers[3];
  int loserCount;
  struct {
    String time;
    String title;
    String busyLevel;
  } events[10];
  int eventCount;
  int dailyBusyPercent;
  struct {
    String symbol;
    int value;
    float dayChangePercent;
    float weightPercent;
  } holdings[5];
  int holdingCount;
} data;

unsigned long lastPollTime = 0;
unsigned long lastRefreshTime = 0;
bool dataValid = false;

// Scrolling ticker state — updated every frame independent of page rotation
// and the 30s data poll, so the scroll stays smooth regardless of either.
// cachedTicker/tickerTextWidth are rebuilt once per successful poll (see
// end of parseMatrixData()) rather than every frame, since building the
// String and remeasuring it 20x/sec would just be wasted work/heap churn.
struct ScrollState {
  int scrollX;
  unsigned long lastScrollUpdate;
};
ScrollState tickerScroll = { DISPLAY_WIDTH, 0 };
String cachedTicker = "";
int tickerTextWidth = 0;
const int TICKER_CHAR_PX = 6;   // approx width of one character at textSize(1)
const int TICKER_LOOP_GAP_PX = 30; // blank gap between the ticker's own repeats

// Live control state (round 51) — polled every COMMAND_POLL_INTERVAL_MS
// from /api/matrix/command, separate from the slower /api/matrix data poll.
struct CommandState {
  String pinnedScreen;              // "" = no pin (null on the wire)
  bool hasNotification;
  String notificationText;
  int notificationSecondsRemaining;
  int lastTestEventId;              // starts at 0; only re-print on a change
};
CommandState command = { "", false, "", 0, 0 };

// Which pages are actually in rotation right now, built from enabledScreens
// each time a command poll succeeds (not every frame). Always has at least
// one entry — if every enabled screen turns out to be one with no renderer
// yet, we fall back to PAGE_TICKER rather than go blank.
Page renderList[PAGE_COUNT] = { PAGE_TICKER };
int renderCount = 1;
int renderIndex = 0;
unsigned long lastCommandPollTime = 0;

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(2000); // Wait for serial

  Serial.println("\n\n=== ESP32 LED Wall ===");

  // Initialize matrix panel
  initMatrix();
  drawSplash("Initializing...");

  // Connect to WiFi
  connectWiFi();

  // Sync time
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1); // Toronto timezone

  // Initial fetch
  pollData();
  pollCommand();

  Serial.println("Setup complete!");
}

// ===== MAIN LOOP =====
void loop() {
  unsigned long now = millis();

  // Poll every POLL_INTERVAL_MS
  if (now - lastPollTime >= POLL_INTERVAL_MS) {
    pollData();
    lastPollTime = now;
  }

  // Live control poll (round 51) — fast, separate cadence from the data poll
  if (now - lastCommandPollTime >= COMMAND_POLL_INTERVAL_MS) {
    pollCommand();
    lastCommandPollTime = now;
  }

  // Rotate through renderList every PAGE_DURATION_MS. Paused while a screen
  // is pinned — per the round-51 handoff: "ignore the normal page-flip
  // timer until this goes back to null" — so un-pinning resumes cleanly
  // rather than jumping ahead by however long the pin was held.
  static unsigned long lastPageChange = 0;
  if (command.pinnedScreen.length() == 0 && now - lastPageChange >= PAGE_DURATION_MS) {
    if (renderCount > 0) renderIndex = (renderIndex + 1) % renderCount;
    lastPageChange = now;
  }

  // Advance the ticker scroll position. Runs every loop iteration
  // regardless of which page is showing, so the ticker is never stalled
  // when we rotate back to it. Catches up on however many 50ms ticks have
  // actually elapsed rather than assuming exactly one, so it stays correct
  // even if a frame takes longer than expected.
  int scrollTicks = (now - tickerScroll.lastScrollUpdate) / 50;
  if (scrollTicks > 0) {
    tickerScroll.scrollX -= 2 * scrollTicks;
    tickerScroll.lastScrollUpdate += scrollTicks * 50;
    int wrapAt = -(tickerTextWidth + TICKER_LOOP_GAP_PX);
    if (tickerTextWidth > 0 && tickerScroll.scrollX < wrapAt) {
      tickerScroll.scrollX = DISPLAY_WIDTH;
    }
  }

  // Render current frame. Priority order per the round-51 handoff:
  // notification overlay > pinned screen > normal rotation. Offline still
  // wins over all of it — without the data poll succeeding there's nothing
  // real to show on any of these screens.
  if (!dataValid) {
    renderOffline(now);
  } else if (command.hasNotification) {
    renderNotification(command.notificationText, command.notificationSecondsRemaining);
  } else if (command.pinnedScreen.length() > 0 && pageForScreenId(command.pinnedScreen) != -1) {
    renderPage((Page)pageForScreenId(command.pinnedScreen), now);
  } else if (renderCount > 0) {
    renderPage(renderList[renderIndex], now);
  } else {
    renderPage(PAGE_TICKER, now);
  }

  delay(50); // 20 FPS for scrolling/animation
}

// ===== MATRIX INITIALIZATION =====
void initMatrix() {
  HUB75_I2S_CFG mxconfig(
    DISPLAY_WIDTH,    // width
    DISPLAY_HEIGHT,   // height
    PANEL_COUNT       // number of panels
  );

  // Optional: Set GPIO pins if different from defaults
  mxconfig.gpio.r1 = R1_PIN;
  mxconfig.gpio.g1 = G1_PIN;
  mxconfig.gpio.b1 = B1_PIN;
  mxconfig.gpio.r2 = R2_PIN;
  mxconfig.gpio.g2 = G2_PIN;
  mxconfig.gpio.b2 = B2_PIN;
  mxconfig.gpio.a = A_PIN;
  mxconfig.gpio.b = B_PIN;
  mxconfig.gpio.c = C_PIN;
  mxconfig.gpio.d = D_PIN;
  mxconfig.gpio.e = E_PIN;
  mxconfig.gpio.lat = LAT_PIN;
  mxconfig.gpio.oe = OE_PIN;
  mxconfig.gpio.clk = CLK_PIN;

  matrix = new MatrixPanel_I2S_DMA(mxconfig);
  matrix->begin();
  matrix->clearScreen();
}

// ===== WIFI CONNECTION =====
void connectWiFi() {
  Serial.printf("Connecting to WiFi: %s\n", SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(SSID, PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nWiFi connection failed!");
  }
}

// ===== API POLLING =====
void pollData() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected, skipping poll");
    return;
  }

  Serial.printf("Polling %s...\n", PI_URL);

  http.begin(wifiClient, PI_URL);
  http.setTimeout(5000);

  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    parseMatrixData(payload);
    lastRefreshTime = millis();
    dataValid = true;
    Serial.println("Data fetched successfully");
  } else {
    Serial.printf("HTTP error: %d\n", httpCode);
    dataValid = false;
  }

  http.end();
}

// ===== LIVE CONTROL POLLING (round 51) =====
// Small, fast, cheap read — the backend explicitly designed this endpoint
// for 1-2s polling (no external calls on its side, just cached meta).
void pollCommand() {
  if (WiFi.status() != WL_CONNECTED) return;

  commandHttp.begin(wifiClient, COMMAND_URL);
  commandHttp.setTimeout(3000);

  int httpCode = commandHttp.GET();
  if (httpCode == 200) {
    String payload = commandHttp.getString();
    parseCommandData(payload);
  }
  // Stay quiet on failure — this polls every 1-2s, so logging every miss
  // would flood the serial monitor. The slower /api/matrix poll above
  // already reports when the Pi is unreachable.

  commandHttp.end();
}

void parseCommandData(const String& json) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) return; // quiet — see pollCommand()'s comment

  // enabledScreens → renderList, keeping only ids we actually have a
  // renderer for (see pageForScreenId()'s comment on "portfolio"/"news").
  int newRenderList[PAGE_COUNT];
  int newRenderCount = 0;
  JsonArray screens = doc["enabledScreens"];
  for (JsonVariant v : screens) {
    int p = pageForScreenId(v.as<String>());
    if (p != -1 && newRenderCount < PAGE_COUNT) {
      newRenderList[newRenderCount++] = p;
    }
  }
  if (newRenderCount == 0) {
    // Safety net: never let the wall go blank just because every enabled
    // screen happens to be one we don't render yet.
    newRenderList[0] = PAGE_TICKER;
    newRenderCount = 1;
  }
  for (int i = 0; i < newRenderCount; i++) renderList[i] = (Page)newRenderList[i];
  renderCount = newRenderCount;
  if (renderIndex >= renderCount) renderIndex = 0;

  // pinnedScreen — null on the wire means no pin
  command.pinnedScreen = doc["pinnedScreen"].isNull() ? "" : doc["pinnedScreen"].as<String>();

  // notification — already resolved to {text, secondsRemaining} or null
  // server-side; no expiry math to do here.
  if (doc["notification"].isNull()) {
    command.hasNotification = false;
  } else {
    command.hasNotification = true;
    command.notificationText = doc["notification"]["text"].as<String>();
    command.notificationSecondsRemaining = doc["notification"]["secondsRemaining"] | 0;
  }

  // testEvent — bring-up only, never touches the display. Print once per
  // id change, not on every poll.
  if (!doc["testEvent"].isNull()) {
    int id = doc["testEvent"]["id"] | 0;
    if (id != command.lastTestEventId) {
      Serial.println(doc["testEvent"]["label"].as<String>());
      command.lastTestEventId = id;
    }
  }
}

// ===== JSON PARSING =====
void parseMatrixData(const String& json) {
  StaticJsonDocument<3072> doc;
  DeserializationError error = deserializeJson(doc, json);

  if (error) {
    Serial.printf("JSON parse error: %s\n", error.c_str());
    return;
  }

  // Portfolio
  if (doc["portfolio"]) {
    data.portfolio.total = doc["portfolio"]["total"] | 0.0f;
    data.portfolio.dayChange = doc["portfolio"]["dayChange"] | 0.0f;
    data.portfolio.dayChangePercent = doc["portfolio"]["dayChangePercent"] | 0.0f;
  }

  // Markets (TSX / NASDAQ / S&P)
  data.marketCount = 0;
  JsonArray marketsArray = doc["markets"];
  for (JsonObject m : marketsArray) {
    if (data.marketCount < 3) {
      data.markets[data.marketCount].symbol = m["symbol"].as<String>();
      data.markets[data.marketCount].changePercent = m["changePercent"] | 0.0f;
      data.marketCount++;
    }
  }

  // Top 3 gainers (holdings, today's move)
  data.gainerCount = 0;
  JsonArray gainersArray = doc["gainers"];
  for (JsonObject g : gainersArray) {
    if (data.gainerCount < 3) {
      data.gainers[data.gainerCount].symbol = g["symbol"].as<String>();
      data.gainers[data.gainerCount].changePercent = g["changePercent"] | 0.0f;
      data.gainerCount++;
    }
  }

  // Top 3 losers (holdings, today's move)
  data.loserCount = 0;
  JsonArray losersArray = doc["losers"];
  for (JsonObject l : losersArray) {
    if (data.loserCount < 3) {
      data.losers[data.loserCount].symbol = l["symbol"].as<String>();
      data.losers[data.loserCount].changePercent = l["changePercent"] | 0.0f;
      data.loserCount++;
    }
  }

  // Events
  data.eventCount = 0;
  JsonArray eventsArray = doc["events"];
  for (JsonObject event : eventsArray) {
    if (data.eventCount < 10) {
      data.events[data.eventCount].time = event["time"].as<String>();
      data.events[data.eventCount].title = event["title"].as<String>();
      data.events[data.eventCount].busyLevel = event["busyLevel"].as<String>();
      data.eventCount++;
    }
  }

  // Busy score
  data.dailyBusyPercent = doc["dailyBusyPercent"] | 0;

  // Holdings
  data.holdingCount = 0;
  JsonArray holdingsArray = doc["holdings"];
  for (JsonObject holding : holdingsArray) {
    if (data.holdingCount < 5) {
      data.holdings[data.holdingCount].symbol = holding["symbol"].as<String>();
      data.holdings[data.holdingCount].value = holding["value"] | 0;
      data.holdings[data.holdingCount].dayChangePercent = holding["dayChangePercent"] | 0.0f;
      data.holdings[data.holdingCount].weightPercent = holding["weightPercent"] | 0.0f;
      data.holdingCount++;
    }
  }

  data.timestamp = doc["timestamp"];
  data.lastRefresh = doc["lastRefresh"];

  // Rebuild the ticker string once per poll (not once per frame) and cache
  // its pixel width so loop()'s scroll wraparound check stays cheap.
  cachedTicker = buildTickerString();
  tickerTextWidth = cachedTicker.length() * TICKER_CHAR_PX;
}

// Build the scrolling ticker string: markets, then a gap, then gainers and
// losers sprinkled together (not sequential blocks) — e.g.
// "+0.4% TSX | -0.1% NASDAQ | +0.8% S&P  |  +2.1% AAPL | -1.4% META | ..."
String buildTickerString() {
  String s = "";

  for (int i = 0; i < data.marketCount; i++) {
    if (i > 0) s += " | ";
    s += formatTicker(data.markets[i].symbol, data.markets[i].changePercent);
  }

  s += "  |  ";

  int maxCount = max(data.gainerCount, data.loserCount);
  for (int i = 0; i < maxCount; i++) {
    if (i > 0) s += " | ";
    if (i < data.gainerCount) {
      s += formatTicker(data.gainers[i].symbol, data.gainers[i].changePercent);
    }
    if (i < data.loserCount) {
      if (i < data.gainerCount) s += " | ";
      s += formatTicker(data.losers[i].symbol, data.losers[i].changePercent);
    }
  }

  return s;
}

// "+2.5% TSX" or "-1.2% NASDAQ" — percent first, then symbol, per the design.
String formatTicker(const String& symbol, float changePercent) {
  char buf[24];
  if (changePercent >= 0) {
    snprintf(buf, sizeof(buf), "+%.1f%% %s", changePercent, symbol.c_str());
  } else {
    snprintf(buf, sizeof(buf), "%.1f%% %s", changePercent, symbol.c_str());
  }
  return String(buf);
}

// ===== RENDERING =====

void drawSplash(const char* text) {
  matrix->clearScreen();
  matrix->setTextColor(matrix->color565(0, 255, 0));
  matrix->setCursor(10, 12);
  matrix->println(text);
  matrix->flipDMABuffer();
}

uint16_t busyColor(const String& level) {
  if (level == "busy") return matrix->color565(255, 0, 0);   // Red
  if (level == "medium") return matrix->color565(255, 165, 0); // Orange
  return matrix->color565(0, 255, 0); // Green
}

void renderPage(Page page, unsigned long now) {
  switch (page) {
    case PAGE_TICKER:
      renderTicker();
      break;
    case PAGE_EVENTS:
      renderEvents();
      break;
    case PAGE_HOLDINGS:
      renderHoldings(now);
      break;
    default:
      break;
  }
}

// Left 150px: continuously scrolling ticker (markets → gap → gainers/losers
// mixed). Right 40px: portfolio side panel — up/down indicator, day % and $.
void renderTicker() {
  matrix->clearScreen();

  matrix->setTextSize(1);
  matrix->setTextColor(matrix->color565(200, 200, 200));

  int x = tickerScroll.scrollX;
  for (size_t i = 0; i < cachedTicker.length() && x < 150; i++) {
    if (x > -TICKER_CHAR_PX) {
      matrix->setCursor(x, 12);
      matrix->print(cachedTicker[i]);
    }
    x += TICKER_CHAR_PX;
  }

  // Draw the next repeat right behind it so the loop is seamless — once the
  // first copy has scrolled far enough left that its tail is approaching
  // the screen, start drawing copy #2 starting right after copy #1 ends.
  int loopWidth = tickerTextWidth + TICKER_LOOP_GAP_PX;
  if (loopWidth > 0 && tickerScroll.scrollX + loopWidth < 150) {
    int x2 = tickerScroll.scrollX + loopWidth;
    for (size_t i = 0; i < cachedTicker.length() && x2 < 150; i++) {
      if (x2 > -TICKER_CHAR_PX) {
        matrix->setCursor(x2, 12);
        matrix->print(cachedTicker[i]);
      }
      x2 += TICKER_CHAR_PX;
    }
  }

  drawTickerSidePanel(152);

  matrix->flipDMABuffer();
}

void drawTickerSidePanel(int x) {
  bool up = data.portfolio.dayChange >= 0;

  // Up/down indicator
  matrix->setTextColor(up ? matrix->color565(0, 255, 0) : matrix->color565(255, 0, 0));
  matrix->setTextSize(2);
  matrix->setCursor(x + 2, 2);
  matrix->print(up ? "^" : "v");

  // "HOLD" label
  matrix->setTextSize(1);
  matrix->setTextColor(matrix->color565(150, 150, 150));
  matrix->setCursor(x, 12);
  matrix->print("HOLD");

  // Day percentage
  matrix->setTextColor(matrix->color565(200, 200, 200));
  matrix->setCursor(x, 20);
  matrix->printf("%+.1f%%", data.portfolio.dayChangePercent);

  // Day dollar amount
  matrix->setCursor(x, 28);
  matrix->printf("$%+.0f", data.portfolio.dayChange);
}

void renderEvents() {
  matrix->clearScreen();

  // Title
  matrix->setTextColor(matrix->color565(100, 100, 100));
  matrix->setCursor(2, 4);
  matrix->print("EVENTS");

  if (data.eventCount == 0) {
    matrix->setTextColor(matrix->color565(100, 100, 100));
    matrix->setCursor(2, 16);
    matrix->print("No events today");
  } else {
    // Show first 3-4 events
    int y = 12;
    for (int i = 0; i < data.eventCount && i < 4; i++) {
      matrix->setTextColor(busyColor(data.events[i].busyLevel));
      matrix->setCursor(2, y);
      matrix->printf("%s %s", data.events[i].time.c_str(),
                     data.events[i].title.c_str());
      y += 6;
    }

    // Busy bar at bottom
    matrix->setTextColor(matrix->color565(100, 100, 100));
    matrix->setCursor(2, 28);
    matrix->printf("Busy: %d%%", data.dailyBusyPercent);
  }

  matrix->flipDMABuffer();
}

void renderHoldings(unsigned long now) {
  matrix->clearScreen();

  // Title
  matrix->setTextColor(matrix->color565(100, 100, 100));
  matrix->setCursor(2, 4);
  matrix->print("HOLDINGS");

  if (data.holdingCount == 0) {
    matrix->setTextColor(matrix->color565(100, 100, 100));
    matrix->setCursor(2, 16);
    matrix->print("No holdings");
  } else {
    // Cycle through holdings
    static unsigned long holdingStartTime = now;
    int holdingDuration = 4000; // 4 sec per holding
    int currentHolding = ((now - holdingStartTime) / holdingDuration) % data.holdingCount;

    const auto& h = data.holdings[currentHolding];

    matrix->setTextColor(matrix->color565(200, 200, 200));
    matrix->setCursor(2, 12);
    matrix->print(h.symbol.c_str());

    matrix->setTextColor(matrix->color565(150, 150, 150));
    matrix->setCursor(2, 20);
    matrix->printf("$%d | %+.1f%% | %.1f%%", h.value, h.dayChangePercent, h.weightPercent);
  }

  matrix->flipDMABuffer();
}

// Full-wall interrupt, drawn on top of whatever would otherwise be
// showing. Word-wraps at a fixed char count (192px / ~6px per char) rather
// than on word boundaries — simple and reliable; the backend already caps
// notification text at 60 chars so this never has more than ~2 lines to fit.
void renderNotification(const String& text, int secondsRemaining) {
  matrix->clearScreen();

  const int maxLineChars = 32;
  String line1 = text.substring(0, min((int)text.length(), maxLineChars));
  String line2 = text.length() > maxLineChars
    ? text.substring(maxLineChars, min((int)text.length(), maxLineChars * 2))
    : "";

  matrix->setTextSize(1);
  matrix->setTextColor(matrix->color565(255, 210, 0)); // amber — visually distinct from every normal page
  matrix->setCursor(2, 4);
  matrix->print(line1);
  if (line2.length() > 0) {
    matrix->setCursor(2, 14);
    matrix->print(line2);
  }

  matrix->setTextColor(matrix->color565(120, 120, 120));
  matrix->setCursor(160, 25);
  matrix->printf("%ds", secondsRemaining);

  matrix->flipDMABuffer();
}

void renderOffline(unsigned long now) {
  matrix->clearScreen();

  matrix->setTextColor(matrix->color565(255, 0, 0));
  matrix->setCursor(2, 8);
  matrix->print("OFFLINE");

  matrix->setTextColor(matrix->color565(100, 100, 100));
  matrix->setCursor(2, 16);
  matrix->print("Could not connect");

  unsigned long minutesSinceRefresh = (now - lastRefreshTime) / 60000;
  matrix->setCursor(2, 24);
  matrix->printf("Last: %lu min ago", minutesSinceRefresh);

  matrix->flipDMABuffer();
}
