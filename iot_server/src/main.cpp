#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <SD.h>
#include <Wire.h>
#include <LoRa.h>
#include <RTClib.h>

/* === AP defaults === */
const char *ap_ssid = "ESP32-Setup";
const char *ap_password = "esp32setup";

/* === Runtime config (RAM) === */
String configApiUrl = "";
int    configIntervalMs = 5000;
String configStaSSID = "";
String configStaPassword = "";

/* === Auth/session === */
String authUser = "admin";  // RAM fallback (used only if SD/config.csv not ready)
String authPass = "admin";
bool   isLoggedIn = false;

/* === Wi-Fi STA state === */
bool   staConnecting = false;
unsigned long staConnectStartMs = 0;

AsyncWebServer server(80);

/* ===== Pins ===== */
#define PIN_SCK       18
#define PIN_MISO      19
#define PIN_MOSI      23
#define PIN_SD_CS     13
#define PIN_I2C_SDA   21
#define PIN_I2C_SCL   22
#define PIN_LORA_SS   5
#define PIN_LORA_RST  14
#define PIN_LORA_DIO0 26
#define LORA_FREQ_HZ  433E6   // change per module/region

/* ===== HW state ===== */
static bool sdReady=false, rtcReady=false, loraReady=false;
static int  hwAttempts=0, hwMaxAttempts=3;
static unsigned long hwNextTryMs=0;
static const unsigned long hwRetryDelayMs=2000;
static bool hwDoneTrying=false;

/* ===== File paths ===== */
static const char* PATH_CONFIG = "/config.csv";
static const char* PATH_LOGS   = "/logs.csv";

/* ===== Config in RAM (mirrors config.csv) ===== */
struct AppConfig {
  String username;
  String password;
  String api;
  int    interval;
  String wifi_ssid;
  String wifi_pass;
} appCfg;

/* ===== RTC ===== */
RTC_DS3231 rtc;

/* ---------- Helpers ---------- */
void listDir(fs::FS &fs, const char * dirname, uint8_t levels = 3){
  Serial.printf("Listing directory: %s\n", dirname);
  File root = fs.open(dirname);
  if(!root){ Serial.println("- failed to open dir"); return; }
  if(!root.isDirectory()){ Serial.println("- not a dir"); return; }
  File file = root.openNextFile();
  while(file){
    if(file.isDirectory()){
      Serial.print("  DIR : "); Serial.println(file.name());
      if(levels){
        String sub = String(dirname);
        if(!sub.endsWith("/")) sub += "/";
        sub += file.name();
        if(!sub.startsWith("/")) sub = "/" + sub;
        listDir(fs, sub.c_str(), levels-1);
      }
    } else {
      Serial.print("  FILE: "); Serial.print(file.name());
      Serial.print("\tSIZE: "); Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

bool initLittleFS(){
  if(!LittleFS.begin(true)){ Serial.println("LittleFS mount failed!"); return false; }
  Serial.println("LittleFS mounted."); listDir(LittleFS,"/");
  return true;
}

static void sendHtml(AsyncWebServerRequest* r, const char* path){
  if(LittleFS.exists(path)) r->send(LittleFS, path, "text/html");
  else r->send(404, "text/plain", String("Missing file: ")+path);
}

static String* beginBodyBuffer(AsyncWebServerRequest *request, size_t total){
  auto *buf = new String(); buf->reserve(total+16);
  request->_tempObject = buf; return buf;
}
static String* getBodyBuffer(AsyncWebServerRequest *request){
  return reinterpret_cast<String*>(request->_tempObject);
}
static void endBodyBuffer(AsyncWebServerRequest *request){
  auto *buf = getBodyBuffer(request); delete buf; request->_tempObject=nullptr;
}

/* ---------- RTC helpers ---------- */
static void i2cScanOnce(Print &out){
  out.println("I2C scan:");
  byte count=0;
  for(byte addr=8; addr<120; addr++){
    Wire.beginTransmission(addr);
    byte err = Wire.endTransmission();
    if(err==0){ out.printf("- 0x%02X found\n", addr); count++; }
  }
  if(!count) out.println("- no devices found");
}
static void rtcInit(){
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  i2cScanOnce(Serial);
  if(rtc.begin()){
    if(rtc.lostPower()){
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
      Serial.println("RTC lost power; set to build time.");
    }
    rtcReady=true; Serial.println("RTC ready");
  } else {
    rtcReady=false; Serial.println("RTC not detected");
  }
}
static String nowTimestamp(){
  if(!rtcReady) return "1970-01-01 00:00:00";
  DateTime now=rtc.now();
  char buf[20];
  snprintf(buf,sizeof(buf),"%04d-%02d-%02d %02d:%02d:%02d",
    now.year(),now.month(),now.day(),now.hour(),now.minute(),now.second());
  return String(buf);
}

/* ---------- CSV utils (simple, comma-delimited, no quotes) ---------- */
static String csvEscape(const String &s){ // basic: forbid commas/newlines
  String x = s; x.replace("\r"," "); x.replace("\n"," "); x.replace(","," "); return x;
}

/* CONFIG.CSV: username,password,api,interval,wifi_ssid,wifi_pass */
static bool ensureConfigCsvExists(){
  if(SD.exists(PATH_CONFIG)) return true;
  File f = SD.open(PATH_CONFIG, FILE_WRITE);
  if(!f){ Serial.println("Failed to create /config.csv"); return false; }
  // defaults: admin,admin,,5000,,
  f.printf("admin,admin,,%d,,\n", 5000);
  f.close();
  Serial.println("Created /config.csv with defaults.");
  return true;
}
static bool readConfigCsv(AppConfig &cfg){
  File f = SD.open(PATH_CONFIG, FILE_READ);
  if(!f){ Serial.println("Cannot open /config.csv"); return false; }
  String line = f.readStringUntil('\n'); line.trim(); f.close();
  // parse 6 fields
  int idx=0, start=0; String fields[6];
  for(int i=0;i<6;i++){ fields[i]=""; }
  for (int i=0; i<=line.length(); i++){
    if(i==line.length() || line.charAt(i)==','){
      fields[idx++] = line.substring(start, i); start=i+1;
      if(idx>=6) break;
    }
  }
  cfg.username   = fields[0];
  cfg.password   = fields[1];
  cfg.api        = fields[2];
  cfg.interval   = fields[3].length()? fields[3].toInt() : 5000;
  cfg.wifi_ssid  = fields[4];
  cfg.wifi_pass  = fields[5];
  return true;
}
static bool writeConfigCsv(const AppConfig &cfg){
  // overwrite (avoid accidental append on some cores)
  SD.remove(PATH_CONFIG);
  File f = SD.open(PATH_CONFIG, FILE_WRITE);
  if(!f){ Serial.println("Failed to open /config.csv for write"); return false; }
  f.printf("%s,%s,%s,%d,%s,%s\n",
    csvEscape(cfg.username).c_str(),
    csvEscape(cfg.password).c_str(),
    csvEscape(cfg.api).c_str(),
    cfg.interval,
    csvEscape(cfg.wifi_ssid).c_str(),
    csvEscape(cfg.wifi_pass).c_str()
  );
  f.flush();
  f.close();
  Serial.println("config.csv saved.");
  return true;
}

/* LOGS.CSV: scanner_id,rfid,timestamp */
static bool ensureLogsCsvExists(){
  if(SD.exists(PATH_LOGS)) return true;
  File f = SD.open(PATH_LOGS, FILE_WRITE);
  if(!f){ Serial.println("Failed to create /logs.csv"); return false; }
  String ts = nowTimestamp();
  f.printf("S-0000001,1234567890,%s\n", ts.c_str());
  f.close();
  Serial.println("Created /logs.csv with a sample row.");
  return true;
}

/* ---------- Bring-up ---------- */
static void spiPreSetup(){
  pinMode(PIN_SD_CS, OUTPUT);    digitalWrite(PIN_SD_CS, HIGH);
  pinMode(PIN_LORA_SS, OUTPUT);  digitalWrite(PIN_LORA_SS, HIGH);
  pinMode(PIN_LORA_RST, OUTPUT); digitalWrite(PIN_LORA_RST, HIGH);
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI);
}
static void loraHardReset(){
  pinMode(PIN_LORA_RST, OUTPUT);
  digitalWrite(PIN_LORA_RST, LOW);  delay(2);
  digitalWrite(PIN_LORA_RST, HIGH); delay(10);
}

static void tryInitHardwareOnce(){
  hwAttempts++;
  Serial.printf("\n=== HW init attempt %d/%d ===\n", hwAttempts, hwMaxAttempts);

  spiPreSetup();

  // SD: try slower SPI if needed
  if(!sdReady){
    Serial.println("Mounting SD...");
    if(SD.begin(PIN_SD_CS, SPI, 25000000)){
      sdReady=true; Serial.println("SD OK @25MHz");
    } else if(SD.begin(PIN_SD_CS, SPI, 10000000)){
      sdReady=true; Serial.println("SD OK @10MHz");
    } else if(SD.begin(PIN_SD_CS, SPI, 4000000)){
      sdReady=true; Serial.println("SD OK @4MHz");
    } else {
      Serial.println("SD init failed (check CS=13, wiring, card FAT32, and module VCC).");
    }
    if(sdReady){
      // Optional: print card info
      Serial.printf("SD size: %llu MB, used: %llu MB\n",
        SD.totalBytes()/(1024ULL*1024ULL), SD.usedBytes()/(1024ULL*1024ULL));

      // Ensure files, then load config and apply
      if(ensureConfigCsvExists()){
        if(readConfigCsv(appCfg)){
          configApiUrl      = appCfg.api;
          configIntervalMs  = appCfg.interval > 0 ? appCfg.interval : 5000;
          configStaSSID     = appCfg.wifi_ssid;
          configStaPassword = appCfg.wifi_pass;
          authUser          = appCfg.username;
          authPass          = appCfg.password;
          Serial.printf("Config loaded: user=%s api=%s interval=%d\n",
                        authUser.c_str(), configApiUrl.c_str(), configIntervalMs);
        }
      }
      ensureLogsCsvExists();
    }
  }

  // RTC
  if(!rtcReady) rtcInit();

  // LoRa
  if(!loraReady){
    loraHardReset();
    LoRa.setSPI(SPI);
    LoRa.setPins(PIN_LORA_SS, PIN_LORA_RST, PIN_LORA_DIO0);
    LoRa.setSPIFrequency(8000000);
    if(LoRa.begin(LORA_FREQ_HZ)){
      loraReady=true; Serial.println("LoRa OK");
    } else {
      Serial.println("LoRa init failed (check pins, 3.3V, antenna, frequency).");
    }
  }

  if(hwAttempts>=hwMaxAttempts) hwDoneTrying=true;
  else hwNextTryMs = millis() + hwRetryDelayMs;
}

/* ---------- Parsers ---------- */
static bool parseLogin(AsyncWebServerRequest *request, const String &body, String &user, String &pass){
  if(request->hasParam("user",true) && request->hasParam("pass",true)){
    user=request->getParam("user",true)->value();
    pass=request->getParam("pass",true)->value();
    return true;
  }
  if(body.length()){
    StaticJsonDocument<256> doc;
    if(deserializeJson(doc, body)==DeserializationError::Ok){
      user = String(doc["username"]|doc["user"]|"");
      pass = String(doc["password"]|doc["pass"]|"");
      if(user.length() && pass.length()) return true;
    }
  }
  return false;
}

static void applyConfigFromBodyOrForm(AsyncWebServerRequest *request, const String &body){
  // Form params (if submitted as form)
  if(request->hasParam("apiUrl",true))      configApiUrl = request->getParam("apiUrl",true)->value();
  if(request->hasParam("interval",true))    configIntervalMs = request->getParam("interval",true)->value().toInt();
  if(request->hasParam("ssid",true))        configStaSSID = request->getParam("ssid",true)->value();
  if(request->hasParam("password",true))    configStaPassword = request->getParam("password",true)->value();
  if(request->hasParam("user",true))        authUser = request->getParam("user",true)->value();
  if(request->hasParam("pass",true))        authPass = request->getParam("pass",true)->value();

  // JSON body
  if(body.length()){
    StaticJsonDocument<512> doc;
    if(deserializeJson(doc, body)==DeserializationError::Ok){
      if(doc.containsKey("apiUrl"))    configApiUrl = String((const char*)doc["apiUrl"]);
      if(doc.containsKey("interval") || doc.containsKey("intervalMs"))
        configIntervalMs = (int)(doc["interval"]|doc["intervalMs"]).as<long>();
      if(doc.containsKey("ssid"))      configStaSSID = String((const char*)doc["ssid"]);
      if(doc.containsKey("password"))  configStaPassword = String((const char*)doc["password"]);
      if(doc.containsKey("user"))      authUser = String((const char*)doc["user"]);
      if(doc.containsKey("pass"))      authPass = String((const char*)doc["pass"]);
    }
  }

  // Mirror back into appCfg for saving
  appCfg.username   = authUser;
  appCfg.password   = authPass;
  appCfg.api        = configApiUrl;
  appCfg.interval   = configIntervalMs;
  appCfg.wifi_ssid  = configStaSSID;
  appCfg.wifi_pass  = configStaPassword;
}

/* ---------- Setup ---------- */
void setup(){
  Serial.begin(115200); delay(200);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.println("AP started.");
  Serial.print("IP: "); Serial.println(WiFi.softAPIP());

  initLittleFS();

  tryInitHardwareOnce();

  /* Health */
  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/plain", "pong"); });

  /* Static routes */
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r){ sendHtml(r, "/index.html"); });
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest* r){ sendHtml(r, "/index.html"); });
  server.on("/login", HTTP_GET, [](AsyncWebServerRequest* r){ sendHtml(r, "/login.html"); });
  server.on("/login.html", HTTP_GET, [](AsyncWebServerRequest* r){ sendHtml(r, "/login.html"); });
  server.on("/configuration", HTTP_GET, [](AsyncWebServerRequest* r){ sendHtml(r, "/configuration.html"); });
  server.on("/configuration.html", HTTP_GET, [](AsyncWebServerRequest* r){ sendHtml(r, "/configuration.html"); });

  server.serveStatic("/css", LittleFS, "/css");
  server.serveStatic("/js",  LittleFS, "/js");
  server.serveStatic("/favicon.ico", LittleFS, "/favicon.ico");

  /* Diagnostics */
  server.on("/api/i2c-scan", HTTP_GET, [](AsyncWebServerRequest *req){
    String out; out.reserve(512);
    struct S: public Print{ String &o; S(String& s):o(s){} size_t write(uint8_t c){o+=(char)c; return 1;} } sink(out);
    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    i2cScanOnce(sink);
    req->send(200, "text/plain", out);
  });

  server.on("/api/diag", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<512> doc;
    doc["sdReady"]=sdReady; doc["rtcReady"]=rtcReady; doc["loraReady"]=loraReady;
    doc["attempt"]=hwAttempts; doc["maxAttempts"]=hwMaxAttempts; doc["doneTrying"]=hwDoneTrying;
    doc["pins"]["SD_CS"]=PIN_SD_CS;
    doc["pins"]["LORA_SS"]=PIN_LORA_SS;
    doc["pins"]["LORA_RST"]=PIN_LORA_RST;
    doc["pins"]["LORA_DIO0"]=PIN_LORA_DIO0;
    doc["i2c"]["sda"]=PIN_I2C_SDA;
    doc["i2c"]["scl"]=PIN_I2C_SCL;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  /* Session/me/logout */
  server.on("/api/me", HTTP_GET, [](AsyncWebServerRequest *req){
    if(!isLoggedIn){ req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    StaticJsonDocument<128> doc;
    doc["user"] = authUser;
    String out; serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest *req){
    isLoggedIn = false;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  /* Login: checks config.csv */
  server.on("/api/login", HTTP_POST,
    [](AsyncWebServerRequest * /*req*/){},
    nullptr,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
      String *buf = (index==0) ? beginBodyBuffer(request,total) : getBodyBuffer(request);
      buf->concat((const char*)data, len);
      if(index+len==total){
        String u,p;
        if(parseLogin(request,*buf,u,p)){
          bool ok = false;
          if(sdReady && SD.exists(PATH_CONFIG) && readConfigCsv(appCfg)){
            ok = (u == appCfg.username && p == appCfg.password);
          } else {
            ok = (u == authUser && p == authPass); // fallback
          }
          if(ok){
            isLoggedIn = true;
            authUser = u; authPass = p;
            request->send(200, "application/json", "{\"ok\":true}");
          } else {
            request->send(401, "application/json", "{\"error\":\"invalid\"}");
          }
        } else {
          request->send(400, "application/json", "{\"error\":\"missing\"}");
        }
        endBodyBuffer(request);
      }
    }
  );

  /* Config get (reads config.csv) */
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req){
    if(!isLoggedIn){ req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    if(sdReady && SD.exists(PATH_CONFIG)) readConfigCsv(appCfg);
    StaticJsonDocument<512> doc;
    doc["user"]       = appCfg.username;
    doc["pass"]       = appCfg.password;
    doc["apiUrl"]     = appCfg.api;
    doc["intervalMs"] = appCfg.interval;
    doc["ssid"]       = appCfg.wifi_ssid;
    doc["password"]   = appCfg.wifi_pass;
    String out; serializeJson(doc,out);
    req->send(200, "application/json", out);
  });

  /* Config save (updates config.csv) */
  server.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest *req){
      if(!isLoggedIn){ req->send(403, "application/json", "{\"error\":\"unauthorized\"}"); return; }
      applyConfigFromBodyOrForm(req, "");
      if(sdReady){ writeConfigCsv(appCfg); }
      req->send(200, "application/json", "{\"ok\":true}");
    },
    nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      if(!isLoggedIn){ req->send(403, "application/json", "{\"error\":\"unauthorized\"}"); return; }
      String *buf=(index==0)?beginBodyBuffer(req,total):getBodyBuffer(req);
      buf->concat((const char*)data,len);
      if(index+len==total){
        applyConfigFromBodyOrForm(req, *buf);
        if(sdReady){ writeConfigCsv(appCfg); }
        req->send(200, "application/json", "{\"ok\":true}");
        endBodyBuffer(req);
      }
    }
  );

  /* Wi-Fi connect: also mirror to config.csv (wifi fields) */
  server.on("/api/wifi/connect", HTTP_POST,
    [](AsyncWebServerRequest *req){
      if(!isLoggedIn){ req->send(403,"application/json","{\"error\":\"unauthorized\"}"); return; }
      if(req->hasParam("ssid",true))     configStaSSID=req->getParam("ssid",true)->value();
      if(req->hasParam("password",true)) configStaPassword=req->getParam("password",true)->value();
      appCfg.wifi_ssid  = configStaSSID;
      appCfg.wifi_pass  = configStaPassword;
      if(sdReady) writeConfigCsv(appCfg);
      WiFi.begin(configStaSSID.c_str(), configStaPassword.c_str());
      staConnecting=true; staConnectStartMs=millis();
      req->send(200,"application/json","{\"ok\":true}");
    },
    nullptr,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      if(!isLoggedIn){ req->send(403,"application/json","{\"error\":\"unauthorized\"}"); return; }
      String *buf=(index==0)?beginBodyBuffer(req,total):getBodyBuffer(req);
      buf->concat((const char*)data,len);
      if(index+len==total){
        StaticJsonDocument<256> doc;
        if(deserializeJson(doc,*buf)==DeserializationError::Ok){
          configStaSSID     = String((const char*)doc["ssid"]);
          configStaPassword = String((const char*)doc["password"]);
        }
        appCfg.wifi_ssid = configStaSSID;
        appCfg.wifi_pass = configStaPassword;
        if(sdReady) writeConfigCsv(appCfg);
        WiFi.begin(configStaSSID.c_str(), configStaPassword.c_str());
        staConnecting=true; staConnectStartMs=millis();
        req->send(200,"application/json","{\"ok\":true}");
        endBodyBuffer(req);
      }
    }
  );

  server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<256> doc;
    doc["ap"]["ip"]=WiFi.softAPIP().toString();
    wl_status_t st=WiFi.status();
    bool connected=(st==WL_CONNECTED);
    doc["sta"]["connected"]=connected;
    doc["sta"]["connecting"]=staConnecting && !connected;
    if(connected){
      doc["sta"]["ssid"]=WiFi.SSID();
      doc["sta"]["ip"]=WiFi.localIP().toString();
      doc["sta"]["rssi"]=WiFi.RSSI();
      staConnecting=false;
    } else {
      if(staConnecting && millis()-staConnectStartMs>20000) staConnecting=false;
    }
    String out; serializeJson(doc,out); req->send(200,"application/json",out);
  });

  /* Logs: stream logs.csv */
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request){
    if(!(sdReady && SD.exists(PATH_LOGS))){
      if(sdReady) ensureLogsCsvExists();
    }
    String json = "[";
    bool first = true;
    if(sdReady){
      File f = SD.open(PATH_LOGS, FILE_READ);
      if(f){
        while(f.available()){
          String line = f.readStringUntil('\n'); line.trim();
          if(!line.length()) continue;
          int c1=line.indexOf(','); if(c1<0) continue;
          int c2=line.indexOf(',', c1+1); if(c2<0) continue;
          String scanner = line.substring(0,c1); scanner.trim();
          String rfid    = line.substring(c1+1,c2); rfid.trim();
          String ts      = line.substring(c2+1);    ts.trim();
          if(!first) json += ",";
          first=false;
          StaticJsonDocument<128> d;
          d["scanner_id"]=scanner;
          d["rfid"]=rfid;
          d["timestamp"]=ts;
          String item; serializeJson(d,item);
          json += item;
        }
        f.close();
      }
    }
    json += "]";
    request->send(200, "application/json", json);
  });

  /* HW status */
  server.on("/api/hw", HTTP_GET, [](AsyncWebServerRequest *req){
    StaticJsonDocument<256> doc;
    doc["sdReady"]=sdReady; doc["rtcReady"]=rtcReady; doc["loraReady"]=loraReady;
    doc["attempt"]=hwAttempts; doc["maxAttempts"]=hwMaxAttempts; doc["doneTrying"]=hwDoneTrying;
    bool serverReady = sdReady && rtcReady;
    doc["serverReady"]=serverReady;
    String out; serializeJson(doc,out); req->send(200,"application/json",out);
  });

  server.onNotFound([](AsyncWebServerRequest *request){
    String msg="Not found: "+request->url()+"\n"; request->send(404,"text/plain",msg);
  });

  server.begin();
}

void loop(){
  if(!hwDoneTrying && millis()>=hwNextTryMs) tryInitHardwareOnce();

  // Example LoRa heartbeat with RTC timestamp
  static unsigned long lastBeat=0;
  if(loraReady && millis()-lastBeat >= (unsigned long)configIntervalMs){
    lastBeat = millis();
    LoRa.beginPacket();
    LoRa.print("hb "); LoRa.print(nowTimestamp());
    LoRa.endPacket();
  }
}
