import numpy as np
"""
study_tracker.py  –  RoboEyes Study Tracker  (MediaPipe Tasks API)
===================================================================
Compatible with mediapipe >= 0.10  (new Tasks API — no mp.solutions)

  • FACE detected    → background study timer starts automatically
  • FACE gone 5s     → study timer pauses, session saved to log
  • OPEN PALM (hold) → stopwatch starts / stops
  • Auto-brightness  → CLAHE applied when room is dark
  • Auto-focus       → hardware AF + software unsharp mask
  • Dashboard        → http://localhost:8080  (sessions + timers)
  • Models auto-downloaded on first run (~7 MB total)

INSTALL:
    pip install mediapipe opencv-python requests flask

RUN:
    python study_tracker.py
    python study_tracker.py --preview   ← live cam window
"""

import cv2, numpy as np, time, json, threading, requests, argparse, sys, logging, urllib.request
from datetime import datetime
from pathlib import Path

# ── MediaPipe (new Tasks API) ──────────────────────────────────
try:
    import mediapipe as mp
    from mediapipe.tasks import python as mp_tasks
    from mediapipe.tasks.python import vision as mp_vision
except ImportError:
    print("❌  Run: pip install mediapipe"); sys.exit(1)

try:
    from flask import Flask, Response, jsonify
    FLASK_OK = True
except ImportError:
    FLASK_OK = False
    print("⚠  pip install flask  (dashboard disabled)")

# ─── CONFIG ────────────────────────────────────────────────────
ESP32_IP          = "192.168.1.9"
AWAY_TIMEOUT      = 5           # sec no face → session ends
CHECK_FPS         = 8           # detection rate
CAMERA_INDEX      = 0
FACE_CONFIDENCE   = 0.5
HAND_CONFIDENCE   = 0.1
PALM_HOLD_SECS    = 0.8         # hold open palm this long to trigger
CONFIRM_FRAMES    = 3           # consecutive face frames to confirm PRESENT
DASHBOARD_PORT    = 8080
SESSION_FILE      = "study_sessions.json"

# Model files (auto-downloaded if missing)
FACE_MODEL_PATH   = "blaze_face_short_range.tflite"
HAND_MODEL_PATH   = "hand_landmarker.task"
FACE_MODEL_URL    = ("https://storage.googleapis.com/mediapipe-models/"
                     "face_detector/blaze_face_short_range/float16/latest/"
                     "blaze_face_short_range.tflite")
HAND_MODEL_URL    = ("https://storage.googleapis.com/mediapipe-models/"
                     "hand_landmarker/hand_landmarker/float16/latest/"
                     "hand_landmarker.task")
# ───────────────────────────────────────────────────────────────

BASE_URL = f"http://{ESP32_IP}"


# ─── Auto-download models ──────────────────────────────────────
def ensure_model(path: str, url: str):
    p = Path(path)
    if not p.exists():
        print(f"⬇  Downloading {p.name} …")
        try:
            urllib.request.urlretrieve(url, path)
            print(f"✅  {p.name} downloaded ({p.stat().st_size // 1024} KB)")
        except Exception as e:
            print(f"❌  Failed to download {p.name}: {e}")
            print(f"   Download manually from:\n   {url}")
            sys.exit(1)


# ─── ESP32 HTTP (fire-and-forget, never blocks) ────────────────
def send(endpoint: str):
    try:
        requests.get(BASE_URL + endpoint, timeout=2)
    except Exception:
        pass


# ─── Helpers ───────────────────────────────────────────────────
def fmt(seconds: float) -> str:
    s = int(max(0, seconds))
    return f"{s//3600:02d}:{(s%3600)//60:02d}:{s%60:02d}"

def ts() -> str:
    return datetime.now().strftime("%H:%M:%S")


# ─── Palm detection (new Tasks landmark format) ────────────────
def is_open_palm(landmarks: list) -> bool:
    """
    True when at least 3 of 4 fingers are extended (tip y < PIP y).
    landmarks: list of NormalizedLandmark with .x .y .z  (0-1 range)
    """
    tips     = [8, 12, 16, 20]
    pips     = [6, 10, 14, 18]
    extended = sum(1 for t, p in zip(tips, pips) if landmarks[t].y < landmarks[p].y)
    return extended >= 3


# ─── Hand skeleton drawing (manual — no mp.solutions.drawing_utils) ──
_HAND_CONNECTIONS = [
    (0,1),(1,2),(2,3),(3,4),
    (0,5),(5,6),(6,7),(7,8),
    (5,9),(9,10),(10,11),(11,12),
    (9,13),(13,14),(14,15),(15,16),
    (13,17),(17,18),(18,19),(19,20),
    (0,17),
]

def draw_hand(frame, landmarks, img_w, img_h):
    pts = [(int(lm.x * img_w), int(lm.y * img_h)) for lm in landmarks]
    for a, b in _HAND_CONNECTIONS:
        cv2.line(frame, pts[a], pts[b], (200, 150, 30), 2)
    for pt in pts:
        cv2.circle(frame, pt, 4, (255, 200, 50), -1)


# ─── Frame preprocessing ──────────────────────────────────────
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


# ─── Session log ───────────────────────────────────────────────
class SessionLog:
    def __init__(self, filepath: str):
        self._lock    = threading.Lock()
        self.path     = Path(filepath)
        self.sessions = self._load()

    def _load(self):
        if self.path.exists():
            try:
                return json.loads(self.path.read_text())
            except Exception:
                pass
        return []

    def add(self, sit_ts: float, stand_ts: float, dur_sec: float):
        entry = {
            "sit"     : datetime.fromtimestamp(sit_ts).strftime("%Y-%m-%d %H:%M:%S"),
            "stand"   : datetime.fromtimestamp(stand_ts).strftime("%Y-%m-%d %H:%M:%S"),
            "duration": round(dur_sec),
        }
        with self._lock:
            self.sessions.append(entry)
            self.path.write_text(json.dumps(self.sessions, indent=2))
        print(f"💾  Session saved: {entry['sit']} → {entry['stand']} ({fmt(dur_sec)})")

    def get_all(self):
        with self._lock:
            return list(reversed(self.sessions))


# ─── App state ─────────────────────────────────────────────────
class AppState:
    def __init__(self):
        self._lock          = threading.RLock()
        self._frame         = None

        self.face_present   = False
        self.face_rects     = []          # list of (x, y, w, h) in pixels

        self.study_active   = False
        self.study_sit_ts   = None
        self.study_elapsed  = 0.0

        self.sw_running     = False
        self.sw_start_ts    = None
        self.sw_elapsed     = 0.0

        self.palm_hold_ts   = None
        self.palm_triggered = False

        # Raw landmark list (NormalizedLandmark) or None
        self.hand_landmarks = None

        self.stop           = threading.Event()

    def set_frame(self, frame):
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


# ─── Detection thread ──────────────────────────────────────────
def detection_worker(state: AppState, session_log: SessionLog):
    # Build detectors using new Tasks API
    # running_mode=IMAGE is required — without it detectors silently return nothing
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

        proc  = preprocess(frame)
        h, w  = proc.shape[:2]

        # MediaPipe Tasks expects a contiguous uint8 RGB array
        rgb      = cv2.cvtColor(proc, cv2.COLOR_BGR2RGB)
        rgb      = np.ascontiguousarray(rgb, dtype=np.uint8)
        mp_image = mp.Image(image_format=mp.ImageFormat.SRGB, data=rgb)

        # ── Face detection ─────────────────────────────────────
        face_result = face_det.detect(mp_image)
        face_found  = False
        face_rects  = []

        for det in (face_result.detections or []):
            score = det.categories[0].score if det.categories else 0.0
            if score >= FACE_CONFIDENCE:
                bb = det.bounding_box          # pixel coords
                face_rects.append((bb.origin_x, bb.origin_y, bb.width, bb.height))
                face_found = True

        # ── Hand detection ─────────────────────────────────────
        hand_result = hand_det.detect(mp_image)
        palm_open   = False
        hand_lms    = None

        if hand_result.hand_landmarks:
            hand_lms  = hand_result.hand_landmarks[0]   # list of NormalizedLandmark
            palm_open = is_open_palm(hand_lms)

        now     = time.time()
        actions = []

        with state._lock:
            if face_found:
                present_streak    = min(present_streak + 1, CONFIRM_FRAMES + 2)
                face_absent_since = None
            else:
                present_streak    = max(present_streak - 1, 0)
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
                                f"✅  {ts()}  SIT  → study timer started"))

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
                                f"💤  {ts()}  STAND  ({fmt(dur)} studied)"))
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
                                        f"⏱  {ts()}  STOPWATCH  START"))
                    else:
                        state.sw_elapsed += now - (state.sw_start_ts or now)
                        state.sw_running  = False
                        state.sw_start_ts = None
                        actions.append(("/stopwatch?action=stop",
                                        f"⏹  {ts()}  STOPWATCH  STOP "
                                        f"({fmt(state.sw_elapsed)})"))
            else:
                if not palm_open:
                    state.palm_hold_ts   = None
                    state.palm_triggered = False

            state.face_rects    = face_rects
            state.hand_landmarks = hand_lms

        for endpoint, msg in actions:
            if endpoint == "__log__":
                sit_t, stand_t, d = msg
                session_log.add(sit_t, stand_t, d)
            else:
                threading.Thread(target=send, args=(endpoint,), daemon=True).start()
                print(msg)

        elapsed = time.time() - t0
        time.sleep(max(0.0, interval - elapsed))

    face_det.close()
    hand_det.close()


# ─── Dashboard HTML ────────────────────────────────────────────
_HTML = r"""<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Study Tracker</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',system-ui,sans-serif;background:#0d0d1a;color:#e0e0f0;min-height:100vh;padding:28px 24px}
h1{font-size:1.55rem;font-weight:700;color:#a78bfa;margin-bottom:4px}
.sub{color:#4b5563;font-size:.82rem;margin-bottom:26px}
.cards{display:flex;gap:14px;flex-wrap:wrap;margin-bottom:28px}
.card{background:#13132b;border:1px solid #252545;border-radius:14px;padding:18px 22px;flex:1;min-width:160px}
.card-label{font-size:.68rem;text-transform:uppercase;letter-spacing:.1em;color:#6b7280;margin-bottom:8px}
.card-value{font-size:2rem;font-weight:700;font-variant-numeric:tabular-nums;letter-spacing:-.02em}
.green{color:#34d399}.blue{color:#60a5fa}.purple{color:#a78bfa}.gray{color:#6b7280}
.badge{display:inline-flex;align-items:center;gap:6px;padding:5px 14px;border-radius:999px;font-size:.78rem;font-weight:600}
.badge.present{background:#064e3b;color:#34d399}.badge.away{background:#1f2937;color:#6b7280}
.badge.swon{background:#1e3a5f;color:#60a5fa}.badge.swoff{background:#1f2937;color:#6b7280}
.dot{width:8px;height:8px;border-radius:50%;background:currentColor}
.hint{background:#13132b;border:1px solid #252545;border-radius:10px;padding:11px 15px;
      font-size:.78rem;color:#6b7280;margin-bottom:22px;line-height:1.7}
.hint b{color:#a78bfa}
h2{font-size:.95rem;color:#a78bfa;margin-bottom:12px}
table{width:100%;border-collapse:collapse}
th{text-align:left;font-size:.68rem;text-transform:uppercase;letter-spacing:.08em;
   color:#4b5563;padding:8px 12px;border-bottom:1px solid #1e1e35}
td{padding:10px 12px;font-size:.85rem;border-bottom:1px solid #13132b}
tr:hover td{background:#13132b}
.dur{color:#34d399;font-weight:600}
.empty{color:#374151;text-align:center;padding:32px;font-size:.85rem}
</style></head>
<body>
<h1>📚 Study Tracker</h1>
<p class="sub">Camera-based presence &amp; focus tracker — RoboEyes</p>
<div class="hint">
  <b>😊 Face detected</b> → Study timer starts automatically<br>
  <b>✋ Open palm</b> (hold 0.8 s) → Stopwatch start / stop
</div>
<div class="cards">
  <div class="card">
    <div class="card-label">Study Timer</div>
    <div class="card-value green" id="study">00:00:00</div>
  </div>
  <div class="card">
    <div class="card-label">Stopwatch</div>
    <div class="card-value blue" id="sw">00:00:00</div>
  </div>
  <div class="card" style="flex:.6;min-width:140px">
    <div class="card-label">Presence</div>
    <div style="padding-top:6px"><span class="badge away" id="face-badge"><span class="dot"></span>AWAY</span></div>
  </div>
  <div class="card" style="flex:.6;min-width:140px">
    <div class="card-label">Stopwatch</div>
    <div style="padding-top:6px"><span class="badge swoff" id="sw-badge"><span class="dot"></span>STOPPED</span></div>
  </div>
</div>
<h2>📋 Session History</h2>
<table>
  <thead><tr><th>#</th><th>Sat Down</th><th>Stood Up</th><th>Duration</th></tr></thead>
  <tbody id="tbody"><tr><td colspan="4" class="empty">No sessions recorded yet.</td></tr></tbody>
</table>
<script>
const fmt = s => {
  s = Math.floor(Math.max(0,s));
  return [s/3600|0,(s%3600/60)|0,s%60].map(n=>String(n).padStart(2,'0')).join(':');
};
async function tick(){
  try{
    const d=await(await fetch('/api/status')).json();
    document.getElementById('study').textContent=fmt(d.study_total);
    document.getElementById('sw').textContent=fmt(d.sw_total);
    const fb=document.getElementById('face-badge');
    fb.innerHTML=`<span class="dot"></span>${d.face_present?'PRESENT':'AWAY'}`;
    fb.className='badge '+(d.face_present?'present':'away');
    const sb=document.getElementById('sw-badge');
    sb.innerHTML=`<span class="dot"></span>${d.sw_running?'RUNNING':'STOPPED'}`;
    sb.className='badge '+(d.sw_running?'swon':'swoff');
  }catch{}
}
async function loadSessions(){
  try{
    const rows=await(await fetch('/api/sessions')).json();
    const tb=document.getElementById('tbody');
    if(!rows.length){tb.innerHTML='<tr><td colspan="4" class="empty">No sessions recorded yet.</td></tr>';return;}
    tb.innerHTML=rows.map((s,i)=>`<tr><td style="color:#4b5563">${rows.length-i}</td><td>${s.sit}</td><td>${s.stand}</td><td class="dur">${fmt(s.duration)}</td></tr>`).join('');
  }catch{}
}
setInterval(tick,1000);setInterval(loadSessions,4000);
tick();loadSessions();
</script>
</body></html>"""


# ─── Flask dashboard ───────────────────────────────────────────
def start_dashboard(state: AppState, session_log: SessionLog, port: int):
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

    threading.Thread(
        target=lambda: app.run(host="0.0.0.0", port=port, debug=False),
        daemon=True,
    ).start()
    print(f"🌐  Dashboard  →  http://localhost:{port}")


# ─── Main camera loop ──────────────────────────────────────────
def main(show_preview: bool, camera_index: int):
    # Download models if needed
    ensure_model(FACE_MODEL_PATH, FACE_MODEL_URL)
    ensure_model(HAND_MODEL_PATH, HAND_MODEL_URL)

    session_log = SessionLog(SESSION_FILE)
    state       = AppState()

    threading.Thread(
        target=detection_worker,
        args=(state, session_log),
        daemon=True,
    ).start()

    start_dashboard(state, session_log, DASHBOARD_PORT)

    cap = cv2.VideoCapture(camera_index)
    if not cap.isOpened():
        print(f"❌  Cannot open camera {camera_index}"); sys.exit(1)

    cap.set(cv2.CAP_PROP_FRAME_WIDTH,   640)
    cap.set(cv2.CAP_PROP_FRAME_HEIGHT,  480)
    cap.set(cv2.CAP_PROP_AUTOFOCUS,     1)
    cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 0.75)

    print(f"\n📷  Study tracker running  (ESP32 = {ESP32_IP})")
    print(f"    Hold open palm {PALM_HOLD_SECS}s to start/stop stopwatch")
    print(f"    {'Preview window – press Q to quit' if show_preview else 'Headless – press Ctrl+C to quit'}\n")

    try:
        while True:
            ret, frame = cap.read()
            if not ret:
                time.sleep(0.1); continue

            state.set_frame(frame)

            if not show_preview:
                time.sleep(0.02)
                continue

            # ── Preview rendering ─────────────────────────────
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

            # Face boxes
            for (x, y, fw, fh) in face_rects:
                cv2.rectangle(disp, (x, y), (x + fw, y + fh),
                              (0, 255, 120) if face_present else (0, 140, 60), 2)

            # Hand skeleton
            if hand_lms:
                draw_hand(disp, hand_lms, w, h)

            # Palm hold bar
            if palm_hold and not palm_trig:
                prog  = min((time.time() - palm_hold) / PALM_HOLD_SECS, 1.0)
                bar_w = int(200 * prog)
                cv2.rectangle(disp, (10, 456), (212, 472), (30, 30, 50), -1)
                cv2.rectangle(disp, (10, 456), (10 + bar_w, 472), (200, 150, 255), -1)
                cv2.putText(disp, "Hold palm...", (10, 452),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.45, (200, 150, 255), 1)

            # HUD
            p_col = (0, 255, 100)   if face_present else (0, 80, 255)
            s_col = (100, 200, 255) if sw_running   else (100, 100, 120)
            for txt, pos, sc, col in [
                (f"{'PRESENT' if face_present else 'AWAY'}",      (10, 30),  .75, p_col),
                (f"Study  {fmt(study_t)}",                         (10, 58),  .60, (80, 255, 160)),
                (f"{'▶' if sw_running else '■'} SW  {fmt(sw_t)}", (10, 84),  .60, s_col),
                (f"ESP32: {ESP32_IP}",                             (10, 108), .40, (100, 100, 120)),
            ]:
                cv2.putText(disp, txt, pos, cv2.FONT_HERSHEY_SIMPLEX, sc, col, 2)

            cv2.imshow("RoboEyes Study Tracker", disp)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break

    except KeyboardInterrupt:
        print("\n🔴  Stopped.")
    finally:
        state.stop.set()
        threading.Thread(target=send, args=("/presence?state=away",), daemon=True).start()
        cap.release()
        if show_preview:
            cv2.destroyAllWindows()
        print(f"    Sessions saved → {SESSION_FILE}")


# ─── CLI ───────────────────────────────────────────────────────
if __name__ == "__main__":
    p = argparse.ArgumentParser(description="RoboEyes Study Tracker")
    p.add_argument("--preview", action="store_true",  help="Live cam window")
    p.add_argument("--camera",  type=int,   default=CAMERA_INDEX)
    p.add_argument("--ip",      type=str,   default=ESP32_IP)
    p.add_argument("--timeout", type=int,   default=AWAY_TIMEOUT)
    p.add_argument("--port",    type=int,   default=DASHBOARD_PORT)
    p.add_argument("--palm",    type=float, default=PALM_HOLD_SECS,
                   help="Seconds to hold palm to trigger (default 0.8)")
    a = p.parse_args()

    ESP32_IP       = a.ip
    BASE_URL       = f"http://{a.ip}"
    AWAY_TIMEOUT   = a.timeout
    CAMERA_INDEX   = a.camera
    DASHBOARD_PORT = a.port
    PALM_HOLD_SECS = a.palm

    main(show_preview=a.preview, camera_index=a.camera)