/**
 * ESP32-S3 LED Wall Display — SIMULATION MODE
 *
 * Test sketch for validating WiFi, API polling, JSON parsing, and page logic
 * WITHOUT physical LED panels. Output displays in Serial Monitor at 115200 baud.
 *
 * This sketch:
 * - Connects to WiFi
 * - Polls /api/matrix endpoint every 30 seconds
 * - Parses JSON response with ArduinoJson
 * - Displays page content as ASCII art in Serial Monitor
 * - Cycles through pages automatically
 *
 * Use this to validate all logic before physical panels arrive.
 *
 * Library required: ArduinoJson (install via Arduino IDE Library Manager)
 * No HUB75 library needed for simulation mode.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ===== CONFIGURATION =====
const char* SSID = "Adidas";           // WiFi network name
const char* PASSWORD = "335607199472";   // WiFi password
const char* PI_URL = "http://192.168.0.130:3001/api/matrix"; // Orange Pi address
const char* COMMAND_URL = "http://192.168.0.130:3001/api/matrix/command"; // Live control poll (round 51)

// Display timing
#define POLL_INTERVAL_MS 30000      // Fetch data every 30s
#define PAGE_DURATION_MS 30000      // Show each page for 30s
#define COMMAND_POLL_INTERVAL_MS 1500 // Live control poll, 1-2s per round-51 handoff

// ===== GLOBALS =====
WiFiClient wifiClient;
HTTPClient http;
HTTPClient commandHttp;

// Page enumeration
enum Page { PAGE_TICKER = 0, PAGE_EVENTS = 1, PAGE_HOLDINGS = 2, PAGE_COUNT = 3 };

// Maps a backend screen id (see pi-secretary/backend/lib/matrixControl.js's
// SCREENS catalog) to a Page this firmware actually renders. -1 = no
// renderer yet — skip it in rotation rather than crash or show garbage.
// See the identical comment in the production .ino for why "markets" (not
// "portfolio") is what maps to PAGE_TICKER.
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

// Scrolling ticker state — mirrors the production firmware's logic exactly
// (same tick math, same wraparound) so simulation testing actually proves
// out the real scroll behavior, not just a stand-in for it. There are no
// physical pixels here, so this just tracks a virtual scroll position over
// a virtual 192px-wide "display" and the ASCII renderer below windows into it.
#define TICKER_LOOP_WIDTH 192
struct ScrollState {
  int scrollX;
  unsigned long lastScrollUpdate;
};
ScrollState tickerScroll = { TICKER_LOOP_WIDTH, 0 };
String cachedTicker = "";
int tickerTextWidth = 0;
const int TICKER_CHAR_PX = 6;
const int TICKER_LOOP_GAP_PX = 30;

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

// Which pages are actually in rotation right now — see the production
// .ino's identical comment.
Page renderList[PAGE_COUNT] = { PAGE_TICKER };
int renderCount = 1;
int renderIndex = 0;
unsigned long lastCommandPollTime = 0;

// ===== SETUP =====
void setup() {
  Serial.begin(115200);
  delay(2000); // Wait for serial

  Serial.println("\n\n=== ESP32 LED Wall (SIMULATION MODE) ===");
  Serial.println("Testing WiFi, API polling, JSON parsing, and page rendering.");
  Serial.println("This sketch does NOT require physical LED panels.\n");

  // Connect to WiFi
  connectWiFi();

  // Initial fetch
  pollData();
  pollCommand();

  Serial.println("Setup complete!\n");
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
  // is pinned — see the production .ino's identical comment.
  static unsigned long lastPageChange = 0;
  if (command.pinnedScreen.length() == 0 && now - lastPageChange >= PAGE_DURATION_MS) {
    if (renderCount > 0) renderIndex = (renderIndex + 1) % renderCount;
    lastPageChange = now;
  }

  // Advance the ticker scroll position every loop, same tick-catchup math
  // as the production firmware (important here since this loop's delay is
  // 500ms, not 50ms, so several ticks land at once).
  int scrollTicks = (now - tickerScroll.lastScrollUpdate) / 50;
  if (scrollTicks > 0) {
    tickerScroll.scrollX -= 2 * scrollTicks;
    tickerScroll.lastScrollUpdate += scrollTicks * 50;
    int wrapAt = -(tickerTextWidth + TICKER_LOOP_GAP_PX);
    if (tickerTextWidth > 0 && tickerScroll.scrollX < wrapAt) {
      tickerScroll.scrollX = TICKER_LOOP_WIDTH;
    }
  }

  // Display current frame. Priority order per the round-51 handoff:
  // notification overlay > pinned screen > normal rotation, with offline
  // still winning over all of it.
  if (!dataValid) {
    displayOffline(now);
  } else if (command.hasNotification) {
    displayNotification(command.notificationText, command.notificationSecondsRemaining);
  } else if (command.pinnedScreen.length() > 0 && pageForScreenId(command.pinnedScreen) != -1) {
    displayPage((Page)pageForScreenId(command.pinnedScreen), now);
  } else if (renderCount > 0) {
    displayPage(renderList[renderIndex], now);
  } else {
    displayPage(PAGE_TICKER, now);
  }

  delay(500); // Update display every 500ms
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
    Serial.println("Data fetched successfully\n");
  } else {
    Serial.printf("HTTP error: %d\n", httpCode);
    dataValid = false;
  }

  http.end();
}

// ===== LIVE CONTROL POLLING (round 51) =====
void pollCommand() {
  if (WiFi.status() != WL_CONNECTED) return;

  commandHttp.begin(wifiClient, COMMAND_URL);
  commandHttp.setTimeout(3000);

  int httpCode = commandHttp.GET();
  if (httpCode == 200) {
    String payload = commandHttp.getString();
    parseCommandData(payload);
  }
  // Stay quiet on failure — see the production .ino's identical comment.

  commandHttp.end();
}

void parseCommandData(const String& json) {
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, json);
  if (error) return;

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
    newRenderList[0] = PAGE_TICKER;
    newRenderCount = 1;
  }
  for (int i = 0; i < newRenderCount; i++) renderList[i] = (Page)newRenderList[i];
  renderCount = newRenderCount;
  if (renderIndex >= renderCount) renderIndex = 0;

  command.pinnedScreen = doc["pinnedScreen"].isNull() ? "" : doc["pinnedScreen"].as<String>();

  if (doc["notification"].isNull()) {
    command.hasNotification = false;
  } else {
    command.hasNotification = true;
    command.notificationText = doc["notification"]["text"].as<String>();
    command.notificationSecondsRemaining = doc["notification"]["secondsRemaining"] | 0;
  }

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

  // Rebuild the ticker string once per poll and cache its virtual width —
  // same reasoning as the production firmware.
  cachedTicker = buildTickerString();
  tickerTextWidth = cachedTicker.length() * TICKER_CHAR_PX;
}

// Build the scrolling ticker string: markets, then a gap, then gainers and
// losers sprinkled together — identical logic to the production firmware.
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

// "+2.5% TSX" or "-1.2% NASDAQ" — percent first, then symbol.
String formatTicker(const String& symbol, float changePercent) {
  char buf[24];
  if (changePercent >= 0) {
    snprintf(buf, sizeof(buf), "+%.1f%% %s", changePercent, symbol.c_str());
  } else {
    snprintf(buf, sizeof(buf), "%.1f%% %s", changePercent, symbol.c_str());
  }
  return String(buf);
}

// ===== DISPLAY FUNCTIONS (ASCII art to Serial Monitor) =====

void clearDisplay() {
  // Print enough newlines to scroll display
  for (int i = 0; i < 20; i++) {
    Serial.println();
  }
}

String busyEmoji(const String& level) {
  if (level == "busy") return "🔴"; // Red
  if (level == "medium") return "🟠"; // Orange
  return "🟢"; // Green
}

void displayPage(Page page, unsigned long now) {
  static unsigned long lastDisplay = 0;

  // Only redraw every 2 seconds to avoid spam
  if (now - lastDisplay < 2000) {
    return;
  }
  lastDisplay = now;

  clearDisplay();

  switch (page) {
    case PAGE_TICKER:
      displayTicker();
      break;
    case PAGE_EVENTS:
      displayEvents();
      break;
    case PAGE_HOLDINGS:
      displayHoldings(now);
      break;
    default:
      break;
  }
}

// Windows into cachedTicker at tickerScroll.scrollX, same as the LED matrix
// would, so you can watch it actually scroll across the Serial Monitor.
void displayTicker() {
  const int visibleWidth = 56; // matches the box interior used by the other pages

  Serial.println("╔════════════════════════════════════════════════════════╗");
  Serial.println("║                  MARKET TICKER                         ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");

  String window = "";
  for (int i = 0; i < visibleWidth; i++) {
    int idx = tickerScroll.scrollX + i;
    if (idx >= 0 && idx < (int)cachedTicker.length()) {
      window += cachedTicker[idx];
    } else {
      window += ' ';
    }
  }
  Serial.printf("║ %s\n", window.c_str());
  Serial.println("║                                                        ║");

  if (data.portfolio.dayChange >= 0) {
    Serial.println("║ Holdings:     📈 UP                                     ║");
  } else {
    Serial.println("║ Holdings:     📉 DOWN                                   ║");
  }
  Serial.printf("║ Day:          %+.2f%%   $%+.0f\n",
                data.portfolio.dayChangePercent, data.portfolio.dayChange);
  Serial.println("╠════════════════════════════════════════════════════════╣");
  Serial.println("║ Full ticker string this poll:                          ║");
  Serial.printf("║ %s\n", cachedTicker.c_str());
  Serial.println("╚════════════════════════════════════════════════════════╝");
}

void displayEvents() {
  Serial.println("╔════════════════════════════════════════════════════════╗");
  Serial.println("║                   TODAY'S EVENTS                       ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");

  if (data.eventCount == 0) {
    Serial.println("║ No events today                                        ║");
  } else {
    for (int i = 0; i < data.eventCount && i < 4; i++) {
      String emoji = busyEmoji(data.events[i].busyLevel);
      Serial.printf("║ %s %s %s\n",
                    data.events[i].time.c_str(),
                    emoji.c_str(),
                    data.events[i].title.c_str());
    }
  }

  Serial.println("╠════════════════════════════════════════════════════════╣");
  Serial.printf("║ Daily Busy: %d%% ", data.dailyBusyPercent);

  // Draw busy bar
  int bars = data.dailyBusyPercent / 10;
  for (int i = 0; i < 5; i++) {
    Serial.print(i < bars ? "█" : "░");
  }
  Serial.println(" ║");
  Serial.println("╚════════════════════════════════════════════════════════╝");
}

void displayHoldings(unsigned long now) {
  Serial.println("╔════════════════════════════════════════════════════════╗");
  Serial.println("║                   TOP HOLDINGS                         ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");

  if (data.holdingCount == 0) {
    Serial.println("║ No holdings                                            ║");
  } else {
    // Cycle through holdings every 4 seconds
    static unsigned long holdingStartTime = now;
    int holdingDuration = 4000;
    int currentHolding = ((now - holdingStartTime) / holdingDuration) % data.holdingCount;

    const auto& h = data.holdings[currentHolding];

    Serial.printf("║ Symbol: %s\n", h.symbol.c_str());
    Serial.printf("║ Value: $%d\n", h.value);
    Serial.printf("║ Day Change: %+.1f%%\n", h.dayChangePercent);
    Serial.printf("║ Weight: %.1f%% of portfolio\n", h.weightPercent);
    Serial.println("║                                                        ║");
    Serial.printf("║ Showing holding %d of %d\n", currentHolding + 1, data.holdingCount);
  }

  Serial.println("╚════════════════════════════════════════════════════════╝");
}

void displayNotification(const String& text, int secondsRemaining) {
  clearDisplay();
  Serial.println("╔════════════════════════════════════════════════════════╗");
  Serial.println("║                    NOTIFICATION                        ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");
  Serial.printf("║ %s\n", text.c_str());
  Serial.println("║                                                        ║");
  Serial.printf("║ (%d seconds remaining)\n", secondsRemaining);
  Serial.println("╚════════════════════════════════════════════════════════╝");
}

void displayOffline(unsigned long now) {
  clearDisplay();
  Serial.println("╔════════════════════════════════════════════════════════╗");
  Serial.println("║                      OFFLINE                           ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");
  Serial.println("║ Could not connect to Orange Pi backend                ║");

  if (lastRefreshTime > 0) {
    unsigned long minutesSinceRefresh = (now - lastRefreshTime) / 60000;
    Serial.printf("║ Last successful refresh: %lu minutes ago\n", minutesSinceRefresh);
  } else {
    Serial.println("║ Never connected");
  }

  Serial.println("║ Retrying every 30 seconds...                          ║");
  Serial.println("╚════════════════════════════════════════════════════════╝");
}
