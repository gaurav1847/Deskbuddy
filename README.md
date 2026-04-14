# Deskbuddy Study Tracker 🤖👀

An automated, hardware-software hybrid study tracker that uses computer vision to monitor your focus. 

The system utilizes a Python-based tracking script (OpenCV + MediaPipe) on your PC to detect your presence and gestures, seamlessly communicating over HTTP with an ESP32. The ESP32 acts as a local web server, managing timers and bringing the tracker to life with animated OLED eyes.

## ✨ Features

* **Automated Face Tracking:** Automatically starts the study timer when you are at your desk and pauses it when you leave, using MediaPipe face detection.
* **Gesture Controls:** Uses an "open palm" gesture detection to toggle a secondary stopwatch without needing to touch your keyboard or mouse.
* **Hardware Feedback:** Features animated robot eyes on an OLED display that react to the system's state.
* **Local Web Dashboard:** Hosts a local web dashboard (port 8080) to view your study logs, timers, and control the hardware remotely.
* **Seamless API Communication:** The PC and ESP32 communicate rapidly via lightweight HTTP requests on your local network.

## 🛠️ Hardware Requirements

* ESP32 Development Board (e.g., ESP32-WROOM)
* I2C OLED Display (e.g., SSD1306)
* Connecting wires & breadboard
* PC/Laptop with a webcam (for the Python tracking module)

## 💻 Software & Libraries

**Python (PC Side):**
* `opencv-python`
* `mediapipe`
* `requests`
* `flask` 

**C++ / Arduino (ESP32 Side):**
* WebServer library (ESP32 native)
* [FluxGarage_RoboEyes](https://github.com/FluxGarage/RoboEyes) (for OLED animations)

## 🚀 Installation & Setup

### 1. ESP32 Setup
1. Open the `.ino` file in the Arduino IDE.
2. Update the Wi-Fi credentials (`SSID` and `PASSWORD`) to match your local network.
3. Install the required OLED and RoboEyes libraries.
4. Flash the code to your ESP32 and note the IP address printed in the Serial Monitor.

### 2. Python Setup
1. Clone or download this repository to your local machine.
2. Install the required Python dependencies:
   ```bash
   pip install opencv-python mediapipe requests flask
   ## 🗺️ Future Roadmap (v1.1 and Beyond)

This project is actively being improved. Here are a few features planned for future releases:

* **Persistent Data Storage:** Upgrading the Python backend to log study sessions into a CSV or SQLite database, so you can track your total study hours over months of exam preparation.
* **Pomodoro Integration:** Adding a Pomodoro timer mode (e.g., 50 minutes of tracking, 10-minute break) where the OLED eyes automatically fall asleep during the break period.
* **Advanced Gesture Controls:** Adding new MediaPipe gestures (like a "thumbs up" to manually log a session, or a "peace sign" to pause the tracker).
* **Hardware Enclosure:** Designing and 3D printing a custom desktop case to house the ESP32 and OLED securely, moving it off the breadboard.
* **Cross-Platform Testing:** Verifying and optimizing the Python script for Linux environments to ensure seamless dual-boot compatibility.

*Want to contribute? Feel free to open an issue or submit a pull request!*
