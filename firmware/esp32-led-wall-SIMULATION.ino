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
const char* SSID = "YOUR_SSID";           // WiFi network name
const char* PASSWORD = "YOUR_PASSWORD";   // WiFi password
const char* PI_URL = "http://192.168.0.130:3001/api/matrix"; // Orange Pi address

// Display timing
#define POLL_INTERVAL_MS 30000      // Fetch data every 30s
#define PAGE_DURATION_MS 30000      // Show each page for 30s

// ===== GLOBALS =====
WiFiClient wifiClient;
HTTPClient http;

// Page enumeration
enum Page { PAGE_PORTFOLIO = 0, PAGE_EVENTS = 1, PAGE_HOLDINGS = 2, PAGE_COUNT = 3 };
Page currentPage = PAGE_PORTFOLIO;

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

  // Rotate pages every PAGE_DURATION_MS
  static unsigned long lastPageChange = 0;
  if (now - lastPageChange >= PAGE_DURATION_MS) {
    currentPage = (Page)((currentPage + 1) % PAGE_COUNT);
    lastPageChange = now;
  }

  // Display current page
  if (dataValid) {
    displayPage(currentPage, now);
  } else {
    displayOffline(now);
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

// ===== JSON PARSING =====
void parseMatrixData(const String& json) {
  StaticJsonDocument<2048> doc;
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
    case PAGE_PORTFOLIO:
      displayPortfolio();
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

void displayPortfolio() {
  Serial.println("╔════════════════════════════════════════════════════════╗");
  Serial.println("║                    PORTFOLIO                           ║");
  Serial.println("╠════════════════════════════════════════════════════════╣");
  Serial.printf("║ Total Value:  $%.2f\n", data.portfolio.total);
  Serial.printf("║ Day Change:   %+.2f (%+.2f%%)\n",
                data.portfolio.dayChange, data.portfolio.dayChangePercent);
  Serial.println("║                                                        ║");
  if (data.portfolio.dayChange >= 0) {
    Serial.println("║ Status:       📈 POSITIVE DAY                          ║");
  } else {
    Serial.println("║ Status:       📉 NEGATIVE DAY                          ║");
  }
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
