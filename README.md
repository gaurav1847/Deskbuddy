# Deskbuddy Study Tracker 🤖👀

An automated, hardware-software hybrid study tracker that uses computer vision to monitor your focus. 

The system utilizes a Python-based tracking script (OpenCV + MediaPipe Tasks API) on your PC to detect your presence and gestures, seamlessly communicating over HTTP with an ESP32. The ESP32 acts as a local web server, managing timers and bringing the tracker to life with animated OLED eyes.

## ✨ What's New in v1.1
* **Study Streaks & CSV Export:** The local dashboard now tracks your daily streaks, total session counts, and allows you to download your entire study history as a CSV file.
* **Auto-Downloading Models:** The Python script automatically fetches the required MediaPipe `.tflite` and `.task` models on the first run.
* **Centralized Config:** Easily tweak camera framing, network IPs, and detection thresholds in a clean `config.py` file.
* **Enhanced Security:** WiFi credentials are now safely isolated in a `secrets.h` file.
* **Branded Hardware Interface:** The ESP32 now features a custom boot splash screen and independently manages the study timer (face) and stopwatch (palm gestures).

## 🛠️ Hardware Requirements

* ESP32 Development Board (e.g., ESP32-WROOM)
* I2C OLED Display (e.g., SSD1306)
* Connecting wires & breadboard
* PC/Laptop with a webcam

## 🚀 Installation & Setup

### 1. ESP32 Setup
1. Open the `.ino` file in the Arduino IDE.
2. **Crucial Security Step:** Create a new tab/file in the Arduino IDE named `secrets.h` and add your WiFi credentials like this:
   ```cpp
   #ifndef SECRETS_H
   #define SECRETS_H
   const char* ssid     = "YOUR_WIFI_NAME";
   const char* password = "YOUR_WIFI_PASSWORD";
   #endif
