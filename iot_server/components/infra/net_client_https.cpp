#include "net_client.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static bool isHttpsUrl(const String& s){ return s.startsWith("https://"); }

// Helper: try a single POST to url with given client; returns true if code>0
static bool doPostOnce(HTTPClient& http, const String& url,
                       WiFiClient* client,
                       const std::string& json,
                       int& code, std::string& resp,
                       const std::string& apiKey)
{
  bool okBegin = false;
  if (isHttpsUrl(url)) {
    okBegin = http.begin(*static_cast<WiFiClientSecure*>(client), url);
  } else {
    okBegin = http.begin(*client, url);
  }
  if (!okBegin) return false;

  http.setTimeout(8000);
  http.setReuse(false);
  // Keep HTTP/1.1 (default)
  http.addHeader("Content-Type","application/json");
  http.addHeader("Accept","*/*");
  http.addHeader("Connection","close");
  if (!apiKey.empty()){
    http.addHeader("X-API-Key", apiKey.c_str());
  }

  // Ask HTTPClient to capture the Location header (for redirects)
  static const char* hdrs[] = {"Location"};
  http.collectHeaders(hdrs, 1);

  // Use the String overload to avoid const->nonconst cast warnings
  code = http.POST(String(json.c_str()));
  resp = http.getString().c_str();

  // Debug redirect target if any
  if (code >= 300 && code < 400) {
    String loc = http.header("Location");
    if (loc.length()) {
      Serial.printf("[HTTP] Redirect %d -> %s\n", code, loc.c_str());
    }
  }

  http.end();
  return (code > 0);
}

class NetClientHttps : public NetClient {
public:
  bool postJson(const std::string& url, const std::string& json,
                int& code, std::string& resp,
                const std::string& apiKey = std::string()) override
  {
    HTTPClient http;
    WiFiClientSecure tls;
    WiFiClient plain;

    // Configure TLS (use setCACert for production)
    tls.setInsecure();
    tls.setTimeout(7000);

    // Normalize URL: many Vercel APIs 308-redirect to a trailing slash URL
    String sUrl = url.c_str();
    if (isHttpsUrl(sUrl) && !sUrl.endsWith("/")) {
      sUrl += "/";
    }

    // First attempt
    WiFiClient* client = isHttpsUrl(sUrl) ? static_cast<WiFiClient*>(&tls) : &plain;
    bool ok = doPostOnce(http, sUrl, client, json, code, resp, apiKey);
    if (!ok) return false;

    // If we still get a redirect (e.g., 308), follow once manually
    if (code >= 300 && code < 400) {
      // Re-run with Location if provided
      String location = http.header("Location"); // captured in previous call
      if (location.length()) {
        Serial.printf("[HTTP] Following redirect to: %s\n", location.c_str());

        // Choose client type based on redirected scheme
        WiFiClient* client2 = isHttpsUrl(location)
          ? static_cast<WiFiClient*>(&tls)
          : static_cast<WiFiClient*>(&plain);

        // If the Location still has no trailing slash, add it to avoid more 308s
        if (isHttpsUrl(location) && !location.endsWith("/")) {
          location += "/";
        }

        // Fresh HTTPClient instance for the second request
        HTTPClient http2;
        ok = doPostOnce(http2, location, client2, json, code, resp, apiKey);
      }
    }

    return (code > 0);
  }
};

NetClient* makeNetClientHttps(){ return new NetClientHttps(); }
