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
const char* CHAT_ID = "-1004435895448"; // Channel ID "Є вода?"

// Wi-Fi and Telegram notifications configuration
const bool ENABLE_WIFI_TELEGRAM = true;

// Sensor logic
const bool SENSOR_LOW_MEANS_WATER_PRESENT = true;

// Timing
const unsigned long SENSOR_DEBOUNCE_MS = 1000;
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
bool lastStableWaterPresent = false;
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
volatile bool isTelegramActive = false;

// Subscriber management using ESP32 Preferences
Preferences preferences;
const int MAX_SUBSCRIBERS = 10;
String subscribers[MAX_SUBSCRIBERS];
int subscriberCount = 0;

void loadSubscribers() {
  preferences.begin("tg_subs", false);
  subscriberCount = preferences.getInt("count", 0);
  if (subscriberCount > MAX_SUBSCRIBERS) {
    subscriberCount = MAX_SUBSCRIBERS;
  }
  for (int i = 0; i < subscriberCount; i++) {
    subscribers[i] = preferences.getString(("s" + String(i)).c_str(), "");
  }
  preferences.end();
  Serial.print("Loaded ");
  Serial.print(subscriberCount);
  Serial.println(" subscribers.");
}

void saveSubscribers() {
  preferences.begin("tg_subs", false);
  preferences.putInt("count", subscriberCount);
  for (int i = 0; i < subscriberCount; i++) {
    preferences.putString(("s" + String(i)).c_str(), subscribers[i]);
  }
  preferences.end();
}

bool addSubscriber(const String& chatId) {
  for (int i = 0; i < subscriberCount; i++) {
    if (subscribers[i] == chatId) {
      return false; // Already subscribed
    }
  }
  if (subscriberCount < MAX_SUBSCRIBERS) {
    subscribers[subscriberCount] = chatId;
    subscriberCount++;
    saveSubscribers();
    Serial.print("Added subscriber: ");
    Serial.println(chatId);
    return true;
  }
  return false; // List full
}

bool removeSubscriber(const String& chatId) {
  int foundIndex = -1;
  for (int i = 0; i < subscriberCount; i++) {
    if (subscribers[i] == chatId) {
      foundIndex = i;
      break;
    }
  }
  if (foundIndex != -1) {
    for (int i = foundIndex; i < subscriberCount - 1; i++) {
      subscribers[i] = subscribers[i + 1];
    }
    subscriberCount--;
    saveSubscribers();
    Serial.print("Removed subscriber: ");
    Serial.println(chatId);
    return true;
  }
  return false;
}

bool isSubscribed(const String& chatId) {
  for (int i = 0; i < subscriberCount; i++) {
    if (subscribers[i] == chatId) {
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
  isTelegramActive = true;
  securedClient.stop();
  securedClient.setInsecure();
  bot.sendMessageWithReplyKeyboard(chatId, welcomeText, "", keyboardJson, true);
  isTelegramActive = false;
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
  if (waterPresent || waterMissingStartedAt == 0 || isTelegramActive) {
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

void telegramTask(void* parameter) {
  unsigned long lastUpdateCheckAt = 0;

  while (true) {
    // Check and print if time has synchronized
    static bool timeSynced = false;
    if (!timeSynced) {
      time_t nowTime = time(nullptr);
      if (nowTime >= 1700000000) {
        timeSynced = true;
        Serial.print("NTP Time synchronized: ");
        Serial.println(ctime(&nowTime));
      }
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
        bool success = false;
        int attempts = 0;
        
        while (!success && attempts < 5) {
          while (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
          }

          time_t nowTime = time(nullptr);
          while (nowTime < 1700000000) {
            vTaskDelay(pdMS_TO_TICKS(1500));
            nowTime = time(nullptr);
          }

          if (pendingTelegramMessage != MSG_NONE) {
            break;
          }

          Serial.print("Telegram task sending: ");
          Serial.println(text);
          isTelegramActive = true;
          securedClient.stop(); // Clean socket before request
          securedClient.setInsecure();
          
          // 1. Send to public Channel ID
          success = bot.sendMessage(CHAT_ID, text, "");
          
          // 2. Send to all private chat subscribers who sent /start
          for (int i = 0; i < subscriberCount; i++) {
            if (subscribers[i].length() > 0) {
              bot.sendMessage(subscribers[i], text, "");
            }
          }
          
          isTelegramActive = false;
          
          if (success) {
            Serial.println("Telegram send SUCCESS");
          } else {
            char err_buf[100];
            securedClient.lastError(err_buf, 100);
            Serial.print("Telegram send FAILED. SSL error: ");
            Serial.println(err_buf);
            Serial.println("Retrying in 5 seconds...");
            attempts++;
            vTaskDelay(pdMS_TO_TICKS(5000));
          }
        }
      }
    }

    // 2. Poll for incoming messages (every 4 seconds)
    const unsigned long now = millis();
    if (WiFi.status() == WL_CONNECTED && (now - lastUpdateCheckAt >= 4000)) {
      lastUpdateCheckAt = now;

      time_t nowTime = time(nullptr);
      if (nowTime >= 1700000000) { // Only poll if time is synced
        isTelegramActive = true;
        securedClient.stop(); // Clean socket before request
        securedClient.setInsecure();
        int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
        isTelegramActive = false;

        while (numNewMessages) {
          for (int i = 0; i < numNewMessages; i++) {
            // Update the offset immediately to prevent reprocessing this message on retry or next tick
            if (bot.messages[i].update_id > bot.last_message_received) {
              bot.last_message_received = bot.messages[i].update_id;
            }

            String chatId = String(bot.messages[i].chat_id);
            String text = bot.messages[i].text;

            if (text == "/start") {
              sendMainMenu(chatId, "Привіт! Скористайтесь кнопками нижче для керування та перевірки води. 💧");
            } 
            else if (text == "Підписатись на сповіщення 🔔" || text == "/start_notify") {
              addSubscriber(chatId);
              sendMainMenu(chatId, "Сповіщення увімкнено. 🔔");
            }
            else if (text == "Не сповіщати 🔕" || text == "/stop_notify" || text == "/stop") {
              removeSubscriber(chatId);
              sendMainMenu(chatId, "Сповіщення вимкнено. 🔕");
            }
            else if (text == "Є вода?" || text == "/status") {
              String reply = "";
              if (stableWaterPresent) {
                reply = "Так, вода є. 🟢";
              } else {
                reply = "Ні, вода закінчилась! 🔴";
              }
              isTelegramActive = true;
              securedClient.stop();
              bot.sendMessage(chatId, reply, "");
              isTelegramActive = false;
            }
          }
          
          isTelegramActive = true;
          securedClient.stop(); // Clean socket before request
          numNewMessages = bot.getUpdates(bot.last_message_received + 1);
          isTelegramActive = false;
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100)); // Poll loop every 100ms
  }
}

void handleNotifications() {
  const unsigned long now = millis();

  if (!stableWaterPresent) {
    // Wait 150ms to let the first buzzer beep finish, so we send the message
    // during the natural silent pause to prevent power dropouts.
    if (waterMissingStartedAt != 0 && (now - waterMissingStartedAt < 150)) {
      return;
    }

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
  delay(300);
  Serial.println();
  Serial.println("Water level monitor started");

  pinMode(WATER_SENSOR_PIN, INPUT);

  if (ENABLE_WIFI_TELEGRAM) {
    loadSubscribers();
  }

  // Initialize Buzzer
  ledcAttach(BUZZER_PIN, 2000, BUZZER_RESOLUTION);
  buzzerOff();

  // Initialize Onboard NeoPixel
  led.begin();
  led.setBrightness(50); // Reduced from 200 to stabilize power rail
  setLed(colorBlue());

  // Initialize WiFi & Background Task
  if (ENABLE_WIFI_TELEGRAM) {
    WiFi.mode(WIFI_STA);
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    configTime(0, 0, "time.google.com", "time.windows.com");
    Serial.println("Connecting to WiFi and syncing time...");

    xTaskCreatePinnedToCore(
      telegramTask,
      "TelegramTask",
      8192,
      NULL,
      1,
      NULL,
      0
    );
  }

  // Initial read
  rawWaterPresent = readWaterSensor();
  lastRawWaterPresent = rawWaterPresent;
  stableWaterPresent = rawWaterPresent;
  lastStableWaterPresent = stableWaterPresent;
  rawChangedAt = millis();
  lastPrintedPinState = digitalRead(WATER_SENSOR_PIN);
  printSensorStatus("START", lastPrintedPinState);
}

void loop() {
  if (ENABLE_WIFI_TELEGRAM) {
    monitorWiFi();
  }

  updateSensorState();

  const int currentPinState = digitalRead(WATER_SENSOR_PIN);
  if (currentPinState != lastPrintedPinState) {
    lastPrintedPinState = currentPinState;
    printSensorStatus("CHANGED", currentPinState);
  }

  if (stableWaterPresent != lastStableWaterPresent) {
    lastStableWaterPresent = stableWaterPresent;
  }

  updateWaterLed(stableWaterPresent);
  updateWaterBuzzer(stableWaterPresent);

  if (ENABLE_WIFI_TELEGRAM) {
    handleNotifications();
  }

  delay(20);
}
