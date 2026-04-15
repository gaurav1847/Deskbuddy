# ============================================================
#  config.py  –  DeskBuddy Study Tracker v2.0 configuration
#
#  Edit this file to configure the tracker.
#  Do NOT edit study_tracker.py for basic setup.
# ============================================================

# ── Network ──────────────────────────────────────────────────
# Use the mDNS hostname (recommended) or the static IP below.
# mDNS works on macOS/Linux natively; Windows needs Bonjour.
ESP32_IP       = "deskbuddy.local"   # OR use "192.168.1.5"
DASHBOARD_PORT = 8080                # http://localhost:8080

# ── Detection tuning ─────────────────────────────────────────
AWAY_TIMEOUT    = 5     # seconds without face before session ends
CHECK_FPS       = 8     # camera detection rate (frames per second)
CAMERA_INDEX    = 0     # 0 = default webcam, 1 = second camera
FACE_CONFIDENCE = 0.5   # face detection threshold (0.0–1.0)
HAND_CONFIDENCE = 0.1   # hand detection threshold (0.0–1.0)
PALM_HOLD_SECS  = 0.8   # seconds to hold open palm to trigger stopwatch
CONFIRM_FRAMES  = 3     # consecutive face frames needed to confirm PRESENT

# ── Data ─────────────────────────────────────────────────────
SESSION_FILE = "study_sessions.json"   # local session log (PC-side)

# ── MediaPipe model paths (auto-downloaded if missing) ───────
FACE_MODEL_PATH = "blaze_face_short_range.tflite"
HAND_MODEL_PATH = "hand_landmarker.task"
FACE_MODEL_URL  = (
    "https://storage.googleapis.com/mediapipe-models/"
    "face_detector/blaze_face_short_range/float16/latest/"
    "blaze_face_short_range.tflite"
)
HAND_MODEL_URL  = (
    "https://storage.googleapis.com/mediapipe-models/"
    "hand_landmarker/hand_landmarker/float16/latest/"
    "hand_landmarker.task"
)
