# Ace Watch

DevX / Ace firmware for the Waveshare ESP32-C6-Touch-AMOLED-2.06 (410 × 502 AMOLED).

## Features
- Startup: DevX logo with a loading bar (skipped after a crash so it's back fast)
- **Pulse Ring face:** steps ring around the edge (yellow when the 8,000 goal is reached), date, time, steps; bottom row battery · sets in this workout · **in gym now** (members training at the gym, from DevX) · GATE; a small bar + count at the very bottom when DevX alerts are waiting. Status text only appears when there's news (syncing, offline, last restart)
- **Ace pop-up:** a new DevX notification slides up over the face as an Ace card (Dismiss / Open) with a ding and lights the screen
- Step counter (QMI8658): counts once 8 evenly spaced steps arrive at walking/running pace; ignores sharp jolts (combing, waving). Saved, resets at midnight, last 7 days kept
- Raise to wake: learns your viewing angle from tap-to-wake, then wakes when you lift your wrist into it (arm must be really down ~0.8 s first, come up within 1 s and be held steady). **Raise wake** tile in the pull-down panel turns it off (driving). Lower the wrist again and a raise-woken screen turns off after ~2 s
- Palm cover: cover the screen with your palm to turn it off straight away
- **Workout screen (swipe left, swipe right to go back):** rest ring countdown, presets 60 s / 90 s / 2 min, big SET DONE button (adds a set + starts rest), workout clock. Beeps at 3-2-1, rest over → three beeps and the screen lights up yellow "GO!". Tap the ring twice while resting to skip the rest. END (tap twice) stops the workout clock and shows the total
- **Swipe left through:** Face → Workout → Stopwatch → Steps → Alerts → Switches → Settings (swipe right to go back)
- **Gate on the face:** tap GATE twice to open the gym gate (shows once the watch key is set)
- **Stopwatch:** START/STOP, LAP while running, RESET when stopped, laps list with the fastest in yellow; keeps running in the background and shows on the face
- **Steps screen:** last 7 days as bars with the 8,000 goal line and the daily average
- **DevX alerts:** a copy of the pushes DevX/Ace send the Director, checked at every sync (every 30 min) and when you open the screen; new ones pop up as a banner with a ding
- **Switches:** Smart Life switches through the DevX server (tap a row to switch it), plus OPEN GATE (tap twice) for the gym entrance gate. Wi-Fi connects on the first tap and stays on 1 min for the next one
- **Pull-down panel (swipe down on the face):** Wi-Fi/sync status, battery, Sync now, Live alerts, Raise wake, brightness, sound, screen-off time, Wi-Fi. Swipe up to close
- **Wi-Fi on the watch:** panel → Wi-Fi: nearby networks (strongest first, Saved / On marked) → tap one → type the password on the watch keyboard → Join. Saved and open networks join with one tap. "Setup page (DevX key)" at the bottom opens the phone setup page (still needed once for the DevX key)
- **Steps to DevX:** at every sync the watch sends today + the last 6 days to DevX; they show as "Steps today" on your member app Home (devx-gym-backend PR #254)
- **Live alerts (off by default):** stays on a known Wi-Fi and checks DevX every minute so Ace pop-ups arrive within ~1 min (uses more battery); off = check every 30 min
- **Settings:** brightness, sound (off / low / high), screen-off time (10 / 20 / 30 s)
- **DevX key:** alerts and switches need `WATCH_TOKEN` on the server (devx-gym-backend) and the same key typed into the setup page (hold the face 3 s)
- Goal reached: bar turns yellow + "GOAL REACHED!" + a chirp at 8,000 steps
- Low battery: red under 15%, "Charge me" under 5%
- Charging screen: barbell loading plates (more plates = more charge)
- Battery saving: slower CPU and no redraws while dark, 25 Hz motion
- **Rest mode:** lying still with the screen dark for 10 min (1 h if a workout is going; never while charging or the stopwatch runs) → deep sleep: screen and touch asleep, Wi-Fi off, only a peek at the motion sensor every 6 s. Pick it up (or plug in the charger) and it's back in a few seconds and syncs DevX straight away
- Screen off after 20 s, tap to wake
- Black box: remembers why it last restarted (crash, freeze, power dip, watchdog) and what it was doing; shows "Last restart: …" for 15 s after boot. Freeze guard restarts the watch if the screen task stops for 10 s
- Sync shows each step in the footer (Wi-Fi → Time → Update)
- Logging is switched off on battery (no laptop listening)
- **Wi-Fi setup:** hold the screen 3 s (or first boot) → join `Ace-Watch-Setup` on your phone → pick Wi-Fi + password. Remembers up to 5 networks.
- Every 30 min: joins the strongest known Wi-Fi, syncs the time, checks for an update, then turns Wi-Fi off to save battery
- **Wi-Fi updates (OTA):** installs the latest GitHub release automatically; rolls back if a new version fails to start

## Releasing a new version
Bump `version.txt` and push to `main`. CI builds and publishes a release; watches update on their next sync.

## Build & flash (laptop)
ESP-IDF v6.x, target `esp32c6`. In VS Code: open this folder, pick `esp32c6`, press 🔥 (Build, Flash and Monitor).
