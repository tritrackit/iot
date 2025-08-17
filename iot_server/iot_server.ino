#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>

// --- Wi-Fi Settings ---
const char* ssid     = "HG8145V5_D0A04";
const char* password = "p75z~${Tn2Iy";

// --- HTTPS Endpoint ---
const char* apiEndpoint = "https://192.168.1.15:3000/api/receiver";

// --- LoRa SPI Pins for ESP32-S3 ---
#define SCK       12
#define MISO      13
#define MOSI      11
#define CS_PIN    10
#define RST_PIN    8
#define DIO0_PIN   9

// ============ TIME ===================
void setupTime() {
  configTime(8 * 3600, 0, "pool.ntp.org", "time.nist.gov");  // UTC+8

  Serial.print("⏳ Syncing NTP");

  time_t now = time(nullptr);
  int retry = 0;
  const int maxRetries = 20;

  while (now < 100000 && retry++ < maxRetries) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }

  Serial.println();

  if (now < 100000) {
    Serial.println("❌ Failed to sync time.");
  } else {
    Serial.println("✅ Time synced.");
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    Serial.printf("🕒 Local Time: %04d-%02d-%02d %02d:%02d:%02d\n",
      timeinfo.tm_year + 1900,
      timeinfo.tm_mon + 1,
      timeinfo.tm_mday,
      timeinfo.tm_hour,
      timeinfo.tm_min,
      timeinfo.tm_sec);
  }
}

String getLocalTimestamp() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buf);
}

// ============ WIFI ===================
void connectToWiFi() {
  WiFi.begin(ssid, password);
  Serial.print("🔌 Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n✅ WiFi Connected. IP: " + WiFi.localIP().toString());
}

// ============ LORA ===================
void setupLoRa() {
  pinMode(RST_PIN, OUTPUT);
  digitalWrite(RST_PIN, LOW); delay(10);
  digitalWrite(RST_PIN, HIGH); delay(10);

  SPI.begin(SCK, MISO, MOSI, CS_PIN);
  LoRa.setPins(CS_PIN, RST_PIN, DIO0_PIN);
  LoRa.setSPIFrequency(1E6);

  if (!LoRa.begin(433E6)) {
    Serial.println("❌ LoRa init failed!");
    while (true);
  }

  LoRa.setSpreadingFactor(7);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.disableCrc();

  Serial.println("✅ LoRa Ready");
}

// ============ SEND TO API ===================
void sendToApi(const String& id, int value, const String& timestamp) {
  WiFiClientSecure client;
  client.setInsecure();  // ⚠️ Dev only

  HTTPClient https;
  if (https.begin(client, apiEndpoint)) {
    https.addHeader("Content-Type", "application/json");

    String json = "{\"sender\":\"" + id + "\",\"value\":" + value + ",\"timestamp\":\"" + timestamp + "\"}";

    int httpCode = https.POST(json);

    if (httpCode > 0) {
      Serial.print("🌐 API Response: ");
      Serial.println(httpCode);
      Serial.println(https.getString());
    } else {
      Serial.print("❌ HTTPS Error: ");
      Serial.println(https.errorToString(httpCode));
    }

    https.end();
  } else {
    Serial.println("❌ HTTPS begin() failed");
  }
}

// ============ MAIN SETUP ===================
void setup() {
  delay(3000);
  Serial.begin(115200);
  Serial.println("🟡 ESP32-S3 Multi-Sender Receiver + WiFi");

  connectToWiFi();
  setupTime();
  setupLoRa();
}

// ============ LOOP ===================
bool isValidPacket(const String& msg) {
  int c1 = msg.indexOf(',');
  if (c1 == -1) return false;

  String id = msg.substring(0, c1);
  String valStr = msg.substring(c1 + 1);
  id.trim(); valStr.trim();

  if (id.length() == 0 || valStr.length() == 0) return false;
  for (char c : valStr) {
    if (!isDigit(c)) return false;
  }

  return true;
}

void loop() {
  int packetSize = LoRa.parsePacket();
  if (packetSize > 0) {
    String msg;
    while (LoRa.available()) {
      char c = (char)LoRa.read();
      if (isPrintable(c)) msg += c;
    }
    msg.trim();

    Serial.print("📨 Received (RSSI ");
    Serial.print(LoRa.packetRssi());
    Serial.println(" dBm):");
    Serial.println(msg);

    if (!isValidPacket(msg)) {
      Serial.println("❌ Invalid Format — Skipped");
      Serial.println("──────────────");
      return;
    }

    int c1 = msg.indexOf(',');
    String id = msg.substring(0, c1);
    String valStr = msg.substring(c1 + 1);
    int randomValue = valStr.toInt();

    String timestamp = getLocalTimestamp();

    Serial.println("✅ Final Parsed Data:");
    Serial.println("{");
    Serial.print("  \"sender\": \""); Serial.println(id + "\",");
    Serial.print("  \"value\": "); Serial.println(String(randomValue) + ",");
    Serial.print("  \"timestamp\": \""); Serial.println(timestamp + "\"");
    Serial.println("}");
    Serial.println("──────────────");

    sendToApi(id, randomValue, timestamp);
  }
}
