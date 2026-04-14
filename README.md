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
   
3. Install the required libraries ( Adafruit SSD1306 and FluxGarage_RoboEyes ).
4. Flash the code to your ESP32 and note the IP address printed on the OLED boot screen or Serial Monitor.
2. Python Setup

    Clone or download this repository to your local machine.

    Install all required dependencies with one command:

Bash

pip install -r requirements.txt

   Open config.py and update the ESP32_IP variable with the IP address from your hardware.

🎮 Usage

   Power up the ESP32. It will connect to your WiFi and display the boot screen.

   Run the Tracker:

Bash

python study_tracker.py

(Add --preview to the command if you want to see the live camera feed and tracking skeletons).

   Start Studying: The webcam will look for your face. Once detected, the ESP32 timer starts.

   Use Gestures: Show an open palm to the camera and hold it for 0.8 seconds to toggle the separate stopwatch.

   View Your Stats: Open http://localhost:8080 in your web browser to view your live dashboard, study streaks, and download your CSV logs.

🤝 Credits & Acknowledgments

   Hardware Animations: The animated eye graphics on the OLED display are powered by the excellent FluxGarage_RoboEyes library. The original copyright notices remain intact within the source code.

   🗺️ Future Roadmap

   Pomodoro Integration: Adding a Pomodoro timer mode where the OLED eyes automatically fall asleep during breaks.

   Hardware Enclosure: Designing a custom 3D-printed desktop case to house the ESP32 and OLED securely.

   Cross-Platform Testing: Verifying and optimizing the Python script for Linux environments.

📝 License

This project is open-source and available under the MIT License.
