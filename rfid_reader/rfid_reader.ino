#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_PN532.h>

// LoRa SPI Pins (hardware SPI on Nano)
#define LORA_CS  10
#define LORA_RST 9
#define LORA_DIO0 2

// PN532 I2C Pins (A4 = SDA, A5 = SCL)
#define PN532_IRQ 2    // Dummy if not used
#define PN532_RST 3    // Optional
Adafruit_PN532 nfc(-1, 3, &Wire);  // IRQ = -1 (unused), RST = D3


void setup() {
  Serial.begin(115200);
  while (!Serial);

  // Setup LoRa
  SPI.begin();  // For Nano: use hardware SPI
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed!");
    while (true);
  }
  Serial.println("✅ LoRa Initialized");

  // Setup PN532 (I2C)
  Wire.begin();
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("Didn't find PN532 board");
    while (1);
  }

  nfc.SAMConfig();  // Enable RFID
  Serial.println("✅ PN532 Ready");
}

void loop() {
  uint8_t uid[7];
  uint8_t uidLength;

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    Serial.print("UID: ");
    for (uint8_t i = 0; i < uidLength; i++) {
      Serial.print(uid[i], HEX);
    }
    Serial.println();

    LoRa.beginPacket();
    LoRa.print("rfid-uid,");
    for (uint8_t i = 0; i < uidLength; i++) {
      LoRa.print(uid[i], HEX);
    }
    LoRa.endPacket();

    delay(2000);  // Prevent spam
  }
}
