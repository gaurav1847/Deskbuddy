// ============================================================
//  RoboEyes Pro – ESP32 / ESP8266  (Study Tracker Edition)
//
//  FIX 1: handlePresence() no longer touches the stopwatch.
//          Face-detected  → ONLY study timer starts.
//          Palm gesture   → ONLY stopwatch starts (via /stopwatch).
//
//  FIX 2: Study tab now shows a smooth, live-updating clock
//          (interpolated locally every 50 ms, same as stopwatch).
//          studyMs + studySitting are now part of /status JSON.
// ============================================================

// 1. Display FIRST
#include <Adafruit_SSD1306.h>
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 2. RoboEyes
#include <FluxGarage_RoboEyes.h>
roboEyes roboEyes;

// 3. WiFi + Web-server
#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266WebServer.h>
  ESP8266WebServer server(80);
#elif defined(ESP32)
  #include <WiFi.h>
  #include <WebServer.h>
  WebServer server(80);
#endif

// ── CHANGE THESE ────────────────────────────────────────────
const char* ssid     = "Airtel_201";
const char* password = "Airtel@123";
// ────────────────────────────────────────────────────────────

#define BUTTON_PIN 4

// ============================================================
//  APP STATE
// ============================================================
enum AppMode { MODE_EYES, MODE_TIMER, MODE_STOPWATCH };
AppMode appMode = MODE_EYES;
int     modeIdx = 0;

// ── Timer ───────────────────────────────────────────────────
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
unsigned long studyTotalMs = 0;   // completed sessions accumulated
unsigned long studySitMs   = 0;   // millis() when current sit began
bool          studySitting = false;

// ── Button debounce ─────────────────────────────────────────
bool          lastBtnState = HIGH;
unsigned long lastDebounce = 0;

// ── OLED redraw throttle ─────────────────────────────────────
unsigned long lastOledUpdate = 0;

// ============================================================
//  STUDY LOG
// ============================================================
#define MAX_LOG_ENTRIES 20
#define LOG_LABEL_LEN   24
#define LOG_DUR_LEN     16

struct LogEntry {
  char type[6];
  char label[LOG_LABEL_LEN];
  char dur[LOG_DUR_LEN];
};

LogEntry studyLog[MAX_LOG_ENTRIES];
int logCount = 0;
int logHead  = 0;

void logPush(const char* type, const char* label, const char* dur) {
  int idx = logHead % MAX_LOG_ENTRIES;
  strncpy(studyLog[idx].type,  type,  5);               studyLog[idx].type[5]            = '\0';
  strncpy(studyLog[idx].label, label, LOG_LABEL_LEN-1); studyLog[idx].label[LOG_LABEL_LEN-1] = '\0';
  strncpy(studyLog[idx].dur,   dur,   LOG_DUR_LEN-1);   studyLog[idx].dur[LOG_DUR_LEN-1]    = '\0';
  logHead++;
  if (logCount < MAX_LOG_ENTRIES) logCount++;
}

// ============================================================
//  UTILITIES
// ============================================================
void fmtTime(unsigned long ms, char* buf, bool centisec = false) {
  unsigned long total_s = ms / 1000;
  unsigned long min     = total_s / 60;
  unsigned long sec     = total_s % 60;
  if (centisec) {
    unsigned long cs = (ms % 1000) / 10;
    sprintf(buf, "%02lu:%02lu.%02lu", min, sec, cs);
  } else {
    sprintf(buf, "%02lu:%02lu", min, sec);
  }
}

unsigned long timerRemaining() {
  if (timerRunning) {
    unsigned long elapsed = millis() - tmrStartMs;
    if (elapsed >= timerDuration) { timerRunning = false; return 0; }
    return timerDuration - elapsed;
  }
  return timerArmed ? timerPausedAt : timerDuration;
}

unsigned long swCurrent() {
  return swRunning ? swElapsed + (millis() - swRefStart) : swElapsed;
}

// Live study milliseconds (safe to call anytime)
unsigned long studyCurrent() {
  return studySitting ? studyTotalMs + (millis() - studySitMs) : studyTotalMs;
}

// ============================================================
//  OLED OVERLAY
// ============================================================
void drawOverlay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  if (appMode == MODE_TIMER) {
    display.setTextSize(1);
    display.setCursor(45, 0);
    display.print("TIMER");

    unsigned long rem = timerRemaining();
    char buf[16];
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

  } else {   // MODE_STOPWATCH
    display.setTextSize(1);
    display.setCursor(30, 0);
    display.print("STOPWATCH");

    char buf[16];
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

// ── /status  (now includes study data for live JS interpolation) ──
void handleStatus() {
  unsigned long rem   = timerRemaining();
  unsigned long sw    = swCurrent();
  unsigned long study = studyCurrent();   // ← NEW: live study ms
  char json[300];
  sprintf(json,
    "{"
      "\"mode\":%d,"
      "\"timerDurMs\":%lu,"
      "\"timerRemMs\":%lu,"
      "\"timerRun\":%s,"
      "\"swMs\":%lu,"
      "\"swRun\":%s,"
      "\"studyMs\":%lu,"        // ← NEW
      "\"studySitting\":%s"     // ← NEW
    "}",
    modeIdx,
    timerDuration, rem,
    timerRunning ? "true" : "false",
    sw,
    swRunning ? "true" : "false",
    study,
    studySitting ? "true" : "false"
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
  if (server.hasArg("blink")) roboEyes.setAutoblinker(server.arg("blink")=="on"?ON:OFF,3,2);
  if (server.hasArg("idle"))  roboEyes.setIdleMode   (server.arg("idle") =="on"?ON:OFF,2,2);
  if (server.hasArg("pos")) {
    String p = server.arg("pos");
    if      (p=="N")  roboEyes.setPosition(N);
    else if (p=="NE") roboEyes.setPosition(NE);
    else if (p=="E")  roboEyes.setPosition(E);
    else if (p=="SE") roboEyes.setPosition(SE);
    else if (p=="S")  roboEyes.setPosition(S);
    else if (p=="SW") roboEyes.setPosition(SW);
    else if (p=="W")  roboEyes.setPosition(W);
    else if (p=="NW") roboEyes.setPosition(NW);
    else              roboEyes.setPosition(DEFAULT);
  }
  server.send(200, "text/plain", "OK");
}

void handleTimer() {
  if (server.hasArg("set")) {
    unsigned long secs = constrain(server.arg("set").toInt(), 1, 5999);
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

// ── /stopwatch  – palm-controlled ONLY, untouched by presence ─
void handleStopwatch() {
  if (server.hasArg("action")) {
    String a = server.arg("action");
    if      (a == "start" && !swRunning) { swRefStart = millis(); swRunning = true; }
    else if (a == "stop"  &&  swRunning) { swElapsed += millis() - swRefStart; swRunning = false; }
    else if (a == "reset")               { swRunning = false; swElapsed = 0; }
  }
  server.send(200, "text/plain", "OK");
}

// ── /presence  – face-detected events (STUDY TIMER ONLY) ─────
//   FIX: Removed all swRunning / swElapsed manipulation here.
//        The stopwatch is now 100% independent from face presence.
void handlePresence() {
  if (server.hasArg("state")) {
    String s = server.arg("state");

    if (s == "active") {
      // Face detected → start study timer only
      if (!studySitting) {
        studySitting = true;
        studySitMs   = millis();
      }
      roboEyes.setMood(HAPPY);
      Serial.println("[presence] ACTIVE – study timer started");

    } else if (s == "away") {
      // Face gone → pause study timer only
      if (studySitting) {
        studyTotalMs += millis() - studySitMs;
        studySitting  = false;
      }
      roboEyes.setMood(TIRED);
      Serial.println("[presence] AWAY – study timer paused");

    } else if (s == "reset") {
      studySitting = false;
      studyTotalMs = 0;
      Serial.println("[presence] RESET");
    }
  }

  // Return current study state (stopwatch state excluded intentionally)
  char json[80];
  sprintf(json,
    "{\"studyMs\":%lu,\"studySitting\":%s}",
    studyCurrent(),
    studySitting ? "true" : "false"
  );
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ── /log ─────────────────────────────────────────────────────
void handleLog() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("clear")) { logCount = 0; logHead = 0; server.send(200,"text/plain","OK"); return; }
  if (server.hasArg("type") && server.hasArg("label")) {
    String dur = server.hasArg("dur") ? server.arg("dur") : "";
    logPush(server.arg("type").c_str(), server.arg("label").c_str(), dur.c_str());
    server.send(200, "text/plain", "OK");
    return;
  }
  String json = "[";
  int start = (logCount < MAX_LOG_ENTRIES) ? 0 : logHead % MAX_LOG_ENTRIES;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % MAX_LOG_ENTRIES;
    if (i) json += ",";
    json += "{\"type\":\"";  json += studyLog[idx].type;
    json += "\",\"label\":\""; json += studyLog[idx].label;
    json += "\",\"dur\":\"";   json += studyLog[idx].dur;
    json += "\"}";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// ── /studystats ───────────────────────────────────────────────
void handleStudyStats() {
  unsigned long total = studyCurrent();
  char json[80];
  sprintf(json, "{\"totalMs\":%lu,\"sitting\":%s}", total, studySitting?"true":"false");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// ============================================================
//  WEB PAGE
// ============================================================
const char index_html[] PROGMEM = R"HTMLPAGE(
<!DOCTYPE HTML><html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>RoboEyes Pro</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:#0a0a0f;color:#ddd;max-width:480px;margin:auto;padding-bottom:40px}
header{background:linear-gradient(135deg,#0d1b2a,#1b2838);padding:14px 18px;display:flex;align-items:center;justify-content:space-between}
header h1{font-size:17px;color:#38bdf8;letter-spacing:.5px}
.badge{font-size:11px;padding:3px 10px;border-radius:20px;background:#38bdf811;color:#38bdf8;border:1px solid #38bdf833}
nav{display:flex;background:#0f0f18;border-bottom:1px solid #1e1e2e}
.tab{flex:1;padding:12px 3px;text-align:center;font-size:11px;cursor:pointer;color:#666;border-bottom:2px solid transparent;transition:.2s}
.tab.on{color:#38bdf8;border-bottom-color:#38bdf8}
.panel{display:none;padding:16px}
.panel.on{display:block}
h3{font-size:11px;color:#38bdf8;text-transform:uppercase;letter-spacing:1.5px;margin:16px 0 7px}
.g2{display:grid;grid-template-columns:1fr 1fr;gap:7px}
.g3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:7px}
.g4{display:grid;grid-template-columns:repeat(4,1fr);gap:6px}
.btn{background:#13131f;color:#bbb;border:1px solid #2a2a3e;padding:11px 6px;border-radius:8px;
     font-size:12px;cursor:pointer;transition:.12s;text-align:center;-webkit-tap-highlight-color:transparent}
.btn:active{background:#38bdf811;border-color:#38bdf8;color:#38bdf8}
.ok {border-color:#22c55e55;color:#22c55e}.ok:active{background:#22c55e11}
.wa {border-color:#f59e0b55;color:#f59e0b}.wa:active{background:#f59e0b11}
.da {border-color:#ef444455;color:#ef4444}.da:active{background:#ef444411}
.pu {border-color:#a78bfa55;color:#a78bfa}.pu:active{background:#a78bfa11}
hr{border:none;border-top:1px solid #1e1e2e;margin:14px 0}
.clock-card{background:#0d1117;border:1px solid #1e293b;border-radius:14px;padding:22px 14px;text-align:center;margin:6px 0 14px}
.clock-time{font-size:50px;font-weight:700;font-family:monospace;letter-spacing:2px;color:#38bdf8;line-height:1}
.clock-sub{font-size:11px;color:#555;margin-top:8px;letter-spacing:2px;text-transform:uppercase}
.prog-wrap{height:6px;background:#1e1e2e;border-radius:6px;overflow:hidden;margin:8px 0 0}
.prog-bar{height:100%;background:#38bdf8;border-radius:6px;transition:width .4s linear}
.dur-row{display:flex;gap:8px;align-items:center;margin:6px 0}
.dur-row input{flex:1;background:#13131f;border:1px solid #2a2a3e;color:#eee;
               padding:10px 6px;border-radius:8px;font-size:20px;text-align:center;width:70px}
.dur-row span{color:#555;font-size:12px}
.dpad{display:grid;grid-template-columns:repeat(3,52px);grid-template-rows:repeat(3,52px);gap:5px;margin:4px auto 0;width:fit-content}
.dpad-btn{background:#13131f;border:1px solid #2a2a3e;border-radius:9px;display:flex;align-items:center;justify-content:center;
          font-size:20px;cursor:pointer;transition:.12s;-webkit-tap-highlight-color:transparent}
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

/* ── Study tab ── */
.study-clock-card{background:#0d1117;border:1px solid #1e293b;border-radius:14px;padding:20px 14px 16px;text-align:center;margin:6px 0 10px}
.study-big-time{font-size:54px;font-weight:700;font-family:monospace;letter-spacing:2px;line-height:1;transition:color .4s}
.study-big-time.sitting{color:#22c55e}
.study-big-time.away{color:#38bdf855}
.study-sub-row{display:flex;justify-content:center;align-items:center;gap:8px;margin-top:8px}
.presence-dot{width:10px;height:10px;border-radius:50%;background:#444;transition:.3s;flex-shrink:0}
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
.log-table td{padding:7px 8px;border-bottom:1px solid #12121e;color:#bbb;vertical-align:middle}
.log-table tr:last-child td{border-bottom:none}
.sit-row td:first-child::before{content:'▶ ';color:#22c55e}
.stand-row td:first-child::before{content:'■ ';color:#f59e0b}
.empty-log{text-align:center;color:#333;padding:24px 0;font-size:13px}
.hint-box{background:#0d1117;border:1px solid #1a2535;border-radius:10px;padding:12px 14px;margin-top:6px;font-size:12px;color:#4a6a8a;line-height:1.7}
.hint-box b{color:#38bdf888}
.sep-label{font-size:10px;text-transform:uppercase;letter-spacing:2px;color:#444;text-align:center;margin:8px 0 4px}
</style>
</head>
<body>

<header>
  <h1>&#129302; RoboEyes Pro</h1>
  <span class="badge" id="badge">EYES</span>
</header>

<nav>
  <div class="tab on" onclick="showTab('eyes',this)">&#128065; Eyes</div>
  <div class="tab"    onclick="showTab('timer',this)">&#9201; Timer</div>
  <div class="tab"    onclick="showTab('sw',this)">&#9202; Stopwatch</div>
  <div class="tab"    onclick="showTab('study',this)">&#129504; Study</div>
</nav>

<!-- ════════════ EYES ════════════ -->
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

<!-- ════════════ TIMER ════════════ -->
<div class="panel" id="tab-timer">
  <div class="clock-card">
    <div class="clock-time" id="tDisp">00:00</div>
    <div class="clock-sub"  id="tStat">READY</div>
    <div class="prog-wrap"><div class="prog-bar" id="tBar" style="width:100%"></div></div>
  </div>
  <h3>&#9201; Set Duration</h3>
  <div class="dur-row">
    <input type="number" id="tMin" value="1" min="0" max="99" placeholder="MM">
    <span>min</span>
    <input type="number" id="tSec" value="0"  min="0" max="59" placeholder="SS">
    <span>sec</span>
    <button class="btn wa" style="padding:11px 14px" onclick="setTimer()">SET</button>
  </div>
  <h3>&#9889; Quick Presets</h3>
  <div class="g4">
    <button class="btn" onclick="quickT(30)">30 s</button>
    <button class="btn" onclick="quickT(60)">1 min</button>
    <button class="btn" onclick="quickT(180)">3 min</button>
    <button class="btn" onclick="quickT(300)">5 min</button>
    <button class="btn" onclick="quickT(600)">10 min</button>
    <button class="btn" onclick="quickT(900)">15 min</button>
    <button class="btn" onclick="quickT(1800)">30 min</button>
    <button class="btn" onclick="quickT(3600)">1 hr</button>
  </div>
  <hr>
  <div class="g3">
    <button class="btn ok" onclick="T('start')">&#9654; Start</button>
    <button class="btn wa" onclick="T('pause')">&#9646;&#9646; Pause</button>
    <button class="btn da" onclick="T('reset')">&#8635; Reset</button>
  </div>
  <hr>
  <button class="btn pu" style="width:100%;margin-top:4px" onclick="setMode(1)">&#128250; Mirror on OLED</button>
</div>

<!-- ════════════ STOPWATCH ════════════ -->
<div class="panel" id="tab-sw">
  <div class="clock-card">
    <div class="clock-time" id="swDisp">00:00.00</div>
    <div class="clock-sub"  id="swStat">READY</div>
  </div>
  <p style="font-size:11px;color:#555;text-align:center;margin:-6px 0 14px">
    ✋ Also controlled by open palm gesture (hold 0.8s)
  </p>
  <div class="g3">
    <button class="btn ok" onclick="SW('start')">&#9654; Start</button>
    <button class="btn wa" onclick="SW('stop')">&#9646;&#9646; Stop</button>
    <button class="btn da" onclick="SW('reset')">&#8635; Reset</button>
  </div>
  <hr>
  <button class="btn pu" style="width:100%;margin-top:4px" onclick="setMode(2)">&#128250; Mirror on OLED</button>
</div>

<!-- ════════════ STUDY ════════════ -->
<div class="panel" id="tab-study">

  <!-- Big live study clock -->
  <div class="study-clock-card">
    <div class="study-big-time away" id="studyClock">00:00:00</div>
    <div class="study-sub-row">
      <div class="presence-dot" id="sDot"></div>
      <div class="presence-label" id="sLabel">AWAY</div>
    </div>
  </div>

  <!-- Stat pills row: stopwatch separate -->
  <div class="stats-row">
    <div class="stat-pill">
      <div class="val" id="sSessionCount">0</div>
      <div class="lbl">Sessions Today</div>
    </div>
    <div class="stat-pill sw-pill">
      <div class="val" id="sSwTime">00:00.00</div>
      <div class="lbl">✋ Palm Stopwatch</div>
    </div>
  </div>

  <!-- Session log -->
  <h3>&#128203; Session Log</h3>
  <div id="logWrap">
    <div class="empty-log">No sessions yet.<br>Start <b>study_tracker.py</b> on your laptop.</div>
  </div>

  <hr>
  <div class="g2">
    <button class="btn da" onclick="clearStudy()">&#128465; Clear All</button>
    <button class="btn wa" onclick="loadLog()">&#8635; Refresh Log</button>
  </div>

  <div class="hint-box" style="margin-top:14px">
    <b>😊 Face detected</b> → Study timer starts automatically<br>
    <b>✋ Open palm</b> (hold 0.8s) → Palm stopwatch only<br>
    These two timers are completely independent.
  </div>
</div>

<script>
function E(q)  { fetch('/eyes?'+q); }
function T(a)  { fetch('/timer?action='+a); }
function SW(a) { fetch('/stopwatch?action='+a); }
function setMode(m){ fetch('/mode?set='+m); document.getElementById('badge').textContent=['EYES','TIMER','WATCH','STUDY'][m]; }

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
  let m=parseInt(document.getElementById('tMin').value)||0;
  let s=parseInt(document.getElementById('tSec').value)||0;
  let tot=m*60+s;
  if(tot<1||tot>5999) return;
  fetch('/timer?set='+tot);
}
function quickT(s){
  document.getElementById('tMin').value=Math.floor(s/60);
  document.getElementById('tSec').value=s%60;
  fetch('/timer?set='+s);
}

function pad(n){ return String(n).padStart(2,'0'); }

function fmtMs(ms, cs){
  let s=Math.floor(ms/1000), m=Math.floor(s/60), h=Math.floor(m/60);
  m%=60; s%=60;
  let r = h>0 ? pad(h)+':'+pad(m)+':'+pad(s) : pad(m)+':'+pad(s);
  if(cs) r+='.'+pad(Math.floor((ms%1000)/10));
  return r;
}

// ── Live state (synced from /status every 600ms) ──────────────
let timerRemMs=60000, timerRunning=false, timerDurMs=60000;
let swMs=0, swRunning=false;
let studyMs=0, studySitting=false;  // ← NEW: study state from server
let localTick=0, studyLocalRef=0;   // ← for local interpolation

function localUpdate(){
  let now=Date.now(), dt=now-localTick; localTick=now;

  // Timer
  if(timerRunning && timerRemMs>0) timerRemMs=Math.max(0,timerRemMs-dt);
  let tEl=document.getElementById('tDisp');
  let tSt=document.getElementById('tStat');
  let tBr=document.getElementById('tBar');
  if(tEl) tEl.textContent=fmtMs(timerRemMs,false);
  if(tSt) tSt.textContent=timerRunning?'RUNNING':(timerRemMs===0?'DONE!':'PAUSED');
  if(tBr){
    let pct=timerDurMs>0?Math.round(timerRemMs/timerDurMs*100):100;
    tBr.style.width=pct+'%';
    tBr.style.background=timerRemMs<10000?'#ef4444':timerRemMs<30000?'#f59e0b':'#38bdf8';
  }

  // Stopwatch (palm)
  if(swRunning) swMs+=dt;
  let swD=document.getElementById('swDisp');
  let swS=document.getElementById('swStat');
  if(swD) swD.textContent=fmtMs(swMs,true);
  if(swS) swS.textContent=swRunning?'RUNNING':'STOPPED';

  // ── Study timer (face) – smooth local interpolation ──────────
  // Add elapsed ms since last server sync if currently sitting
  let liveStudy = studySitting ? studyMs + (now - studyLocalRef) : studyMs;
  let clockEl  = document.getElementById('studyClock');
  let dotEl    = document.getElementById('sDot');
  let labelEl  = document.getElementById('sLabel');
  let swPillEl = document.getElementById('sSwTime');
  if(clockEl){
    clockEl.textContent = fmtMs(liveStudy, false);  // hh:mm:ss or mm:ss
    if(studySitting){ clockEl.className='study-big-time sitting'; }
    else            { clockEl.className='study-big-time away'; }
  }
  if(dotEl)   { studySitting ? dotEl.classList.add('active')   : dotEl.classList.remove('active'); }
  if(labelEl) { labelEl.textContent=studySitting?'STUDYING':'AWAY';
                studySitting ? labelEl.classList.add('active') : labelEl.classList.remove('active'); }
  if(swPillEl) swPillEl.textContent = fmtMs(swMs,true);
}

async function syncServer(){
  try{
    let d=await (await fetch('/status')).json();
    timerRemMs    = d.timerRemMs;
    timerRunning  = d.timerRun;
    timerDurMs    = d.timerDurMs;
    swMs          = d.swMs;
    swRunning     = d.swRun;
    studyMs       = d.studyMs;       // ← from new /status field
    studySitting  = d.studySitting;  // ← from new /status field
    studyLocalRef = Date.now();      // ← reset local interpolation anchor
    document.getElementById('badge').textContent=['EYES','TIMER','WATCH','STUDY'][d.mode];
  }catch(e){}
}

async function loadLog(){
  try{
    let entries=await (await fetch('/log')).json();
    let wrap=document.getElementById('logWrap');
    let sits=entries.filter(e=>e.type==='sit').length;
    document.getElementById('sSessionCount').textContent=sits;
    if(!entries||entries.length===0){
      wrap.innerHTML='<div class="empty-log">No sessions yet.<br>Start <b>study_tracker.py</b> on your laptop.</div>';
      return;
    }
    let html='<table class="log-table"><thead><tr><th>Event</th><th>Time</th><th>Duration</th></tr></thead><tbody>';
    for(let i=entries.length-1;i>=0;i--){
      let e=entries[i];
      let rowCls=e.type==='sit'?'sit-row':'stand-row';
      let icon=e.type==='sit'?'Sat down':'Got up';
      html+=`<tr class="${rowCls}"><td>${icon}</td><td>${e.label}</td><td>${e.dur||'—'}</td></tr>`;
    }
    html+='</tbody></table>';
    wrap.innerHTML=html;
  }catch(e){
    document.getElementById('logWrap').innerHTML='<div class="empty-log" style="color:#555">Could not load log.</div>';
  }
}

function clearStudy(){
  fetch('/presence?state=reset');
  fetch('/log?clear=1');
  setTimeout(loadLog, 300);
}

localTick = studyLocalRef = Date.now();
setInterval(localUpdate, 50);   // 20 fps – smooth clock
setInterval(syncServer, 600);   // server sync
setInterval(()=>{ if(document.getElementById('tab-study').classList.contains('on')) loadLog(); }, 8000);
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

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 failed"));
    for (;;);
  }
  display.clearDisplay(); display.display();

  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 2);

  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  display.setCursor(20, 20); display.print("Connecting WiFi..."); display.display();

  WiFi.begin(ssid, password);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) { delay(500); Serial.print("."); tries++; }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    display.clearDisplay();
    display.setCursor(0, 18); display.setTextSize(1);
    display.print("Connected!\n"); display.print(WiFi.localIP().toString());
    display.display(); delay(2000);
  } else {
    Serial.println("\nWiFi failed.");
    display.clearDisplay(); display.setCursor(10, 25);
    display.print("WiFi failed. Offline."); display.display(); delay(2000);
  }

  server.on("/",           []() { server.send(200, "text/html", index_html); });
  server.on("/status",     handleStatus);
  server.on("/mode",       handleMode);
  server.on("/eyes",       handleEyes);
  server.on("/timer",      handleTimer);
  server.on("/stopwatch",  handleStopwatch);
  server.on("/presence",   handlePresence);
  server.on("/log",        handleLog);
  server.on("/studystats", handleStudyStats);
  server.begin();
  Serial.println("HTTP server started.");
}

// ============================================================
//  LOOP
// ============================================================
void loop() {
  server.handleClient();
  checkButton();

  if (appMode == MODE_EYES) {
    roboEyes.update();
  } else {
    if (millis() - lastOledUpdate >= 100) {
      lastOledUpdate = millis();
      drawOverlay();
    }
  }
}
