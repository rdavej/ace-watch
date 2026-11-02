# Ace Watch

DevX / Ace firmware for the Waveshare ESP32-C6-Touch-AMOLED-2.06 (410 × 502 AMOLED).

## Features
- DevX Pulse face: time, date, battery %, steps today / 8,000 goal (Electric blue look)
- Step counter (QMI8658): counts once 8 evenly spaced steps arrive at walking/running pace; ignores sharp jolts (combing, waving). Saved, resets at midnight, last 7 days kept
- Raise to wake: learns your viewing angle from tap-to-wake, then wakes when you lift your wrist into it
- Goal reached: bar turns yellow + "GOAL REACHED!" at 8,000 steps
- Low battery: red under 15%, "Charge me" under 5%
- Charging screen: barbell loading plates (more plates = more charge)
- Battery saving: light sleep, no redraws while dark, 25 Hz motion
- Screen off after 20 s, tap to wake
- **Wi-Fi setup:** hold the screen 3 s (or first boot) → join `Ace-Watch-Setup` on your phone → pick Wi-Fi + password. Remembers up to 5 networks.
- Every 30 min: joins the strongest known Wi-Fi, syncs the time, checks for an update, then turns Wi-Fi off to save battery
- **Wi-Fi updates (OTA):** installs the latest GitHub release automatically; rolls back if a new version fails to start

## Releasing a new version
Bump `version.txt` and push to `main`. CI builds and publishes a release; watches update on their next sync.

## Build & flash (laptop)
ESP-IDF v6.x, target `esp32c6`. In VS Code: open this folder, pick `esp32c6`, press 🔥 (Build, Flash and Monitor).
