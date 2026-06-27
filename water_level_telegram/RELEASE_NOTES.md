# YeVoda Project Release Notes

## [v1.1] - 2026-06-28
### Added
- **Robotic Alarm Melodies**: Ported Everton Ramires' `CuteBuzzerSounds` player logic (containing `S_HAPPY`, `S_CUDDLY`, `S_SUPER_HAPPY`, etc.) directly into the sketch to cycle sounds on consecutive triggers.
- **Melody Looping**: Melodies loop continuously during empty tank alarms with a quick 0.2s pause.
- **Smooth Opacity Blinking**: Replaced blocky character swapping (`🔴` / `⚫`) with smooth CSS opacity transitions (`1.0` ↔ `0.15`) at a 50ms polling loop, creating a premium light pulse effect.
- **White Wi-Fi Icons & Shifted Layout**: Status icons are styled white (`#ffffff`) and shifted 15px left relative to the status text for a cleaner header layout.

### Changed
- **Credentials Security**: Private Wi-Fi credentials and Telegram Bot Token replaced with generic placeholders to prevent repository exposure.
- **Empty State Timing**: The indicator switches immediately to solid red `🔴` for exactly 5 seconds upon water loss, before entering the melody loop phase.
- **Client-Side Rendering Delegation**: Moved all fading, timing, and blinking logic to local client-side JavaScript, leaving the ESP32 to only serve raw data. This significantly reduces CPU and network overhead.
- **Subscriber Bypass**: The Telegram notification queue completely shuts down when there are no active subscribers, eliminating unnecessary secure socket negotiations and Serial log spam.
- **Driver Optimizations**: Swapped low-level ESP32 PWM `ledcWrite` calls for standard `tone()`/`noTone()` calls, preventing driver channel conflicts with playCuteSound.

### Fixed
- **Arduino Compilation Scopes**: Moved the `TelegramMessage` enum definition to the top of the C++ file, resolving IDE auto-generated prototype compilation errors.
- **Arduino Core v3.0 Compatibility**: Ported `CuteBuzzerSounds` logic directly, resolving legacy `ledcSetup` compilation errors on ESP32 Arduino Core v3.x.

---

## [v1.0] - 2026-06-27
### Added
- **Always-on Web Server**: Accessible continuously at `http://voda.local` (mDNS) over local home Wi-Fi and fallback SoftAP.
- **SoftAP Captive Portal**: Seamless redirection of client mobile devices to the configuration portal on connecting to the open `YeVoda` Access Point.
- **Flat VSCode Theme**: A dark-theme UI featuring warm-grey background panels, blue headers, and gold highlight accents (no borders or card shadows).
- **Collapsible Spoiler**: SSID/Password credentials form wrapped in a summary panel (starts closed if connected, open if disconnected).
- **Asynchronous AJAX Controls**: Forms are saved and indicators are toggled asynchronously via `fetch` without page reloads.
- **iOS Safari Focus Optimization**: Configured input font sizes to `16px` to prevent viewport auto-zooming, and disabled focus rings during toggle switch clicks.
- **Password Paste Toggle**: Added a show/hide password toggle to bypass mobile clipboard restrictions.
- **Splashing Debounce**: Configured a `1500ms` lockout delay to handle rapid water splashes.
