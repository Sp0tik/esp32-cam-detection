# ESP32-CAM Real-Time Person Detection

An embedded computer-vision project that runs an Edge Impulse object-detection model on an ESP32-CAM and sends Telegram alerts with a photo when a person is detected.

## Overview

The camera captures frames, the device runs inference locally, and a temporal confirmation filter helps reduce false alerts. When a detection is confirmed, the firmware captures a higher-resolution image and sends it to a Telegram chat.

**Built with:** C++, Arduino framework, PlatformIO, ESP32-CAM, Edge Impulse, FOMO, MobileNetV2, Wi-Fi, and Telegram Bot API.

## Highlights

- On-device person detection using an Edge Impulse model.
- Confidence threshold and bounding-box filtering.
- Multi-frame confirmation before triggering an alert.
- Telegram notifications with a captured JPEG photo.
- Alert cooldown to limit repeated notifications.
- Serial Monitor output for setup and detection diagnostics.
- Configurable detection parameters in the firmware.

## Hardware and software

| Component | Details |
|---|---|
| Board | AI Thinker-compatible ESP32-CAM |
| Framework | Arduino |
| Build and upload | PlatformIO |
| ML deployment | Edge Impulse Arduino library |
| Detection approach | FOMO object detection with a MobileNetV2 0.35 backbone |
| Notification service | Telegram Bot API |

The firmware configures a low-resolution frame for inference and switches to VGA for alert photos. Exact performance depends on the camera module, lighting, Wi-Fi connection, and deployed model.

## How it works

1. Initialize the camera and connect to a 2.4 GHz Wi-Fi network.
2. Capture frames and prepare them for the Edge Impulse classifier.
3. Run inference on the ESP32-CAM.
4. Filter detections using confidence, bounding-box size, and consecutive-frame confirmation.
5. When a person is confirmed and the cooldown has elapsed, capture a VGA photo.
6. Send the photo and detection details to Telegram.
7. Print status and diagnostic messages to the Serial Monitor.

## Project screenshots

### Hardware

![ESP32-CAM-MB board and camera module](screenshots/photo_1_esp32cam_mb.jpg)

![Back of the ESP32-CAM and expansion board](screenshots/photo_2_esp32s.jpg)

![Camera module](screenshots/10_ov3660.webp)

![ESP32-CAM pinout](screenshots/7_pinout.webp)

### Edge Impulse model workflow

![Training dataset](screenshots/3_dataset.png)

![Impulse configuration](screenshots/4_impulse.png)

![Image processing configuration](screenshots/2_processing.png)

![Model training](screenshots/1_training.png)

### Runtime and Telegram alerts

![Serial Monitor startup](screenshots/11_serial_start.png)

![Serial Monitor detection flow](screenshots/12_serial_detection.png)

![Telegram activation notification](screenshots/photo_3_tg_activation.jpg)

![Telegram detection photo alert](screenshots/photo_4_tg_detection.jpg)

## Repository structure

```text
esp32-cam-detection/
├── platformio.ini
├── src/
│   └── main.cpp
├── lib/
│   └── D_inferencing/   # Edge Impulse inference library and model
├── include/
├── test/
└── screenshots/         # Hardware, model, and runtime screenshots
```

## Getting started

### Prerequisites

- [Visual Studio Code](https://code.visualstudio.com/)
- [PlatformIO IDE](https://platformio.org/install/ide?install=vscode)
- An ESP32-CAM board and a suitable USB-to-serial programmer or ESP32-CAM-MB
- A 2.4 GHz Wi-Fi network
- A Telegram bot and the destination chat ID

### 1. Clone the repository

```bash
git clone https://github.com/Sp0tik/esp32-cam-detection.git
cd esp32-cam-detection
```

Open the project folder in VS Code with PlatformIO installed.

### 2. Check the Edge Impulse library

The firmware includes `D_inferencing.h` from the Edge Impulse library under `lib/D_inferencing/`. If the model library is missing or you want to use your own trained model, export an Arduino library from [Edge Impulse Studio](https://studio.edgeimpulse.com/) and install it according to the generated library structure.

### 3. Configure Wi-Fi and Telegram

The current firmware reads these settings from macros near the top of `src/main.cpp`:

```cpp
#define WIFI_SSID       "Change"
#define WIFI_PASSWORD   "Change"
#define BOT_TOKEN       "Change"
#define CHAT_ID         "Change"
```

Replace the placeholder values on your local machine before uploading the firmware.

**Security:** never commit real Wi-Fi credentials, Telegram bot tokens, or private chat identifiers. Keep local credential changes out of commits. If a token is exposed, revoke it through [@BotFather](https://t.me/BotFather) and create a new one.

To create a bot, message BotFather in Telegram and follow the `/newbot` flow. Obtain the destination chat ID for the chat where alerts should be delivered.

### 4. Connect and upload

Connect the board over USB. Check the correct serial port in PlatformIO and update `monitor_port` in `platformio.ini` if necessary.

Build the firmware:

```bash
pio run -e esp32cam
```

Upload it:

```bash
pio run -e esp32cam --target upload
```

Open the serial monitor:

```bash
pio device monitor -b 115200
```

If upload fails, follow the boot-mode instructions for your specific ESP32-CAM board. Some boards require GPIO0 to be connected to GND while entering flashing mode; disconnect it from GND before normal operation.

## Detection settings

The following parameters are defined near the top of `src/main.cpp`:

| Parameter | Default | Purpose |
|---|---:|---|
| `CONFIDENCE_THR` | `0.70` | Minimum detection confidence |
| `ALERT_COOLDOWN` | `30000` ms | Minimum interval between alerts |
| `CONFIRM_FRAMES` | `3` | Consecutive frames required to confirm a person |
| `MIN_BOX_SIZE` | `20` | Minimum accepted bounding-box size |
| `STABILITY_FRAMES` | `2` | No-detection frames used to reset confirmation |

Adjust these values only after testing with the target camera position and lighting. A higher confidence threshold or longer confirmation window may reduce false alerts but can also miss brief or distant detections.

## Model evaluation

The previous project notes report the following model evaluation values:

| Metric | Reported value |
|---|---:|
| Precision | 93.6% |
| Recall | 88.0% |
| F1-score | 0.907 |

These figures describe the recorded model evaluation, not a guarantee of real-world detection performance. Results can vary with the dataset, lighting, camera placement, and distance from the camera.

## Troubleshooting

| Issue | Things to check |
|---|---|
| Camera initialization fails | Reseat the camera ribbon cable, confirm the sensor and pin configuration, and check the serial error code. |
| No serial output | Select the correct COM/serial port and set the baud rate to 115200; reset the board after opening the monitor. |
| Wi-Fi connection fails | Verify the SSID and password and use a 2.4 GHz network. |
| Telegram alert is not received | Check bot token and chat ID locally, ensure the bot can message the destination chat, and check serial output. |
| Photo upload fails | Check Wi-Fi stability, available power, camera frame capture, and the Telegram API response. |
| Too many false detections | Test a higher confidence threshold or require more consecutive confirmation frames. |
| Upload fails | Check the selected serial port, USB cable, power supply, and board flashing mode. |

## References

- [Espressif ESP32 documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/)
- [Edge Impulse documentation](https://docs.edgeimpulse.com/)
- [FOMO object detection](https://docs.edgeimpulse.com/docs/edge-impulse-studio/learning-blocks/object-detection/fomo-object-detection-for-constrained-devices)
- [Telegram Bot API](https://core.telegram.org/bots/api)
- [PlatformIO ESP32 platform](https://docs.platformio.org/en/latest/platforms/espressif32.html)
- Sandler et al., [MobileNetV2: Inverted Residuals and Linear Bottlenecks](https://arxiv.org/abs/1801.04381), 2018.

---

Made as an embedded AI / IoT project using ESP32-CAM and Edge Impulse.
