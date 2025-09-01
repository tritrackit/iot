#include "http_api.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "infra/config_store.h"    // << NEW
#include "infra/sd_fs.h"
#include "infra/log_repo.h"
#include "services/uploader_service.h"
#include <LittleFS.h>
#include <SD.h>
#include <esp_wifi.h>

// You likely already have a shared server instance in your project;
// if not, we create one here:
static AsyncWebServer server(80);
static AsyncWebServer server_alt(81); // auxiliary port for diagnostics
// STA-only mode managed in main; no AP manager is used here.
// Track last STA connect attempt/result for better UX in status
static uint32_t g_lastConnectMs = 0;
static int      g_lastConnectResult = -1;  // wl_status_t or -1 unknown
static bool     g_staConnected = false;
static uint8_t  g_staDiscReason = 0; // wifi_err_reason_t

// We need access to repo/uploader that HttpApi wraps
extern SdFsImpl SDfs;              // provided in main.cpp

// --- Simple in-memory cache for small UI assets ---
// Caches avoid repeated LittleFS opens and reduce intermittent FS timing issues
static String s_index_html, s_login_html, s_configuration_html;
static String s_styles_css, s_app_js, s_login_js, s_configuration_js;

// Global mutex to serialize LittleFS access across tasks/handlers
static SemaphoreHandle_t g_lfs_mutex = nullptr;
static inline void lfs_lock(){ if (g_lfs_mutex) xSemaphoreTake(g_lfs_mutex, portMAX_DELAY); }
static inline void lfs_unlock(){ if (g_lfs_mutex) xSemaphoreGive(g_lfs_mutex); }

// Small MIME guesser (good enough for our UI)
const char* guessMime(const String& path) {
  String p = path; p.toLowerCase();
  if (p.endsWith(".html") || p.endsWith(".htm")) return "text/html; charset=utf-8";
  if (p.endsWith(".js"))   return "application/javascript";
  if (p.endsWith(".css"))  return "text/css; charset=utf-8";
  if (p.endsWith(".json")) return "application/json; charset=utf-8";
  if (p.endsWith(".csv"))  return "text/csv; charset=utf-8";
  if (p.endsWith(".ico"))  return "image/x-icon";
  if (p.endsWith(".txt"))  return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

static bool readAllFileFS(const char* path, String& out){
  lfs_lock();
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  out.clear();
  uint32_t fsize = (uint32_t)f.size();
  out.reserve((size_t)fsize + 16);
  while (f.available()){
    char buf[512];
    size_t n = f.readBytes(buf, sizeof(buf) - 1); // leave room for NUL
    buf[n] = '\0';
    if (n) out.concat(buf);
    else break;
  }
  f.close();
  bool ok = (out.length() == (size_t)fsize);
  lfs_unlock();
  return ok;
}

// Simple helpers
static void sendJson(AsyncWebServerRequest* req, int code, const JsonVariantConst& v){
  String s; serializeJson(v, s);
  req->send(code, "application/json", s);
}
// Put these in the same helper section near the top of http_api_async.cpp
static void sendJsonText(AsyncWebServerRequest* req, int code, const String& body) {
  auto* resp = req->beginResponse(code, "application/json; charset=utf-8", body);
  resp->addHeader("Cache-Control", "no-store");
  req->send(resp);
}
static void sendJsonText(AsyncWebServerRequest* req, int code, const char* text){
  req->send(code, "application/json", text);
}
static bool timeIsValid(){
  time_t now = time(nullptr);
  return now > 1700000000; // ~2023-11-14
}
static const char* wlStatusName(wl_status_t st){
  switch(st){
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID";
    case WL_SCAN_COMPLETED: return "SCAN_DONE";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

static const char* authModeName(wifi_auth_mode_t m){
  switch(m){
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
    default: return "UNKNOWN";
  }
}
static void splitLinesFromTail(const String& chunk, std::vector<String>& out, bool appendToFirst) {
  int start = 0;
  while (true) {
    int nl = chunk.indexOf('\n', start);
    if (nl < 0) {
      String tail = chunk.substring(start);
      if (appendToFirst && !out.empty()) {
        out.front() = tail + out.front();
      } else {
        if (tail.length()) out.insert(out.begin(), tail);
      }
      break;
    }
    String line = chunk.substring(start, nl); // no '\n'
    // push to front to preserve reverse scan
    out.insert(out.begin(), line);
    start = nl + 1;
  }
}

// ---- REST ----
static void installWifiRoutes(ConfigStore& store){
  // GET /api/wifi/status
  server.on("/api/wifi/status", HTTP_GET, [&](AsyncWebServerRequest* req){
    StaticJsonDocument<384> doc;
    auto ap = doc.createNestedObject("ap");
    ap["ssid"] = WiFi.softAPSSID();
    ap["ip"]   = WiFi.softAPIP().toString();

    auto sta = doc.createNestedObject("sta");
    wl_status_t st = WiFi.status();
    bool connected = (st==WL_CONNECTED);
    sta["connected"] = connected;
    sta["connecting"] = false;
    sta["status"] = (int)st;
    sta["status_name"] = wlStatusName(st);
    if (!connected){
      switch(st){
        case WL_NO_SSID_AVAIL: sta["error"] = "NO_SSID"; break;
        case WL_CONNECT_FAILED: sta["error"] = "CONNECT_FAILED"; break;
        case WL_CONNECTION_LOST: sta["error"] = "CONNECTION_LOST"; break;
        default: break;
      }
    }
    if (connected){
      sta["ssid"] = WiFi.SSID();
      sta["ip"]   = WiFi.localIP().toString();
      sta["rssi"] = WiFi.RSSI();
    }

    doc["time_valid"] = timeIsValid();
    sendJson(req, 200, doc.as<JsonVariantConst>());
  });

  // GET /api/wifi/scan  -> non-blocking scan of nearby APs
  server.on("/api/wifi/scan", HTTP_GET, [&](AsyncWebServerRequest* req){
    int st = WiFi.scanComplete();
    if (st == WIFI_SCAN_RUNNING || st == -1){
      // start scan if not running
      if (st == -1) WiFi.scanNetworks(/*async*/true, /*hidden*/true);
      req->send(200, "application/json", "{\"running\":true}");
      return;
    }
    if (st >= 0){
      StaticJsonDocument<4096> out;
      JsonArray arr = out.to<JsonArray>();
      for (int i=0;i<st;i++){
        JsonObject o = arr.createNestedObject();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["channel"] = WiFi.channel(i);
        o["hidden"] = (WiFi.SSID(i).length() == 0);
        o["auth"] = authModeName((wifi_auth_mode_t)WiFi.encryptionType(i));
      }
      // free results
      WiFi.scanDelete();
      sendJson(req, 200, out.as<JsonVariantConst>());
      return;
    }
    // Should not happen; ensure we re-trigger
    WiFi.scanNetworks(/*async*/true, /*hidden*/true);
    req->send(200, "application/json", "{\"running\":true}");
  });

  // POST /api/wifi/save  {ssid, password|pass} -> persist STA creds into SD:/config.json
  server.on("/api/wifi/save", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [&](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total){
      String* buf = (index==0) ? new String() : (String*)req->_tempObject;
      if(index==0){ buf->reserve(total+16); req->_tempObject = buf; }
      buf->concat((const char*)data, len);
      if (index + len != total) return;
      StaticJsonDocument<256> in;
      bool ok=false;
      if (!deserializeJson(in, *buf)){
        const char* ssid = in["ssid"] | "";
        const char* pass = in["password"] | (const char*)(in["pass"] | "");
        StaticJsonDocument<512> cfg;
        const char* CFG_JSON = "/config.json";
        { String raw; if (SDfs.readAll(CFG_JSON, raw)) deserializeJson(cfg, raw); }
        cfg["wifi_sta_ssid"] = ssid;
        cfg["wifi_sta_password"] = pass;
        { String tmp; serializeJson(cfg, tmp); ok = SDfs.writeAll(CFG_JSON, tmp); }
      }
      delete buf; req->_tempObject=nullptr;
      if(!ok){ req->send(500, "text/plain", "save failed"); return; }
      req->send(200, "text/plain", "ok");
    });

  // POST /api/wifi/connect  {ssid?, pass?}
  // If body is empty, use saved config
  server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [&](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total){
      String ssid, pass;
      String* buf = (index==0) ? new String() : (String*)req->_tempObject;
      if(index==0){ buf->reserve(total+16); req->_tempObject = buf; }
      buf->concat((const char*)data, len);
      if (index + len != total) return;
      if(buf->length()){
        StaticJsonDocument<256> doc;
        if(!deserializeJson(doc, *buf)){
          ssid = (const char*) (doc["ssid"] | "");
          const char* pw1 = doc["password"] | nullptr;
          const char* pw2 = doc["pass"] | nullptr;
          pass = pw1 ? pw1 : (pw2 ? pw2 : "");
        }
      }
      delete buf; req->_tempObject=nullptr;
      // Fill missing fields from saved creds on SD:/config.json
      if(!ssid.length() || !pass.length()){
        const char* CFG_JSON = "/config.json";
        String raw; if (SDfs.readAll(CFG_JSON, raw)){
          StaticJsonDocument<256> d; if(!deserializeJson(d,raw)){
            if(!ssid.length()) ssid = (const char*)(d["wifi_sta_ssid"] | "");
            if(!pass.length()) pass = (const char*)(d["wifi_sta_password"] | "");
          }
        }
      }
      if(!ssid.length()){ req->send(400, "text/plain", "no ssid"); return; }
      if(!pass.length()){
        // Avoid attempting with empty password; ask client to send or save credentials first
        req->send(422, "application/json", "{\"error\":\"missing_password\"}");
        return;
      }

      // Connect STA while keeping AP running to avoid disconnecting clients
      struct Cfg { String ssid; String pass; };
      auto* cfgp = new Cfg{ ssid, pass };
      auto worker = +[](void* arg){
        std::unique_ptr<Cfg> c((Cfg*)arg);
        // Ensure dual mode; keep AP active for the portal
        WiFi.mode(WIFI_AP_STA);
        WiFi.setSleep(false);
        esp_wifi_set_ps(WIFI_PS_NONE);
        Serial.printf("[WIFI] Connect STA '%s' (pwlen=%u)\n", c->ssid.c_str(), (unsigned)c->pass.length());
        WiFi.begin(c->ssid.c_str(), c->pass.c_str());
        wl_status_t res = (wl_status_t)WiFi.waitForConnectResult(20000);
        Serial.printf("[WIFI] Result=%d status=%d\n", (int)res, (int)WiFi.status());
        vTaskDelete(nullptr);
      };
      xTaskCreate(worker, "sta_connect", 4096, cfgp, 1, nullptr);

      StaticJsonDocument<128> doc;
      doc["ok"] = true;
      doc["status"] = (int)WiFi.status();
      doc["status_name"] = wlStatusName(WiFi.status());
      sendJson(req, 200, doc.as<JsonVariantConst>());
    });

  // GET /api/wifi/creds  -> debug helper to read saved wifi.json
  server.on("/api/wifi/creds", HTTP_GET, [&](AsyncWebServerRequest* req){
    WifiCfg c; ConfigStore st;
    StaticJsonDocument<256> out;
    if (st.loadWifi(c)){
      out["ssid"] = c.ssid.c_str();
      out["present"] = true;
      out["len"] = (uint32_t)c.pass.size();
    } else {
      out["present"] = false;
    }
    sendJson(req, 200, out.as<JsonVariantConst>());
  });

  // POST /api/wifi/disconnect
  server.on("/api/wifi/disconnect", HTTP_POST, [&](AsyncWebServerRequest* req){
    WiFi.disconnect(false /*keep AP*/);
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("Device-Setup","12345678"); 
    req->send(200, "text/plain", "ok");
  });

  // POST /api/time/sync  -> set time for TLS when STA is connected
  server.on("/api/time/sync", HTTP_POST, [&](AsyncWebServerRequest* req){
    if(WiFi.status()!=WL_CONNECTED){ req->send(409, "text/plain", "sta not connected"); return; }
    configTime(0,0,"pool.ntp.org","time.google.com");
    // don't block forever
    uint32_t t0=millis();
    while(!timeIsValid() && (millis()-t0)<10000){ delay(200); }
    StaticJsonDocument<128> doc;
    doc["time_valid"] = timeIsValid();
    sendJson(req, 200, doc.as<JsonVariantConst>());
  });
}

bool HttpApi::begin(){
  static bool installed=false;
  if(installed) return true;
  installed=true;
  Serial.println("[HTTP] init begin()");
  // Initialize WiFi manager (AP + events). Keep AP available for UI and do not pause AP during connect.
  // STA-only: no AP setup here.

  // Wi-Fi event hooks for accurate status
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
    switch(event){
      case ARDUINO_EVENT_WIFI_STA_CONNECTED:
        g_staConnected = false; // wait for IP
        Serial.println("[WIFI] STA_CONNECTED");
        break;
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        g_staConnected = true;
        g_lastConnectResult = (int)WL_CONNECTED;
        Serial.printf("[WIFI] GOT_IP: %s\n", WiFi.localIP().toString().c_str());
        break;
      case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
        g_staConnected = false;
        g_staDiscReason = info.wifi_sta_disconnected.reason;
        Serial.printf("[WIFI] DISCONNECTED reason=%u\n", (unsigned)g_staDiscReason);
        break;
      default: break;
    }
  });

  // Existing routes you already have…
  // (e.g., logs list, upload control, etc.)

  // Mount LittleFS for static frontend (data/ -> LittleFS)
  // Auto-format on first failure (Arduino-ESP32 supports LittleFS.begin(true))
  static bool lfs_ok = LittleFS.begin(true);
  if (!lfs_ok) {
    Serial.println("[HTTP] LittleFS mount failed");
  } else {
    Serial.println("[HTTP] LittleFS mounted OK");
    // Quick listing of root to aid field debugging (first few entries)
    lfs_lock();
    File rt = LittleFS.open("/", "r");
    if (rt) {
      uint8_t shown=0; Serial.print("[HTTP] FS: /"); Serial.println();
      for (File e = rt.openNextFile(); e && shown<8; e = rt.openNextFile()){
        Serial.printf("[HTTP]   %s %s %u\n", e.isDirectory()?"<DIR>":"FILE ", e.name(), (unsigned)e.size());
        e.close();
        shown++;
      }
      rt.close();
    }
    lfs_unlock();
  }
  if (lfs_ok) { lfs_lock(); LittleFS.mkdir("/js"); lfs_unlock(); }

  static ConfigStore cfg; // uses LittleFS for wifi.json
  // Ensure a default config.json exists so reads don't spam errors on first boot
  if (lfs_ok) {
    const char* kCfg = "/config.json";
    lfs_lock();
    bool exists = LittleFS.exists(kCfg);
    if (!exists) {
      File f = LittleFS.open(kCfg, "w");
      if (f) {
        StaticJsonDocument<256> d;
        d["user"] = "admin";
        d["pass"] = "admin";
        d["apiUrl"] = "";
        d["intervalMs"] = 15000;
        serializeJson(d, f);
        f.close();
      }
    }
    lfs_unlock();
  }
  // Ensure SD:/config.json exists with sane defaults at boot (not only on API read)
  {
    SDfs.lock();
    bool mounted = SDfs.isMounted();
    if (mounted){
      const char* CFG_JSON = "/config.json";
      String raw;
      bool have = SDfs.readAll(CFG_JSON, raw);
      StaticJsonDocument<512> d;
      if (have){
        // try parse; if it fails, rewrite defaults
        if (deserializeJson(d, raw)) have = false;
      }
      if (!have){
        // defaults
        d.clear();
        d["auth_user"]        = "admin";
        d["auth_password"]    = "admin";
        d["wifi_ap_ssid"]     = "Device-Portal";
        d["wifi_ap_password"] = "12345678";
        d["wifi_sta_ssid"]    = "";
        d["wifi_sta_password"]= "";
        d["api_url"]          = "";
        d["upload_interval"]  = 0;
        String out; serializeJson(d, out);
        SDfs.writeAll(CFG_JSON, out);
      }
    }
    SDfs.unlock();
  }

  installWifiRoutes(cfg);

  // === Auth + Config + Logs (for frontend) ===
  static bool isLoggedIn = false;
  static String authUser = "admin";
  static String authPass = "admin";
  const char* CFG_JSON = "/config.json";

  auto hasSession = [](AsyncWebServerRequest* req){
    if (req->hasHeader("Cookie")){
      auto* h = req->getHeader("Cookie");
      String v = h ? h->value() : String();
      return v.indexOf("SID=1") >= 0;
    }
    return false;
  };

  auto loadAuthFromFile = [&](){
    if (!lfs_ok) return;
    File f = LittleFS.open(CFG_JSON, "r"); if(!f) return;
    StaticJsonDocument<256> d; if (deserializeJson(d,f)) { f.close(); return; }
    f.close();
    authUser = String((const char*)(d["user"] | authUser.c_str()));
    authPass = String((const char*)(d["pass"] | authPass.c_str()));
  };
  loadAuthFromFile();

  // Apply saved uploader cfg on boot if present (from SD-backed config)
  {
    String raw;
    if (SDfs.readAll("/config.json", raw)){
      StaticJsonDocument<512> d;
      if (!deserializeJson(d, raw)){
        UploadCfg uc;
        uc.api = (const char*)(d["api_url"] | d["apiUrl"] | "");
        uc.interval_ms = (uint32_t)(d["upload_interval"] | d["intervalMs"] | 0);
        uc.batch_size = 10;
        up_.set(uc);
      }
    }
  }

  auto saveAuthToFile = [&](){
    if (!lfs_ok) return;
    // merge with existing
    StaticJsonDocument<256> d;
    File fr = LittleFS.open(CFG_JSON, "r");
    if (fr){ deserializeJson(d, fr); fr.close(); }
    d["user"] = authUser;
    d["pass"] = authPass;
    File fw = LittleFS.open(CFG_JSON, "w"); if(!fw) return;
    serializeJson(d, fw); fw.close();
  };

  // /api/login
  server.on("/api/login", HTTP_POST,
    [](AsyncWebServerRequest* /*req*/){}, nullptr,
    [&, hasSession](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
      StaticJsonDocument<256> d; if (deserializeJson(d, data, len)) { sendJsonText(req,400,"{\"error\":\"missing\"}"); return; }
      String u = String((const char*)(d["username"] | ""));
      String p = String((const char*)(d["password"] | ""));
      if (u == authUser && p == authPass){
        isLoggedIn = true;
        auto* resp = req->beginResponse(200, "application/json", "{\"ok\":true}");
        resp->addHeader("Set-Cookie", "SID=1; Path=/");
        req->send(resp);
      } else {
        sendJsonText(req,401,"{\"error\":\"invalid\"}");
      }
    }
  );

  // /api/logout
  server.on("/api/logout", HTTP_POST, [&, hasSession](AsyncWebServerRequest* req){
    isLoggedIn = false;
    auto* resp = req->beginResponse(200, "application/json", "{\"ok\":true}");
    resp->addHeader("Set-Cookie", "SID=; Max-Age=0; Path=/");
    req->send(resp);
  });

  // /api/me
  server.on("/api/me", HTTP_GET, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) { sendJsonText(req,401,"{\"error\":\"unauthorized\"}"); return; }
    StaticJsonDocument<128> d; d["user"] = authUser; sendJson(req,200,d.as<JsonVariantConst>());
  });

  // /api/config GET (stored on SD as /config.json) with legacy mapping + default fill
  server.on("/api/config", HTTP_GET, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) { sendJsonText(req,401,"{\"error\":\"unauthorized\"}"); return; }
    const char* CFG_JSON = "/config.json";
    StaticJsonDocument<512> src; bool existed=false;
    {
      String raw;
      if (SDfs.readAll(CFG_JSON, raw)) { deserializeJson(src, raw); existed=true; }
    }

    // Build normalized view with defaults and legacy fallbacks
    StaticJsonDocument<512> out;
    out["auth_user"]        = src["auth_user"]        | src["user"]        | "admin";
    out["auth_password"]    = src["auth_password"]    | src["pass"]        | "admin";
    out["wifi_ap_ssid"]     = src["wifi_ap_ssid"]     | "Device-Portal";
    out["wifi_ap_password"] = src["wifi_ap_password"] | "12345678";
    out["wifi_sta_ssid"]    = src["wifi_sta_ssid"]    | src["ssid"]       | "";
    out["wifi_sta_password"]= src["wifi_sta_password"]| src["password"]   | "";
    out["api_url"]          = src["api_url"]          | src["apiUrl"]     | "";
    out["upload_interval"]  = src["upload_interval"]  | src["intervalMs"] | 0;

    // Persist migration/backfill so future reads see normalized keys
    if (!existed ||
        !src.containsKey("auth_user") || !src.containsKey("auth_password") ||
        !src.containsKey("wifi_ap_ssid") || !src.containsKey("wifi_ap_password") ||
        !src.containsKey("wifi_sta_ssid") || !src.containsKey("wifi_sta_password") ||
        !src.containsKey("api_url") || !src.containsKey("upload_interval")){
      String tmp; serializeJson(out, tmp);
      SDfs.writeAll(CFG_JSON, tmp);
    }

    sendJson(req,200,out.as<JsonVariantConst>());
  });

  // /api/config POST (partial updates; persist on SD)
  server.on("/api/config", HTTP_POST,
    [&, hasSession](AsyncWebServerRequest* req){ if (!(isLoggedIn || hasSession(req))) { sendJsonText(req,403,"{\"error\":\"unauthorized\"}"); return; } },
    nullptr,
    [&, hasSession, saveAuthToFile, this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t){
      if (!(isLoggedIn || hasSession(req))) { sendJsonText(req,403,"{\"error\":\"unauthorized\"}"); return; }
      StaticJsonDocument<512> in; if (deserializeJson(in,data,len)) { sendJsonText(req,400,"{\"error\":\"bad json\"}"); return; }
      // merge existing
      StaticJsonDocument<512> cfgDoc;
      const char* CFG_JSON = "/config.json";
      {
        String raw; if (SDfs.readAll(CFG_JSON, raw)) { deserializeJson(cfgDoc, raw); }
      }

      bool authChanged=false, uploaderChanged=false, apChanged=false;

      // Type-scoped updates: "auth" | "ap" | "sta" | "api". If missing, allow all (back-compat)
      String type = String((const char*)(in["type"] | ""));
      bool allowAuth = !type.length() || type == "auth";
      bool allowAp   = !type.length() || type == "ap";
      bool allowSta  = !type.length() || type == "sta";
      bool allowApi  = !type.length() || type == "api";

      // Track previous AP values to detect change
      String prevApSsid = String((const char*)(cfgDoc["wifi_ap_ssid"] | ""));
      String prevApPass = String((const char*)(cfgDoc["wifi_ap_password"] | ""));

      // New fields (SD-backed), scoped by type
      if (allowAuth && in.containsKey("auth_user")) { authUser = String((const char*)in["auth_user"]); cfgDoc["auth_user"] = authUser; authChanged=true; }
      if (allowAuth && in.containsKey("auth_password")) { authPass = String((const char*)in["auth_password"]); cfgDoc["auth_password"] = authPass; authChanged=true; }
      if (allowAp   && in.containsKey("wifi_ap_ssid")) cfgDoc["wifi_ap_ssid"] = (const char*)in["wifi_ap_ssid"];
      if (allowAp   && in.containsKey("wifi_ap_password")) cfgDoc["wifi_ap_password"] = (const char*)in["wifi_ap_password"];
      if (allowSta  && in.containsKey("wifi_sta_ssid")) cfgDoc["wifi_sta_ssid"] = (const char*)in["wifi_sta_ssid"];
      if (allowSta  && in.containsKey("wifi_sta_password")) cfgDoc["wifi_sta_password"] = (const char*)in["wifi_sta_password"];
      if (allowApi  && in.containsKey("api_url")) { cfgDoc["api_url"] = (const char*)in["api_url"]; uploaderChanged=true; }
      if (allowApi  && in.containsKey("upload_interval")) { cfgDoc["upload_interval"] = (uint32_t)in["upload_interval"]; uploaderChanged=true; }

      // Legacy keys (map to new), scoped by type
      if (allowAuth && in.containsKey("user")) { authUser = String((const char*)in["user"]); cfgDoc["auth_user"] = authUser; authChanged=true; }
      if (allowAuth && in.containsKey("pass")) { authPass = String((const char*)in["pass"]); cfgDoc["auth_password"] = authPass; authChanged=true; }
      if (allowSta  && in.containsKey("ssid")) { cfgDoc["wifi_sta_ssid"] = (const char*)in["ssid"]; }
      if (allowSta  && in.containsKey("password")) { cfgDoc["wifi_sta_password"] = (const char*)in["password"]; }
      if (allowApi  && in.containsKey("apiUrl")) { cfgDoc["api_url"] = (const char*)in["apiUrl"]; uploaderChanged=true; }
      if (allowApi  && in.containsKey("intervalMs")) { cfgDoc["upload_interval"] = (uint32_t)in["intervalMs"]; uploaderChanged=true; }

      // Detect AP changes (will not apply immediately to avoid dropping client connection)
      if (allowAp){
        String newApSsid = String((const char*)(cfgDoc["wifi_ap_ssid"] | ""));
        String newApPass = String((const char*)(cfgDoc["wifi_ap_password"] | ""));
        apChanged = (newApSsid != prevApSsid) || (newApPass != prevApPass);
      }

      { String tmp; serializeJson(cfgDoc, tmp); SDfs.writeAll(CFG_JSON, tmp); }
      if (authChanged) saveAuthToFile();

      if (uploaderChanged){
        UploadCfg uc; uc.api = (const char*)(cfgDoc["api_url"] | ""); uc.interval_ms = (uint32_t)(cfgDoc["upload_interval"] | 0); uc.batch_size = 10; 
        up_.set(uc);
      }

      // Do NOT apply AP changes immediately; avoid disconnecting the client.
      // Frontend can prompt for manual reboot if desired.

      StaticJsonDocument<128> resp;
      resp["ok"] = true;
      if (apChanged) resp["ap_change_pending"] = true;
      if (uploaderChanged) resp["uploader_updated"] = true;
      sendJson(req,200,resp.as<JsonVariantConst>());
    }
  );

  // /api/logs  -> list UNSENT items from spool (LOG.<rfid>.<YYYYMMDDHHMMSS>.<scanner>)
  server.on("/api/logs", HTTP_GET, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) {
      sendJsonText(req, 401, "{\"error\":\"unauthorized\"}");
      return;
    }

    // ---- params ----
    size_t limit = 100;
    if (req->hasParam("limit")) {
      long v = req->getParam("limit")->value().toInt();
      if (v > 0 && v < 2000) limit = (size_t)v;
    }

    // ---- helpers ----
    auto basenameOf = [](const String& path)->String {
      int p = path.lastIndexOf('/');
      return (p >= 0) ? path.substring(p+1) : path;
    };

    // filename: LOG.<rfid>.<YYYYMMDDHHMMSS>.<scanner>[.N]
    auto parseSpoolName = [](const String& base, String& rfid, String& ts14, String& scanner)->bool {
      if (!base.startsWith("LOG.")) return false;

      int d1 = base.indexOf('.', 4);            // after "LOG."
      if (d1 < 0) return false;
      int d2 = base.indexOf('.', d1 + 1);       // after ts14
      if (d2 < 0) return false;

      rfid = base.substring(4, d1);
      ts14 = base.substring(d1 + 1, d2);

      // scanner may have optional collision suffix ".N" -> strip it if present
      int d3 = base.indexOf('.', d2 + 1);
      scanner = (d3 < 0) ? base.substring(d2 + 1)
                        : base.substring(d2 + 1, d3);

      // validate pieces
      if (rfid.isEmpty() || scanner.isEmpty()) return false;
      if (ts14.length() != 14) return false;
      for (size_t i=0;i<ts14.length();++i) {
        if (ts14[i] < '0' || ts14[i] > '9') return false;
      }
      return true;
    };

    auto ts14ToIso = [](const String& ts14)->String {
      // ts14 = YYYYMMDDHHMMSS
      if (ts14.length() != 14) return String("");
      String iso;
      iso.reserve(19);
      iso  = ts14.substring(0,4)  + "-"  // YYYY-
          +  ts14.substring(4,6)  + "-"  // MM-
          +  ts14.substring(6,8)  + " "  // DD␠
          +  ts14.substring(8,10) + ":"  // HH:
          +  ts14.substring(10,12)+ ":"  // MM:
          +  ts14.substring(12,14);      // SS
      return iso;
    };

    struct Item { String scanner; String rfid; String ts14; String fname; };

    std::vector<Item> items;
    items.reserve(limit ? limit : 64);

    // ---- scan spool dir ----
    const char* kSpoolDir = "/spool";
    SDfs.lock();
    File dir = SD.open(kSpoolDir);
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      SDfs.unlock();
      sendJsonText(req, 200, "[]");
      return;
    }

    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      if (!f.isDirectory()) {
        String base = basenameOf(f.name());
        String rfid, ts14, scanner;
        if (parseSpoolName(base, rfid, ts14, scanner)) {
          items.push_back({scanner, rfid, ts14, base});
        }
      }
      f.close();
      // soft cap on scanning so we don't spend forever in a huge dir
      if (items.size() >= (limit ? (limit * 8) : 800)) break;
    }
    dir.close();
    SDfs.unlock();

    // ---- sort newest -> oldest by timestamp (then scanner, then rfid) ----
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b){
      int tc = a.ts14.compareTo(b.ts14);
      if (tc != 0) return tc > 0; // newer first
      int sc = a.scanner.compareTo(b.scanner);
      if (sc != 0) return sc < 0;
      return a.rfid.compareTo(b.rfid) < 0;
    });

    // ---- limit to requested count ----
    if (items.size() > limit) items.resize(limit);

    // ---- render JSON ----
    StaticJsonDocument<16384> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& it : items) {
      JsonObject o = arr.createNestedObject();
      o["scanner_id"] = it.scanner;
      o["rfid"]       = it.rfid;
      o["timestamp"]  = ts14ToIso(it.ts14);  // derived from filename
      o["code"]       = 0;
      o["msg"]        = "";
    }
    String out; serializeJson(arr, out);
    sendJsonText(req, 200, out);
  });

  // POST /api/logs/reset
  // Clears SD:/spool of all pending LOG.<rfid>.<scanner> files.
  // Also removes /upload.cursor (legacy) if present.
  server.on("/api/logs/reset", HTTP_POST, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) {
      sendJsonText(req, 401, "{\"error\":\"unauthorized\"}");
      return;
    }

    const char* kSpoolDir = "/spool";
    const char* kCursor   = "/upload.cursor";

    size_t removed = 0;
    uint64_t bytesFreed = 0;
    bool cursorDeleted = false;

    SDfs.lock();

    if (!SDfs.isMounted()) {
      SDfs.unlock();
      sendJsonText(req, 500, "{\"error\":\"sd_not_mounted\"}");
      return;
    }

    // Open spool dir (create if missing to keep system consistent)
    File dir = SD.open(kSpoolDir);
    if (!dir) {
      // No dir yet -> just ensure it exists after this call
      SD.mkdir(kSpoolDir);
    } else if (!dir.isDirectory()) {
      // Something odd is at /spool; try to clean and recreate
      dir.close();
      (void)SD.remove(kSpoolDir);
      SD.mkdir(kSpoolDir);
    } else {
      // Iterate and delete files inside /spool
      for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) { f.close(); continue; }
        // capture info before close
        String name = f.name();        // may be "spool/..." or "/spool/..."
        uint64_t sz = f.size();
        f.close();

        // Build absolute path if needed
        String full = name;
        if (!full.startsWith("/")) full = String(kSpoolDir) + "/" + full;

        if (SD.remove(full)) {
          removed++;
          bytesFreed += sz;
        }
      }
      dir.close();
    }

    // Make sure /spool exists after reset
    SD.mkdir(kSpoolDir);

    // Remove legacy cursor file (safe even if unused in spool-mode)
    cursorDeleted = SD.remove(kCursor);

    SDfs.unlock();

    // Response
    StaticJsonDocument<256> d;
    d["ok"]             = true;
    d["spool_cleared"]  = removed;                  // number of files deleted
    d["bytes_freed"]    = (uint32_t)bytesFreed;     // truncated to 32-bit for payload
    d["cursor_deleted"] = cursorDeleted;

    String out; serializeJson(d, out);
    sendJsonText(req, 200, out);
  });

  // === Uploader controls ===
  server.on("/api/upload/status", HTTP_GET, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    StaticJsonDocument<256> d;
    d["enabled"] = up_.isEnabled();

    // Prefer in-memory cfg; if missing, lazily hydrate from SD:/config.json
    String api_url = String(up_.cfg().api.c_str());
    uint32_t interval_ms = (uint32_t)up_.cfg().interval_ms;
    if (api_url.length()==0 || interval_ms==0){
      String raw; StaticJsonDocument<512> cfg;
      if (SDfs.readAll("/config.json", raw) && !deserializeJson(cfg, raw)){
        String sd_api = String((const char*)(cfg["api_url"] | cfg["apiUrl"] | ""));
        uint32_t sd_int = (uint32_t)(cfg["upload_interval"] | cfg["intervalMs"] | 0);
        if (api_url.length()==0) api_url = sd_api;
        if (interval_ms==0) interval_ms = sd_int;
        // Keep uploader in sync for subsequent operations
        if (api_url.length() || interval_ms){ UploadCfg uc = up_.cfg(); uc.api = api_url.c_str(); uc.interval_ms = interval_ms; up_.set(uc); }
      }
    }

    d["api_url"] = api_url;
    d["interval_ms"] = interval_ms;
    bool sta_connected = (WiFi.status() == WL_CONNECTED);
    d["sta_connected"] = sta_connected;

    bool valid = (api_url.length() > 0 && interval_ms > 1000 && sta_connected);
    if (valid){
      String api = api_url; api.toLowerCase();
      if (api.indexOf("localhost")>=0 || api.indexOf("127.0.0.1")>=0){ valid = false; d["reason"] = "api_url_localhost_unreachable_from_device"; }
    }
    if (!valid){
      if (!d.containsKey("reason")){
        if (api_url.length()==0) d["reason"] = "missing_api_url";
        else if (interval_ms <= 1000) d["reason"] = "interval_too_low";
        else if (!sta_connected) d["reason"] = "sta_not_connected";
      }
    }
    d["valid"] = valid;
    sendJson(req,200,d.as<JsonVariantConst>());
  });

  // /api/upload/start  -> start uploader (SPOOL by default), non-blocking
  server.on("/api/upload/start", HTTP_POST, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) {
      req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
      return;
    }

    // ---- Build effective config from stored /config.json if fields are missing
    UploadCfg uc = up_.cfg();
    if (uc.api.empty() || uc.interval_ms == 0){
      String raw;
      JsonDocument cfg;
      if (SDfs.readAll("/config.json", raw) == true && !deserializeJson(cfg, raw)) {
        if (uc.api.empty()) {
          uc.api = (const char*)(cfg["api_url"] | cfg["apiUrl"] | "");
        }
        uint32_t iv = (uint32_t)(cfg["upload_interval"] | cfg["intervalMs"] | 0);
        if (iv) uc.interval_ms = iv;
      }
    }

    // ---- Spool is the default source
    uc.use_sd_spool = true;
    uc.spool_dir    = uc.spool_dir.length() ? uc.spool_dir : String(F("/spool"));

    // Optional overrides from query/body params
    if (req->hasParam("source")) {
      String src = req->getParam("source")->value();
      // "repo" -> disable SD spool (fallback to in-memory repo)
      uc.use_sd_spool = !src.equalsIgnoreCase("repo");
    }
    if (req->hasParam("dir")) {
      String d = req->getParam("dir")->value();
      if (!d.startsWith("/")) d = "/" + d;
      uc.spool_dir = d;
    }
    if (req->hasParam("batch")) {
      long v = req->getParam("batch")->value().toInt();
      if (v > 0 && v <= 500) uc.batch_size = (size_t)v;
    }
    if (req->hasParam("intervalMs")) {
      long v = req->getParam("intervalMs")->value().toInt();
      if (v >= 1000) uc.interval_ms = (uint32_t)v;
    }

    // ---- Validate
    if (uc.api.empty()){ req->send(400, "application/json", "{\"error\":\"missing_api_url\"}"); return; }
    if (uc.interval_ms < 1000){ req->send(400, "application/json", "{\"error\":\"interval_too_low\"}"); return; }
    if (WiFi.status() != WL_CONNECTED){ req->send(409, "application/json", "{\"error\":\"sta_not_connected\"}"); return; }
    {
      String api = uc.api.c_str(); api.toLowerCase();
      if (api.indexOf("localhost")>=0 || api.indexOf("127.0.0.1")>=0){
        req->send(400, "application/json", "{\"error\":\"api_url_localhost_unreachable_from_device\"}");
        return;
      }
    }

    // If using SD spool, make sure SD is mounted and directory exists
    if (uc.use_sd_spool) {
      SDfs.lock();
      bool mounted = SDfs.isMounted();
      if (mounted) {
        if (!SD.exists(uc.spool_dir.c_str())) {
          SD.mkdir(uc.spool_dir.c_str());
        }
      }
      SDfs.unlock();
      if (!mounted) { req->send(500, "application/json", "{\"error\":\"sd_not_mounted\"}"); return; }
    }

    // ---- Respond immediately (do not block HTTP task)
    {
      StaticJsonDocument<256> resp;
      resp["ok"] = true;
      resp["started"] = true;
      resp["mode"] = uc.use_sd_spool ? "spool" : "repo";
      resp["spool_dir"] = uc.use_sd_spool ? uc.spool_dir : "";
      String out; serializeJson(resp, out);
      req->send(202, "application/json", out);
    }

    // ---- Kick the worker
    up_.set(uc);
    up_.setEnabled(true);
    up_.armWarmup(1500);   // short grace to avoid racing immediately after HTTP route
    up_.ensureTask();

    Serial.printf("[UPLOAD] Start request: api='%s' interval=%ums batch=%u mode=%s spool_dir='%s'\n",
      uc.api.c_str(), (unsigned)uc.interval_ms, (unsigned)uc.batch_size,
      uc.use_sd_spool ? "spool" : "repo", uc.spool_dir.c_str());
    Serial.println("[UPLOAD] Started");
  });


  server.on("/api/upload/stop", HTTP_POST, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    up_.setEnabled(false);
    req->send(200, "application/json", "{\"ok\":true,\"started\":false}");
  });

  // Debug: last upload attempt summary
  server.on("/api/upload/last", HTTP_GET, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    const auto& d = up_.debug();
    StaticJsonDocument<256> j;
    j["last_ms"] = d.last_ms;
    j["code"] = d.code;
    j["success"] = d.success;
    j["error"] = d.error.c_str();
    j["sent"] = (uint32_t)d.sent;
    j["resp_size"] = (uint32_t)d.resp_size;
    j["url"] = d.url.c_str();
    j["scanner"] = d.scanner.c_str();
    j["items"] = (uint32_t)d.items;
    j["payload"] = d.array_body ? "array" : "object";
    sendJson(req,200,j.as<JsonVariantConst>());
  });

  // Health
  server.on("/ping", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/plain", "pong"); });
  // Quick check route that does not depend on FS/auth
  server.on("/alive", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/plain", "alive"); });
  // Alt port for tough environments where :80 is blocked or conflicted
  server_alt.on("/alive81", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/plain", "alive81"); });
  server.on("/api/reboot", HTTP_POST, [&, hasSession](AsyncWebServerRequest* r){
    if (!(isLoggedIn || hasSession(r))) { r->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    // Clear login cookie so next load requires auth
    auto* resp = r->beginResponse(200, "text/plain", "rebooting");
    resp->addHeader("Cache-Control", "no-store");
    resp->addHeader("Set-Cookie", "SID=; Max-Age=0; Path=/");
    r->send(resp);
    delay(250);
    ESP.restart();
  });

  // SD card: list files and status
  server.on("/api/sd/status", HTTP_GET, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) { sendJsonText(req,401,"{\"error\":\"unauthorized\"}"); return; }
    StaticJsonDocument<128> d;
    SDfs.lock();
    bool mounted = SDfs.isMounted();
    d["mounted"] = mounted;
    if (mounted){
      File r = SD.open("/");
      bool ok = (bool)r;
      d["root_open"] = ok;
      if (ok) r.close();
    }
    SDfs.unlock();
    sendJson(req, 200, d.as<JsonVariantConst>());
  });

  server.on("/api/sd/list", HTTP_GET, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) { req->send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    SDfs.lock();
    bool mounted = SDfs.isMounted();
    String path = "/";
    if (req->hasParam("path")) { path = req->getParam("path")->value(); }
    if (!path.length()) path = "/";
    if (path[0] != '/') path = String("/") + path;

    if (!mounted){
      String out = "{\"mounted\":false,\"path\":\"" + path + "\",\"items\":[]}";
      SDfs.unlock();
      req->send(200, "application/json", out);
      return;
    }

    // Try direct path; if it fails and path doesn't include mount point, try with "/sd" prefix
    File f = SD.open(path.c_str(), "r");
    if (!f){
      if (!path.startsWith("/sd")){
        String alt = String("/sd") + (path=="/"? String("") : path);
        f = SD.open(alt.c_str(), "r");
        if (f) path = alt; // update reporting path
      }
    }
    if (!f){
      String out = "{\"mounted\":true,\"path\":\"" + path + "\",\"error\":\"open_failed\"}";
      SDfs.unlock();
      req->send(404, "application/json", out);
      return;
    }

    String out;
    out.reserve(1024);
    if (!f.isDirectory()){
      // By default, do NOT stream the whole file to avoid long blocking.
      // Return metadata unless raw=1 is explicitly requested.
      bool wantRaw = req->hasParam("raw");
      if (!wantRaw){
        uint32_t sz = (uint32_t)f.size();
        f.close(); SDfs.unlock();
        out = "{\"mounted\":true,\"path\":\"" + path + "\",\"isFile\":true,\"size\":" + String(sz) + "}";
        req->send(200, "application/json", out);
        return;
      }
      // raw content with limits and cooperative yields
      String lower = path; lower.toLowerCase();
      String ctype = "application/octet-stream";
      if (lower.endsWith(".json")) ctype = "application/json";
      else if (lower.endsWith(".csv")) ctype = "text/csv";
      else if (lower.endsWith(".txt")) ctype = "text/plain";

      size_t maxBytes = 65536; // 64KB cap by default
      if (req->hasParam("max")){
        maxBytes = (size_t) strtoul(req->getParam("max")->value().c_str(), nullptr, 10);
        if (maxBytes < 1024) maxBytes = 1024; if (maxBytes > 200000) maxBytes = 200000;
      }
      String content; content.reserve(4096);
      size_t readBytes = 0; uint32_t iter = 0;
      while (f.available() && readBytes < maxBytes){
        char buf[512];
        size_t n = f.readBytes(buf, sizeof(buf));
        if (!n) break;
        if (readBytes + n > maxBytes) n = maxBytes - readBytes;
        content.concat(String(buf).substring(0, n));
        readBytes += n;
        if ((++iter % 8) == 0) { delay(0); }
      }
      f.close(); SDfs.unlock();
      auto* resp = req->beginResponse(200, ctype, content);
      resp->addHeader("Cache-Control", "no-store");
      req->send(resp);
      return;
    }

    out = "{\"mounted\":true,\"path\":\"" + path + "\",\"items\":[";
    uint32_t count = 0;
    uint32_t iter = 0;
    for (File e = f.openNextFile(); e; e = f.openNextFile()){
      if (count++) out += ",";
      out += "{\"name\":\"";
      out += String(e.name());
      out += "\",\"dir\":";
      out += e.isDirectory()? "true" : "false";
      out += ",\"size\":";
      out += String((uint32_t)e.size());
      out += "}";
      e.close();
      if ((++iter % 16) == 0) { delay(0); }
      if (count >= 500) break; // prevent overly large payloads
    }
    f.close(); SDfs.unlock();
    out += "]}";
    req->send(200, "application/json", out);
  });

  
  // GET /api/file?fs=sd|lfs&path=/file&dl=1
  server.on("/api/file", HTTP_GET, [&, hasSession](AsyncWebServerRequest* req){
    if (!(isLoggedIn || hasSession(req))) {
      sendJsonText(req, 401, "{\"error\":\"unauthorized\"}");
      return;
    }

    String fs  = req->hasParam("fs")   ? req->getParam("fs")->value()   : "sd";
    String path= req->hasParam("path") ? req->getParam("path")->value() : "";
    bool dl    = req->hasParam("dl");

    // Basic validation + sanitization
    path.trim();
    if (path.length() == 0) { sendJsonText(req, 400, "{\"error\":\"missing_path\"}"); return; }
    if (!path.startsWith("/")) path = "/" + path;
    if (path.indexOf("..") >= 0) { sendJsonText(req, 400, "{\"error\":\"invalid_path\"}"); return; }

    String data;
    bool ok = false;

    // Default to SD; allow explicit "lfs" / "littlefs"
    if (fs.equalsIgnoreCase("sd")) {
      ok = SDfs.readAll(path.c_str(), data);
    } else if (fs.equalsIgnoreCase("lfs") || fs.equalsIgnoreCase("littlefs")) {
      ok = readAllFileFS(path.c_str(), data);
    } else {
      sendJsonText(req, 400, "{\"error\":\"invalid_fs\"}");
      return;
    }

    if (!ok) {
      // Not found or failed to read
      sendJsonText(req, 404, "{\"error\":\"open_failed\"}");
      return;
    }

    const char* mime = guessMime(path);

    // Stream back the file content
    auto* resp = req->beginResponse(200, mime, data);
    resp->addHeader("Cache-Control", "no-store");
    if (dl) {
      // Force download if requested
      String fname = path.substring(path.lastIndexOf('/') + 1);
      if (!fname.length()) fname = "download.bin";
      resp->addHeader("Content-Disposition", String("attachment; filename=\"") + fname + "\"");
    }
    req->send(resp);
  });

  // Captive portal helpers for Android/iOS/Windows
  server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(204); });
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/html", "<html><body>OK</body></html>"); });
  server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(200, "text/plain", "Microsoft NCSI"); });

  // Static routes for frontend
  // Determine LittleFS availability for static assets
  if (!g_lfs_mutex) g_lfs_mutex = xSemaphoreCreateMutex();
  bool lfs_root_ok = false, lfs_js_ok = false, lfs_css_ok = false;
  {
    lfs_lock();
    File rtest = LittleFS.open("/", "r");
    if (rtest){ lfs_root_ok = true; rtest.close(); }
    File j = LittleFS.open("/js", "r"); if (j){ lfs_js_ok = j.isDirectory(); j.close(); }
    File c = LittleFS.open("/css", "r"); if (c){ lfs_css_ok = c.isDirectory(); c.close(); }
    lfs_unlock();
  }

  // Preload small UI assets into RAM to improve reliability/perf
  if (lfs_root_ok){
    readAllFileFS("/index.html", s_index_html);
    readAllFileFS("/login.html", s_login_html);
    readAllFileFS("/configuration.html", s_configuration_html);
  }
  if (lfs_css_ok){ readAllFileFS("/css/styles.css", s_styles_css); }
  if (lfs_js_ok){
    readAllFileFS("/js/app.js", s_app_js);
    readAllFileFS("/js/login.js", s_login_js);
    readAllFileFS("/js/configuration.js", s_configuration_js);
  }

  if (lfs_root_ok){
    auto sendInline = [](AsyncWebServerRequest* r, const char* body){
      auto* resp = r->beginResponse(200, "text/html", body);
      resp->addHeader("Cache-Control", "no-store");
      r->send(resp);
    };
    server.on("/", HTTP_GET, [=](AsyncWebServerRequest* r){
      if (!s_index_html.length()){
        String tmp; if (readAllFileFS("/index.html", tmp)) s_index_html = tmp;
      }
      if (s_index_html.length()){
        auto* resp = r->beginResponse(200, "text/html", s_index_html);
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        r->send(resp); return;
      }
      sendInline(r, "<html><body>UI missing: index.html</body></html>");
    });
    server.on("/index.html", HTTP_GET, [=](AsyncWebServerRequest* r){
      if (!s_index_html.length()){
        String tmp; if (readAllFileFS("/index.html", tmp)) s_index_html = tmp;
      }
      if (s_index_html.length()){
        auto* resp = r->beginResponse(200, "text/html", s_index_html);
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        r->send(resp); return;
      }
      sendInline(r, "<html><body>UI missing: index.html</body></html>");
    });
    server.on("/login", HTTP_GET, [=](AsyncWebServerRequest* r){
      if (!s_login_html.length()){
        String tmp; if (readAllFileFS("/login.html", tmp)) s_login_html = tmp;
      }
      if (s_login_html.length()){
        auto* resp = r->beginResponse(200, "text/html", s_login_html);
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        r->send(resp); return;
      }
      sendInline(r, "<html><body>UI missing: login.html</body></html>");
    });
    server.on("/login.html", HTTP_GET, [=](AsyncWebServerRequest* r){
      if (!s_login_html.length()){
        String tmp; if (readAllFileFS("/login.html", tmp)) s_login_html = tmp;
      }
      if (s_login_html.length()){
        auto* resp = r->beginResponse(200, "text/html", s_login_html);
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        r->send(resp); return;
      }
      sendInline(r, "<html><body>UI missing: login.html</body></html>");
    });
    server.on("/configuration", HTTP_GET, [=](AsyncWebServerRequest* r){
      if (!s_configuration_html.length()){
        String tmp; if (readAllFileFS("/configuration.html", tmp)) s_configuration_html = tmp;
      }
      if (s_configuration_html.length()){
        auto* resp = r->beginResponse(200, "text/html", s_configuration_html);
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        r->send(resp); return;
      }
      sendInline(r, "<html><body>UI missing: configuration.html</body></html>");
    });
    server.on("/configuration.html", HTTP_GET, [=](AsyncWebServerRequest* r){
      if (!s_configuration_html.length()){
        String tmp; if (readAllFileFS("/configuration.html", tmp)) s_configuration_html = tmp;
      }
      if (s_configuration_html.length()){
        auto* resp = r->beginResponse(200, "text/html", s_configuration_html);
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Connection", "close");
        r->send(resp); return;
      }
      sendInline(r, "<html><body>UI missing: configuration.html</body></html>");
    });

    // Explicit key assets (avoid catch-all/static directory handlers)
    if (lfs_css_ok){
      server.on("/css/styles.css", HTTP_GET, [=](AsyncWebServerRequest* r){
        if (!s_styles_css.length()){
          String tmp; if (readAllFileFS("/css/styles.css", tmp)) s_styles_css = tmp;
        }
        if (s_styles_css.length()){
          auto* resp = r->beginResponse(200, "text/css", s_styles_css);
          resp->addHeader("Cache-Control", "no-store");
          resp->addHeader("Connection", "close");
          r->send(resp); return;
        }
        r->send(404, "text/plain", "styles.css not found");
      });
    }
    if (lfs_js_ok){
      server.on("/js/app.js", HTTP_GET, [=](AsyncWebServerRequest* r){
        if (!s_app_js.length()){
          String tmp; if (readAllFileFS("/js/app.js", tmp)) s_app_js = tmp;
        }
        if (s_app_js.length()){
          auto* resp = r->beginResponse(200, "application/javascript", s_app_js);
          resp->addHeader("Cache-Control", "no-store");
          resp->addHeader("Connection", "close");
          r->send(resp); return;
        }
        r->send(404, "text/plain", "app.js not found");
      });
      server.on("/js/login.js", HTTP_GET, [=](AsyncWebServerRequest* r){
        if (!s_login_js.length()){
          String tmp; if (readAllFileFS("/js/login.js", tmp)) s_login_js = tmp;
        }
        if (s_login_js.length()){
          auto* resp = r->beginResponse(200, "application/javascript", s_login_js);
          resp->addHeader("Cache-Control", "no-store");
          resp->addHeader("Connection", "close");
          r->send(resp); return;
        }
        r->send(404, "text/plain", "login.js not found");
      });
    }
  } else {
    // Minimal inline pages to avoid touching LittleFS when not present
    auto sendInline = [](AsyncWebServerRequest* r, const char* body){
      auto* resp = r->beginResponse(200, "text/html", body);
      resp->addHeader("Cache-Control", "no-store");
      r->send(resp);
    };
    server.on("/", HTTP_GET, [=](AsyncWebServerRequest* r){ sendInline(r, "<html><body><h3>UI not installed</h3><p>Please upload LittleFS data.</p></body></html>"); });
    server.on("/index.html", HTTP_GET, [=](AsyncWebServerRequest* r){ sendInline(r, "<html><body>UI missing</body></html>"); });
    server.on("/login", HTTP_GET, [=](AsyncWebServerRequest* r){ sendInline(r, "<html><body>UI missing</body></html>"); });
    server.on("/login.html", HTTP_GET, [=](AsyncWebServerRequest* r){ sendInline(r, "<html><body>UI missing</body></html>"); });
    server.on("/configuration", HTTP_GET, [=](AsyncWebServerRequest* r){ sendInline(r, "<html><body>UI missing</body></html>"); });
    server.on("/configuration.html", HTTP_GET, [=](AsyncWebServerRequest* r){ sendInline(r, "<html><body>UI missing</body></html>"); });
  }
  if (lfs_root_ok && LittleFS.exists("/favicon.ico")){
    server.serveStatic("/favicon.ico", LittleFS, "/favicon.ico").setCacheControl("no-store");
  } else {
    server.on("/favicon.ico", HTTP_GET, [](AsyncWebServerRequest* r){ r->send(404, "text/plain", "not found"); });
  }

  // IMPORTANT: Do not install a catch-all static handler for "/".
  // It can swallow API routes like /api/* and cause AsyncWebServer to try
  // to open non-existent files (e.g., /api/fs/list) from LittleFS, which
  // leads to errors and can crash on some framework versions.
  // We already serve explicit pages (/, /index.html, /login, /configuration)
  // above, and static assets from /css and /js.

  // Explicit static route for configuration.js
  server.on("/js/configuration.js", HTTP_GET, [=](AsyncWebServerRequest* r){
    if (!s_configuration_js.length()){
      String tmp; if (readAllFileFS("/js/configuration.js", tmp)) s_configuration_js = tmp;
    }
    if (s_configuration_js.length()){
      auto* resp = r->beginResponse(200, "application/javascript", s_configuration_js);
      resp->addHeader("Cache-Control", "no-store");
      resp->addHeader("Connection", "close");
      r->send(resp); return;
    }
    r->send(404, "text/plain", "configuration.js not found");
  });

  // LittleFS debug listing (optional): /api/fs/list?path=/js
  server.on("/api/fs/list", HTTP_GET, [](AsyncWebServerRequest* req){
    String path = "/";
    if (req->hasParam("path")) path = req->getParam("path")->value();
    if (!path.length()) path = "/";
    // Normalize: ensure leading slash; strip trailing slash except root
    if (path[0] != '/') path = String("/") + path;
    while (path.length() > 1 && path.endsWith("/")) path.remove(path.length()-1);

    lfs_lock();
    File f = LittleFS.open(path.c_str(), "r");
    if (!f){
      lfs_unlock();
      auto* r = req->beginResponse(404, "application/json", "{\"error\":\"open_failed\"}");
      r->addHeader("Cache-Control", "no-store");
      req->send(r);
      return;
    }

    if (!f.isDirectory()){
      AsyncResponseStream* res = req->beginResponseStream("application/json");
      res->addHeader("Cache-Control", "no-store");
      res->print("{\"path\":\""); res->print(path); res->print("\",\"file\":{");
      res->print("\"name\":\""); res->print(f.name()); res->print("\",");
      res->print("\"size\":"); res->print((uint32_t)f.size()); res->print(",");
      res->print("\"dir\":false}}");
      f.close();
      lfs_unlock();
      req->send(res);
      return;
    }

    AsyncResponseStream* res = req->beginResponseStream("application/json");
    res->addHeader("Cache-Control", "no-store");
    res->print("{\"path\":\""); res->print(path); res->print("\",\"items\":[");
    uint32_t count = 0;
    for (File e = f.openNextFile(); e; e = f.openNextFile()){
      if (count++) res->print(",");
      res->print("{\"name\":\""); res->print(e.name()); res->print("\",");
      res->print("\"dir\":"); res->print(e.isDirectory()? "true" : "false"); res->print(",");
      res->print("\"size\":"); res->print((uint32_t)e.size()); res->print("}");
      e.close();
      if (count >= 500) break;
    }
    f.close();
    res->print("]}");
    lfs_unlock();
    req->send(res);
  });

  server.on("/api/fs/read", HTTP_GET, [](AsyncWebServerRequest* req){
    String path = "/";
    if (req->hasParam("path")) path = req->getParam("path")->value();
    if (!path.length()) path = "/";
    if (path[0] != '/') path = String("/") + path;
    lfs_lock();
    File f = LittleFS.open(path.c_str(), "r");
    if (!f){
      lfs_unlock();
      auto* r = req->beginResponse(404, "text/plain", "open_failed");
      r->addHeader("Cache-Control", "no-store");
      req->send(r);
      return;
    }
    String out;
    out.reserve(600);
    out += "size="; out += String((uint32_t)f.size()); out += "\n";
    out += "preview=\n";
    char buf[513]; size_t n = f.readBytes(buf, 512); buf[n] = 0; out += buf;
    f.close();
    lfs_unlock();
    auto* r = req->beginResponse(200, "text/plain", out);
    r->addHeader("Cache-Control", "no-store");
    req->send(r);
  });

  // Helpful 404
  server.onNotFound([](AsyncWebServerRequest* req){
    req->send(404, "text/plain", "Not found. Try /, /login, /configuration or /api/wifi/status");
  });

  server.begin();
  server_alt.begin();
  Serial.printf("[HTTP] server started on port 80. AP=%s STA=%s\n",
                WiFi.softAPIP().toString().c_str(), WiFi.localIP().toString().c_str());
  Serial.println("[HTTP] alt server started on port 81 (GET /alive81)");
  return true;
}
