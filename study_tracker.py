"""
study_tracker.py  –  DeskBuddy Study Tracker  v2.1
====================================================
Compatible with mediapipe >= 0.10  (Tasks API)

CHANGES IN v2.0:
  • Auto-reconnect: monitors ESP32, resumes on power-cycle without restart
  • Proximity emoji: face/hand → show timer 2s → HAPPY; neither → TIRED
  • Daily/monthly/hourly analytics stored on PC, served via API
  • Unified dashboard at localhost:8080 with Chart.js analytics graphs
  • ESP32 status proxied into the PC dashboard (one unified view)
  • New API routes: /api/analytics/daily, /monthly, /hourly, /api/esp32/status

INSTALL:
    pip install -r requirements.txt

RUN:
    python study_tracker.py
    python study_tracker.py --preview
"""

import cv2
import numpy as np
import time
import json
import threading
import requests
import argparse
import sys
import logging
import urllib.request
import csv
import io
from datetime import datetime, date, timedelta
from pathlib import Path
from collections import defaultdict

# ── Config ──────────────────────────────────────────────────
from config import (
    ESP32_IP, AWAY_TIMEOUT, CHECK_FPS, CAMERA_INDEX,
    FACE_CONFIDENCE, HAND_CONFIDENCE, PALM_HOLD_SECS,
    CONFIRM_FRAMES, DASHBOARD_PORT, SESSION_FILE,
    FACE_MODEL_PATH, HAND_MODEL_PATH,
    FACE_MODEL_URL, HAND_MODEL_URL,
)

# ── MediaPipe ────────────────────────────────────────────────
try:
    import mediapipe as mp
    from mediapipe.tasks import python as mp_tasks
    from mediapipe.tasks.python import vision as mp_vision
except ImportError:
    print("❌  Run: pip install mediapipe")
    sys.exit(1)

try:
    from flask import Flask, Response, jsonify, request
    FLASK_OK = True
except ImportError:
    FLASK_OK = False
    print("⚠  pip install flask  (dashboard disabled)")

BASE_URL = f"http://{ESP32_IP}"


# ─── Model auto-download ───────────────────────────────────
def ensure_model(path: str, url: str) -> None:
    p = Path(path)
    if not p.exists():
        print(f"⬇  Downloading {p.name} …")
        try:
            urllib.request.urlretrieve(url, path)
            print(f"✅  {p.name} downloaded ({p.stat().st_size // 1024} KB)")
        except Exception as e:
            print(f"❌  Failed to download {p.name}: {e}")
            sys.exit(1)


# ─── ESP32 connectivity monitor ────────────────────────────
_esp32_lock   = threading.Lock()
_esp32_online = False


def _monitor_esp32() -> None:
    """Background thread: polls ESP32 every 5 s, updates _esp32_online."""
    global _esp32_online
    while True:
        try:
            r = requests.get(BASE_URL + "/status", timeout=3)
            ok = r.status_code == 200
        except Exception:
            ok = False

        with _esp32_lock:
            prev = _esp32_online
            _esp32_online = ok

        if ok and not prev:
            print(f"✅  {_ts()}  ESP32 back online ({ESP32_IP})")
        elif not ok and prev:
            print(f"⚠️   {_ts()}  ESP32 offline – will auto-resume when it returns…")

        time.sleep(5)


def send(endpoint: str) -> None:
    """Fire-and-forget HTTP call; silently skipped if ESP32 is offline."""
    with _esp32_lock:
        online = _esp32_online
    if not online:
        return
    try:
        requests.get(BASE_URL + endpoint, timeout=2)
    except Exception:
        pass


def send_async(endpoint: str) -> None:
    threading.Thread(target=send, args=(endpoint,), daemon=True).start()


# ─── Helpers ─────────────────────────────────────────────────
def fmt(seconds: float) -> str:
    s = int(max(0, seconds))
    return f"{s//3600:02d}:{(s % 3600)//60:02d}:{s % 60:02d}"

def _ts() -> str:
    return datetime.now().strftime("%H:%M:%S")


# ─── Palm detection ──────────────────────────────────────────
def is_open_palm(landmarks: list) -> bool:
    tips     = [8, 12, 16, 20]
    pips     = [6, 10, 14, 18]
    extended = sum(1 for t, p in zip(tips, pips) if landmarks[t].y < landmarks[p].y)
    return extended >= 3


# ─── Hand skeleton drawing ───────────────────────────────────
_HAND_CONNECTIONS = [
    (0,1),(1,2),(2,3),(3,4),(0,5),(5,6),(6,7),(7,8),
    (5,9),(9,10),(10,11),(11,12),(9,13),(13,14),(14,15),(15,16),
    (13,17),(17,18),(18,19),(19,20),(0,17),
]

def draw_hand(frame, landmarks, img_w: int, img_h: int) -> None:
    pts = [(int(lm.x * img_w), int(lm.y * img_h)) for lm in landmarks]
    for a, b in _HAND_CONNECTIONS:
        cv2.line(frame, pts[a], pts[b], (200, 150, 30), 2)
    for pt in pts:
        cv2.circle(frame, pt, 4, (255, 200, 50), -1)


# ─── Frame preprocessing ─────────────────────────────────────
def preprocess(frame):
    gray     = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    mean_lum = float(gray.mean())
    if mean_lum < 80:
        lab  = cv2.cvtColor(frame, cv2.COLOR_BGR2LAB)
        l, a, b = cv2.split(lab)
        clahe = cv2.createCLAHE(clipLimit=3.0, tileGridSize=(8, 8))
        frame = cv2.cvtColor(cv2.merge([clahe.apply(l), a, b]), cv2.COLOR_LAB2BGR)
        gray  = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    if cv2.Laplacian(gray, cv2.CV_64F).var() < 100:
        gauss = cv2.GaussianBlur(frame, (0, 0), 3)
        frame = cv2.addWeighted(frame, 1.5, gauss, -0.5, 0)
    return frame


# ─── Session log ─────────────────────────────────────────────
class SessionLog:
    def __init__(self, filepath: str):
        self._lock    = threading.Lock()
        self.path     = Path(filepath)
        self.sessions = self._load()

    def _load(self) -> list:
        if self.path.exists():
            try:
                return json.loads(self.path.read_text())
            except Exception:
                pass
        return []

    def add(self, sit_ts: float, stand_ts: float, dur_sec: float) -> None:
        entry = {
            "sit"     : datetime.fromtimestamp(sit_ts).strftime("%Y-%m-%d %H:%M:%S"),
            "stand"   : datetime.fromtimestamp(stand_ts).strftime("%Y-%m-%d %H:%M:%S"),
            "duration": round(dur_sec),
        }
        with self._lock:
            self.sessions.append(entry)
            self.path.write_text(json.dumps(self.sessions, indent=2))
        print(f"💾  Session saved: {entry['sit']} → {entry['stand']} ({fmt(dur_sec)})")

    def get_all(self) -> list:
        with self._lock:
            return list(reversed(self.sessions))

    def get_stats(self) -> dict:
        with self._lock:
            sessions = list(self.sessions)
        today_str = date.today().isoformat()
        today_sec = sum(s["duration"] for s in sessions if s["sit"].startswith(today_str))
        days_with_sessions = {s["sit"][:10] for s in sessions}
        streak = 0
        check  = date.today()
        while check.isoformat() in days_with_sessions:
            streak += 1
            check  -= timedelta(days=1)
        return {
            "today_sec"      : today_sec,
            "today_fmt"      : fmt(today_sec),
            "streak_days"    : streak,
            "total_sessions" : len(sessions),
        }

    # ── Analytics: daily (last N days) ──────────────────────
    def get_daily_stats(self, days: int = 7) -> list:
        with self._lock:
            sessions = list(self.sessions)
        result = []
        for i in range(days - 1, -1, -1):
            d    = (date.today() - timedelta(days=i)).isoformat()
            secs = sum(s["duration"] for s in sessions if s["sit"].startswith(d))
            result.append({
                "date"   : d,
                "label"  : (date.today() - timedelta(days=i)).strftime("%a"),
                "seconds": secs,
                "hours"  : round(secs / 3600, 2),
                "minutes": round(secs / 60),
            })
        return result

    # ── Analytics: monthly (last 6 months) ──────────────────
    def get_monthly_stats(self) -> list:
        with self._lock:
            sessions = list(self.sessions)
        result = []
        today  = date.today()
        for i in range(5, -1, -1):
            # Walk back by month
            month_date = date(today.year, today.month, 1)
            m = (month_date.month - i - 1) % 12 + 1
            y = month_date.year + ((month_date.month - i - 1) // 12)
            month_str = f"{y:04d}-{m:02d}"
            secs = sum(s["duration"] for s in sessions if s["sit"].startswith(month_str))
            result.append({
                "month"  : month_str,
                "label"  : datetime.strptime(month_str, "%Y-%m").strftime("%b %Y"),
                "seconds": secs,
                "hours"  : round(secs / 3600, 2),
            })
        return result

    # ── Analytics: 24-hour distribution ─────────────────────
    def get_hourly_distribution(self) -> list:
        with self._lock:
            sessions = list(self.sessions)
        dist = [0] * 24
        for s in sessions:
            try:
                hour = int(s["sit"][11:13])
                dist[hour] += s["duration"]
            except Exception:
                pass
        return [
            {"hour": h, "label": f"{h:02d}:00", "seconds": dist[h], "minutes": round(dist[h] / 60)}
            for h in range(24)
        ]


# ─── App state ────────────────────────────────────────────────
class AppState:
    def __init__(self):
        self._lock           = threading.RLock()
        self._frame          = None

        self.face_present    = False
        self.face_rects      = []

        self.study_active    = False
        self.study_sit_ts    = None
        self.study_elapsed   = 0.0

        self.sw_running      = False
        self.sw_start_ts     = None
        self.sw_elapsed      = 0.0

        self.palm_hold_ts    = None
        self.palm_triggered  = False
        self.hand_landmarks  = None

        # Proximity emoji state tracking
        self.any_detection   = False   # face OR hand
        self.hand_only_state = False   # hand-only was triggering emoji

        self.stop            = threading.Event()

    def set_frame(self, frame) -> None:
        with self._lock:
            self._frame = frame

    def get_frame(self):
        with self._lock:
            return self._frame

    def study_total(self) -> float:
        with self._lock:
            base = self.study_elapsed
            if self.study_active and self.study_sit_ts:
                base += time.time() - self.study_sit_ts
            return base

    def sw_total(self) -> float:
        with self._lock:
            base = self.sw_elapsed
            if self.sw_running and self.sw_start_ts:
                base += time.time() - self.sw_start_ts
            return base


# ─── Detection thread ─────────────────────────────────────────
def detection_worker(state: AppState, session_log: SessionLog) -> None:
    face_opts = mp_vision.FaceDetectorOptions(
        base_options=mp_tasks.BaseOptions(model_asset_path=FACE_MODEL_PATH),
        running_mode=mp_vision.RunningMode.IMAGE,
        min_detection_confidence=FACE_CONFIDENCE,
    )
    hand_opts = mp_vision.HandLandmarkerOptions(
        base_options=mp_tasks.BaseOptions(model_asset_path=HAND_MODEL_PATH),
        running_mode=mp_vision.RunningMode.IMAGE,
        num_hands=1,
        min_hand_detection_confidence=HAND_CONFIDENCE,
        min_hand_presence_confidence=0.5,
        min_tracking_confidence=0.5,
    )

    face_det = mp_vision.FaceDetector.create_from_options(face_opts)
    hand_det = mp_vision.HandLandmarker.create_from_options(hand_opts)

    interval          = 1.0 / CHECK_FPS
    present_streak    = 0
    face_absent_since = None

    while not state.stop.is_set():
        t0    = time.time()
        frame = state.get_frame()

        if frame is None:
            time.sleep(0.05)
            continue

        proc = preprocess(frame)
        h, w = proc.shape[:2]

        rgb      = cv2.cvtColor(proc, cv2.COLOR_BGR2RGB)
        rgb      = np.ascontiguousarray(rgb, dtype=np.uint8)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

        # ── Face detection ─────────────────────────────────
        face_result = face_det.detect(mp_image)
        face_found  = False
        face_rects  = []
        for det in (face_result.detections or []):
            score = det.categories[0].score if det.categories else 0.0
            if score >= FACE_CONFIDENCE:
                bb = det.bounding_box
                face_rects.append((bb.origin_x, bb.origin_y, bb.width, bb.height))
                face_found = True

        # ── Hand detection ─────────────────────────────────
        hand_result = hand_det.detect(mp_image)
        palm_open   = False
        hand_lms    = None
        if hand_result.hand_landmarks:
            hand_lms  = hand_result.hand_landmarks[0]
            palm_open = is_open_palm(hand_lms)

        now     = time.time()
        actions = []

        with state._lock:
            if face_found:
                present_streak    = min(present_streak + 1, CONFIRM_FRAMES + 2)
                face_absent_since = None
            else:
                present_streak = max(present_streak - 1, 0)
                if face_absent_since is None:
                    face_absent_since = now

            confirmed = present_streak >= CONFIRM_FRAMES

            # PRESENT transition
            if confirmed and not state.face_present:
                state.face_present  = True
                state.study_active  = True
                state.study_sit_ts  = now
                state.study_elapsed = 0.0
                actions.append(("/presence?state=active",
                                f"✅  {_ts()}  SIT  → study timer started"))

            # AWAY transition
            elif (not confirmed
                  and state.face_present
                  and face_absent_since
                  and (now - face_absent_since) >= AWAY_TIMEOUT):
                dur = (now - state.study_sit_ts) if state.study_sit_ts else 0
                sit = state.study_sit_ts
                state.face_present  = False
                state.study_active  = False
                state.study_elapsed = 0.0
                actions.append(("/presence?state=away",
                                f"💤  {_ts()}  STAND  ({fmt(dur)} studied)"))
                actions.append(("__log__", (sit, now, dur)))

            # Palm → stopwatch toggle
            if palm_open and not state.palm_triggered:
                if state.palm_hold_ts is None:
                    state.palm_hold_ts = now
                elif (now - state.palm_hold_ts) >= PALM_HOLD_SECS:
                    state.palm_triggered = True
                    if not state.sw_running:
                        state.sw_running  = True
                        state.sw_start_ts = now
                        actions.append(("/stopwatch?action=start",
                                        f"⏱  {_ts()}  STOPWATCH  START"))
                    else:
                        state.sw_elapsed += now - (state.sw_start_ts or now)
                        state.sw_running  = False
                        state.sw_start_ts = None
                        actions.append(("/stopwatch?action=stop",
                                        f"⏹  {_ts()}  STOPWATCH  STOP ({fmt(state.sw_elapsed)})"))
            else:
                if not palm_open:
                    state.palm_hold_ts   = None
                    state.palm_triggered = False

            state.face_rects     = face_rects
            state.hand_landmarks = hand_lms

            # ── Proximity-based emoji logic ────────────────
            # face OR hand → /showTimer (2s) → HAPPY
            # neither      → TIRED
            current_any = face_found or bool(hand_lms)
            prev_any    = state.any_detection
            state.any_detection = current_any

            # Hand-only detection (no face): track separately for TIRED on departure
            hand_only_now = bool(hand_lms) and not face_found

            if current_any and not prev_any and not confirmed:
                # Something just appeared but study timer hasn't confirmed yet
                # (face confirmed is handled via /presence which triggers showTimer on Arduino)
                if hand_only_now and not state.hand_only_state:
                    state.hand_only_state = True
                    actions.append(("/showTimer", f"👋  {_ts()}  HAND detected → show timer"))

            elif not current_any and prev_any:
                # Everything gone → TIRED (only if face wasn't managing the transition)
                if state.hand_only_state and not state.face_present:
                    state.hand_only_state = False
                    actions.append(("/eyes?mood=TIRED",
                                    f"😴  {_ts()}  No detection → TIRED"))
                elif not state.face_present:
                    state.hand_only_state = False

            if not hand_only_now:
                state.hand_only_state = False

        # ── Dispatch actions ──────────────────────────────
        for endpoint, msg in actions:
            if endpoint == "__log__":
                sit_t, stand_t, d = msg
                session_log.add(sit_t, stand_t, d)
            else:
                send_async(endpoint)
                print(msg)

        elapsed = time.time() - t0
        time.sleep(max(0.0, interval - elapsed))

    face_det.close()
    hand_det.close()


# ─── Dashboard HTML ──────────────────────────────────────────
_HTML = r"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DeskBuddy Dashboard</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:#0d0d1a;color:#e0e0f0;min-height:100vh;padding:24px 20px}
h1{font-size:1.4rem;font-weight:700;color:#a78bfa;margin-bottom:3px}
.sub{color:#4b5563;font-size:.8rem;margin-bottom:18px}
/* Tabs */
.tabs{display:flex;gap:0;border-bottom:1px solid #252545;margin-bottom:20px}
.tablink{padding:10px 18px;font-size:.82rem;cursor:pointer;color:#6b7280;border-bottom:2px solid transparent;background:none;border-top:none;border-left:none;border-right:none}
.tablink.on{color:#a78bfa;border-bottom-color:#a78bfa}
.tabpanel{display:none}.tabpanel.on{display:block}
/* Cards */
.cards{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:18px}
.card{background:#13132b;border:1px solid #252545;border-radius:14px;padding:16px 20px;flex:1;min-width:150px}
.card-label{font-size:.65rem;text-transform:uppercase;letter-spacing:.1em;color:#6b7280;margin-bottom:7px}
.card-value{font-size:1.9rem;font-weight:700;font-variant-numeric:tabular-nums}
.green{color:#34d399}.blue{color:#60a5fa}.purple{color:#a78bfa}.amber{color:#fbbf24}
.badge{display:inline-flex;align-items:center;gap:6px;padding:4px 12px;border-radius:999px;font-size:.76rem;font-weight:600}
.badge.present{background:#064e3b;color:#34d399}.badge.away{background:#1f2937;color:#6b7280}
.badge.swon{background:#1e3a5f;color:#60a5fa}.badge.swoff{background:#1f2937;color:#6b7280}
.badge.online{background:#064e3b;color:#34d399}.badge.offline{background:#3b1f1f;color:#ef4444}
.dot{width:7px;height:7px;border-radius:50%;background:currentColor}
.stats-bar{display:flex;gap:10px;flex-wrap:wrap;margin-bottom:18px}
.stat-chip{background:#13132b;border:1px solid #252545;border-radius:10px;padding:10px 14px;flex:1;min-width:100px;text-align:center}
.stat-chip .sv{font-size:1.35rem;font-weight:700;font-family:monospace}
.stat-chip .sl{font-size:.62rem;text-transform:uppercase;letter-spacing:.08em;color:#6b7280;margin-top:3px}
/* ESP32 block */
.esp-block{background:#13132b;border:1px solid #252545;border-radius:14px;padding:16px 20px;margin-bottom:18px}
.esp-block h2{font-size:.85rem;color:#60a5fa;margin-bottom:12px}
.esp-cards{display:flex;gap:10px;flex-wrap:wrap}
.esp-card{background:#0d0d1a;border:1px solid #1e1e35;border-radius:10px;padding:12px 14px;flex:1;min-width:120px;text-align:center}
.esp-card .ev{font-size:1.5rem;font-weight:700;font-family:monospace;color:#60a5fa}
.esp-card .el{font-size:.62rem;color:#4b5563;text-transform:uppercase;letter-spacing:.08em;margin-top:3px}
/* Chart */
.chart-wrap{background:#13132b;border:1px solid #252545;border-radius:14px;padding:18px;margin-bottom:18px}
.chart-wrap h2{font-size:.85rem;color:#a78bfa;margin-bottom:14px}
.chart-wrap canvas{max-height:220px}
/* Table */
.export-btn{display:inline-block;margin-bottom:14px;padding:7px 16px;background:#1e1e35;border:1px solid #a78bfa55;color:#a78bfa;border-radius:8px;font-size:.78rem;text-decoration:none}
.export-btn:hover{background:#2a2a4a}
h2{font-size:.9rem;color:#a78bfa;margin-bottom:10px}
table{width:100%;border-collapse:collapse}
th{text-align:left;font-size:.65rem;text-transform:uppercase;letter-spacing:.08em;color:#4b5563;padding:8px 10px;border-bottom:1px solid #1e1e35}
td{padding:9px 10px;font-size:.83rem;border-bottom:1px solid #13132b}
tr:hover td{background:#13132b}
.dur{color:#34d399;font-weight:600}
.empty{color:#374151;text-align:center;padding:28px;font-size:.83rem}
</style></head>
<body>
<h1>&#129302; DeskBuddy Dashboard</h1>
<p class="sub">Unified study tracker — PC data &amp; live ESP32 status</p>

<div style="display:flex;align-items:center;gap:10px;margin-bottom:16px">
  <span class="badge offline" id="esp-badge"><span class="dot"></span>ESP32 offline</span>
  <span class="badge away" id="face-badge"><span class="dot"></span>AWAY</span>
  <span class="badge swoff" id="sw-badge"><span class="dot"></span>SW stopped</span>
</div>

<!-- Tabs -->
<div class="tabs">
  <button class="tablink on" onclick="openTab('overview',this)">&#128218; Overview</button>
  <button class="tablink"    onclick="openTab('analytics',this)">&#128200; Analytics</button>
  <button class="tablink"    onclick="openTab('sessions',this)">&#128203; Sessions</button>
</div>

<!-- ══ OVERVIEW ══ -->
<div class="tabpanel on" id="tab-overview">
  <div class="cards">
    <div class="card">
      <div class="card-label">Study Timer (today)</div>
      <div class="card-value green" id="study">00:00:00</div>
    </div>
    <div class="card">
      <div class="card-label">Stopwatch</div>
      <div class="card-value blue" id="sw">00:00:00</div>
    </div>
  </div>

  <div class="stats-bar">
    <div class="stat-chip">
      <div class="sv amber" id="streak-val">0</div>
      <div class="sl">&#128293; Streak</div>
    </div>
    <div class="stat-chip">
      <div class="sv green" id="today-val">00:00:00</div>
      <div class="sl">&#128197; Today</div>
    </div>
    <div class="stat-chip">
      <div class="sv purple" id="sessions-val">0</div>
      <div class="sl">&#128203; Sessions</div>
    </div>
  </div>

  <!-- Live ESP32 hardware data -->
  <div class="esp-block">
    <h2>&#128249; ESP32 Hardware — Live</h2>
    <div class="esp-cards">
      <div class="esp-card">
        <div class="ev" id="esp-study">—</div>
        <div class="el">&#129504; Study</div>
      </div>
      <div class="esp-card">
        <div class="ev" id="esp-sw">—</div>
        <div class="el">&#9996; Stopwatch</div>
      </div>
      <div class="esp-card">
        <div class="ev" id="esp-timer">—</div>
        <div class="el">&#9201; Timer</div>
      </div>
    </div>
  </div>
</div>

<!-- ══ ANALYTICS ══ -->
<div class="tabpanel" id="tab-analytics">
  <div class="chart-wrap">
    <h2>&#128200; Daily Study Time (last 7 days)</h2>
    <canvas id="chartDaily"></canvas>
  </div>
  <div class="chart-wrap">
    <h2>&#128202; Monthly Study Hours</h2>
    <canvas id="chartMonthly"></canvas>
  </div>
  <div class="chart-wrap">
    <h2>&#9200; Study by Hour of Day</h2>
    <canvas id="chartHourly"></canvas>
  </div>
</div>

<!-- ══ SESSIONS ══ -->
<div class="tabpanel" id="tab-sessions">
  <a class="export-btn" href="/api/export">&#8595; Download CSV</a>
  <h2>&#128203; Session History</h2>
  <table>
    <thead><tr><th>#</th><th>Sat Down</th><th>Stood Up</th><th>Duration</th></tr></thead>
    <tbody id="tbody"><tr><td colspan="4" class="empty">No sessions yet.</td></tr></tbody>
  </table>
</div>

<script>
// ── Tab switching ──────────────────────────────────────────
let chartsLoaded = false;
function openTab(name, el) {
  document.querySelectorAll('.tabpanel').forEach(p=>p.classList.remove('on'));
  document.querySelectorAll('.tablink').forEach(t=>t.classList.remove('on'));
  document.getElementById('tab-'+name).classList.add('on');
  el.classList.add('on');
  if (name === 'analytics' && !chartsLoaded) { loadCharts(); chartsLoaded = true; }
  if (name === 'sessions') loadSessions();
}

// ── Formatters ────────────────────────────────────────────
const fmtMs = ms => {
  ms = Math.floor(Math.max(0, ms));
  let s=Math.floor(ms/1000),m=Math.floor(s/60),h=Math.floor(m/60);
  m%=60; s%=60;
  return [h,m,s].map(n=>String(n).padStart(2,'0')).join(':');
};
const fmt = s => {
  s = Math.floor(Math.max(0, s));
  return [s/3600|0,(s%3600/60)|0,s%60].map(n=>String(n).padStart(2,'0')).join(':');
};

// ── PC Study timer tick ───────────────────────────────────
async function tick() {
  try {
    const d = await (await fetch('/api/status')).json();
    document.getElementById('study').textContent = fmt(d.study_total);
    document.getElementById('sw').textContent    = fmt(d.sw_total);
    const fb = document.getElementById('face-badge');
    fb.innerHTML = `<span class="dot"></span>${d.face_present ? 'PRESENT' : 'AWAY'}`;
    fb.className = 'badge ' + (d.face_present ? 'present' : 'away');
    const sb = document.getElementById('sw-badge');
    sb.innerHTML = `<span class="dot"></span>SW ${d.sw_running ? 'running' : 'stopped'}`;
    sb.className = 'badge ' + (d.sw_running ? 'swon' : 'swoff');
  } catch {}
}

// ── ESP32 status proxy ────────────────────────────────────
async function tickESP32() {
  try {
    const d = await (await fetch('/api/esp32/status')).json();
    const eb = document.getElementById('esp-badge');
    if (d.error) {
      eb.innerHTML = '<span class="dot"></span>ESP32 offline';
      eb.className = 'badge offline';
      document.getElementById('esp-study').textContent  = '—';
      document.getElementById('esp-sw').textContent     = '—';
      document.getElementById('esp-timer').textContent  = '—';
    } else {
      eb.innerHTML = '<span class="dot"></span>ESP32 online';
      eb.className = 'badge online';
      document.getElementById('esp-study').textContent  = fmtMs(d.studyMs);
      document.getElementById('esp-sw').textContent     = fmtMs(d.swMs);
      document.getElementById('esp-timer').textContent  = fmtMs(d.timerRemMs);
    }
  } catch {
    document.getElementById('esp-badge').innerHTML = '<span class="dot"></span>ESP32 offline';
    document.getElementById('esp-badge').className = 'badge offline';
  }
}

// ── Stats & sessions ──────────────────────────────────────
async function loadStats() {
  try {
    const s = await (await fetch('/api/stats')).json();
    document.getElementById('streak-val').textContent   = s.streak_days;
    document.getElementById('today-val').textContent    = s.today_fmt;
    document.getElementById('sessions-val').textContent = s.total_sessions;
  } catch {}
}

async function loadSessions() {
  try {
    const rows = await (await fetch('/api/sessions')).json();
    const tb   = document.getElementById('tbody');
    if (!rows.length) {
      tb.innerHTML = '<tr><td colspan="4" class="empty">No sessions yet.</td></tr>';
      return;
    }
    tb.innerHTML = rows.map((s,i) =>
      `<tr><td style="color:#4b5563">${rows.length-i}</td><td>${s.sit}</td><td>${s.stand}</td><td class="dur">${fmt(s.duration)}</td></tr>`
    ).join('');
  } catch {}
}

// ── Analytics charts ──────────────────────────────────────
const CHART_DEFAULTS = {
  plugins:{legend:{display:false},tooltip:{callbacks:{label:ctx=>`${ctx.parsed.y.toFixed(2)} hrs`}}},
  scales:{
    x:{grid:{color:'#1e1e35'},ticks:{color:'#6b7280',font:{size:10}}},
    y:{grid:{color:'#1e1e35'},ticks:{color:'#6b7280',font:{size:10}},beginAtZero:true,title:{display:true,text:'Hours',color:'#4b5563',font:{size:10}}}
  }
};
let charts = {};

async function loadCharts() {
  try {
    const [daily, monthly, hourly] = await Promise.all([
      fetch('/api/analytics/daily').then(r=>r.json()),
      fetch('/api/analytics/monthly').then(r=>r.json()),
      fetch('/api/analytics/hourly').then(r=>r.json()),
    ]);

    // Daily bar chart
    charts.daily = new Chart(document.getElementById('chartDaily'), {
      type: 'bar',
      data: {
        labels: daily.map(d=>d.label),
        datasets: [{
          data: daily.map(d=>d.hours),
          backgroundColor: daily.map(d => d.hours > 0 ? '#a78bfa88' : '#252545'),
          borderColor: '#a78bfa',
          borderWidth: 1, borderRadius: 4,
        }]
      },
      options: {...CHART_DEFAULTS}
    });

    // Monthly line chart
    charts.monthly = new Chart(document.getElementById('chartMonthly'), {
      type: 'line',
      data: {
        labels: monthly.map(d=>d.label),
        datasets: [{
          data: monthly.map(d=>d.hours),
          borderColor: '#34d399', backgroundColor: '#34d39920',
          fill: true, tension: 0.4, pointBackgroundColor: '#34d399',
        }]
      },
      options: {...CHART_DEFAULTS}
    });

    // Hourly bar chart (minutes for readability)
    charts.hourly = new Chart(document.getElementById('chartHourly'), {
      type: 'bar',
      data: {
        labels: hourly.map(d=>d.label),
        datasets: [{
          data: hourly.map(d=>d.minutes),
          backgroundColor: '#60a5fa55',
          borderColor: '#60a5fa',
          borderWidth: 1, borderRadius: 3,
        }]
      },
      options: {
        ...CHART_DEFAULTS,
        plugins:{legend:{display:false},tooltip:{callbacks:{label:ctx=>`${ctx.parsed.y} min`}}},
        scales:{
          x:{grid:{color:'#1e1e35'},ticks:{color:'#6b7280',font:{size:9},maxRotation:0}},
          y:{grid:{color:'#1e1e35'},ticks:{color:'#6b7280',font:{size:10}},beginAtZero:true,title:{display:true,text:'Minutes',color:'#4b5563',font:{size:10}}}
        }
      }
    });
  } catch(e) { console.error('Chart load failed:', e); }
}

// ── Polling ───────────────────────────────────────────────
setInterval(tick,        1000);
setInterval(loadStats,   5000);
setInterval(tickESP32,   3000);
tick(); loadStats(); tickESP32();
</script>
</body></html>"""


# ─── Flask dashboard ─────────────────────────────────────────
def start_dashboard(state: AppState, session_log: SessionLog, port: int) -> None:
    if not FLASK_OK:
        return
    app = Flask(__name__)
    logging.getLogger("werkzeug").setLevel(logging.ERROR)

    @app.route("/")
    def index():
        return Response(_HTML, mimetype="text/html")

    @app.route("/api/status")
    def api_status():
        with state._lock:
            fp = state.face_present
            sa = state.study_active
            sr = state.sw_running
        return jsonify({
            "face_present": fp,
            "study_active": sa,
            "sw_running"  : sr,
            "study_total" : state.study_total(),
            "sw_total"    : state.sw_total(),
        })

    @app.route("/api/sessions")
    def api_sessions():
        return jsonify(session_log.get_all())

    @app.route("/api/stats")
    def api_stats():
        return jsonify(session_log.get_stats())

    @app.route("/api/export")
    def api_export():
        sessions = session_log.get_all()
        output   = io.StringIO()
        writer   = csv.DictWriter(output, fieldnames=["sit", "stand", "duration"])
        writer.writeheader()
        writer.writerows(sessions)
        return Response(
            output.getvalue(),
            mimetype="text/csv",
            headers={"Content-Disposition": "attachment; filename=study_sessions.csv"},
        )

    @app.route("/api/analytics/daily")
    def api_analytics_daily():
        days = int(request.args.get("days", 7))
        return jsonify(session_log.get_daily_stats(min(days, 90)))

    @app.route("/api/analytics/monthly")
    def api_analytics_monthly():
        return jsonify(session_log.get_monthly_stats())

    @app.route("/api/analytics/hourly")
    def api_analytics_hourly():
        return jsonify(session_log.get_hourly_distribution())

    # ── ESP32 status proxy (avoids CORS issues) ───────────
    @app.route("/api/esp32/status")
    def api_esp32_status():
        try:
            r = requests.get(f"{BASE_URL}/status", timeout=2)
            return Response(r.content, mimetype="application/json")
        except Exception:
            return jsonify({"error": "ESP32 offline"}), 503

    threading.Thread(
        target=lambda: app.run(host="0.0.0.0", port=port, debug=False),
        daemon=True,
    ).start()
    print(f"🌐  Dashboard  →  http://localhost:{port}   (also http://YOUR_PC_IP:{port})")


# ─── Main camera loop ─────────────────────────────────────────
def main(show_preview: bool, camera_index: int) -> None:
    ensure_model(FACE_MODEL_PATH, FACE_MODEL_URL)
    ensure_model(HAND_MODEL_PATH, HAND_MODEL_URL)

    session_log = SessionLog(SESSION_FILE)
    state       = AppState()

    # Start ESP32 connectivity monitor
    threading.Thread(target=_monitor_esp32, daemon=True).start()
    print(f"🔍  Monitoring ESP32 at {ESP32_IP} (auto-reconnect enabled)")

    threading.Thread(
        target=detection_worker,
        args=(state, session_log),
        daemon=True,
    ).start()

    start_dashboard(state, session_log, DASHBOARD_PORT)

    cap = cv2.VideoCapture(camera_index)
    if not cap.isOpened():
        print(f"❌  Cannot open camera {camera_index}")
        sys.exit(1)

    cap.set(cv2.CAP_PROP_FRAME_WIDTH,   640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT,  480)
    cap.set(cv2.CAP_PROP_AUTOFOCUS,     1)
    cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 0.75)

    print(f"\n📷  Study tracker running  (ESP32 = {ESP32_IP})")
    print(f"    Hold open palm {PALM_HOLD_SECS}s → start/stop stopwatch")
    print(f"    {'Preview window – press Q to quit' if show_preview else 'Headless – Ctrl+C to quit'}\n")

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                time.sleep(0.1)
                continue
            state.set_frame(frame)

            if not show_preview:
                time.sleep(0.02)
                continue

            disp = frame.copy()
            h, w = disp.shape[:2]

            with state._lock:
                face_rects   = list(state.face_rects)
                hand_lms     = state.hand_landmarks
                face_present = state.face_present
                sw_running   = state.sw_running
                palm_hold    = state.palm_hold_ts
                palm_trig    = state.palm_triggered

            study_t = state.study_total()
            sw_t    = state.sw_total()

            for (x, y, fw, fh) in face_rects:
                cv2.rectangle(disp, (x, y), (x + fw, y + fh),
                              (0, 255, 120) if face_present else (0, 140, 60), 2)
            if hand_lms:
                draw_hand(disp, hand_lms, w, h)
            if palm_hold and not palm_trig:
                prog  = min((time.time() - palm_hold) / PALM_HOLD_SECS, 1.0)
                bar_w = int(200 * prog)
                cv2.rectangle(disp, (10, 456), (212, 472), (30, 30, 50), -1)
                cv2.rectangle(disp, (10, 456), (10 + bar_w, 472), (200, 150, 255), -1)
                cv2.putText(disp, "Hold palm...", (10, 452),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, (200, 150, 255), 1)

            with _esp32_lock:
                online = _esp32_online
            esp_col = (0, 200, 80) if online else (0, 80, 200)
            p_col   = (0, 255, 100) if face_present else (0, 80, 255)
            s_col   = (100, 200, 255) if sw_running else (100, 100, 120)
            for txt, pos, sc, col in [
                (f"{'PRESENT' if face_present else 'AWAY'}",      (10, 30),  .75, p_col),
                (f"Study  {fmt(study_t)}",                         (10, 58),  .60, (80, 255, 160)),
                (f"{'>' if sw_running else '|'} SW  {fmt(sw_t)}", (10, 84),  .60, s_col),
                (f"ESP32: {'online' if online else 'OFFLINE'}",   (10, 108), .40, esp_col),
            ]:
                cv2.putText(disp, txt, pos, cv2.FONT_HERSHEY_SIMPLEX, sc, col, 2)

            cv2.imshow("DeskBuddy Study Tracker", disp)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

    except KeyboardInterrupt:
        print("\n🔴  Stopped.")
    finally:
        state.stop.set()
        send_async("/presence?state=away")
        cap.release()
        if show_preview:
            cv2.destroyAllWindows()
        print(f"    Sessions saved → {SESSION_FILE}")


# ─── CLI ─────────────────────────────────────────────────────
if __name__ == "__main__":
    p = argparse.ArgumentParser(description="DeskBuddy Study Tracker v2.0")
    p.add_argument("--preview", action="store_true", help="Live cam window")
    p.add_argument("--camera",  type=int,   default=CAMERA_INDEX)
    p.add_argument("--ip",      type=str,   default=ESP32_IP)
    p.add_argument("--timeout", type=int,   default=AWAY_TIMEOUT)
    p.add_argument("--port",    type=int,   default=DASHBOARD_PORT)
    p.add_argument("--palm",    type=float, default=PALM_HOLD_SECS)
    a = p.parse_args()

    ESP32_IP       = a.ip
    BASE_URL       = f"http://{a.ip}"
    AWAY_TIMEOUT   = a.timeout
    CAMERA_INDEX   = a.camera
    DASHBOARD_PORT = a.port
    PALM_HOLD_SECS = a.palm

    main(show_preview=a.preview, camera_index=a.camera)
