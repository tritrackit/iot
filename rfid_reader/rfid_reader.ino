#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <LoRa.h>
#include <Adafruit_PN532.h>

/* ===================== LoRa wiring (Arduino Nano) ===================== */
// HW SPI: SCK=D13, MISO=D12, MOSI=D11
#define LORA_CS    10
#define LORA_RST    9
#define LORA_DIO0   2

/* ===================== PN532 (I2C) wiring ===================== */
// A4 = SDA, A5 = SCL
#define PN532_IRQ  -1      // polling; no IRQ pin
#define PN532_RST   3      // PN532 RSTO/RST -> D3
Adafruit_PN532 nfc(PN532_IRQ, PN532_RST, &Wire);

/* ===================== Buzzer ===================== */
// Passive buzzer (piezo) -> use tone(); Active buzzer -> drive HIGH/LOW
#define BUZZER_PIN       6
#define PASSIVE_BUZZER   1   // 1 = passive (tone), 0 = active (on/off)

// Louder pattern settings (you can tweak)
#define BUZZ_OK_CHIRP1_FREQ  2600
#define BUZZ_OK_CHIRP2_FREQ  3100
#define BUZZ_OK_CHIRP3_FREQ  3600
#define BUZZ_OK_CHIRP_MS     120   // each chirp duration
#define BUZZ_OK_GAP_MS        30

#define BUZZ_ERR_FREQ        1400
#define BUZZ_ERR_PULSE_MS      90
#define BUZZ_ERR_GAP_MS        70
#define BUZZ_ERR_REPEAT         3  // triple-beep error (more noticeable)

/* ===================== LoRa radio settings ===================== */
#define LORA_FREQ       433E6
#define LORA_BW         125E3
#define LORA_SF         7
#define LORA_CR         5
#define LORA_TX_POWER   17
#define LORA_SYNC_WORD  0x42   // all nodes must match

/* ===================== Addressing ===================== */
#define NET_ID     0xA1
#define MY_ID      0x01       // this sender
#define DEST_ID    0x02       // target receiver
#define BROADCAST  0xFF

/* ===================== App identity ===================== */
#define SCANNER_CODE "SCANNER_1"

/* ===================== Header ===================== */
struct __attribute__((packed)) MsgHdr {
  uint8_t net, dst, src, seq, len;
};
static uint8_t g_seq = 0;

/* ===================== Helpers ===================== */
static inline String hex2(uint8_t b){ char buf[3]; sprintf(buf, "%02X", b); return String(buf); }

bool sendTo(uint8_t destId, const uint8_t* data, uint8_t len) {
  MsgHdr h{ NET_ID, destId, MY_ID, g_seq++, len };
  if (!LoRa.beginPacket()) return false;
  LoRa.write((uint8_t*)&h, sizeof(h));
  LoRa.write(data, len);
  LoRa.endPacket();  // blocking send
  return true;
}

/* ===================== Buzzer patterns (louder) ===================== */
void buzz_on() {
#if PASSIVE_BUZZER
  // tone() will be called with specific frequencies below
#else
  digitalWrite(BUZZER_PIN, HIGH);
#endif
}
void buzz_off() {
#if PASSIVE_BUZZER
  noTone(BUZZER_PIN);
#else
  digitalWrite(BUZZER_PIN, LOW);
#endif
}

void buzz_ok_loud() {
#if PASSIVE_BUZZER
  // 3-chirp sweep across the typical piezo resonance band for perceived loudness
  tone(BUZZER_PIN, BUZZ_OK_CHIRP1_FREQ, BUZZ_OK_CHIRP_MS);
  delay(BUZZ_OK_CHIRP_MS + BUZZ_OK_GAP_MS);
  tone(BUZZER_PIN, BUZZ_OK_CHIRP2_FREQ, BUZZ_OK_CHIRP_MS);
  delay(BUZZER_PIN ? (BUZZ_OK_CHIRP_MS + BUZZ_OK_GAP_MS) : 0);
  tone(BUZZER_PIN, BUZZ_OK_CHIRP3_FREQ, BUZZ_OK_CHIRP_MS + 40);  // slightly longer final chirp
  delay(BUZZ_OK_CHIRP_MS + 40);
  noTone(BUZZER_PIN);
#else
  // Active buzzer: longer ON time and repeat for higher perceived loudness
  for (int i = 0; i < 2; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(180);
    digitalWrite(BUZZER_PIN, LOW);
    delay(60);
  }
#endif
}

void buzz_error_loud() {
#if PASSIVE_BUZZER
  for (int i = 0; i < BUZZ_ERR_REPEAT; i++) {
    tone(BUZZER_PIN, BUZZ_ERR_FREQ, BUZZ_ERR_PULSE_MS);
    delay(BUZZ_ERR_PULSE_MS + BUZZ_ERR_GAP_MS);
  }
  noTone(BUZZER_PIN);
#else
  for (int i = 0; i < BUZZ_ERR_REPEAT; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(BUZZ_ERR_PULSE_MS);
    digitalWrite(BUZZER_PIN, LOW);  delay(BUZZ_ERR_GAP_MS);
  }
#endif
}

/* ===================== Setup ===================== */
void setup() {
  Serial.begin(115200);
  while (!Serial) {}

  pinMode(BUZZER_PIN, OUTPUT);
#if !PASSIVE_BUZZER
  digitalWrite(BUZZER_PIN, LOW);
#endif

  // --- LoRa ---
  SPI.begin();
  LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
  if (!LoRa.begin(LORA_FREQ)) {
    Serial.println(F("LoRa init failed!"));
    buzz_error_loud();
    while (true) {}
  }
  LoRa.setSyncWord(LORA_SYNC_WORD);
  LoRa.setSignalBandwidth(LORA_BW);
  LoRa.setSpreadingFactor(LORA_SF);
  LoRa.setCodingRate4(LORA_CR);
  LoRa.setTxPower(LORA_TX_POWER);
  Serial.println(F("✅ LoRa Initialized"));

  // --- PN532 (I2C) ---
  Wire.begin();
  delay(10);
  nfc.begin();
  delay(10);
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println(F("Didn't find PN532 board"));
    buzz_error_loud();
    while (true) {}
  }
  nfc.SAMConfig();
  Serial.println(F("✅ PN532 Ready"));
}

/* ===================== Main loop ===================== */
void loop() {
  uint8_t uid[7]; uint8_t uidLength;
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength)) {
    // Build UID hex
    String uidHex; uidHex.reserve(uidLength * 2);
    for (uint8_t i = 0; i < uidLength; i++) uidHex += hex2(uid[i]);

    // CSV payload
    String payload = String(SCANNER_CODE) + "," + uidHex;

    Serial.print(F("TX -> DST 0x")); Serial.print(DEST_ID, HEX);
    Serial.print(F(" : "));         Serial.println(payload);

    bool ok = sendTo(DEST_ID, (const uint8_t*)payload.c_str(), (uint8_t)payload.length());
    if (ok) buzz_ok_loud(); else buzz_error_loud();

    delay(500); // cooldown
  } else {
    delay(20);
  }
}
