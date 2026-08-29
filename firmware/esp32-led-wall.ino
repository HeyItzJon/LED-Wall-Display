/**
 * ESP32-S3 LED Wall Display
 *
 * Controls 3x Lyson 32×64 LED panels chained horizontally (192×32 total)
 * via HUB75 protocol. Displays 4 rotating pages of pi-secretary data.
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
const char* SSID = "YOUR_SSID";           // WiFi network name
const char* PASSWORD = "YOUR_PASSWORD";   // WiFi password
const char* PI_URL = "http://192.168.0.130:3001/api/matrix"; // Orange Pi address
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

// ===== GLOBALS =====
MatrixPanel_I2S_DMA* matrix = nullptr;
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

  // Rotate pages every PAGE_DURATION_MS
  static unsigned long lastPageChange = 0;
  if (now - lastPageChange >= PAGE_DURATION_MS) {
    currentPage = (Page)((currentPage + 1) % PAGE_COUNT);
    lastPageChange = now;
    Serial.printf("Page: %d\n", currentPage);
  }

  // Render current page
  if (dataValid) {
    renderPage(currentPage, now);
  } else {
    renderOffline(now);
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
    case PAGE_PORTFOLIO:
      renderPortfolio();
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

void renderPortfolio() {
  matrix->clearScreen();

  // Title
  matrix->setTextColor(matrix->color565(100, 100, 100));
  matrix->setCursor(2, 4);
  matrix->print("PORTFOLIO");

  // Total value
  matrix->setTextColor(matrix->color565(200, 200, 200));
  matrix->setCursor(2, 14);
  matrix->printf("$%.0f", data.portfolio.total);

  // Day change
  uint16_t changeColor = data.portfolio.dayChange >= 0
    ? matrix->color565(0, 255, 0)
    : matrix->color565(255, 0, 0);
  matrix->setTextColor(changeColor);
  matrix->setCursor(2, 24);
  matrix->printf("%+.0f (%+.2f%%)", data.portfolio.dayChange, data.portfolio.dayChangePercent);

  matrix->flipDMABuffer();
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
