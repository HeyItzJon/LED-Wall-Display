/**
 * ESP32-S3 LED Wall - Simulation Mode (Stocks Ticker Version)
 *
 * Tests WITHOUT physical panels:
 * - WiFi connection
 * - API polling
 * - JSON parsing
 * - Scrolling ticker logic
 * - Portfolio display
 *
 * Output: ASCII art display to Serial Monitor at 115200 baud
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ============= CONFIGURATION =============
const char* SSID = "YOUR_SSID";
const char* PASSWORD = "YOUR_PASSWORD";
const char* PI_URL = "http://192.168.0.130:3001/api/matrix";

// Display dimensions (simulated)
const int DISPLAY_WIDTH = 192;
const int DISPLAY_HEIGHT = 32;

// ============= GLOBAL STATE =============
unsigned long lastPoll = 0;
unsigned long pollInterval = 30000; // 30 seconds

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
  unsigned long lastUpdate;
};

PortfolioData portfolio;

// Scrolling state
struct ScrollState {
  int scrollX;
  unsigned long lastUpdate;
  String currentTicker;
};

ScrollState scroll;

// ============= SETUP =============
void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println("\n╔════════════════════════════════════════════════════════╗");
  Serial.println("║  ESP32 LED Wall - SIMULATION MODE (Stocks Ticker)      ║");
  Serial.println("║  No physical panels required - ASCII art output only    ║");
  Serial.println("╚════════════════════════════════════════════════════════╝");
  Serial.println();
  Serial.println("Testing WiFi, API polling, JSON parsing, and page logic.");
  Serial.println();

  // Connect to WiFi
  Serial.printf("Connecting to WiFi: %s\n", SSID);
  WiFi.begin(SSID, PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("✓ WiFi connected! IP: %s\n\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("✗ WiFi failed - will show OFFLINE\n");
  }

  // Initialize portfolio data
  memset(&portfolio, 0, sizeof(portfolio));
  portfolio.offline = true;

  // Initialize scroll state
  scroll.scrollX = DISPLAY_WIDTH;
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
  if (now - scroll.lastUpdate >= 100) {
    scroll.scrollX -= 2;
    if (scroll.scrollX < -200) {
      scroll.scrollX = DISPLAY_WIDTH;
    }
    scroll.lastUpdate = now;
  }

  // Draw frame
  drawFrame();

  delay(100); // Update display every 100ms
}

// ============= API POLLING =============
void pollData() {
  Serial.println("Polling http://192.168.0.130:3001/api/matrix...");

  if (WiFi.status() != WL_CONNECTED) {
    portfolio.offline = true;
    Serial.println("✗ WiFi not connected\n");
    return;
  }

  HTTPClient http;
  http.begin(PI_URL);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    parseMatrixData(payload);
    portfolio.offline = false;
    portfolio.lastUpdate = millis();
    Serial.println("✓ Data fetched successfully\n");
  } else {
    portfolio.offline = true;
    Serial.printf("✗ HTTP Error: %d\n\n", httpCode);
  }

  http.end();
}

// ============= JSON PARSING =============
void parseMatrixData(const String& json) {
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, json);

  if (error) {
    Serial.print("✗ JSON parse error: ");
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
void drawFrame() {
  clearScreen();

  if (portfolio.offline) {
    drawOfflineScreen();
  } else {
    drawStocksScreen();
  }
}

void clearScreen() {
  // Move cursor up and clear (ANSI escape codes)
  Serial.write(27);
  Serial.print("[2J");
  Serial.write(27);
  Serial.print("[H");
}

void drawStocksScreen() {
  Serial.println("╔════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗");

  // Top line: Scrolling ticker
  String ticker = buildTickerString();
  scroll.currentTicker = ticker;

  Serial.print("║ "); // left border
  int visible = 0;
  int pos = scroll.scrollX;

  // Draw scrolling text
  for (int x = 0; x < 150 && visible < 150; x++) {
    if (pos + x >= 0 && pos + x < (int)ticker.length()) {
      Serial.print(ticker[pos + x]);
      visible++;
    } else if (pos + x < 0) {
      Serial.print(" ");
      visible++;
    }
  }

  // Right side: Portfolio stats
  int rightStart = 150;
  while (visible < 150) {
    Serial.print(" ");
    visible++;
  }

  if (portfolio.dayChangePercent >= 0) {
    Serial.print(" 📈 "); // Up arrow
  } else {
    Serial.print(" 📉 "); // Down arrow
  }

  Serial.println("║");

  // Middle line: Portfolio info
  Serial.print("║ ");
  char line[160];
  sprintf(line, "Portfolio: Total $%.0f | Day: $%+.0f (%+.1f%%)",
          portfolio.total, portfolio.dayChange, portfolio.dayChangePercent);
  Serial.print(line);

  // Pad to right
  for (int i = strlen(line); i < 150; i++) {
    Serial.print(" ");
  }
  Serial.print("  HOLDINGS  ");
  Serial.println("║");

  // Bottom line: Market and holdings summary
  Serial.print("║ ");
  char summaryLine[160];
  sprintf(summaryLine,
          "Markets: TSX/NASDAQ/NYSE | Top 3 Gainers | Top 3 Losers (rotating...)");
  Serial.print(summaryLine);

  // Pad to right
  for (int i = strlen(summaryLine); i < 150; i++) {
    Serial.print(" ");
  }
  Serial.print("             ");
  Serial.println("║");

  Serial.println("╚════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝");

  // Detailed breakdown
  Serial.println();
  Serial.println("═══ MARKETS ═══════════════════════════════════");
  for (int i = 0; i < portfolio.marketCount; i++) {
    char sign = portfolio.markets[i].changePercent >= 0 ? '+' : ' ';
    Serial.printf("  %s: %c%.1f%%\n",
                  portfolio.markets[i].symbol,
                  sign,
                  portfolio.markets[i].changePercent);
  }

  Serial.println("\n═══ TOP GAINERS ═══════════════════════════════");
  for (int i = 0; i < portfolio.gainersCount; i++) {
    Serial.printf("  %s: +%.1f%%\n",
                  portfolio.gainers[i].symbol,
                  portfolio.gainers[i].changePercent);
  }

  Serial.println("\n═══ TOP LOSERS ════════════════════════════════");
  for (int i = 0; i < portfolio.losersCount; i++) {
    Serial.printf("  %s: %.1f%%\n",
                  portfolio.losers[i].symbol,
                  portfolio.losers[i].changePercent);
  }

  Serial.println();
}

void drawOfflineScreen() {
  Serial.println("╔════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╗");
  Serial.println("║                                          🔴 OFFLINE - Retrying every 30 seconds...                                                                                         ║");
  Serial.println("║                                                                                                                                                                                                                                ║");
  Serial.println("║  Make sure:                                                                                                                                                                                                                    ║");
  Serial.println("║    • Orange Pi backend is running: npm start                                                                                                                                                                                   ║");
  Serial.println("║    • API endpoint responds: curl http://192.168.0.130:3001/api/matrix                                                                                                                                                         ║");
  Serial.println("║    • WiFi is connected to same network as Orange Pi                                                                                                                                                                            ║");
  Serial.println("║    • ESP32 has correct SSID/password in sketch                                                                                                                                                                                 ║");
  Serial.println("╚════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════════╝");
  Serial.println();
}

// ============= HELPERS =============
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

String formatTicker(const char* symbol, float changePercent) {
  char buf[20];
  if (changePercent >= 0) {
    sprintf(buf, "+%.1f%% %s", changePercent, symbol);
  } else {
    sprintf(buf, "%.1f%% %s", changePercent, symbol);
  }
  return String(buf);
}
