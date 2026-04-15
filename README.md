# DeskBuddy Study Tracker 🤖👀  v2.0

An automated, hardware-software hybrid study tracker that uses computer vision to monitor your focus.

The system uses a Python script (OpenCV + MediaPipe) on your PC to detect your presence and gestures, communicating over HTTP with an ESP32. The ESP32 acts as a local web server, managing timers and displaying animated OLED robot eyes.

---

## ✨ Features

### Core
- **Automated Face Tracking** — Study timer starts when you sit down, pauses when you leave.
- **Gesture Controls** — Open palm gesture toggles a secondary stopwatch.
- **Animated OLED Eyes** — React to your presence: Idle → shows study time for 2s → Happy when detected; Tired when away.
- **Unified Dashboard** — Full analytics at `http://localhost:8080`, hardware controls at `http://deskbuddy.local`.

### v2.0 Additions
- **Captive Portal Wi-Fi Setup** — First boot creates a "DeskBuddy-Setup" hotspot. Connect from any phone/laptop and enter your home WiFi credentials via a browser page. No hardcoded passwords.
- **NVS Credential Storage** — WiFi details saved to ESP32 flash (survives power cycles). Reset via Settings tab.
- **mDNS** — Access at `http://deskbuddy.local` instead of remembering an IP.
- **Static IP Fallback** — Attempts `192.168.1.5`; falls back to DHCP with OLED notification.
- **Auto-Reconnect** — Python script monitors ESP32 connectivity and resumes automatically after a power cycle, with no manual restart needed.
- **Timer Overflow Fix** — Correctly handles times over 60 minutes: `01:05:30` instead of `01:65:30`.
- **Proximity-Based Emoji** — Face or hand detected → OLED shows study time for 2s → Happy mood. Nobody home → Tired mood.
- **Analytics Dashboard** — Daily (7-day bar), monthly (line), and hourly distribution (bar) charts rendered via Chart.js.
- **OTA Firmware Updates** — Upload new `.bin` firmware via browser at `/update`. No USB cable needed.
- **Simplified Boot Screen** — Clean "DeskBuddy / by Gaurav" splash replacing the old branded header.

---

## 🛠️ Hardware Requirements

- ESP32 Development Board (e.g., ESP32-WROOM-32)
- I2C OLED Display — SSD1306 128×64
- Connecting wires & breadboard
- PC/Laptop with a webcam

---

## 💻 Software & Libraries

**Python (PC Side)**
- `opencv-python` >= 4.8
- `mediapipe` >= 0.10
- `requests` >= 2.31
- `flask` >= 3.0
- `numpy` >= 1.24

**Arduino / ESP32**
- `WiFi.h` (ESP32 native)
- `WebServer.h` (ESP32 native)
- `Preferences.h` (ESP32 NVS — built-in)
- `DNSServer.h` (ESP32 — built-in)
- `ESPmDNS.h` (ESP32 — built-in)
- `Update.h` (ESP32 OTA — built-in)
- `Adafruit_SSD1306`
- [FluxGarage_RoboEyes](https://github.com/FluxGarage/RoboEyes)

---

## 🚀 Installation & Setup

### 1. ESP32 Setup

1. Open `DESKBUDDY_StudyTracker.ino` in the Arduino IDE.
2. Install the required libraries: `Adafruit_SSD1306` and `FluxGarage_RoboEyes`.
3. Flash the firmware — **no WiFi credentials needed in the code**.
4. On first boot, the OLED shows "WiFi Setup" and the ESP32 creates a hotspot named **DeskBuddy-Setup**.
5. Connect your phone or laptop to **DeskBuddy-Setup**.
6. A setup page automatically opens (captive portal). Enter your home WiFi SSID and password.
7. DeskBuddy saves the credentials and reboots into normal mode.
8. The OLED and Serial Monitor display the assigned IP address.

> **Tip:** DeskBuddy is also accessible at `http://deskbuddy.local` via mDNS (no IP needed on macOS/Linux; Windows requires [Bonjour](https://support.apple.com/kb/DL999)).

### 2. Python Setup

```bash
# Clone or download the repo
git clone https://github.com/yourusername/deskbuddy

# Install dependencies
pip install -r requirements.txt

# Edit config.py — set ESP32_IP to "deskbuddy.local" or the IP shown on OLED
# Then run the tracker
python study_tracker.py

# Optional: open a preview window to see the camera feed
python study_tracker.py --preview
```

### 3. Dashboard Access

| Interface | URL | What it shows |
|---|---|---|
| Unified PC Dashboard | `http://localhost:8080` | Study sessions, analytics graphs, live ESP32 status |
| ESP32 Hardware Controls | `http://deskbuddy.local` | Eye moods, timer, stopwatch, OTA update |

---

## 📊 Analytics

The PC dashboard includes three charts:

- **Daily** — Study hours for the past 7 days (bar chart)
- **Monthly** — Study hours per month for the past 6 months (line chart)
- **Hourly** — Which hours of the day you study most (bar chart, minutes)

All data is stored in `study_sessions.json` on your PC and can be exported as CSV via the Sessions tab.

---

## 🔄 OTA Firmware Updates

1. Build your updated firmware in Arduino IDE: **Sketch → Export Compiled Binary** (`.bin`).
2. Open `http://deskbuddy.local/update` in your browser.
3. Select the `.bin` file and click Upload & Flash.
4. DeskBuddy reboots with the new firmware automatically.

---

## ♻️ Resetting WiFi

To change your WiFi network or re-run the first-boot setup:

1. Open `http://deskbuddy.local` in a browser.
2. Go to the **Settings** tab.
3. Tap **Reset WiFi & Setup Mode**.
4. DeskBuddy clears its saved credentials and reboots into captive portal mode.

---

## 🗺️ Roadmap

- **Pomodoro Mode** — 50-min study / 10-min break cycles; eyes fall asleep during breaks.
- **Advanced Gestures** — Thumbs up to manually log a session; peace sign to pause.
- **Hardware Enclosure** — 3D-printed desktop case for ESP32 + OLED.
- **Cross-Platform Testing** — Full Linux dual-boot verification.
- **SQLite Backend** — Upgrade from JSON to SQLite for long-term data retention.

*Want to contribute? Open an issue or submit a pull request!*
