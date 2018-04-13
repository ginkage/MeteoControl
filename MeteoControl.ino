#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <MitsubishiHeavyHeatpumpIR.h>
#include <Timer.h>

#include <OLEDDisplay.h>
#include <OLEDDisplayFonts.h>
#include <OLEDDisplayUi.h>
#include <SSD1306.h>
#include <SSD1306Wire.h>

#include <DHT.h>
#include <DHT_U.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <SPI.h>

#define TOPSZ                  60           // Max number of characters in topic string
#define MESSZ                  240          // Max number of characters in JSON message string
#define WIFI_SSID "GinWiFi"
#define WIFI_PASS "deadbeef"
#define MQTT_HOST "londonpi"
#define MQTT_PORT 1883
#define MQTT_USER "homeassistant"
#define MQTT_PASS "aiyvoicehass"
#define MQTT_CLIENT_ID "livingroomhvac"
#define MQTT_STATUS_CHANNEL "livingroom/meteo/"

SSD1306Wire display(0x3c, D7, D5);
DHT dht(D6, DHT22);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
IRSenderBitBang irSender(D2);
MitsubishiHeavyZJHeatpumpIR *heatpump;
Timer t;
float last_temp = sqrt(-1), last_hum = sqrt(-1);

// Set defaults
uint8_t AC_POWER = POWER_OFF,
        AC_MODE = MODE_AUTO,
        AC_FAN = FAN_AUTO,
        AC_TEMP = 24,
        AC_VSWING = VDIR_SWING,
        AC_HSWING = HDIR_SWING;

void setup() {
  Serial.begin(9600);
  Serial.println("Booting");
  delay(10);

  Serial.println("Setup TFT");
  display.init();
  display.flipScreenVertically();
  display.setContrast(255);

  display.clear();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.drawString(64, 32, "Hello!");
  display.display();

  Serial.println("Setup DHT");
  dht.begin();

  initWIFI();
  initOTA();

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // Init loop timers
  t.every(2000, twoLoop, (void *)0);
  t.every(5000, fiveLoop, (void *)0);
  t.every(10000, tenLoop, (void *)0);

  Serial.println("Setup HVAC");
  heatpump = new MitsubishiHeavyZJHeatpumpIR();

  Serial.println("Setup MQTT");
  initMQTT();
}

void loop() {
  ArduinoOTA.handle();
  mqttClient.loop();
  t.update();
}

void tenLoop(void *context) {
  publishState();
}

// Reconnect to mqtt every 5 seconds if connection is lost
void fiveLoop(void *context) {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
}

void twoLoop(void *context) {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) || !isnan(h)) {
    display.clear();
    display.setFont(ArialMT_Plain_10);

    char message[MESSZ];
    sprintf(
      message,
      " power: %d, mode: %d, fan: %d\n temp: %d\n vswing: %d, hswing: %d",
      AC_POWER, AC_MODE, AC_FAN, AC_TEMP, AC_VSWING, AC_HSWING
    );
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 0, message);
    display.setTextAlignment(TEXT_ALIGN_CENTER);

    if (!isnan(t)) {
      display.drawString(64, 40, "t " + String(t, 1) + " °C\n");
      last_temp = t;
    }

    if (!isnan(h)) {
      display.drawString(64, 52, "h " + String(h, 1) + " %");
      last_hum = h;
    }

    display.display();
  }
}

void initMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
  publishState();
}

void mqttDataCb(char* topic, byte* data, unsigned int data_len) {
  char svalue[MESSZ];
  char topicBuf[TOPSZ];
  char dataBuf[data_len + 1];

  strncpy(topicBuf, topic, sizeof(topicBuf));
  memcpy(dataBuf, data, sizeof(dataBuf));
  dataBuf[sizeof(dataBuf) - 1] = 0;

  snprintf_P(svalue, sizeof(svalue), PSTR("RSLT: Receive topic %s, data size %d, data %s"), topicBuf, data_len, dataBuf);
  Serial.println(svalue);

  // Extract command
  char *suffix = topicBuf + sizeof(MQTT_STATUS_CHANNEL) - 1;
  char *setter = strstr(suffix, "/set");
  if (!setter) {
    return;
  }
  *setter = 0;
  int16_t payload = atoi(dataBuf);     // -32766 - 32767

  if (!strcmp(suffix, "mode")) {
    Serial.print("mode ");
    Serial.println(payload);
    if (!strcmp(dataBuf, "off")) {
      AC_POWER = POWER_OFF;
    } else {
      AC_POWER = POWER_ON;
      if (!strcmp(dataBuf, "auto")) {
        AC_MODE = MODE_AUTO;
      } else if (!strcmp(dataBuf, "heat")) {
        AC_MODE = MODE_HEAT;
      } else if (!strcmp(dataBuf, "cool")) {
        AC_MODE = MODE_COOL;
      } else if (!strcmp(dataBuf, "dry")) {
        AC_MODE = MODE_DRY;
      } else if (!strcmp(dataBuf, "fan")) {
        AC_MODE = MODE_FAN;
      } else {
        AC_POWER = POWER_OFF;
      }
    }
  } else if (!strcmp(suffix, "fan")) {
    Serial.print("fan ");
    Serial.println(payload);
    if (!strcmp(dataBuf, "auto")) {
      AC_FAN = FAN_AUTO;
    } else if (!strcmp(dataBuf, "low")) {
      AC_FAN = FAN_1;
    } else if (!strcmp(dataBuf, "medium")) {
      AC_FAN = FAN_2;
    } else if (!strcmp(dataBuf, "high")) {
      AC_FAN = FAN_3;
    }
  } else if (!strcmp(suffix, "target")) {
    Serial.print("target ");
    Serial.println(payload);
    AC_TEMP = payload;
  } else if (!strcmp(suffix, "swing")) {
    if (!strcmp(dataBuf, "auto")) {
      AC_VSWING = VDIR_SWING;
      AC_HSWING = HDIR_SWING;
    } else {
      AC_VSWING = VDIR_UP;
      AC_HSWING = HDIR_MIDDLE;
    }
  }

  bool cleanMode = false;
  bool silentMode = true;
  bool _3DAuto = (AC_HSWING == HDIR_SWING);
  heatpump->send(irSender, AC_POWER, AC_MODE, AC_FAN, AC_TEMP, AC_VSWING, AC_HSWING, cleanMode, silentMode, _3DAuto);
  publishState();
}

void publishState() {
  Serial.println("Publish state");

  String mode;
  if (AC_POWER == POWER_OFF) {
    mode = "off";
  } else if (AC_MODE == MODE_AUTO) {
    mode = "auto";
  } else if (AC_MODE == MODE_HEAT) {
    mode = "heat";
  } else if (AC_MODE == MODE_COOL) {
    mode = "cool";
  } else if (AC_MODE == MODE_DRY) {
    mode = "dry";
  } else if (AC_MODE == MODE_FAN) {
    mode = "fan";
  }
  publishTopic(MQTT_STATUS_CHANNEL "mode", mode);

  String fan;
  if (AC_FAN == FAN_AUTO) {
    fan = "auto";
  } else if (AC_FAN == FAN_1) {
    fan = "low";
  } else if (AC_FAN == FAN_2) {
    fan = "medium";
  } else if (AC_FAN == FAN_3) {
    fan = "high";
  }
  publishTopic(MQTT_STATUS_CHANNEL "fan", fan);

  publishTopic(MQTT_STATUS_CHANNEL "target", String(AC_TEMP));

  String swing;
  if (AC_HSWING == HDIR_SWING) {
    swing = "auto";
  } else {
    swing = "off";
  }
  publishTopic(MQTT_STATUS_CHANNEL "swing", swing);

  if (!isnan(last_temp) && !isnan(last_hum)) {
    publishTopic(MQTT_STATUS_CHANNEL "temperature", String(last_temp, 1));
    publishTopic(MQTT_STATUS_CHANNEL "humidity", String(last_hum, 1));
  }
}

void publishTopic(char *topic, String state) {
  if (state == "") {
    return;
  }
  char message[MESSZ];
  state.toCharArray(message, MESSZ);
  mqttClient.publish(topic, message, true);
  Serial.print(topic);
  Serial.print(" = ");
  Serial.println(state);

  char topicBuf[TOPSZ];
  strcpy(topicBuf, topic);
  strcat(topicBuf, "/set");
  mqttClient.subscribe(topicBuf);
}

void reconnectMQTT() {
  // Loop until we're reconnected
  Serial.println("Attempting MQTT connection...");
  // Attempt to connect
  if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
    Serial.println("connected");
    mqttClient.setCallback(mqttDataCb);
    mqttClient.subscribe(MQTT_STATUS_CHANNEL);
  } else {
    Serial.print("failed, rc=");
    Serial.println(mqttClient.state());
  }
}

void initWIFI() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
}

void initOTA() {
  ArduinoOTA.begin();
  ArduinoOTA.onStart([]() {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2 - 10, "OTA Update");
    display.display();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    display.drawProgressBar(4, 32, 120, 8, progress / (total / 100) );
    display.display();
  });

  ArduinoOTA.onEnd([]() {
    display.clear();
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display.drawString(display.getWidth() / 2, display.getHeight() / 2, "Restart");
    display.display();
  });
}

