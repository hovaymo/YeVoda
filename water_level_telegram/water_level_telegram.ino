#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

// Pins
const uint8_t WATER_SENSOR_PIN = 4;  // Yellow OUT from XKC-Y25-NPN
const uint8_t LED_PIN = 8;           // Onboard addressable NeoPixel LED
const uint8_t BUZZER_PIN = 2;        // Buzzer pin (Passive or Active)
const uint8_t LED_COUNT = 1;
const uint8_t BUZZER_RESOLUTION = 8;
const uint8_t BUZZER_DUTY = 128;
const unsigned int BUZZER_ALARM_LOW_HZ = 2600;
const unsigned int BUZZER_ALARM_HIGH_HZ = 4300;

// Fallback Wi-Fi (leave empty or set your own credentials here)
const char* DEFAULT_WIFI_SSID = "Your_WiFi_SSID";
const char* DEFAULT_WIFI_PASSWORD = "Your_WiFi_Password";

// Telegram Bot configuration
const char* BOT_TOKEN = "YOUR_TELEGRAM_BOT_TOKEN";




// Sensor logic
const bool SENSOR_LOW_MEANS_WATER_PRESENT = true;

// Timing
const unsigned long SENSOR_DEBOUNCE_MS = 300;  // Quick 300ms debounce
const unsigned long STATE_LOCKOUT_MS = 1500;   // Ignore rapid toggles within 1.5 seconds
const unsigned long TELEGRAM_MIN_REPEAT_MS = 30UL * 60UL * 1000UL;
const unsigned long LED_EMPTY_HOLD_GREEN_MS = 0;
const unsigned long LED_EMPTY_FADE_MS = 5000;
const unsigned long LED_RED_SOLID_MS = 2000;
const unsigned long LED_RED_BLINK_MS = 250;

enum TelegramMessage {
  MSG_NONE,
  MSG_EMPTY,
  MSG_BACK
};
volatile TelegramMessage pendingTelegramMessage = MSG_NONE;

// Pre-defined robotic melodies array to cycle through (indexes 0 to 7)
const int CUTE_MELODIES_COUNT = 8;
int currentMelodyIndex = 0;
bool alarmMelodyPlayed = false;
unsigned long lastMelodyFinishedAt = 0;

// Note frequencies for robot sounds
#define CUTE_NOTE_E5  659
#define CUTE_NOTE_E6  1319
#define CUTE_NOTE_A6  1760
#define CUTE_NOTE_G6  1568
#define CUTE_NOTE_D7  2349
#define CUTE_NOTE_B5  988
#define CUTE_NOTE_C6  1047
#define CUTE_NOTE_D5  587
#define CUTE_NOTE_G5  784
#define CUTE_NOTE_A5  880

void _cuteTone(float noteFrequency, long noteDuration, int silentDuration) {
  if (silentDuration == 0) { silentDuration = 1; }
  tone(BUZZER_PIN, (unsigned int)noteFrequency);
  delay(noteDuration);
  noTone(BUZZER_PIN);
  delay(silentDuration);
}

void cuteBendTones(float initFrequency, float finalFrequency, float prop, long noteDuration, int silentDuration) {
  if (silentDuration == 0) { silentDuration = 1; }
  if (initFrequency < finalFrequency) {
    for (float i = initFrequency; i < finalFrequency; i = i * prop) {
      _cuteTone(i, noteDuration, silentDuration);
    }
  } else {
    for (float i = initFrequency; i > finalFrequency; i = i / prop) {
      _cuteTone(i, noteDuration, silentDuration);
    }
  }
}

void playCuteSound(int soundName) {
  switch(soundName) {
    case 0: // S_HAPPY
      cuteBendTones(1500, 2500, 1.05, 20, 8);
      cuteBendTones(2499, 1500, 1.05, 25, 8);
      break;
    case 1: // S_CUDDLY
      cuteBendTones(700, 900, 1.03, 16, 4);
      cuteBendTones(899, 650, 1.01, 18, 7);
      break;
    case 2: // S_SUPER_HAPPY
      cuteBendTones(2000, 6000, 1.05, 8, 3);
      delay(50);
      cuteBendTones(5999, 2000, 1.05, 13, 2);
      break;
    case 3: // S_OHOOH
      cuteBendTones(880, 2000, 1.04, 8, 3);
      delay(200);
      for (float i = 880; i < 2000; i = i * 1.04) {
        _cuteTone(CUTE_NOTE_B5, 5, 10);
      }
      break;
    case 4: // S_SURPRISE
      cuteBendTones(800, 2150, 1.02, 10, 1);
      cuteBendTones(2149, 800, 1.03, 7, 1);
      break;
    case 5: // S_CONNECTION
      _cuteTone(CUTE_NOTE_E5, 50, 30);
      _cuteTone(CUTE_NOTE_E6, 55, 25);
      _cuteTone(CUTE_NOTE_A6, 60, 10);
      break;
    case 6: // S_MODE1
      cuteBendTones(CUTE_NOTE_E6, CUTE_NOTE_A6, 1.02, 30, 10);
      break;
    case 7: // S_JUMP
      cuteBendTones(880, 2000, 1.04, 8, 3);
      delay(200);
      break;
  }
}

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRBW + NEO_KHZ800);
WiFiClientSecure securedClient;
UniversalTelegramBot bot(BOT_TOKEN, securedClient);

// Wi-Fi Config
String wifiSsid = "";
String wifiPassword = "";
bool ledEnabled = true;
bool buzzerEnabled = true;

bool apModeActive = false;
bool wifiConnecting = true;
bool wifiConnectionFailed = false;
unsigned long wifiConnectionStartedAt = 0;
unsigned long wifiConnectedAt = 0;

WebServer webServer(80);
DNSServer dnsServer;

// BoxBox styled webpage html
const char HTML_CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>YeVoda Config</title>
<style>
  body {
    margin: 0;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    background-color: #1a1a22;
    color: #cbd5e1;
    display: flex;
    justify-content: center;
    align-items: flex-start;
    min-height: 100vh;
    padding-top: 30px;
    box-sizing: border-box;
  }
  .content {
    width: 320px;
    text-align: center;
    padding: 10px;
  }
  .header-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 25px;
  }
  .logo-title {
    font-weight: 700;
    color: #38bdf8;
    font-size: 24px;
    letter-spacing: -0.02em;
  }
  .header-right {
    display: flex;
    align-items: center;
  }
  #wifi-icon {
    display: inline-flex;
    align-items: center;
    margin-right: 15px;
  }
  #wifi-status {
    font-size: 16px;
    font-weight: 600;
    color: #cbd5e1;
  }
  details {
    margin-bottom: 20px;
    text-align: left;
  }
  summary {
    display: flex;
    justify-content: space-between;
    align-items: center;
    padding: 12px 14px;
    background-color: #17171f;
    border: 1px solid #334155;
    color: #cbd5e1;
    font-size: 15px;
    font-weight: 600;
    border-radius: 6px;
    cursor: pointer;
    outline: none;
    user-select: none;
    transition: border-color 0.2s;
  }
  summary:hover {
    border-color: #f59e0b;
  }
  summary::-webkit-details-marker {
    display: none;
  }
  summary::after {
    content: "▼";
    font-size: 10px;
    color: #f59e0b;
    transition: transform 0.2s;
  }
  details[open] summary::after {
    transform: rotate(180deg);
  }
  .spoiler-content {
    padding: 15px 5px 5px 5px;
  }
  .group {
    margin-bottom: 20px;
    text-align: left;
  }
  label {
    display: block;
    font-size: 13px;
    color: #f59e0b;
    margin-bottom: 6px;
    text-transform: uppercase;
    letter-spacing: 0.05em;
    font-weight: 600;
  }
  input[type="text"], input[type="password"] {
    width: 100%;
    box-sizing: border-box;
    padding: 12px 10px;
    border: 1px solid #334155;
    background-color: #17171f;
    color: #f8fafc;
    border-radius: 6px;
    font-size: 16px;
    outline: none;
    transition: border-color 0.2s;
  }
  input:focus {
    border-color: #f59e0b;
    box-shadow: none;
  }
  .btn {
    width: 100%;
    padding: 14px;
    background-color: #f59e0b;
    border: none;
    color: #171722;
    font-weight: 700;
    border-radius: 6px;
    font-size: 16px;
    cursor: pointer;
    transition: background-color 0.2s, transform 0.1s;
  }
  .btn:hover {
    background-color: #d97706;
  }
  .btn:active {
    transform: scale(0.98);
  }
  .divider {
    height: 1px;
    background-color: #2d2d3a;
    margin: 20px 0;
  }
  .toggle-row {
    display: flex;
    justify-content: space-between;
    align-items: center;
    margin-bottom: 15px;
  }
  .toggle-label {
    font-size: 16px;
    color: #e2e8f0;
  }
  .switch {
    position: relative;
    display: inline-block;
    width: 50px;
    height: 28px;
  }
  .switch input {
    opacity: 0;
    width: 0;
    height: 0;
  }
  .slider {
    position: absolute;
    cursor: pointer;
    top: 0; left: 0; right: 0; bottom: 0;
    background-color: #475569;
    transition: .3s;
    border-radius: 24px;
  }
  .slider:before {
    position: absolute;
    content: "";
    height: 20px;
    width: 20px;
    left: 4px;
    bottom: 4px;
    background-color: white;
    transition: .3s;
    border-radius: 50%;
  }
  input:checked + .slider {
    background-color: #f59e0b;
  }
  input:checked + .slider:before {
    transform: translateX(22px);
  }
</style>
</head>
<body>
<div class="content">
  <div class="header-row">
    <span class="logo-title">YeVoda</span>
    <div class="header-right">
      <span id="wifi-icon">%WIFI_ICON%</span>
      <span id="wifi-status">%WIFI_STATUS%</span>
    </div>
  </div>
  
  <details %SPOILER_STATE%>
    <summary>Налаштування Wi-Fi</summary>
    <div class="spoiler-content">
      <form id="wifi-form" onsubmit="saveWiFi(event)">
        <div class="group">
          <label>Wi-Fi Мережа</label>
          <input type="text" name="ssid" id="ssid-input" value="%SSID%" required placeholder="Введіть назву">
        </div>
        <div class="group">
          <label>Пароль</label>
          <div style="position: relative;">
            <input type="password" name="pass" id="pass-input" value="%PASS%" placeholder="Введіть пароль" style="padding-right: 85px;">
            <span id="toggle-eye" onclick="togglePassView()" style="position: absolute; right: 12px; top: 50%; transform: translateY(-50%); cursor: pointer; color: #f59e0b; font-size: 12px; font-weight: bold; user-select: none; text-transform: uppercase;">Показати</span>
          </div>
        </div>
        <button type="submit" class="btn">Зберегти та підключити</button>
      </form>
    </div>
  </details>
  
  <div class="divider"></div>
  
  <div class="toggle-row">
    <span class="toggle-label">Світлодіод 💡</span>
    <label class="switch" onclick="event.stopPropagation();">
      <input type="checkbox" id="led-chk" %LED_CHECKED% onchange="toggle('led')">
      <span class="slider"></span>
    </label>
  </div>
  
  <div class="toggle-row">
    <span class="toggle-label">Звук бузера 🔊</span>
    <label class="switch" onclick="event.stopPropagation();">
      <input type="checkbox" id="buz-chk" %BUZ_CHECKED% onchange="toggle('buz')">
      <span class="slider"></span>
    </label>
  </div>
</div>

<script>
  // White SVGs
  var wifiOn = `<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" style="vertical-align: middle;"><path d="M5 12.55a11 11 0 0 1 14.08 0"></path><path d="M1.42 9a16 16 0 0 1 21.16 0"></path><path d="M8.53 16.11a6 6 0 0 1 6.95 0"></path><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="3"></line></svg>`;
  var wifiOff = `<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#ffffff" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round" style="vertical-align: middle;"><line x1="1" y1="1" x2="23" y2="23"></line><path d="M16.72 11.06A10.94 10.94 0 0 1 19 12.55"></path><path d="M5 12.55a10.94 10.94 0 0 1 5.83-2.84"></path><path d="M1.42 9a16 16 0 0 1 12.58-4.7"></path><path d="M20 7.24a15.7 15.7 0 0 1 2.58 1.76"></path><path d="M8.53 16.11a6 6 0 0 1 6.95 0"></path><line x1="12" y1="20" x2="12.01" y2="20" stroke-width="3"></line></svg>`;

  let waterPresent = true;
  let elapsedMs = 0;
  let lastUpdateAt = Date.now();
  let currentEmoji = "";
  let currentOpacity = "";

  function saveWiFi(event) {
    event.preventDefault();
    var ssid = document.getElementById('ssid-input').value;
    var pass = document.getElementById('pass-input').value;
    var params = 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass);
    
    fetch('/save', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/x-www-form-urlencoded'
      },
      body: params
    });
    document.querySelectorAll('input[type="text"], input[type="password"]').forEach(el => el.blur());
  }

  function toggle(type) {
    var chk = document.getElementById(type + '-chk').checked;
    fetch('/toggle?type=' + type + '&val=' + (chk ? '1' : '0'));
    // Force keyboard dismiss and prevent focus shifting
    document.querySelectorAll('input[type="text"], input[type="password"]').forEach(el => el.blur());
  }

  function togglePassView() {
    var input = document.getElementById('pass-input');
    var btn = document.getElementById('toggle-eye');
    if (input.type === 'password') {
      input.type = 'text';
      btn.innerText = 'Приховати';
    } else {
      input.type = 'password';
      btn.innerText = 'Показати';
    }
  }

  function getStatusEmoji(water, elapsed) {
    if (water) {
      return { emoji: "🟢", opacity: "1" };
    }
    const solidRedTime = 5000; // 5 seconds solid red
    
    if (elapsed < solidRedTime) {
      return { emoji: "🔴", opacity: "1" };
    }
    
    // Blinking phase (500ms cycle, 250ms ON, 250ms OFF)
    const alarmFor = elapsed - solidRedTime;
    const phase = alarmFor % 500;
    const opacity = (phase < 250) ? "1" : "0.15";
    return { emoji: "🔴", opacity: opacity };
  }

  function updateLocalState() {
    const now = Date.now();
    const dt = now - lastUpdateAt;
    lastUpdateAt = now;
    
    if (!waterPresent) {
      elapsedMs += dt;
    } else {
      elapsedMs = 0;
    }
    
    const res = getStatusEmoji(waterPresent, elapsedMs);
    
    var emojiEl = document.getElementById('water-emoji');
    if (emojiEl) {
      if (currentEmoji !== res.emoji) {
        currentEmoji = res.emoji;
        emojiEl.innerText = res.emoji;
      }
      if (currentOpacity !== res.opacity) {
        currentOpacity = res.opacity;
        emojiEl.style.opacity = res.opacity;
      }
    }
  }

  function fetchStatus() {
    fetch('/status_json')
      .then(response => response.json())
      .then(data => {
        document.getElementById('wifi-icon').innerHTML = data.connected ? wifiOn : wifiOff;
        document.getElementById('led-chk').checked = data.led;
        document.getElementById('buz-chk').checked = data.buz;
        
        waterPresent = data.water;
        elapsedMs = data.elapsed;
        lastUpdateAt = Date.now();
      })
      .catch(err => console.error(err));
  }

  // Poll server status every 1.5 seconds
  setInterval(fetchStatus, 1500);
  // High-frequency render loop (50ms) for smooth blinking and transitions locally
  setInterval(updateLocalState, 50);
</script>
</body>
</html>
)rawliteral";

// States
bool rawWaterPresent = false;
bool stableWaterPresent = false;
bool lastRawWaterPresent = false;
bool lastNotifiedEmpty = false;
int lastPrintedPinState = -1;

unsigned long rawChangedAt = 0;
unsigned long lastEmptyNotificationAt = 0;
unsigned long waterMissingStartedAt = 0;
unsigned long lastStateChangeAt = 0;

// Subscriber management using ESP32 Preferences (up to 20 clients)
Preferences preferences;
const int MAX_USERS = 20;
String users[MAX_USERS];
bool wantsNotifications[MAX_USERS];
int userCount = 0;

void loadSubscribers() {
  preferences.begin("tg_users", false);
  int rawCount = preferences.getInt("count", 0);
  userCount = 0;

  for (int i = 0; i < rawCount; i++) {
    String u = preferences.getString(("u" + String(i)).c_str(), "");
    bool n = preferences.getBool(("n" + String(i)).c_str(), false);

    // Filter out group IDs (starting with '-') and duplicate entries
    if (u.length() > 0 && !u.startsWith("-")) {
      bool duplicate = false;
      for (int j = 0; j < userCount; j++) {
        if (users[j] == u) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate && userCount < MAX_USERS) {
        users[userCount] = u;
        wantsNotifications[userCount] = n;
        userCount++;
      }
    }
  }
  preferences.end();

  // Re-save clean registry back to flash memory
  preferences.begin("tg_users", false);
  preferences.putInt("count", userCount);
  for (int i = 0; i < userCount; i++) {
    preferences.putString(("u" + String(i)).c_str(), users[i]);
    preferences.putBool(("n" + String(i)).c_str(), wantsNotifications[i]);
  }
  preferences.end();

  Serial.print("Loaded and cleaned: ");
  Serial.print(userCount);
  Serial.println(" users.");
}

void saveUser(int index) {
  preferences.begin("tg_users", false);
  preferences.putString(("u" + String(index)).c_str(), users[index]);
  preferences.putBool(("n" + String(index)).c_str(), wantsNotifications[index]);
  preferences.putInt("count", userCount);
  preferences.end();
}

void registerUser(const String& chatId) {
  if (chatId.startsWith("-")) {
    return; // Do not register groups or channels as private subscribers
  }
  for (int i = 0; i < userCount; i++) {
    if (users[i] == chatId) {
      return; // Already registered
    }
  }
  if (userCount < MAX_USERS) {
    users[userCount] = chatId;
    wantsNotifications[userCount] = true; // Subscribed by default when first starting the bot
    userCount++;
    saveUser(userCount - 1);
    Serial.print("Registered new user: ");
    Serial.println(chatId);
  }
}

void setNotificationPreference(const String& chatId, bool enable) {
  registerUser(chatId); // Ensure they are registered first
  
  for (int i = 0; i < userCount; i++) {
    if (users[i] == chatId) {
      if (wantsNotifications[i] != enable) {
        wantsNotifications[i] = enable;
        saveUser(i);
        Serial.print("Updated notification preference for ");
        Serial.print(chatId);
        Serial.print(" to ");
        Serial.println(enable ? "ENABLED" : "DISABLED");
      }
      return;
    }
  }
}

bool isSubscribed(const String& chatId) {
  for (int i = 0; i < userCount; i++) {
    if (users[i] == chatId) {
      return wantsNotifications[i];
    }
  }
  return false;
}

bool hasActiveSubscribers() {
  for (int i = 0; i < userCount; i++) {
    if (wantsNotifications[i]) {
      return true;
    }
  }
  return false;
}

void sendMainMenu(const String& chatId, const String& welcomeText) {
  String keyboardJson = "";
  if (isSubscribed(chatId)) {
    keyboardJson = "[[\"Є вода?\"],[\"Не сповіщати 🔕\"]]";
  } else {
    keyboardJson = "[[\"Є вода?\"],[\"Підписатись на сповіщення 🔔\"]]";
  }

  // Build JSON payload manually to support disable_notification
  String payload = "{\"chat_id\":\"" + chatId + "\",";
  payload += "\"text\":\"" + welcomeText + "\",";
  payload += "\"disable_notification\":true,";
  payload += "\"reply_markup\":{";
  payload += "\"keyboard\":" + keyboardJson + ",";
  payload += "\"resize_keyboard\":true,";
  payload += "\"one_time_keyboard\":false";
  payload += "}}";

  securedClient.stop();
  securedClient.setInsecure();
  
  if (securedClient.connect("api.telegram.org", 443)) {
    String url = "/bot" + String(BOT_TOKEN) + "/sendMessage";
    securedClient.print("POST " + url + " HTTP/1.1\r\n");
    securedClient.print("Host: api.telegram.org\r\n");
    securedClient.print("Content-Type: application/json\r\n");
    securedClient.print("Content-Length: " + String(payload.length()) + "\r\n");
    securedClient.print("Connection: close\r\n\r\n");
    securedClient.print(payload);
    
    // Read response briefly to clear buffer
    while (securedClient.connected()) {
      String line = securedClient.readStringUntil('\n');
      if (line == "\r") break;
    }
  }
  securedClient.stop();
}

uint32_t colorGreen() {
  return led.Color(0, 80, 0, 0);
}

uint32_t colorYellow() {
  return led.Color(110, 80, 0, 0);
}

uint32_t colorOrange() {
  return led.Color(140, 35, 0, 0);
}

uint32_t colorRed() {
  return led.Color(160, 0, 0, 0);
}

uint32_t colorBlue() {
  return led.Color(0, 0, 60, 0);
}

uint32_t colorWhite() {
  return led.Color(0, 0, 0, 80);
}

uint32_t colorOff() {
  return led.Color(0, 0, 0, 0);
}



void setLed(uint32_t color) {
  if (!ledEnabled) {
    led.setPixelColor(0, colorOff());
  } else {
    led.setPixelColor(0, color);
  }
  led.show();
}

uint32_t blendColor(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t w1,
                    uint8_t r2, uint8_t g2, uint8_t b2, uint8_t w2,
                    float amount) {
  amount = constrain(amount, 0.0f, 1.0f);
  const uint8_t r = r1 + (r2 - r1) * amount;
  const uint8_t g = g1 + (g2 - g1) * amount;
  const uint8_t b = b1 + (b2 - b1) * amount;
  const uint8_t w = w1 + (w2 - w1) * amount;
  return led.Color(r, g, b, w);
}

void updateWaterLed(bool waterPresent) {
  if (waterPresent) {
    waterMissingStartedAt = 0;
    setLed(colorGreen());
    return;
  }

  const unsigned long now = millis();
  if (waterMissingStartedAt == 0) {
    waterMissingStartedAt = now;
  }

  const unsigned long missingFor = now - waterMissingStartedAt;
  
  if (missingFor < LED_EMPTY_FADE_MS) {
    setLed(colorRed());
    return;
  }

  // After 5 seconds, blink red at 0.5s rate (500ms period, 250ms ON, 250ms OFF)
  // Note: if the buzzer is playing a blocking melody, this will stay solid red
  // until the melody finishes, then begin blinking.
  const unsigned long blinkFor = missingFor - LED_EMPTY_FADE_MS;
  const unsigned long phase = blinkFor % 500;
  if (phase < 250) {
    setLed(colorRed());
  } else {
    setLed(colorOff());
  }
}

void buzzerOff() {
  noTone(BUZZER_PIN);
}

void buzzerTone(unsigned int frequency) {
  tone(BUZZER_PIN, frequency);
}

void updateWaterBuzzer(bool waterPresent) {
  if (waterPresent) {
    buzzerOff();
    if (alarmMelodyPlayed) {
      // Only increment the index if the alarm actually went off (played at least once)
      currentMelodyIndex = (currentMelodyIndex + 1) % CUTE_MELODIES_COUNT;
      alarmMelodyPlayed = false;
    }
    return;
  }

  if (!buzzerEnabled || waterMissingStartedAt == 0) {
    buzzerOff();
    return;
  }

  const unsigned long now = millis();
  const unsigned long missingFor = now - waterMissingStartedAt;

  // Immediate beep when water disappears (0 - 150 ms)
  if (missingFor < 150) {
    buzzerTone(3000);
    return;
  }

  // Beep-beep after 2 seconds:
  if (missingFor >= 2000 && missingFor < 2150) {
    buzzerTone(3000);
    return;
  }
  if (missingFor >= 2300 && missingFor < 2450) {
    buzzerTone(3000);
    return;
  }

  if (missingFor < LED_EMPTY_FADE_MS) {
    buzzerOff();
    return;
  }

  // After 5 seconds, play the current melody in a loop with a 4-second pause
  if (!alarmMelodyPlayed) {
    playCuteSound(currentMelodyIndex);
    alarmMelodyPlayed = true;
    lastMelodyFinishedAt = millis();
  } else {
    buzzerOff();
    if (millis() - lastMelodyFinishedAt >= 200) { // 0.2 seconds of silence between repeats
      alarmMelodyPlayed = false; // Trigger playing again
    }
  }
}

bool readWaterSensor() {
  const bool pinIsLow = digitalRead(WATER_SENSOR_PIN) == LOW;
  return SENSOR_LOW_MEANS_WATER_PRESENT ? pinIsLow : !pinIsLow;
}

void handleStatusJson() {
  unsigned long elapsed = 0;
  if (!stableWaterPresent && waterMissingStartedAt > 0) {
    elapsed = millis() - waterMissingStartedAt;
  }
  
  String json = "{";
  json += "\"water\":" + String(stableWaterPresent ? "true" : "false") + ",";
  json += "\"led\":" + String(ledEnabled ? "true" : "false") + ",";
  json += "\"buz\":" + String(buzzerEnabled ? "true" : "false") + ",";
  json += "\"failed\":" + String(wifiConnectionFailed ? "true" : "false") + ",";
  json += "\"connected\":" + String((WiFi.status() == WL_CONNECTED) ? "true" : "false") + ",";
  json += "\"elapsed\":" + String(elapsed);
  json += "}";
  webServer.send(200, "application/json", json);
}

void serveConfigPage() {
  String html = HTML_CONFIG_PAGE;
  
  bool isConnected = (WiFi.status() == WL_CONNECTED);
  
  String wifiIconStr = isConnected ? 
    "<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#ffffff\" stroke-width=\"2.5\" stroke-linecap=\"round\" stroke-linejoin=\"round\" style=\"vertical-align: middle;\"><path d=\"M5 12.55a11 11 0 0 1 14.08 0\"></path><path d=\"M1.42 9a16 16 0 0 1 21.16 0\"></path><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"></path><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"3\"></line></svg>" :
    "<svg width=\"18\" height=\"18\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#ffffff\" stroke-width=\"2.5\" stroke-linecap=\"round\" stroke-linejoin=\"round\" style=\"vertical-align: middle;\"><line x1=\"1\" y1=\"1\" x2=\"23\" y2=\"23\"></line><path d=\"M16.72 11.06A10.94 10.94 0 0 1 19 12.55\"></path><path d=\"M5 12.55a10.94 10.94 0 0 1 5.83-2.84\"></path><path d=\"M1.42 9a16 16 0 0 1 12.58-4.7\"></path><path d=\"M20 7.24a15.7 15.7 0 0 1 2.58 1.76\"></path><path d=\"M8.53 16.11a6 6 0 0 1 6.95 0\"></path><line x1=\"12\" y1=\"20\" x2=\"12.01\" y2=\"20\" stroke-width=\"3\"></line></svg>";
  
  unsigned long elapsed = 0;
  if (!stableWaterPresent && waterMissingStartedAt > 0) {
    elapsed = millis() - waterMissingStartedAt;
  }
  
  // Calculate initial emoji in C++ for the first pageload
  String initialEmoji = "🟢";
  if (!stableWaterPresent) {
    initialEmoji = "🔴";
  }
  String waterStatusText = "Вода: <span id=\"water-emoji\" style=\"display: inline-block; transition: opacity 0.15s ease-in-out;\">" + initialEmoji + "</span>";
  
  html.replace("%WIFI_ICON%", wifiIconStr);
  html.replace("%WIFI_STATUS%", waterStatusText);
  html.replace("%SPOILER_STATE%", isConnected ? "" : "open");
  html.replace("%SSID%", wifiSsid);
  html.replace("%PASS%", wifiPassword);
  html.replace("%LED_CHECKED%", ledEnabled ? "checked" : "");
  html.replace("%BUZ_CHECKED%", buzzerEnabled ? "checked" : "");
  webServer.send(200, "text/html", html);
}

void handleSave() {
  if (webServer.hasArg("ssid")) {
    wifiSsid = webServer.arg("ssid");
    wifiPassword = webServer.arg("pass");
    
    preferences.begin("wifi_config", false);
    preferences.putString("ssid", wifiSsid);
    preferences.putString("pass", wifiPassword);
    preferences.end();
    
    webServer.send(200, "text/plain", "OK");
    
    Serial.println("Saved new WiFi configurations. Reconnecting...");
    
    WiFi.disconnect();
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
    wifiConnecting = true;
    wifiConnectionStartedAt = millis();
    wifiConnectionFailed = false;
  } else {
    webServer.send(400, "text/plain", "Bad Request");
  }
}

void handleToggle() {
  if (webServer.hasArg("type") && webServer.hasArg("val")) {
    String type = webServer.arg("type");
    bool val = (webServer.arg("val") == "1");
    
    preferences.begin("wifi_config", false);
    if (type == "led") {
      ledEnabled = val;
      preferences.putBool("led_en", ledEnabled);
      if (!ledEnabled) {
        setLed(colorOff());
      } else {
        setLed(stableWaterPresent ? colorGreen() : colorRed());
      }
      Serial.print("LED toggled via Web: ");
      Serial.println(ledEnabled ? "ON" : "OFF");
    } else if (type == "buz") {
      buzzerEnabled = val;
      preferences.putBool("buz_en", buzzerEnabled);
      if (!buzzerEnabled) {
        buzzerOff();
      }
      Serial.print("Buzzer toggled via Web: ");
      Serial.println(buzzerEnabled ? "ON" : "OFF");
    }
    preferences.end();
    
    webServer.send(200, "text/plain", "OK");
  } else {
    webServer.send(400, "text/plain", "Bad Request");
  }
}

void monitorWiFi() {
  const unsigned long now = millis();
  wl_status_t currentStatus = WiFi.status();

  static wl_status_t lastStatus = WL_NO_SHIELD;
  if (currentStatus != lastStatus) {
    lastStatus = currentStatus;
    Serial.print("WiFi status changed: ");
    switch (currentStatus) {
      case WL_CONNECTED:
        Serial.print("CONNECTED, IP: ");
        Serial.println(WiFi.localIP());
        break;
      case WL_DISCONNECTED:
        Serial.println("DISCONNECTED");
        break;
      default:
        Serial.println(currentStatus);
        break;
    }
  }

  // 1. Connection success handling
  if (currentStatus == WL_CONNECTED) {
    if (wifiConnecting) {
      wifiConnecting = false;
      wifiConnectionFailed = false;
      wifiConnectedAt = now;
      Serial.println("WiFi connection established.");
    }
    
    // If AP is active, turn it off after a 3-second delay
    if (apModeActive) {
      if (now - wifiConnectedAt >= 3000) {
        dnsServer.stop();
        webServer.stop();
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_STA);
        apModeActive = false;
        Serial.println("AP mode 'YeVoda' disabled after 3s delay.");
      }
    }
  } 
  // 2. Connection failure / drop handling
  else {
    if (wifiConnecting) {
      if (now - wifiConnectionStartedAt >= 15000) {
        wifiConnecting = false;
        wifiConnectionFailed = true;
        
        // Start AP Mode in pure WIFI_AP mode for maximum stability
        WiFi.disconnect();
        WiFi.mode(WIFI_AP);
        IPAddress local_IP(192, 168, 4, 1);
        IPAddress gateway(192, 168, 4, 1);
        IPAddress subnet(255, 255, 255, 0);
        WiFi.softAPConfig(local_IP, gateway, subnet);
        WiFi.softAP("YeVoda");
        
        dnsServer.start(53, "*", WiFi.softAPIP());
        
        apModeActive = true;
        Serial.println("AP mode 'YeVoda' enabled (SSID: YeVoda, no password).");
      }
    } else if (!apModeActive) {
      // Normal connection loss during operation -> trigger reconnect
      Serial.println("WiFi disconnected. Reconnecting...");
      WiFi.disconnect();
      WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
      wifiConnecting = true;
      wifiConnectionStartedAt = now;
      wifiConnectionFailed = false;
    }
  }

  // Handle server loops if AP is active
  if (apModeActive) {
    dnsServer.processNextRequest();
  }
}

void queueTelegramMessage(TelegramMessage msg) {
  pendingTelegramMessage = msg;
}

void telegramTask(void* parameter) {
  unsigned long lastUpdateCheckAt = 0;
  bool wifiWasConnected = false;

  while (true) {
    // 0. Handle Wi-Fi connection/reconnection update flushing
    bool wifiConnectedNow = (WiFi.status() == WL_CONNECTED);
    if (wifiConnectedNow && !wifiWasConnected) {
      Serial.println("Wi-Fi connected/reconnected. Flushing pending Telegram updates...");
      securedClient.stop();
      securedClient.setInsecure();
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      while (numNewMessages > 0) {
        bot.last_message_received = bot.messages[numNewMessages - 1].update_id;
        securedClient.stop();
        numNewMessages = bot.getUpdates(bot.last_message_received + 1);
      }
      Serial.println("Updates flushed.");
      wifiWasConnected = true;
    } else if (!wifiConnectedNow) {
      wifiWasConnected = false;
    }

    // 1. Process pending notifications
    TelegramMessage msg = pendingTelegramMessage;
    if (msg != MSG_NONE) {
      pendingTelegramMessage = MSG_NONE; // Clear it

      String text = "";
      if (msg == MSG_EMPTY) {
        text = "Вода закінчилась. Будь ласка, наповніть бак. ⚠️";
      } else if (msg == MSG_BACK) {
        text = "Вода з'явилась. 💧";
      }

      if (text.length() > 0) {
        if (!hasActiveSubscribers()) {
          Serial.println("No active subscribers. Skipping Telegram notification.");
        } else {
          while (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(500));
          }

          Serial.print("Telegram notification sending: ");
          Serial.println(text);
          
          securedClient.stop(); // Clean socket before request
          securedClient.setInsecure();
          
          bool success = false;
          // Send only to active subscribers
          for (int i = 0; i < userCount; i++) {
            if (users[i].length() > 0 && wantsNotifications[i]) {
              if (bot.sendMessage(users[i], text, "")) {
                success = true;
              }
            }
          }
          securedClient.stop();
          
          if (success) {
            Serial.println("Telegram notification SUCCESS");
          } else {
            char err_buf[100];
            securedClient.lastError(err_buf, 100);
            Serial.print("Telegram notification FAILED. SSL error: ");
            Serial.println(err_buf);
          }
        }
      }
    }

    // 2. Poll for incoming messages (every 1.5 seconds)
    const unsigned long now = millis();
    if (WiFi.status() == WL_CONNECTED && (now - lastUpdateCheckAt >= 1500)) {
      lastUpdateCheckAt = now;

      securedClient.stop(); // Clean socket before request
      securedClient.setInsecure();
      int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

      if (numNewMessages > 0) {
        for (int i = 0; i < numNewMessages; i++) {
          if (bot.messages[i].update_id > bot.last_message_received) {
            bot.last_message_received = bot.messages[i].update_id;
          }

          String chatId = String(bot.messages[i].chat_id);
          if (chatId.startsWith("-")) {
            continue; // Ignore group chats, supergroups, and channels entirely
          }
          String text = bot.messages[i].text;

          if (text == "/start") {
            registerUser(chatId);
            sendMainMenu(chatId, "Привіт! Скористайтесь кнопками нижче для керування та перевірки води. 💧");
          } 
          else if (text == "Підписатись на сповіщення 🔔" || text == "/start_notify") {
            setNotificationPreference(chatId, true);
            sendMainMenu(chatId, "Сповіщення увімкнено. 🔔");
          }
          else if (text == "Не сповіщати 🔕" || text == "/stop_notify" || text == "/stop") {
            setNotificationPreference(chatId, false);
            sendMainMenu(chatId, "Сповіщення вимкнено. 🔕");
          }
          else if (text == "Є вода?" || text == "/status") {
            String reply = "";
            if (stableWaterPresent) {
              reply = "Так, вода є. 🟢";
            } else {
              reply = "Ні, вода закінчилась! 🔴";
            }
            securedClient.stop();
            bot.sendMessage(chatId, reply, "");
          }
        }
        securedClient.stop(); // Clean socket after processing
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // Poll loop every 100ms
  }
}

void handleNotifications() {
  // If there are no active subscribers, bypass the notification queue entirely
  if (!hasActiveSubscribers()) {
    return;
  }

  const unsigned long now = millis();

  if (!stableWaterPresent) {
    const bool repeatAllowed = (lastEmptyNotificationAt == 0) || (now - lastEmptyNotificationAt >= TELEGRAM_MIN_REPEAT_MS);
    if (!lastNotifiedEmpty || repeatAllowed) {
      lastNotifiedEmpty = true;
      lastEmptyNotificationAt = now;
      queueTelegramMessage(MSG_EMPTY);
    }
    return;
  }

  if (lastNotifiedEmpty) {
    lastNotifiedEmpty = false;
    queueTelegramMessage(MSG_BACK);
  }
}

void updateSensorState() {
  bool currentRaw = readWaterSensor();
  const unsigned long now = millis();

  if (currentRaw != lastRawWaterPresent) {
    lastRawWaterPresent = currentRaw;
    rawChangedAt = now;
  }

  if (now - rawChangedAt >= SENSOR_DEBOUNCE_MS) {
    if (currentRaw != stableWaterPresent) {
      if (lastStateChangeAt == 0 || (now - lastStateChangeAt >= STATE_LOCKOUT_MS)) {
        stableWaterPresent = currentRaw;
        lastStateChangeAt = now;
        Serial.print("State changed to: ");
        Serial.println(stableWaterPresent ? "WATER PRESENT" : "WATER EMPTY");
      }
    }
  }
}

void printSensorStatus(const char* reason, int pinState) {
  const bool waterPresent = SENSOR_LOW_MEANS_WATER_PRESENT ? pinState == LOW : pinState == HIGH;

  Serial.print(reason);
  Serial.print(" | GPIO");
  Serial.print(WATER_SENSOR_PIN);
  Serial.print("=");
  Serial.print(pinState == LOW ? "LOW" : "HIGH");
  Serial.print(" | WATER: ");
  Serial.print(waterPresent ? "YES" : "NO");
  Serial.print(" | wifi=");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "NOT_CONNECTED");
}

void setup() {
  Serial.begin(115200);
  setCpuFrequencyMhz(160); // Set CPU to max 160MHz to speed up SSL math
  delay(300);
  Serial.println();
  Serial.println("Water level monitor started");

  pinMode(WATER_SENSOR_PIN, INPUT);

  loadSubscribers();

  // Initialize Buzzer
  pinMode(BUZZER_PIN, OUTPUT);
  buzzerOff();

  // Initialize Onboard NeoPixel
  led.begin();
  led.setBrightness(50); // Reduced from 200 to stabilize power rail
  setLed(colorBlue());

  preferences.begin("wifi_config", false);
  wifiSsid = preferences.getString("ssid", DEFAULT_WIFI_SSID);
  wifiPassword = preferences.getString("pass", DEFAULT_WIFI_PASSWORD);
  ledEnabled = preferences.getBool("led_en", true);
  buzzerEnabled = preferences.getBool("buz_en", true);
  preferences.end();

  // Initialize WiFi & Background Task
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  Serial.println("Connecting to WiFi...");
  wifiConnecting = true;
  wifiConnectionStartedAt = millis();

  // Setup Web Server routes always-on
  webServer.on("/", serveConfigPage);
  webServer.on("/save", HTTP_POST, handleSave);
  webServer.on("/toggle", HTTP_GET, handleToggle);
  webServer.on("/status_json", HTTP_GET, handleStatusJson);
  webServer.onNotFound([]() {
    serveConfigPage();
  });
  webServer.begin();

  // Start mDNS Responder (voda.local)
  if (MDNS.begin("voda")) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS responder started: http://voda.local");
  }

  xTaskCreatePinnedToCore(
    telegramTask,
    "TelegramTask",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  // Initial read
  rawWaterPresent = readWaterSensor();
  lastRawWaterPresent = rawWaterPresent;
  stableWaterPresent = rawWaterPresent;
  rawChangedAt = millis();
  lastPrintedPinState = digitalRead(WATER_SENSOR_PIN);
  printSensorStatus("START", lastPrintedPinState);
}

void loop() {
  monitorWiFi();
  webServer.handleClient(); // Always process webserver requests

  updateSensorState();

  const int currentPinState = digitalRead(WATER_SENSOR_PIN);
  if (currentPinState != lastPrintedPinState) {
    lastPrintedPinState = currentPinState;
    printSensorStatus("CHANGED", currentPinState);
  }

  updateWaterLed(stableWaterPresent);
  updateWaterBuzzer(stableWaterPresent);

  handleNotifications();

  delay(20);
}
