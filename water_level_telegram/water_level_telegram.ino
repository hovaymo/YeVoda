#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>

// Pins
const uint8_t WATER_SENSOR_PIN = 4;  // Yellow OUT from XKC-Y25-NPN
const uint8_t LED_PIN = 8;           // Onboard addressable NeoPixel LED
const uint8_t BUZZER_PIN = 2;        // Buzzer pin (Passive or Active)
const uint8_t LED_COUNT = 1;
const uint8_t BUZZER_RESOLUTION = 8;
const uint8_t BUZZER_DUTY = 128;
const unsigned int BUZZER_ALARM_LOW_HZ = 2600;
const unsigned int BUZZER_ALARM_HIGH_HZ = 4300;
const unsigned long BUZZER_ALARM_PERIOD_MS = 300;
const unsigned long BUZZER_ALARM_ON_MS = 210;

// Wi-Fi
const char* WIFI_SSID = "TP-LINK_C070";
const char* WIFI_PASSWORD = "98337606";

// Telegram
const char* BOT_TOKEN = "8992360486:AAFE6qnkkK2D55kRQLnYbDa_aW4Spo6Qzb4";




// Sensor logic
const bool SENSOR_LOW_MEANS_WATER_PRESENT = true;

// Timing
const unsigned long SENSOR_DEBOUNCE_MS = 300;
const unsigned long TELEGRAM_MIN_REPEAT_MS = 30UL * 60UL * 1000UL;
const unsigned long LED_EMPTY_HOLD_GREEN_MS = 0;
const unsigned long LED_EMPTY_FADE_MS = 3000;
const unsigned long LED_RED_SOLID_MS = 2000;
const unsigned long LED_RED_BLINK_MS = 450;

Adafruit_NeoPixel led(LED_COUNT, LED_PIN, NEO_GRBW + NEO_KHZ800);
WiFiClientSecure securedClient;
UniversalTelegramBot bot(BOT_TOKEN, securedClient);

// States
bool rawWaterPresent = false;
bool stableWaterPresent = false;
bool lastRawWaterPresent = false;
bool lastNotifiedEmpty = false;
int lastPrintedPinState = -1;

unsigned long rawChangedAt = 0;
unsigned long lastEmptyNotificationAt = 0;
unsigned long waterMissingStartedAt = 0;

// FreeRTOS background Telegram task
enum TelegramMessage {
  MSG_NONE,
  MSG_EMPTY,
  MSG_BACK
};
volatile TelegramMessage pendingTelegramMessage = MSG_NONE;
SemaphoreHandle_t telegramMutex;

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

bool isFinalAlarmToneOn(unsigned long alarmFor) {
  return alarmFor % BUZZER_ALARM_PERIOD_MS < BUZZER_ALARM_ON_MS;
}

void setLed(uint32_t color) {
  led.setPixelColor(0, color);
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
  if (missingFor < LED_EMPTY_HOLD_GREEN_MS) {
    setLed(colorGreen());
    return;
  }

  const unsigned long fadeFor = missingFor - LED_EMPTY_HOLD_GREEN_MS;
  if (fadeFor >= LED_EMPTY_FADE_MS) {
    const unsigned long redFor = fadeFor - LED_EMPTY_FADE_MS;
    if (redFor < LED_RED_SOLID_MS) {
      setLed(colorRed());
    } else if (isFinalAlarmToneOn(redFor - LED_RED_SOLID_MS)) {
      setLed(colorRed());
    } else {
      setLed(colorOff());
    }
    return;
  }

  const float progress = (float)fadeFor / (float)LED_EMPTY_FADE_MS;
  if (progress < 0.5f) {
    setLed(blendColor(0, 80, 0, 0, 110, 80, 0, 10, progress / 0.5f));
  } else if (progress < 0.75f) {
    setLed(blendColor(110, 80, 0, 10, 140, 35, 0, 0, (progress - 0.5f) / 0.25f));
  } else {
    setLed(blendColor(140, 35, 0, 0, 160, 0, 0, 0, (progress - 0.75f) / 0.25f));
  }
}

void buzzerOff() {
  ledcWrite(BUZZER_PIN, 0);
}

void buzzerTone(unsigned int frequency) {
  ledcWriteTone(BUZZER_PIN, frequency);
  ledcWrite(BUZZER_PIN, BUZZER_DUTY);
}

void updateWaterBuzzer(bool waterPresent) {
  if (waterPresent || waterMissingStartedAt == 0) {
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
  // First beep: 2000 - 2150 ms
  // Second beep: 2300 - 2450 ms
  if (missingFor >= 2000 && missingFor < 2150) {
    buzzerTone(3000);
    return;
  }
  if (missingFor >= 2300 && missingFor < 2450) {
    buzzerTone(3000);
    return;
  }

  const unsigned long alarmStartAt = LED_EMPTY_HOLD_GREEN_MS + LED_EMPTY_FADE_MS + LED_RED_SOLID_MS;
  if (missingFor < alarmStartAt) {
    buzzerOff();
    return;
  }

  const unsigned long alarmFor = missingFor - alarmStartAt;
  const unsigned long phase = alarmFor % BUZZER_ALARM_PERIOD_MS;

  if (phase >= BUZZER_ALARM_ON_MS) {
    buzzerOff();
    return;
  }

  const float pulseProgress = (float)phase / (float)BUZZER_ALARM_ON_MS;
  const unsigned int sweepHz = BUZZER_ALARM_LOW_HZ + (unsigned int)(pulseProgress * (BUZZER_ALARM_HIGH_HZ - BUZZER_ALARM_LOW_HZ));

  if (phase < 70) {
    buzzerTone(BUZZER_ALARM_HIGH_HZ);
  } else if (phase < 135) {
    buzzerTone(BUZZER_ALARM_LOW_HZ);
  } else {
    buzzerTone(sweepHz);
  }
}

bool readWaterSensor() {
  const bool pinIsLow = digitalRead(WATER_SENSOR_PIN) == LOW;
  return SENSOR_LOW_MEANS_WATER_PRESENT ? pinIsLow : !pinIsLow;
}

void monitorWiFi() {
  const unsigned long now = millis();
  static unsigned long lastCheck = 0;
  if (now - lastCheck < 2000) {
    return;
  }
  lastCheck = now;

  static wl_status_t lastStatus = WL_IDLE_STATUS;
  wl_status_t currentStatus = WiFi.status();

  if (currentStatus != lastStatus) {
    lastStatus = currentStatus;
    Serial.print("WiFi status changed: ");
    switch (currentStatus) {
      case WL_CONNECTED:
        Serial.print("CONNECTED, IP: ");
        Serial.println(WiFi.localIP());
        break;
      case WL_NO_SSID_AVAIL:
        Serial.println("SSID NOT AVAILABLE");
        break;
      case WL_CONNECT_FAILED:
        Serial.println("CONNECTION FAILED");
        break;
      case WL_CONNECTION_LOST:
        Serial.println("CONNECTION LOST");
        break;
      case WL_DISCONNECTED:
        Serial.println("DISCONNECTED");
        break;
      default:
        Serial.println(currentStatus);
        break;
    }
  }
}

void queueTelegramMessage(TelegramMessage msg) {
  pendingTelegramMessage = msg;
}

void telegramNotificationTask(void* parameter) {
  while (true) {
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
          
          bool success = false;
          // Acquire mutex for thread-safe SSL socket usage
          if (xSemaphoreTake(telegramMutex, portMAX_DELAY) == pdTRUE) {
            securedClient.stop(); // Clean socket before request
            securedClient.setInsecure();
            
            // Send only to active subscribers
            for (int i = 0; i < userCount; i++) {
              if (users[i].length() > 0 && wantsNotifications[i]) {
                if (bot.sendMessage(users[i], text, "")) {
                  success = true; // Mark as success if at least one succeeds
                }
              }
            }
            securedClient.stop();
            xSemaphoreGive(telegramMutex);
          }
          
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
    vTaskDelay(pdMS_TO_TICKS(50)); // Check queue every 50ms
  }
}

void telegramPollingTask(void* parameter) {
  unsigned long lastUpdateCheckAt = 0;

  while (true) {
    const unsigned long now = millis();
    if (WiFi.status() == WL_CONNECTED && (now - lastUpdateCheckAt >= 1500)) {
      lastUpdateCheckAt = now;

      int numNewMessages = 0;

      // Acquire mutex for thread-safe SSL socket usage
      if (xSemaphoreTake(telegramMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        securedClient.stop(); // Clean socket before request
        securedClient.setInsecure();
        numNewMessages = bot.getUpdates(bot.last_message_received + 1);

        if (numNewMessages == 0) {
          securedClient.stop();
          xSemaphoreGive(telegramMutex);
        } else {
          // Process messages while holding the mutex
          for (int i = 0; i < numNewMessages; i++) {
            if (bot.messages[i].update_id > bot.last_message_received) {
              bot.last_message_received = bot.messages[i].update_id;
            }

            String chatId = String(bot.messages[i].chat_id);
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
          securedClient.stop();
          xSemaphoreGive(telegramMutex);
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100)); // Poll loop every 100ms
  }
}

void handleNotifications() {
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
    stableWaterPresent = currentRaw;
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
  ledcAttach(BUZZER_PIN, 2000, BUZZER_RESOLUTION);
  buzzerOff();

  // Initialize Onboard NeoPixel
  led.begin();
  led.setBrightness(50); // Reduced from 200 to stabilize power rail
  setLed(colorBlue());

  // Initialize WiFi & Background Task
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Connecting to WiFi...");

  telegramMutex = xSemaphoreCreateMutex();

  xTaskCreatePinnedToCore(
    telegramNotificationTask,
    "TelegramNotify",
    8192,
    NULL,
    1,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    telegramPollingTask,
    "TelegramPoll",
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
