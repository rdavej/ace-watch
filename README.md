# Atlas Helmet HUD

Firmware for the Waveshare ESP32-C6-Touch-AMOLED-2.06 (410 × 502 AMOLED), used as a helmet HUD.
The board lies face-up in the chin bar under a loupe lens, and the picture reflects off the visor,
so it is drawn flipped.

Since v0.9.0 the board runs **only the HUD**: the Ace Watch firmware (watch face, steps, DevX alerts,
switches, workout, stopwatch) was removed. It's in the git history up to v0.8.0.

## What it does
- Starts straight into the HUD: start-up (ring draws, Atlas wakes, greeting, checks), then the riding dash
- Everything sits in one round area under the lens; black everywhere else
- **Tap:** next screen — Dash → Next turn → Alert → Call → Start-up
- **Hold 3 s:** set-up — arrows move the circle, − / + change its size, Flip (up-down / none / both / left-right),
  ⟳ checks Wi-Fi and updates now, Done saves. Shows the version, Wi-Fi status and battery
- Speed, heading, turns, the call and the song are demo data until the phone link is added
- Wi-Fi: uses the networks the watch saved; at start-up and every 30 min it sets the clock and installs
  a newer release, then turns Wi-Fi off
- Black box (diag.c): remembers why it restarted; restarts itself if the screen task freezes

## Updates
Pushing to `main` with a new `version.txt` builds and publishes a release (`ace_watch.bin` + `version.txt`);
the HUD picks it up over Wi-Fi.
