/**
 * ESP32-S3 LED Wall - Production Firmware with Scrolling Stocks Ticker
 *
 * Displays:
 * 1. Scrolling ticker: Markets (TSX, NASDAQ, NYSE) → Gainers/Losers → repeat
 * 2. Portfolio stats: emoji + holdings performance
 *
 * WiFi + API polling every 30 seconds
 * HUB75 LED panel output (3x chained 32x64 panels = 192x32 display)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ============= CONFIGURATION =============
const char* SSID = "YOUR_SSID";
const char* PASSWORD = "YOUR_PASSWORD";
const char* PI_URL = "http://192.168.0.130:3001/api/matrix";

// HUB75 GPIO Pin Assignments (for 3x chained 32x64 panels = 192x32)
#define R1_PIN 25
#define G1_PIN 26
#define B1_PIN 27
#define R2_PIN 14
#define G2_PIN 12
#define B2_PIN 13
#define A_PIN 23
#define B_PIN 19
#define C_PIN 5
#define D_PIN 17
#define LAT_PIN 4
#define OE_PIN 15
#define CLK_PIN 16

// Display config
#define PANEL_RES_X 64      // pixels wide per panel
#define PANEL_RES_Y 32      // pixels tall
#define PANEL_CHAIN 3       // 3 panels chained = 192 wide total

// ============= GLOBAL STATE =============
MatrixPanel_I2S_DMA *dma_display = nullptr;
unsigned long lastPoll = 0;
unsigned long pollInterval = 30000; // 30 seconds

// Ticker data
struct TickerItem {
  char symbol[10];
  float changePercent;
};

struct PortfolioData {
  float total;
  float dayChange;
  float dayChangePercent;
  TickerItem markets[3];
  TickerItem gainers[3];
  TickerItem losers[3];
  int marketCount;
  int gainersCount;
  int losersCount;
  bool offline;
};

PortfolioData portfolio;

// Scrolling ticker animation
struct ScrollState {
  int scrollX;
  unsigned long lastUpdate;
  int scrollSpeed; // pixels per update
  String currentTicker; // the full ticker string to display
};

ScrollState scroll;

// ============= SETUP =============
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=== ESP32 LED Wall - Stocks Ticker ===");
  Serial.println("Connecting to WiFi...");

  // Connect to WiFi
  WiFi.begin(SSID, PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected! IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed - will show offline");
  }

  // Initialize display
  HUB75_I2S_CFG mxconfig(
    PANEL_RES_X,
    PANEL_RES_Y,
    PANEL_CHAIN
  );

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(180);
  dma_display->clearScreen();

  Serial.println("Display initialized!");
  Serial.printf("Resolution: %d x %d\n", dma_display->width(), dma_display->height());

  // Initialize portfolio data
  memset(&portfolio, 0, sizeof(portfolio));
  portfolio.offline = true;

  // Initialize scroll state
  scroll.scrollX = dma_display->width();
  scroll.scrollSpeed = 2; // pixels per frame
  scroll.lastUpdate = millis();

  // Initial poll
  pollData();
}

// ============= MAIN LOOP =============
void loop() {
  unsigned long now = millis();

  // Poll API every 30 seconds
  if (now - lastPoll >= pollInterval) {
    pollData();
    lastPoll = now;
  }

  // Update scrolling position
  if (now - scroll.lastUpdate >= 50) { // update every 50ms (~20fps)
    scroll.scrollX -= scroll.scrollSpeed;

    // Reset scroll when it exits left side
    // (we'll calculate exact wrap point based on text width)
    if (scroll.scrollX < -300) { // rough estimate for long ticker string
      scroll.scrollX = dma_display->width();
    }
    scroll.lastUpdate = now;
  }

  // Draw frame
  drawStocksPage();
}

// ============= API POLLING =============
void pollData() {
  if (WiFi.status() != WL_CONNECTED) {
    portfolio.offline = true;
    Serial.println("WiFi disconnected");
    return;
  }

  HTTPClient http;
  http.begin(PI_URL);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    parseMatrixData(payload);
    portfolio.offline = false;
    Serial.println("Data fetched successfully");
  } else {
    portfolio.offline = true;
    Serial.printf("HTTP Error: %d\n", httpCode);
  }

  http.end();
}

// ============= JSON PARSING =============
void parseMatrixData(const String& json) {
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, json);

  if (error) {
    Serial.print("JSON parse error: ");
    Serial.println(error.c_str());
    portfolio.offline = true;
    return;
  }

  // Portfolio
  portfolio.total = doc["portfolio"]["total"] | 0;
  portfolio.dayChange = doc["portfolio"]["dayChange"] | 0;
  portfolio.dayChangePercent = doc["portfolio"]["dayChangePercent"] | 0;

  // Markets
  portfolio.marketCount = 0;
  for (int i = 0; i < 3 && i < doc["markets"].size(); i++) {
    strncpy(portfolio.markets[i].symbol,
            doc["markets"][i]["symbol"] | "", 10);
    portfolio.markets[i].changePercent = doc["markets"][i]["changePercent"] | 0;
    portfolio.marketCount++;
  }

  // Gainers
  portfolio.gainersCount = 0;
  for (int i = 0; i < 3 && i < doc["gainers"].size(); i++) {
    strncpy(portfolio.gainers[i].symbol,
            doc["gainers"][i]["symbol"] | "", 10);
    portfolio.gainers[i].changePercent = doc["gainers"][i]["changePercent"] | 0;
    portfolio.gainersCount++;
  }

  // Losers
  portfolio.losersCount = 0;
  for (int i = 0; i < 3 && i < doc["losers"].size(); i++) {
    strncpy(portfolio.losers[i].symbol,
            doc["losers"][i]["symbol"] | "", 10);
    portfolio.losers[i].changePercent = doc["losers"][i]["changePercent"] | 0;
    portfolio.losersCount++;
  }
}

// ============= DRAWING =============
void drawStocksPage() {
  dma_display->clearScreen();

  if (portfolio.offline) {
    drawOfflinePage();
    return;
  }

  // Right side: Portfolio stats (40px wide)
  int statsX = 152;
  drawPortfolioStats(statsX);

  // Left side: Scrolling ticker (150px wide)
  drawScrollingTicker();
}

// Draw portfolio summary on right side
void drawPortfolioStats(int x) {
  uint16_t color;

  // Emoji: up or down
  if (portfolio.dayChangePercent >= 0) {
    dma_display->setTextColor(dma_display->color565(0, 255, 0)); // Green
    dma_display->setCursor(x + 2, 2);
    dma_display->setTextSize(2);
    dma_display->print("^"); // up arrow
  } else {
    dma_display->setTextColor(dma_display->color565(255, 0, 0)); // Red
    dma_display->setCursor(x + 2, 2);
    dma_display->setTextSize(2);
    dma_display->print("v"); // down arrow
  }

  // "HOLDINGS" label
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(255, 255, 255)); // White
  dma_display->setCursor(x, 12);
  dma_display->print("PORT:");

  // Percentage
  dma_display->setCursor(x, 20);
  char pctStr[10];
  sprintf(pctStr, "%+.1f%%", portfolio.dayChangePercent);
  dma_display->print(pctStr);

  // Dollar amount
  dma_display->setCursor(x, 26);
  char dolStr[15];
  sprintf(dolStr, "$%.0f", portfolio.dayChange);
  dma_display->print(dolStr);
}

// Draw scrolling ticker on left side
void drawScrollingTicker() {
  // Build ticker string from markets, gainers, losers
  String ticker = buildTickerString();
  scroll.currentTicker = ticker;

  // Draw scrolling text at scroll.scrollX
  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(255, 255, 255));

  int x = scroll.scrollX;
  for (size_t i = 0; i < scroll.currentTicker.length() && x < 150; i++) {
    dma_display->setCursor(x, 8);
    dma_display->print(scroll.currentTicker[i]);
    x += 6; // approximate char width
  }

  // Also draw continuation to make it seamless
  if (scroll.scrollX < 50) {
    int x2 = scroll.scrollX + scroll.currentTicker.length() * 6;
    for (size_t i = 0; i < scroll.currentTicker.length() && x2 < 150; i++) {
      dma_display->setCursor(x2, 8);
      dma_display->print(scroll.currentTicker[i]);
      x2 += 6;
    }
  }

  // Display portfolio daily change as second line
  dma_display->setCursor(5, 18);
  char line2[30];
  sprintf(line2, "Portfolio: $%.0f %+.1f%%", portfolio.dayChange, portfolio.dayChangePercent);
  dma_display->print(line2);
}

// Build the scrolling ticker string
String buildTickerString() {
  String s = "";

  // Markets section
  for (int i = 0; i < portfolio.marketCount; i++) {
    if (i > 0) s += " | ";
    s += formatTicker(portfolio.markets[i].symbol, portfolio.markets[i].changePercent);
  }

  // Gap
  s += "  |  ";

  // Gainers and losers mixed
  int maxCount = max(portfolio.gainersCount, portfolio.losersCount);
  for (int i = 0; i < maxCount; i++) {
    if (i > 0) s += " | ";
    if (i < portfolio.gainersCount) {
      s += formatTicker(portfolio.gainers[i].symbol, portfolio.gainers[i].changePercent);
    }
    if (i < portfolio.losersCount) {
      if (i < portfolio.gainersCount) s += " | ";
      s += formatTicker(portfolio.losers[i].symbol, portfolio.losers[i].changePercent);
    }
  }

  return s;
}

// Format a single ticker item
String formatTicker(const char* symbol, float changePercent) {
  char buf[20];
  if (changePercent >= 0) {
    sprintf(buf, "+%.1f%% %s", changePercent, symbol);
  } else {
    sprintf(buf, "%.1f%% %s", changePercent, symbol);
  }
  return String(buf);
}

// Draw offline page
void drawOfflinePage() {
  dma_display->setTextSize(2);
  dma_display->setTextColor(dma_display->color565(255, 0, 0));
  dma_display->setCursor(50, 8);
  dma_display->print("OFFLINE");

  dma_display->setTextSize(1);
  dma_display->setTextColor(dma_display->color565(200, 200, 200));
  dma_display->setCursor(30, 20);
  dma_display->print("Retrying...");
}
