# DeskBuddy Study Tracker 🤖👀

> An automated, hardware-software hybrid study tracker that uses computer vision to monitor your focus — hands-free, no timers to start or stop.

The system uses a **Python script** (OpenCV + MediaPipe Tasks API) on your PC to detect your presence and hand gestures, communicating over HTTP with an **ESP32 microcontroller**. The ESP32 acts as a local web server, managing timers and bringing the tracker to life with animated OLED robot eyes that react to whether you're sitting or away.

---

## 📸 How It Works — At a Glance

```
 [Webcam] ──► [Python on PC] ──► [ESP32 over HTTP] ──► [OLED Display]
               Face detected?         Start timer           Happy eyes 😊
               Face gone?             Pause timer           Tired eyes 😴
               Open palm held?        Toggle stopwatch      Show timer
```

---

## ✨ Features

### Core Functionality
| Feature | Description |
|---|---|
| **Automated Face Tracking** | Study timer starts the moment you sit down, pauses automatically when you leave |
| **Gesture Controls** | Hold an open palm for 0.8s to toggle a secondary stopwatch |
| **Animated OLED Eyes** | Idle → shows study time for 2s → Happy when detected; Tired when away |
| **Unified Dashboard** | Full analytics at `http://localhost:8080`; hardware controls at `http://deskbuddy.local` |
| **Study Streaks** | Tracks your daily streaks and total session counts |
| **CSV Export** | Download your entire study history from the Sessions tab |
| **Auto-Downloading Models** | Required MediaPipe `.tflite` and `.task` model files are fetched automatically on first run |
| **Centralized Config** | Tweak camera, network, and detection settings in a single `config.py` — no need to touch the main script |

### v2.0 Additions
| Feature | Description |
|---|---|
| **Captive Portal Wi-Fi Setup** | First boot creates a **DeskBuddy-Setup** hotspot. Enter your home Wi-Fi credentials via any browser — no hardcoded passwords ever |
| **NVS Credential Storage** | Wi-Fi credentials are saved to ESP32 flash and survive power cycles. Reset any time via the Settings tab |
| **mDNS** | Access the ESP32 at `http://deskbuddy.local` — no IP address to remember |
| **Static IP Fallback** | Attempts `192.168.1.5`; falls back to DHCP with an OLED notification if unavailable |
| **Auto-Reconnect** | Python script monitors ESP32 connectivity and resumes automatically after a power cycle — no manual restart needed |
| **Timer Overflow Fix** | Times over 60 minutes display correctly as `01:05:30` instead of `01:65:30` |
| **Analytics Dashboard** | Daily (7-day bar), monthly (line), and hourly distribution (bar) charts powered by Chart.js |
| **OTA Firmware Updates** | Upload new `.bin` firmware directly from your browser at `/update` — no USB cable required |

---

## 🛠️ Hardware Requirements

| Component | Details |
|---|---|
| **ESP32 Dev Board** | e.g., ESP32-WROOM-32 |
| **I2C OLED Display** | SSD1306, 128×64 pixels |
| **Wires & Breadboard** | Standard jumper wires |
| **PC / Laptop** | Must have a working webcam |

---

## 💻 Software & Libraries

### Python (PC Side)

| Package | Version |
|---|---|
| `opencv-python` | ≥ 4.8 |
| `mediapipe` | ≥ 0.10 |
| `requests` | ≥ 2.31 |
| `flask` | ≥ 3.0 |
| `numpy` | ≥ 1.24 |

### Arduino / ESP32 Side

| Library | Source |
|---|---|
| `WiFi.h` | ESP32 built-in |
| `WebServer.h` | ESP32 built-in |
| `Preferences.h` | ESP32 built-in (NVS) |
| `DNSServer.h` | ESP32 built-in |
| `ESPmDNS.h` | ESP32 built-in |
| `Update.h` | ESP32 built-in (OTA) |
| `Adafruit_SSD1306` | Arduino Library Manager |
| `FluxGarage_RoboEyes` | [GitHub →](https://github.com/FluxGarage/RoboEyes) |

---

## 🚀 Installation & Setup

### Step 1 — Flash the ESP32

1. Open `DESKBUDDY_StudyTracker.ino` in the **Arduino IDE**.
2. Install the two required libraries via Arduino Library Manager:
   - `Adafruit SSD1306`
   - `FluxGarage_RoboEyes`
3. Flash the firmware to your ESP32. **No Wi-Fi credentials are needed in the code.**
4. On first boot, the OLED displays **"WiFi Setup"** and the ESP32 broadcasts a hotspot named **`DeskBuddy-Setup`**.
5. Connect your phone or laptop to **`DeskBuddy-Setup`**.
6. A captive portal page opens automatically in your browser. Enter your home Wi-Fi SSID and password.
7. DeskBuddy saves the credentials to flash and reboots into normal mode.
8. The **OLED** and **Serial Monitor** display the assigned IP address.

> **💡 Tip:** After setup, DeskBuddy is reachable at `http://deskbuddy.local` via mDNS.  
> macOS and Linux support this natively. Windows users may need to install [Bonjour](https://support.apple.com/kb/DL999).

> **🔒 Optional — Hardcoded credentials:** If you prefer to skip the captive portal, fill in `secrets.h` and set `USE_HARDCODED_WIFI` to `1` in the `.ino` file. **Never commit `secrets.h` to a public repository.**

---

### Step 2 — Set Up Python

```bash
# 1. Clone or download the repository
git clone https://github.com/yourusername/deskbuddy
cd deskbuddy

# 2. Install all dependencies
pip install -r requirements.txt

# 3. Open config.py and set the ESP32 address
#    Use "deskbuddy.local"  (recommended — works via mDNS)
#    OR use the IP shown on the OLED, e.g. "192.168.1.5"
ESP32_IP = "deskbuddy.local"
```

> MediaPipe model files (`.tflite` and `.task`) are **downloaded automatically** on the first run. No manual setup required.

---

### Step 3 — Run the Tracker

```bash
# Standard mode (no camera window)
python study_tracker.py

# Preview mode — shows the live camera feed with detection overlays
python study_tracker.py --preview
```

---

## 🎮 Usage

| Action | What Happens |
|---|---|
| **Sit at your desk** | Face detected → ESP32 study timer starts → OLED eyes go Happy |
| **Leave your desk** | Face gone for 5s → Timer pauses → OLED eyes go Tired |
| **Hold open palm for 0.8s** | Toggles the secondary stopwatch on/off |
| **Return to desk** | Face re-detected → Timer resumes automatically |

---

## 🌐 Dashboard & Interfaces

| Interface | URL | What It Shows |
|---|---|---|
| **PC Dashboard** | `http://localhost:8080` | Study sessions, analytics charts, live ESP32 status, CSV export |
| **ESP32 Hardware UI** | `http://deskbuddy.local` | Eye moods, timer, stopwatch, OTA firmware upload |

### Analytics Charts
- **Daily** — Study hours for the past 7 days (bar chart)
- **Monthly** — Study hours per month for the past 6 months (line chart)
- **Hourly** — Which hours of the day you study most (bar chart, in minutes)

All session data is stored locally in `study_sessions.json` on your PC.

---

## 🔄 OTA Firmware Updates

Update your ESP32 firmware without a USB cable:

1. In Arduino IDE: **Sketch → Export Compiled Binary** to generate a `.bin` file.
2. Open `http://deskbuddy.local/update` in your browser.
3. Select the `.bin` file and click **Upload & Flash**.
4. DeskBuddy reboots automatically with the new firmware.

---

## ♻️ Resetting Wi-Fi

To switch to a different Wi-Fi network or re-run first-boot setup:

1. Open `http://deskbuddy.local` in your browser.
2. Go to the **Settings** tab.
3. Click **Reset WiFi & Setup Mode**.
4. DeskBuddy clears its saved credentials and reboots into captive portal mode.

---

## ⚙️ Configuration Reference (`config.py`)

```python
# ── Network ───────────────────────────────
ESP32_IP       = "deskbuddy.local"  # Or use a static IP like "192.168.1.5"
DASHBOARD_PORT = 8080               # PC dashboard → http://localhost:8080

# ── Detection Tuning ──────────────────────
AWAY_TIMEOUT    = 5     # Seconds without face before session pauses
CHECK_FPS       = 8     # Camera frames processed per second
CAMERA_INDEX    = 0     # 0 = default webcam, 1 = second camera
FACE_CONFIDENCE = 0.5   # Face detection confidence threshold (0.0–1.0)
HAND_CONFIDENCE = 0.1   # Hand detection confidence threshold (0.0–1.0)
PALM_HOLD_SECS  = 0.8   # Seconds to hold open palm to trigger stopwatch
CONFIRM_FRAMES  = 3     # Consecutive frames needed to confirm presence

# ── Data ──────────────────────────────────
SESSION_FILE = "study_sessions.json"  # Local session log (PC-side)
```

---

## 🗂️ File Structure

```
deskbuddy/
├── DESKBUDDY_StudyTracker.ino  # ESP32 firmware (Arduino)
├── secrets.h                   # Wi-Fi fallback credentials (⚠️ add to .gitignore)
├── study_tracker.py            # Main Python tracking script (PC)
├── config.py                   # All user-configurable settings
├── requirements.txt            # Python dependencies
└── study_sessions.json         # Auto-generated session log (PC)
```

> ⚠️ **Add `secrets.h` and `study_sessions.json` to your `.gitignore`** before pushing to a public repository.

---

## 🗺️ Roadmap

- [ ] **Pomodoro Mode** — 50-min study / 10-min break cycles; OLED eyes fall asleep during breaks
- [ ] **Advanced Gestures** — Thumbs up to manually log a session; peace sign to pause
- [ ] **Hardware Enclosure** — 3D-printed desktop case for ESP32 + OLED
- [ ] **Cross-Platform Testing** — Full Linux verification
- [ ] **SQLite Backend** — Upgrade from JSON to SQLite for long-term data retention

---

## 🤝 Credits

- **Animated Eye Graphics** — Powered by the excellent [FluxGarage_RoboEyes](https://github.com/FluxGarage/RoboEyes) library. Original copyright notices are preserved in the source code.
- **Computer Vision** — [MediaPipe Tasks API](https://developers.google.com/mediapipe) by Google.
- **Project** — Built by Gaurav.

---

## 📝 License

This project is open-source and available under the [MIT License](LICENSE).
