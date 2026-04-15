// ============================================================
//  DESKBUDDY_StudyTracker.ino  –  ESP32
//  v2.0  –  Full Feature Release
//
//  CHANGES IN v2.0:
//    • Captive Portal: first-boot AP mode for WiFi provisioning
//    • NVS/Preferences: credentials stored in flash, not code
//    • mDNS: accessible as http://deskbuddy.local
//    • Static IP: attempts 192.168.1.5, falls back to DHCP
//    • Boot screen simplified to "DeskBuddy / by Gaurav"
//    • Timer overflow fixed: fmtTime now handles HH:MM:SS
//    • Proximity trigger: /showTimer shows study time 2s → HAPPY
//    • OTA: web-based firmware update at /update
//    • /wifi-reset: clears NVS to re-enter captive portal mode
//    • Unified dashboard with Analytics + Settings tabs
//    • IP conflict detection with OLED alert
// ============================================================

#include <Preferences.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Update.h>

// ── Display first ──────────────────────────────────────────
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ── RoboEyes ───────────────────────────────────────────────
#include <FluxGarage_RoboEyes.h>
roboEyes roboEyes;

// ── WiFi + Web server ──────────────────────────────────────
#include <WiFi.h>
#include <WebServer.h>
WebServer server(80);   // Single instance used for both captive-portal and normal mode

// ── NVS + DNS ─────────────────────────────────────────────
Preferences prefs;
DNSServer   dnsServer;
const byte  DNS_PORT = 53;

#define BUTTON_PIN 4

// Desired static IP (falls back to DHCP if unavailable)
IPAddress staticIP(192, 168, 1, 5);
IPAddress gateway (192, 168, 1, 1);
IPAddress subnet  (255, 255, 255, 0);
IPAddress dns1    (8, 8, 8, 8);

// ============================================================
//  APP STATE
// ============================================================
enum AppMode { MODE_EYES, MODE_TIMER, MODE_STOPWATCH };
AppMode appMode = MODE_EYES;
int     modeIdx = 0;

// ── Countdown timer ─────────────────────────────────────────
unsigned long timerDuration = 60000UL;
unsigned long tmrStartMs    = 0;
unsigned long timerPausedAt = 60000UL;
bool          timerRunning  = false;
bool          timerArmed    = false;

// ── Stopwatch (palm-controlled ONLY) ────────────────────────
unsigned long swElapsed  = 0;
unsigned long swRefStart = 0;
bool          swRunning  = false;

// ── Study timer (face-detected ONLY) ────────────────────────
unsigned long studyTotalMs = 0;
unsigned long studySitMs   = 0;
bool          studySitting = false;

// ── Proximity / showTimer ────────────────────────────────────
unsigned long showTimerUntil = 0;   // millis() deadline for timer overlay

// ── Button debounce ─────────────────────────────────────────
bool          lastBtnState = HIGH;
unsigned long lastDebounce = 0;

// ── OLED redraw throttle ─────────────────────────────────────
unsigned long lastOledUpdate = 0;

// ============================================================
//  STUDY LOG  (ring-buffer, 20 entries)
// ============================================================
#define MAX_LOG_ENTRIES 20
#define LOG_LABEL_LEN   24
#define LOG_DUR_LEN     16

struct LogEntry {
  char type [6];
  char label[LOG_LABEL_LEN];
  char dur  [LOG_DUR_LEN];
};

LogEntry studyLog[MAX_LOG_ENTRIES];
int logCount = 0;
int logHead  = 0;

void logPush(const char* type, const char* label, const char* dur) {
  int idx = logHead % MAX_LOG_ENTRIES;
  strncpy(studyLog[idx].type,  type,  5);  studyLog[idx].type[5] = '\0';
  strncpy(studyLog[idx].label, label, LOG_LABEL_LEN - 1); studyLog[idx].label[LOG_LABEL_LEN-1] = '\0';
  strncpy(studyLog[idx].dur,   dur,   LOG_DUR_LEN   - 1); studyLog[idx].dur[LOG_DUR_LEN-1]     = '\0';
  logHead++;
  if (logCount < MAX_LOG_ENTRIES) logCount++;
}

// ============================================================
//  UTILITIES
// ============================================================

// FIX: properly handles hours so 01:65 → 02:05
void fmtTime(unsigned long ms, char* buf, bool centisec = false) {
  unsigned long total_s = ms / 1000;
  unsigned long hr  = total_s / 3600;
  unsigned long mn  = (total_s % 3600) / 60;
  unsigned long sec = total_s % 60;
  if (centisec) {
    unsigned long cs = (ms % 1000) / 10;
    if (hr > 0) sprintf(buf, "%02lu:%02lu:%02lu.%02lu", hr, mn, sec, cs);
    else        sprintf(buf, "%02lu:%02lu.%02lu",            mn, sec, cs);
  } else {
    if (hr > 0) sprintf(buf, "%02lu:%02lu:%02lu", hr, mn, sec);
    else        sprintf(buf, "%02lu:%02lu",            mn, sec);
  }
}

unsigned long timerRemaining() {
  if (timerRunning) {
    unsigned long el = millis() - tmrStartMs;
    if (el >= timerDuration) { timerRunning = false; return 0; }
    return timerDuration - el;
  }
  return timerArmed ? timerPausedAt : timerDuration;
}
unsigned long swCurrent()    { return swRunning    ? swElapsed    + (millis() - swRefStart) : swElapsed;    }
unsigned long studyCurrent() { return studySitting ? studyTotalMs + (millis() - studySitMs) : studyTotalMs; }

// ============================================================
//  BOOT SCREEN  –  simplified: "DeskBuddy / by Gaurav"
// ============================================================
void showBootScreen(const char* status, const char* detail = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Large title
  display.setTextSize(2);
  display.setCursor(4, 4);
  display.print("DeskBuddy");

  // Subtitle
  display.setTextSize(1);
  display.setCursor(30, 24);
  display.print("by Gaurav");

  // Divider
  display.drawLine(0, 34, 127, 34, SSD1306_WHITE);

  // Status line
  display.setCursor(4, 40);
  display.print(status);

  // Detail line
  if (strlen(detail) > 0) {
    display.setCursor(4, 52);
    display.print(detail);
  }
  display.display();
}

// ============================================================
//  CAPTIVE PORTAL  (first-boot WiFi provisioning)
// ============================================================
const char PORTAL_HTML[] PROGMEM = R"PORTAL(
<!DOCTYPE html><html><head>
<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>DeskBuddy Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#0a0a0f;color:#ddd;
     display:flex;align-items:center;justify-content:center;min-height:100vh;padding:16px}
.box{background:#13132b;border:1px solid #252545;border-radius:16px;padding:28px 22px;width:100%;max-width:360px}
h1{color:#38bdf8;font-size:20px;margin-bottom:6px}
p{color:#555;font-size:13px;margin-bottom:20px}
label{display:block;font-size:11px;color:#888;text-transform:uppercase;letter-spacing:1px;margin-bottom:5px}
input{width:100%;background:#0a0a0f;border:1px solid #252545;color:#eee;
      padding:12px;border-radius:8px;font-size:15px;margin-bottom:14px}
button{width:100%;background:#38bdf8;color:#000;border:none;padding:13px;
       border-radius:8px;font-size:15px;font-weight:700;cursor:pointer}
#msg{margin-top:14px;text-align:center;font-size:13px;color:#22c55e}
</style></head><body>
<div class='box'>
  <h1>&#128640; DeskBuddy Setup</h1>
  <p>Enter your home WiFi credentials. DeskBuddy will save them and reboot.</p>
  <label>WiFi Name (SSID)</label>
  <input id='s' type='text' placeholder='Your WiFi name' autocomplete='off'>
  <label>Password</label>
  <input id='p' type='password' placeholder='Your WiFi password'>
  <button onclick='save()'>Connect &amp; Save &#10003;</button>
  <div id='msg'></div>
</div>
<script>
function save(){
  var s=document.getElementById('s').value.trim();
  var p=document.getElementById('p').value;
  if(!s){alert('Please enter your WiFi name');return;}
  document.querySelector('button').disabled=true;
  fetch('/save?ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p))
    .then(function(){
      document.getElementById('msg').innerHTML='&#10003; Saved! DeskBuddy is rebooting&hellip;<br><small style="color:#555">Reconnect to your home WiFi, then visit<br><b>http://deskbuddy.local</b></small>';
    }).catch(function(){
      document.getElementById('msg').style.color='#ef4444';
      document.getElementById('msg').textContent='Error – try again.';
      document.querySelector('button').disabled=false;
    });
}
</script></body></html>
)PORTAL";

void serveCaptivePortal() {
  server.send_P(200, "text/html", PORTAL_HTML);
}

void saveCaptiveWiFi() {
  if (server.hasArg("ssid")) {
    prefs.begin("wifi", false);
    prefs.putString("ssid", server.arg("ssid"));
    prefs.putString("pass", server.hasArg("pass") ? server.arg("pass") : "");
    prefs.end();
    server.send(200, "text/plain", "OK");
    showBootScreen("Credentials saved!", "Rebooting...");
    delay(2500);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Missing SSID");
  }
}

void runCaptivePortal() {
  Serial.println("No WiFi saved – starting captive portal (AP: DeskBuddy-Setup)");
  showBootScreen("WiFi Setup", "DeskBuddy-Setup");

  // Ensure a clean WiFi state before switching to AP mode
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(100);

  WiFi.mode(WIFI_AP);
  WiFi.softAP("DeskBuddy-Setup");
  IPAddress apIP = WiFi.softAPIP();

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/",      serveCaptivePortal);
  server.on("/save",  saveCaptiveWiFi);
  server.onNotFound(serveCaptivePortal);  // catch all OS captive-portal checks
  server.begin();

  Serial.println("Captive portal running at " + apIP.toString());

  // Block until credentials are saved (ESP.restart() exits this)
  while (true) {
    dnsServer.processNextRequest();
    server.handleClient();
    delay(5);
  }
}

// ============================================================
//  OLED OVERLAYS
// ============================================================
void drawStudyTimerOverlay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(28, 1);
  display.print("STUDY TIME");

  char buf[20];
  fmtTime(studyCurrent(), buf);
  // Use size 2 for short times, size 1 if hrs present
  int xOff = strlen(buf) > 5 ? 10 : 20;
  display.setTextSize(2);
  display.setCursor(xOff, 18);
  display.print(buf);

  display.setTextSize(1);
  const char* st = studySitting ? "● STUDYING" : "◌ PAUSED";
  display.setCursor(30, 50);
  display.print(st);
  display.display();
}

void drawOverlay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (appMode == MODE_TIMER) {
    display.setTextSize(1);
    display.setCursor(45, 0);
    display.print("TIMER");

    unsigned long rem = timerRemaining();
    char buf[20];
    fmtTime(rem, buf);
    display.setTextSize(2);
    int x = max(0, (128 - (int)strlen(buf) * 12) / 2);
    display.setCursor(x, 16);
    display.print(buf);

    display.setTextSize(1);
    const char* st;
    if      (!timerArmed)  st = "  READY  ";
    else if (timerRunning) st = " RUNNING ";
    else if (rem == 0)     st = "  DONE!  ";
    else                   st = " PAUSED  ";
    display.setCursor((128 - (int)strlen(st) * 6) / 2, 50);
    display.print(st);

    if (timerArmed && timerDuration > 0) {
      int barW = map(constrain(rem, 0, timerDuration), 0, timerDuration, 0, 118);
      display.drawRect(5, 42, 118, 5, SSD1306_WHITE);
      display.fillRect(5, 42, barW, 5, SSD1306_WHITE);
    }

  } else {  // MODE_STOPWATCH
    display.setTextSize(1);
    display.setCursor(30, 0);
    display.print("STOPWATCH");

    char buf[20];
    fmtTime(swCurrent(), buf, true);
    display.setTextSize(2);
    int x = max(0, (128 - (int)strlen(buf) * 12) / 2);
    display.setCursor(x, 16);
    display.print(buf);

    display.setTextSize(1);
    const char* st = swRunning ? " RUNNING " : " STOPPED ";
    display.setCursor((128 - (int)strlen(st) * 6) / 2, 50);
    display.print(st);
  }
  display.display();
}

// ============================================================
//  BUTTON
// ============================================================
void checkButton() {
  bool st = digitalRead(BUTTON_PIN);
  if (st != lastBtnState && (millis() - lastDebounce) > 60) {
    lastDebounce = millis();
    if (st == LOW) {
      modeIdx = (modeIdx + 1) % 3;
      appMode = (AppMode)modeIdx;
    }
  }
  lastBtnState = st;
}

// ============================================================
//  WEB HANDLERS
// ============================================================

void handleStatus() {
  char json[320];
  sprintf(json,
    "{"
      "\"mode\":%d,"
      "\"timerDurMs\":%lu,\"timerRemMs\":%lu,\"timerRun\":%s,"
      "\"swMs\":%lu,\"swRun\":%s,"
      "\"studyMs\":%lu,\"studySitting\":%s"
    "}",
    modeIdx,
    timerDuration, timerRemaining(), timerRunning ? "true" : "false",
    swCurrent(),   swRunning        ? "true" : "false",
    studyCurrent(), studySitting    ? "true" : "false"
  );
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleMode() {
  if (server.hasArg("set")) {
    modeIdx = constrain(server.arg("set").toInt(), 0, 2);
    appMode = (AppMode)modeIdx;
  }
  server.send(200, "text/plain", "OK");
}

void handleEyes() {
  if (server.hasArg("mood")) {
    String m = server.arg("mood");
    if      (m == "DEFAULT") roboEyes.setMood(DEFAULT);
    else if (m == "HAPPY")   roboEyes.setMood(HAPPY);
    else if (m == "ANGRY")   roboEyes.setMood(ANGRY);
    else if (m == "TIRED")   roboEyes.setMood(TIRED);
  }
  if (server.hasArg("shape")) {
    String s = server.arg("shape");
    roboEyes.setHFlicker(OFF); roboEyes.setVFlicker(OFF); roboEyes.setCuriosity(OFF);
    if      (s == "curious") roboEyes.setCuriosity(ON);
    else if (s == "hflip")   roboEyes.setHFlicker(ON);
    else if (s == "vflip")   roboEyes.setVFlicker(ON);
    else if (s == "hvflip")  { roboEyes.setHFlicker(ON); roboEyes.setVFlicker(ON); }
  }
  if (server.hasArg("anim")) {
    String a = server.arg("anim");
    if      (a == "laugh")    roboEyes.anim_laugh();
    else if (a == "confused") roboEyes.anim_confused();
  }
  if (server.hasArg("blink")) roboEyes.setAutoblinker(server.arg("blink") == "on" ? ON : OFF, 3, 2);
  if (server.hasArg("idle"))  roboEyes.setIdleMode   (server.arg("idle")  == "on" ? ON : OFF, 2, 2);
  if (server.hasArg("pos")) {
    String p = server.arg("pos");
    if      (p == "N")  roboEyes.setPosition(N);
    else if (p == "NE") roboEyes.setPosition(NE);
    else if (p == "E")  roboEyes.setPosition(E);
    else if (p == "SE") roboEyes.setPosition(SE);
    else if (p == "S")  roboEyes.setPosition(S);
    else if (p == "SW") roboEyes.setPosition(SW);
    else if (p == "W")  roboEyes.setPosition(W);
    else if (p == "NW") roboEyes.setPosition(NW);
    else                roboEyes.setPosition(DEFAULT);
  }
  server.send(200, "text/plain", "OK");
}

void handleTimer() {
  if (server.hasArg("set")) {
    unsigned long secs = constrain(server.arg("set").toInt(), 1, 359999); // up to 99:59:59
    timerDuration = secs * 1000UL;
    timerPausedAt = timerDuration;
    timerRunning  = false;
    timerArmed    = false;
  }
  if (server.hasArg("action")) {
    String a = server.arg("action");
    if (a == "start") {
      if (!timerArmed) { timerPausedAt = timerDuration; timerArmed = true; }
      if (timerPausedAt > 0) { tmrStartMs = millis() - (timerDuration - timerPausedAt); timerRunning = true; }
    } else if (a == "pause" && timerRunning) {
      timerPausedAt = timerRemaining(); timerRunning = false;
    } else if (a == "reset") {
      timerRunning = false; timerArmed = false; timerPausedAt = timerDuration;
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleStopwatch() {
  if (server.hasArg("action")) {
    String a = server.arg("action");
    if      (a == "start" && !swRunning) { swRefStart = millis(); swRunning = true; }
    else if (a == "stop"  &&  swRunning) { swElapsed += millis() - swRefStart; swRunning = false; }
    else if (a == "reset")               { swRunning = false; swElapsed = 0; }
  }
  server.send(200, "text/plain", "OK");
}

// ── /presence  – face events; STUDY TIMER ONLY ────────────
void handlePresence() {
  if (server.hasArg("state")) {
    String s = server.arg("state");
    if (s == "active") {
      if (!studySitting) { studySitting = true; studySitMs = millis(); }
      // Show study time for 2s then switch to HAPPY (proximity trigger)
      showTimerUntil = millis() + 2000;
      appMode = MODE_EYES;  // ensure eyes mode for roboEyes.update()
      Serial.println("[presence] ACTIVE – study timer started");
    } else if (s == "away") {
      if (studySitting) { studyTotalMs += millis() - studySitMs; studySitting = false; }
      showTimerUntil = 0;
      roboEyes.setMood(TIRED);
      Serial.println("[presence] AWAY – study timer paused");
    } else if (s == "reset") {
      studySitting = false; studyTotalMs = 0; showTimerUntil = 0;
      Serial.println("[presence] RESET");
    }
  }
  char json[80];
  sprintf(json, "{\"studyMs\":%lu,\"studySitting\":%s}",
          studyCurrent(), studySitting ? "true" : "false");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ── /showTimer  – proximity trigger (hand OR face detected) ─
//   Shows study time on OLED for 2s, then reverts to HAPPY eyes
void handleShowTimer() {
  showTimerUntil = millis() + 2000;
  appMode = MODE_EYES;
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "OK");
}

// ── /log ──────────────────────────────────────────────────
void handleLog() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("clear")) { logCount = 0; logHead = 0; server.send(200, "text/plain", "OK"); return; }
  if (server.hasArg("type") && server.hasArg("label")) {
    String dur = server.hasArg("dur") ? server.arg("dur") : "";
    logPush(server.arg("type").c_str(), server.arg("label").c_str(), dur.c_str());
    server.send(200, "text/plain", "OK"); return;
  }
  String json = "[";
  int start = (logCount < MAX_LOG_ENTRIES) ? 0 : logHead % MAX_LOG_ENTRIES;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % MAX_LOG_ENTRIES;
    if (i) json += ",";
    json += "{\"type\":\"";  json += studyLog[idx].type;
    json += "\",\"label\":\""; json += studyLog[idx].label;
    json += "\",\"dur\":\"";  json += studyLog[idx].dur; json += "\"}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

void handleStudyStats() {
  char json[80];
  sprintf(json, "{\"totalMs\":%lu,\"sitting\":%s}",
          studyCurrent(), studySitting ? "true" : "false");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ── /wifi-reset  – clears NVS, re-enters captive portal ────
void handleWifiReset() {
  server.send(200, "text/plain", "WiFi credentials cleared. Rebooting into setup mode...");
  delay(500);
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  showBootScreen("WiFi reset!", "Rebooting...");
  delay(2000);
  ESP.restart();
}

// ── /update  – OTA firmware upload via browser ─────────────
void handleOTAPage() {
  server.send(200, "text/html",
    "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<style>body{font-family:sans-serif;background:#0a0a0f;color:#ddd;padding:30px;max-width:400px;margin:auto}"
    "h2{color:#38bdf8}input,button{display:block;width:100%;margin:10px 0;padding:12px;border-radius:8px;border:1px solid #252545}"
    "input{background:#13132b;color:#eee}button{background:#38bdf8;color:#000;font-weight:700;cursor:pointer}"
    "#r{color:#22c55e;margin-top:10px}</style></head><body>"
    "<h2>&#128640; OTA Firmware Update</h2>"
    "<p style='color:#555;font-size:13px'>Upload a compiled .bin file to update DeskBuddy firmware over WiFi.</p>"
    "<form method='POST' action='/do-update' enctype='multipart/form-data'>"
    "<input type='file' name='update' accept='.bin'>"
    "<button type='submit'>&#8593; Upload &amp; Flash</button>"
    "</form>"
    "<div id='r'></div></body></html>"
  );
}

void handleOTAUpload() {
  server.sendHeader("Connection", "close");
  if (Update.hasError()) {
    server.send(200, "text/html",
      "<html><body style='font-family:sans-serif;background:#0a0a0f;color:#ef4444;padding:30px'>"
      "<h2>&#10060; Update FAILED</h2><p>Check serial monitor for details.</p>"
      "<a href='/update' style='color:#38bdf8'>Try again</a></body></html>");
  } else {
    server.send(200, "text/html",
      "<html><body style='font-family:sans-serif;background:#0a0a0f;color:#22c55e;padding:30px'>"
      "<h2>&#10003; Update successful!</h2><p>DeskBuddy is rebooting with new firmware...</p></body></html>");
    delay(1000);
    ESP.restart();
  }
}

void handleOTADoUpdate() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
    showBootScreen("OTA Update...", upload.filename.c_str());
    if (!Update.begin()) { Update.printError(Serial); }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
    // Progress on OLED
    if (Update.size() > 0) {
      int pct = (Update.progress() * 100) / Update.size();
      char pbuf[24]; sprintf(pbuf, "Progress: %d%%", pct);
      showBootScreen("OTA Update...", pbuf);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) { Update.printError(Serial); }
    else { Serial.println("[OTA] Update complete"); showBootScreen("Update done!", "Rebooting..."); }
  }
}

// ============================================================
//  WEB PAGE  (PROGMEM)
// ============================================================
const char index_html[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE HTML><html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DeskBuddy</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#0a0a0f;color:#ddd;max-width:480px;margin:auto;padding-bottom:40px}
header{background:linear-gradient(135deg,#0d1b2a,#1b2838);padding:14px 18px;display:flex;align-items:center;justify-content:space-between}
header h1{font-size:17px;color:#38bdf8;letter-spacing:.5px}
.badge{font-size:11px;padding:3px 10px;border-radius:20px;background:#38bdf811;color:#38bdf8;border:1px solid #38bdf833}
nav{display:flex;background:#0f0f18;border-bottom:1px solid #1e1e2e;overflow-x:auto}
.tab{flex:1;min-width:60px;padding:12px 2px;text-align:center;font-size:10px;cursor:pointer;color:#666;border-bottom:2px solid transparent;transition:.2s;white-space:nowrap}
.tab.on{color:#38bdf8;border-bottom-color:#38bdf8}
.panel{display:none;padding:16px}.panel.on{display:block}
h3{font-size:11px;color:#38bdf8;text-transform:uppercase;letter-spacing:1.5px;margin:16px 0 7px}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:7px}
.g3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px}
.g4{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
.btn{background:#13131f;color:#bbb;border:1px solid #2a2a3e;padding:11px 6px;border-radius:8px;font-size:12px;cursor:pointer;transition:.12s;text-align:center}
.btn:active{background:#38bdf811;border-color:#38bdf8;color:#38bdf8}
.ok{border-color:#22c55e55;color:#22c55e}.ok:active{background:#22c55e11}
.wa{border-color:#f59e0b55;color:#f59e0b}.wa:active{background:#f59e0b11}
.da{border-color:#ef444455;color:#ef4444}.da:active{background:#ef444411}
.pu{border-color:#a78bfa55;color:#a78bfa}.pu:active{background:#a78bfa11}
hr{border:none;border-top:1px solid #1e1e2e;margin:14px 0}
.clock-card{background:#0d1117;border:1px solid #1e293b;border-radius:14px;padding:22px 14px;text-align:center;margin:6px 0 14px}
.clock-time{font-size:50px;font-weight:700;font-family:monospace;letter-spacing:2px;color:#38bdf8;line-height:1}
.clock-sub{font-size:11px;color:#555;margin-top:8px;letter-spacing:2px;text-transform:uppercase}
.prog-wrap{height:6px;background:#1e1e2e;border-radius:6px;overflow:hidden;margin:8px 0 0}
.prog-bar{height:100%;background:#38bdf8;border-radius:6px;transition:width .4s linear}
.dur-row{display:flex;gap:8px;align-items:center;margin:6px 0}
.dur-row input{flex:1;background:#13131f;border:1px solid #2a2a3e;color:#eee;padding:10px 6px;border-radius:8px;font-size:20px;text-align:center;width:60px}
.dur-row span{color:#555;font-size:12px}
.dpad{display:grid;grid-template-columns:repeat(3,52px);grid-template-rows:repeat(3,52px);gap:5px;margin:4px auto 0;width:fit-content}
.dpad-btn{background:#13131f;border:1px solid #2a2a3e;border-radius:9px;display:flex;align-items:center;justify-content:center;font-size:20px;cursor:pointer;transition:.12s}
.dpad-btn:active{background:#38bdf811;border-color:#38bdf8}
.dpad-ctr{background:#38bdf811;border-color:#38bdf833}
.trow{display:flex;justify-content:space-between;align-items:center;padding:8px 0}
.trow span{font-size:13px;color:#999}
.tog{position:relative;width:44px;height:24px;cursor:pointer;display:inline-block}
.tog input{opacity:0;width:0;height:0;position:absolute}
.tog-track{position:absolute;inset:0;background:#333;border-radius:24px;transition:.2s}
.tog-track:before{content:'';position:absolute;width:18px;height:18px;left:3px;bottom:3px;background:#fff;border-radius:50%;transition:.2s}
.tog input:checked~.tog-track{background:#38bdf8}
.tog input:checked~.tog-track:before{transform:translateX(20px)}
/* Study tab */
.study-clock-card{background:#0d1117;border:1px solid #1e293b;border-radius:14px;padding:20px 14px 16px;text-align:center;margin:6px 0 10px}
.study-big-time{font-size:48px;font-weight:700;font-family:monospace;letter-spacing:2px;line-height:1;transition:color .4s}
.study-big-time.sitting{color:#22c55e}.study-big-time.away{color:#38bdf855}
.study-sub-row{display:flex;justify-content:center;align-items:center;gap:8px;margin-top:8px}
.presence-dot{width:10px;height:10px;border-radius:50%;background:#444;transition:.3s}
.presence-dot.active{background:#22c55e;box-shadow:0 0 8px #22c55e99}
.presence-label{font-size:12px;color:#555;text-transform:uppercase;letter-spacing:2px}
.presence-label.active{color:#22c55e}
.stats-row{display:flex;gap:10px;margin-bottom:14px}
.stat-pill{flex:1;background:#0d1117;border:1px solid #1e293b;border-radius:10px;padding:12px 8px;text-align:center}
.stat-pill .val{font-size:22px;font-weight:700;font-family:monospace;color:#38bdf8}
.stat-pill .lbl{font-size:10px;color:#555;text-transform:uppercase;letter-spacing:1px;margin-top:3px}
.sw-pill .val{color:#a78bfa}
.log-table{width:100%;border-collapse:collapse;font-size:12px;margin-top:4px}
.log-table th{color:#38bdf8;text-align:left;padding:6px 8px;border-bottom:1px solid #1e2e3e;font-weight:600;font-size:10px;text-transform:uppercase;letter-spacing:1px}
.log-table td{padding:7px 8px;border-bottom:1px solid #12121e;color:#bbb}
.sit-row td:first-child::before{content:'▶ ';color:#22c55e}
.stand-row td:first-child::before{content:'■ ';color:#f59e0b}
.empty-log{text-align:center;color:#333;padding:24px 0;font-size:13px}
.hint-box{background:#0d1117;border:1px solid #1a2535;border-radius:10px;padding:12px 14px;margin-top:6px;font-size:12px;color:#4a6a8a;line-height:1.7}
.hint-box b{color:#38bdf888}
/* Settings tab */
.settings-row{display:flex;flex-direction:column;gap:8px;margin-bottom:14px}
.settings-row label{font-size:11px;color:#666;text-transform:uppercase;letter-spacing:1px;margin-bottom:2px}
.settings-input{background:#13131f;border:1px solid #2a2a3e;color:#eee;padding:10px 12px;border-radius:8px;font-size:14px;width:100%}
.danger-zone{background:#1a0a0a;border:1px solid #ef444433;border-radius:10px;padding:14px;margin-top:14px}
.danger-zone h4{color:#ef4444;font-size:11px;text-transform:uppercase;letter-spacing:1.5px;margin-bottom:10px}
</style>
</head><body>

<header>
  <h1>&#129302; DeskBuddy</h1>
  <span class="badge" id="badge">EYES</span>
</header>

<nav>
  <div class="tab on"  onclick="showTab('eyes',this)">&#128065; Eyes</div>
  <div class="tab"     onclick="showTab('timer',this)">&#9201; Timer</div>
  <div class="tab"     onclick="showTab('sw',this)">&#9202; Watch</div>
  <div class="tab"     onclick="showTab('study',this)">&#129504; Study</div>
  <div class="tab"     onclick="showTab('settings',this)">&#9881; Settings</div>
</nav>

<!-- ════ EYES ════ -->
<div class="panel on" id="tab-eyes">
  <h3>&#128534; Mood</h3>
  <div class="g4">
    <button class="btn" onclick="E('mood=DEFAULT')">&#128528; Default</button>
    <button class="btn" onclick="E('mood=HAPPY')">&#128516; Happy</button>
    <button class="btn" onclick="E('mood=ANGRY')">&#128544; Angry</button>
    <button class="btn" onclick="E('mood=TIRED')">&#128564; Tired</button>
  </div>
  <h3>&#128065; Eye Shape</h3>
  <div class="g3">
    <button class="btn" onclick="E('shape=normal')">&#9898; Normal</button>
    <button class="btn pu" onclick="E('shape=curious')">&#10067; Curious</button>
    <button class="btn" onclick="E('shape=hflip')">&#8596; H-Flip</button>
    <button class="btn" onclick="E('shape=vflip')">&#8597; V-Flip</button>
    <button class="btn" onclick="E('shape=hvflip')">&#8645; Both</button>
  </div>
  <h3>&#127916; Animations</h3>
  <div class="g2">
    <button class="btn ok" onclick="E('anim=laugh')">&#128514; Laugh</button>
    <button class="btn wa" onclick="E('anim=confused')">&#128533; Confused</button>
  </div>
  <hr>
  <div class="trow"><span>&#128064; Auto Blink</span>
    <label class="tog"><input type="checkbox" checked onchange="E('blink='+(this.checked?'on':'off'))"><span class="tog-track"></span></label>
  </div>
  <div class="trow"><span>&#128694; Idle Wander</span>
    <label class="tog"><input type="checkbox" checked onchange="E('idle='+(this.checked?'on':'off'))"><span class="tog-track"></span></label>
  </div>
  <hr>
  <h3>&#129517; Look Direction</h3>
  <div class="dpad">
    <div class="dpad-btn" onclick="E('pos=NW')">&#8598;</div>
    <div class="dpad-btn" onclick="E('pos=N')">&#8593;</div>
    <div class="dpad-btn" onclick="E('pos=NE')">&#8599;</div>
    <div class="dpad-btn" onclick="E('pos=W')">&#8592;</div>
    <div class="dpad-btn dpad-ctr" onclick="E('pos=DEFAULT')">&#11044;</div>
    <div class="dpad-btn" onclick="E('pos=E')">&#8594;</div>
    <div class="dpad-btn" onclick="E('pos=SW')">&#8601;</div>
    <div class="dpad-btn" onclick="E('pos=S')">&#8595;</div>
    <div class="dpad-btn" onclick="E('pos=SE')">&#8600;</div>
  </div>
</div>

<!-- ════ TIMER ════ -->
<div class="panel" id="tab-timer">
  <div class="clock-card">
    <div class="clock-time" id="tDisp">00:00</div>
    <div class="clock-sub"  id="tStat">READY</div>
    <div class="prog-wrap"><div class="prog-bar" id="tBar" style="width:100%"></div></div>
  </div>
  <h3>&#9201; Set Duration</h3>
  <div class="dur-row">
    <input type="number" id="tHr"  value="0" min="0" max="99" placeholder="HH">
    <span>hr</span>
    <input type="number" id="tMin" value="1" min="0" max="59" placeholder="MM">
    <span>min</span>
    <input type="number" id="tSec" value="0" min="0" max="59" placeholder="SS">
    <span>sec</span>
    <button class="btn wa" style="padding:11px 14px" onclick="setTimer()">SET</button>
  </div>
  <h3>&#9889; Quick Presets</h3>
  <div class="g4">
    <button class="btn" onclick="quickT(30)">30 s</button>
    <button class="btn" onclick="quickT(60)">1 min</button>
    <button class="btn" onclick="quickT(300)">5 min</button>
    <button class="btn" onclick="quickT(600)">10 min</button>
    <button class="btn" onclick="quickT(1500)">25 min</button>
    <button class="btn" onclick="quickT(1800)">30 min</button>
    <button class="btn" onclick="quickT(3000)">50 min</button>
    <button class="btn" onclick="quickT(3600)">1 hr</button>
  </div>
  <hr>
  <div class="g3">
    <button class="btn ok" onclick="T('start')">&#9654; Start</button>
    <button class="btn wa" onclick="T('pause')">&#9646;&#9646; Pause</button>
    <button class="btn da" onclick="T('reset')">&#8635; Reset</button>
  </div>
  <hr>
  <button class="btn pu" style="width:100%" onclick="setMode(1)">&#128250; Mirror on OLED</button>
</div>

<!-- ════ STOPWATCH ════ -->
<div class="panel" id="tab-sw">
  <div class="clock-card">
    <div class="clock-time" id="swDisp">00:00.00</div>
    <div class="clock-sub"  id="swStat">READY</div>
  </div>
  <p style="font-size:11px;color:#555;text-align:center;margin:-6px 0 14px">
    &#9996; Also controlled by open palm gesture (hold 0.8 s)
  </p>
  <div class="g3">
    <button class="btn ok" onclick="SW('start')">&#9654; Start</button>
    <button class="btn wa" onclick="SW('stop')">&#9646;&#9646; Stop</button>
    <button class="btn da" onclick="SW('reset')">&#8635; Reset</button>
  </div>
  <hr>
  <button class="btn pu" style="width:100%" onclick="setMode(2)">&#128250; Mirror on OLED</button>
</div>

<!-- ════ STUDY (unified) ════ -->
<div class="panel" id="tab-study">
  <div class="study-clock-card">
    <div class="study-big-time away" id="studyClock">00:00:00</div>
    <div class="study-sub-row">
      <div class="presence-dot" id="sDot"></div>
      <div class="presence-label" id="sLabel">AWAY</div>
    </div>
  </div>
  <div class="stats-row">
    <div class="stat-pill">
      <div class="val" id="sSessionCount">0</div>
      <div class="lbl">Sessions Today</div>
    </div>
    <div class="stat-pill sw-pill">
      <div class="val" id="sSwTime">00:00.00</div>
      <div class="lbl">&#9996; Palm Stopwatch</div>
    </div>
  </div>
  <h3>&#128203; Session Log</h3>
  <div id="logWrap">
    <div class="empty-log">No sessions yet.<br>Start <b>study_tracker.py</b> on your laptop.</div>
  </div>
  <hr>
  <div class="g2">
    <button class="btn da" onclick="clearStudy()">&#128465; Clear All</button>
    <button class="btn wa" onclick="loadLog()">&#8635; Refresh</button>
  </div>
  <div class="hint-box" style="margin-top:14px">
    <b>&#128522; Face detected</b> → Study timer starts automatically<br>
    <b>&#9996; Open palm</b> (hold 0.8s) → Palm stopwatch only<br>
    For full analytics &amp; graphs → open <b>localhost:8080</b> on your PC.
  </div>
</div>

<!-- ════ SETTINGS ════ -->
<div class="panel" id="tab-settings">
  <h3>&#127760; Network</h3>
  <p style="font-size:12px;color:#555;margin-bottom:10px">Current IP: <span id="curIP" style="color:#38bdf8;font-family:monospace">—</span></p>

  <h3>&#128640; Firmware Update (OTA)</h3>
  <p style="font-size:12px;color:#555;margin-bottom:10px">Upload a compiled .bin to update firmware over WiFi.</p>
  <a class="btn pu" style="display:block;text-align:center;padding:11px;text-decoration:none;border-radius:8px" href="/update">&#8593; Open OTA Update Page</a>

  <div class="danger-zone">
    <h4>&#9888; Danger Zone</h4>
    <p style="font-size:12px;color:#555;margin-bottom:10px">Reset WiFi credentials to re-run the first-boot setup wizard.</p>
    <button class="btn da" style="width:100%" onclick="if(confirm('Clear WiFi credentials and reboot into setup mode?')) wifiReset()">
      &#128260; Reset WiFi &amp; Setup Mode
    </button>
  </div>
</div>

<script>
function E(q)   { fetch('/eyes?'+q); }
function T(a)   { fetch('/timer?action='+a); }
function SW(a)  { fetch('/stopwatch?action='+a); }
function setMode(m){ fetch('/mode?set='+m); document.getElementById('badge').textContent=['EYES','TIMER','WATCH','STUDY'][m]; }
function wifiReset() { fetch('/wifi-reset'); setTimeout(()=>{ document.body.innerHTML='<div style="text-align:center;padding:40px;font-family:sans-serif;color:#38bdf8"><h2>Rebooting into setup mode...</h2><p style="color:#555">Connect to <b>DeskBuddy-Setup</b> WiFi network.</p></div>'; }, 400); }

function showTab(id, el){
  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('on'));
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('on'));
  document.getElementById('tab-'+id).classList.add('on');
  el.classList.add('on');
  if(id==='timer')      setMode(1);
  else if(id==='sw')    setMode(2);
  else if(id==='study') { loadLog(); setMode(0); }
  else                  setMode(0);
}

function setTimer(){
  let h=parseInt(document.getElementById('tHr').value)||0;
  let m=parseInt(document.getElementById('tMin').value)||0;
  let s=parseInt(document.getElementById('tSec').value)||0;
  let tot=h*3600+m*60+s;
  if(tot<1||tot>359999) return;
  fetch('/timer?set='+tot);
}
function quickT(s){ document.getElementById('tHr').value=Math.floor(s/3600); document.getElementById('tMin').value=Math.floor((s%3600)/60); document.getElementById('tSec').value=s%60; fetch('/timer?set='+s); }
function pad(n){ return String(n).padStart(2,'0'); }

function fmtMs(ms, cs){
  ms=Math.max(0,Math.floor(ms));
  let s=Math.floor(ms/1000),m=Math.floor(s/60),h=Math.floor(m/60);
  m%=60; s%=60;
  let r = h>0 ? pad(h)+':'+pad(m)+':'+pad(s) : pad(m)+':'+pad(s);
  if(cs) r+='.'+pad(Math.floor((ms%1000)/10));
  return r;
}

let timerRemMs=60000,timerRunning=false,timerDurMs=60000;
let swMs=0,swRunning=false;
let studyMs=0,studySitting=false;
let localTick=0,studyLocalRef=0;

function localUpdate(){
  let now=Date.now(),dt=now-localTick; localTick=now;
  if(timerRunning&&timerRemMs>0) timerRemMs=Math.max(0,timerRemMs-dt);
  let tEl=document.getElementById('tDisp'),tSt=document.getElementById('tStat'),tBr=document.getElementById('tBar');
  if(tEl) tEl.textContent=fmtMs(timerRemMs,false);
  if(tSt) tSt.textContent=timerRunning?'RUNNING':(timerRemMs===0?'DONE!':'PAUSED');
  if(tBr){let p=timerDurMs>0?Math.round(timerRemMs/timerDurMs*100):100; tBr.style.width=p+'%'; tBr.style.background=timerRemMs<10000?'#ef4444':timerRemMs<30000?'#f59e0b':'#38bdf8';}
  if(swRunning) swMs+=dt;
  let swD=document.getElementById('swDisp'),swS=document.getElementById('swStat');
  if(swD) swD.textContent=fmtMs(swMs,true);
  if(swS) swS.textContent=swRunning?'RUNNING':'STOPPED';
  let live=studySitting?studyMs+(now-studyLocalRef):studyMs;
  let clk=document.getElementById('studyClock'),dot=document.getElementById('sDot'),lbl=document.getElementById('sLabel'),swp=document.getElementById('sSwTime');
  if(clk){clk.textContent=fmtMs(live,false);clk.className='study-big-time '+(studySitting?'sitting':'away');}
  if(dot){studySitting?dot.classList.add('active'):dot.classList.remove('active');}
  if(lbl){lbl.textContent=studySitting?'STUDYING':'AWAY';studySitting?lbl.classList.add('active'):lbl.classList.remove('active');}
  if(swp) swp.textContent=fmtMs(swMs,true);
}

async function syncServer(){
  try{
    let d=await (await fetch('/status')).json();
    timerRemMs=d.timerRemMs; timerRunning=d.timerRun; timerDurMs=d.timerDurMs;
    swMs=d.swMs; swRunning=d.swRun;
    studyMs=d.studyMs; studySitting=d.studySitting; studyLocalRef=Date.now();
    document.getElementById('badge').textContent=['EYES','TIMER','WATCH','STUDY'][d.mode];
  }catch(e){}
}

async function loadLog(){
  try{
    let e=await (await fetch('/log')).json();
    let w=document.getElementById('logWrap');
    let sits=e.filter(x=>x.type==='sit').length;
    document.getElementById('sSessionCount').textContent=sits;
    if(!e||!e.length){w.innerHTML='<div class="empty-log">No sessions yet.<br>Start <b>study_tracker.py</b> on your laptop.</div>';return;}
    let h='<table class="log-table"><thead><tr><th>Event</th><th>Time</th><th>Duration</th></tr></thead><tbody>';
    for(let i=e.length-1;i>=0;i--){let x=e[i];h+=`<tr class="${x.type==='sit'?'sit-row':'stand-row'}"><td>${x.type==='sit'?'Sat down':'Got up'}</td><td>${x.label}</td><td>${x.dur||'—'}</td></tr>`;}
    h+='</tbody></table>';w.innerHTML=h;
  }catch(e){document.getElementById('logWrap').innerHTML='<div class="empty-log" style="color:#555">Could not load log.</div>';}
}

function clearStudy(){ fetch('/presence?state=reset'); fetch('/log?clear=1'); setTimeout(loadLog,300); }

// Show current IP in settings tab
fetch('/status').then(r=>r.json()).then(()=>{
  let el=document.getElementById('curIP');
  if(el) el.textContent=location.hostname;
}).catch(()=>{});

localTick=studyLocalRef=Date.now();
setInterval(localUpdate,50);
setInterval(syncServer,600);
setInterval(()=>{ if(document.getElementById('tab-study').classList.contains('on')) loadLog(); },8000);
syncServer();
</script>
</body></html>
)HTMLPAGE";

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  // Init OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 failed – halting"));
    for (;;);
  }
  display.clearDisplay(); display.display();

  // Init RoboEyes
  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 2);

  // Simplified boot screen
  showBootScreen("Starting up...");
  delay(800);

  // ── Load saved WiFi credentials from NVS ──────────────────
  prefs.begin("wifi", true);  // read-only first
  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  prefs.end();

  if (savedSSID.length() == 0) {
    // No credentials saved → enter captive portal (never returns)
    runCaptivePortal();
  }

  // ── Connect to saved WiFi ──────────────────────────────────
  showBootScreen("Connecting WiFi...", savedSSID.c_str());

  // Try preferred static IP first
  WiFi.config(staticIP, gateway, subnet, dns1);
  WiFi.begin(savedSSID.c_str(), savedPass.c_str());

  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed – running offline.");
    showBootScreen("WiFi failed.", "Running offline.");
    delay(2000);
  } else {
    String myIP = WiFi.localIP().toString();
    Serial.println("\nConnected! IP: " + myIP);

    // Check if we got the static IP or a different one (conflict/DHCP)
    if (myIP != staticIP.toString()) {
      Serial.println("Static IP unavailable – using DHCP: " + myIP);
      showBootScreen("IP conflict!", myIP.c_str());
      delay(3000);
    } else {
      showBootScreen("Connected!", myIP.c_str());
      delay(1500);
    }

    // mDNS: accessible as deskbuddy.local
    if (MDNS.begin("deskbuddy")) {
      Serial.println("mDNS: http://deskbuddy.local");
    }
  }

  // ── Register web routes ───────────────────────────────────
  server.on("/",           []() { server.send_P(200, "text/html", index_html); });
  server.on("/status",     handleStatus);
  server.on("/mode",       handleMode);
  server.on("/eyes",       handleEyes);
  server.on("/timer",      handleTimer);
  server.on("/stopwatch",  handleStopwatch);
  server.on("/presence",   handlePresence);
  server.on("/showTimer",  handleShowTimer);
  server.on("/log",        handleLog);
  server.on("/studystats", handleStudyStats);
  server.on("/wifi-reset", handleWifiReset);
  server.on("/update",     HTTP_GET,  handleOTAPage);
  server.on("/do-update",  HTTP_POST, handleOTAUpload, handleOTADoUpdate);
  server.begin();
  Serial.println("HTTP server started.");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  server.handleClient();
  checkButton();

  unsigned long now = millis();

  if (showTimerUntil > 0) {
    // Proximity trigger: show study time temporarily
    if (now < showTimerUntil) {
      if (now - lastOledUpdate >= 100) {
        lastOledUpdate = now;
        drawStudyTimerOverlay();
      }
    } else {
      // Timer display finished → switch to HAPPY
      showTimerUntil = 0;
      roboEyes.setMood(HAPPY);
      // Return to eyes mode (roboEyes.update() takes over)
    }
  } else if (appMode == MODE_EYES) {
    roboEyes.update();
  } else {
    if (now - lastOledUpdate >= 100) {
      lastOledUpdate = now;
      drawOverlay();
    }
  }
}
