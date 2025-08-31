#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_PN532.h>

/* ===================== LoRa wiring (Arduino Nano) ===================== */
// HW SPI pins are fixed: SCK=D13, MISO=D12, MOSI=D11
#define LORA_CS    10
#define LORA_RST    9
#define LORA_DIO0   2   // DIO0

/* ===================== PN532 (I2C) wiring ===================== */
// A4 = SDA, A5 = SCL
#define PN532_IRQ  -1   // we poll, no IRQ pin used
#define PN532_RST   3   // connect PN532 RSTO/RST here
Adafruit_PN532 nfc(PN532_IRQ, PN532_RST, &Wire);

/* ===================== LoRa radio settings ===================== */
#define LORA_FREQ       433E6
#define LORA_BW         125E3
#define LORA_SF         7
#define LORA_CR         5
#define LORA_TX_POWER   17
#define LORA_SYNC_WORD  0x42   // choose a private sync word (all nodes must match)

/* ===================== Addressing (change per deployment) ===================== */
#define NET_ID     0xA1       // your app/network id (all nodes match)
#define MY_ID      0x01       // this sender's ID
#define DEST_ID    0x02       // receiver's ID (change to your target)
#define BROADCAST  0xFF

/* ===================== App identity ===================== */
#define SCANNER_CODE "SCANNER1"

/* ===================== Header definition ===================== */
struct __attribute__((packed)) MsgHdr {
  uint8_t net;   // network id
  uint8_t dst;   // destination id
  uint8_t src;   // source id
  uint8_t seq;   // sequence counter
  uint8_t len;   // payload length (bytes following the header)
};

static uint8_t g_seq = 0;

/* ===================== Helpers ===================== */
static inline String hex2(uint8_t b) {
  char buf[3];
  sprintf(buf, "%02X", b);
  return String(buf);
}

bool sendTo(uint8_t destId, const uint8_t* data, uint8_t len) {
  MsgHdr h{ NET_ID, destId, MY_ID, g_seq++, len };
  if (!LoRa.beginPacket()) return false;
  LoRa.write((uint8_t*)&h, sizeof(h));
  LoRa.write(data, len);
  LoRa.endPacket();  // blocking send
  return true;
}

/* ===================== Setup ===================== */
void setup() {
  Serial.begin(115200);
  while (!Serial) { /* safe on Nano; returns immediately */ }

  // --- LoRa init ---
  SPI.begin(); // SCK D13, MISO D12, MOSI D11
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println(F("LoRa init failed!"));
    while (true) {}
  }
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setTxPower(LORA_TX_POWER); // PA_BOOST on most SX1278/RFM95 boards
  Serial.println(F("✅ LoRa Initialized"));

  // --- PN532 init (I2C) ---
  Wire.begin();        // A4/A5
  delay(10);
  nfc.begin();
  delay(10);

  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println(F("Didn't find PN532 board"));
    while (true) {}
  }
  nfc.SAMConfig();  // enable RFID
  Serial.println(F("✅ PN532 Ready"));
}

/* ===================== Main loop ===================== */
void loop() {
  uint8_t uid[7];
  uint8_t uidLength;

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    // Build contiguous uppercase UID hex
    String uidHex; uidHex.reserve(uidLength * 2);
    for (uint8_t i = 0; i < uidLength; i++) uidHex += hex2(uid[i]);

    // CSV payload: SCANNER_CODE,UIDHEX
    String payload = String(SCANNER_CODE) + "," + uidHex;

    // Debug
    Serial.print(F("TX -> DST 0x"));
    Serial.print(DEST_ID, HEX);
    Serial.print(F(" : "));
    Serial.println(payload);

    // Send to specific receiver
    bool ok = sendTo(DEST_ID, (const uint8_t*)payload.c_str(), (uint8_t)payload.length());
    if (!ok) {
      Serial.println(F("LoRa beginPacket() failed"));
    }

    delay(2000);  // avoid rapid repeats on the same tag
  } else {
    delay(20);
  }
}
