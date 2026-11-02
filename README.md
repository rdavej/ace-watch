# Ace Watch

DevX / Ace firmware for the Waveshare ESP32-C6-Touch-AMOLED-2.06 (410 × 502 AMOLED).

## Features
- DevX Pulse face: time, date, battery %, steps today / 8,000 goal (Electric blue look)
- Step counter (QMI8658 motion sensor, counts only in a walking rhythm; saved, resets at midnight)
- Raise to wake: lift your wrist to look and the screen turns on
- Screen off after 20 s, tap to wake
- **Wi-Fi setup:** hold the screen 3 s (or first boot) → join `Ace-Watch-Setup` on your phone → pick Wi-Fi + password. Remembers up to 5 networks.
- Every 30 min: joins the strongest known Wi-Fi, syncs the time, checks for an update, then turns Wi-Fi off to save battery
- **Wi-Fi updates (OTA):** installs the latest GitHub release automatically; rolls back if a new version fails to start

## Releasing a new version
Bump `version.txt` and push to `main`. CI builds and publishes a release; watches update on their next sync.

## Build & flash (laptop)
ESP-IDF v6.x, target `esp32c6`. In VS Code: open this folder, pick `esp32c6`, press 🔥 (Build, Flash and Monitor).
