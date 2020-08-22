#include <Arduino.h>

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <MitsubishiHeavyHeatpumpIR.h>
#include <Schedule.h>
#include <Ticker.h>

#include <OLEDdisplay.h>
#include <OLEDDisplayFonts.h>
#include <OLEDDisplayUi.h>
#include <SSD1306.h>
#include <SSD1306Wire.h>

#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <SPI.h>

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>

#include <EEPROM.h>
#include "spi_flash.h"
#include <Embedis.h>

#define TOPSZ                  60           // Max number of characters in topic string
#define MESSZ                  240          // Max number of characters in JSON message string
#define WIFI_SSID "********"
#define WIFI_PASS "********"
#define MQTT_HOST "********"
#define MQTT_PORT 1883
#define MQTT_USER "********"
#define MQTT_PASS "********"
#define MQTT_CLIENT_ID "livingroomhvac"
#define MQTT_STATUS_CHANNEL "livingroom/meteo/"
#define HTTP_PORT 80  // The port the HTTP server is listening on.
#define HOSTNAME "lrclimate"  // Name of the device you want in mDNS.

ESP8266WebServer server(HTTP_PORT);
MDNSResponder mdns;
WiFiManager wifiManager;
WiFiClient espClient;
PubSubClient mqttClient(espClient);
Adafruit_BME280 bme; // I2C
SSD1306Wire *display;
IRSenderBitBang irSender(D8);
MitsubishiHeavyZJHeatpumpIR *heatpump;
Ticker looper, cleaner;
float last_temp = sqrt(-1), last_hum = sqrt(-1);
const int buttonPin = D3;
const int resetPin = D0;
bool lastBS = false;
String curPower = "none", curMode = "none", curFan = "none", curTarget = "none", curSwing = "none", curTemp = "none", curHumdid = "none";
bool sleepMode = false;

// Set defaults
uint8_t AC_POWER = POWER_OFF,
        AC_MODE = MODE_AUTO,
        AC_FAN = FAN_AUTO,
        AC_TEMP = 24,
        AC_VSWING = VDIR_SWING,
        AC_HSWING = HDIR_SWING;

int getSetting(const String& key, int defaultValue) {
  String value;
  if (!Embedis::get(key, value)) {
    value = String(defaultValue);
  }
  return value.toInt();
}

bool setSetting(const String& key, int value) {
    return Embedis::set(key, String(value));
}

void clearDisplay() {
  display->clear();
  display->display();
}

void scheduleClearDisplay(void *context) {
  schedule_function(std::bind(&clearDisplay));
}

void displayInfo() {
  if (sleepMode) {
    return;
  }

  display->clear();
  display->setFont(ArialMT_Plain_10);

  char message[MESSZ];
  sprintf(
    message,
    " power: %d, mode: %d, fan: %d\n t: %d, vs: %d, hs: %d\n ip: %s",
    AC_POWER, AC_MODE, AC_FAN, AC_TEMP, AC_VSWING, AC_HSWING, WiFi.localIP().toString().c_str()
  );
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, 0, message);
  display->setTextAlignment(TEXT_ALIGN_CENTER);

  if (!isnan(last_temp)) {
    display->drawString(64, 40, "t " + String(last_temp, 1) + " °C\n");
  }

  if (!isnan(last_hum)) {
    display->drawString(64, 52, "h " + String(last_hum, 1) + " %");
  }

  display->display();

  cleaner.once_ms(5000, scheduleClearDisplay, (void *)0);
}

void publishTopic(const char *topic, String state) {
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

void publishState() {
  String power;
  if (AC_POWER == POWER_ON) {
    power = "ON";
  } else {
    power = "OFF";
  }

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
    mode = "fan_only";
  }

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

  String swing;
  if (AC_HSWING == HDIR_SWING) {
    swing = "auto";
  } else {
    swing = "off";
  }

  String target = String(AC_TEMP);
  String temp = "N/A", hum = "N/A";
  if (!isnan(last_temp) && !isnan(last_hum)) {
    temp = String(last_temp, 1);
    hum = String(last_hum, 1);
  }

  if (curPower != power || curMode != mode || curFan != fan || curTarget != target || curSwing != swing)
    displayInfo();

  /* if (curPower != power) {
    publishTopic(MQTT_STATUS_CHANNEL "power", power);
    curPower = power;
  } */
  if (curTemp != temp) {
    publishTopic(MQTT_STATUS_CHANNEL "temperature", temp);
    curTemp = temp;
  }
  if (curHumdid != hum) {
    publishTopic(MQTT_STATUS_CHANNEL "humidity", hum);
    curHumdid = hum;
  }
  if (curMode != mode) {
    publishTopic(MQTT_STATUS_CHANNEL "mode", mode);
    curMode = mode;
  }
  if (curFan != fan) {
    publishTopic(MQTT_STATUS_CHANNEL "fan", fan);
    curFan = fan;
  }
  if (curTarget != target) {
    publishTopic(MQTT_STATUS_CHANNEL "target", target);
    curTarget = target;
  }
  if (curSwing != swing) {
    publishTopic(MQTT_STATUS_CHANNEL "swing", swing);
    curSwing = swing;
  }
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

  if (!strcasecmp(suffix, "power")) {
    Serial.print("power ");
    Serial.println(payload);
    if (!strcasecmp(dataBuf, "ON")) {
      AC_POWER = POWER_ON;
      setSetting("mode", AC_MODE);
    } else {
      AC_POWER = POWER_OFF;
    }
    setSetting("power", AC_POWER);
  } else if (!strcasecmp(suffix, "mode")) {
    Serial.print("mode ");
    Serial.println(payload);
    if (!strcasecmp(dataBuf, "off")) {
      AC_POWER = POWER_OFF;
    } else {
      AC_POWER = POWER_ON;
      if (!strcasecmp(dataBuf, "auto")) {
        AC_MODE = MODE_AUTO;
      } else if (!strcasecmp(dataBuf, "heat")) {
        AC_MODE = MODE_HEAT;
      } else if (!strcasecmp(dataBuf, "cool")) {
        AC_MODE = MODE_COOL;
      } else if (!strcasecmp(dataBuf, "dry")) {
        AC_MODE = MODE_DRY;
      } else if (!strcasecmp(dataBuf, "fan_only")) {
        AC_MODE = MODE_FAN;
      } else {
        AC_POWER = POWER_OFF;
      }
    }
    setSetting("power", AC_POWER);
    setSetting("mode", AC_MODE);
  } else if (!strcasecmp(suffix, "fan")) {
    Serial.print("fan ");
    Serial.println(payload);
    if (!strcasecmp(dataBuf, "auto")) {
      AC_FAN = FAN_AUTO;
    } else if (!strcasecmp(dataBuf, "low")) {
      AC_FAN = FAN_1;
    } else if (!strcasecmp(dataBuf, "medium")) {
      AC_FAN = FAN_2;
    } else if (!strcasecmp(dataBuf, "high")) {
      AC_FAN = FAN_3;
    }
    setSetting("fan", AC_FAN);
  } else if (!strcasecmp(suffix, "target")) {
    Serial.print("target ");
    Serial.println(payload);
    AC_TEMP = payload;
    setSetting("temp", AC_TEMP);
  } else if (!strcasecmp(suffix, "swing")) {
    if (!strcasecmp(dataBuf, "auto")) {
      AC_VSWING = VDIR_SWING;
      AC_HSWING = HDIR_SWING;
    } else {
      AC_VSWING = VDIR_UP;
      AC_HSWING = HDIR_MIDDLE;
    }
    setSetting("vswing", AC_VSWING);
    setSetting("hswing", AC_HSWING);
  }

  bool cleanMode = false;
  bool silentMode = true;
  bool _3DAuto = (AC_HSWING == HDIR_SWING);
  heatpump->send(irSender, AC_POWER, AC_MODE, AC_FAN, AC_TEMP, AC_VSWING, AC_HSWING, cleanMode, silentMode, _3DAuto);
  publishState();
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

void tenLoop() {
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }

  bme.takeForcedMeasurement();
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  bool need_update = false;

  if (!isnan(t)) {
    if (isnan(last_temp)) {
      need_update = true;
    }
    last_temp = t;
  }

  if (!isnan(h)) {
    if (isnan(last_hum)) {
      need_update = true;
    }
    last_hum = h;
  }

  if (need_update) {
    displayInfo();
  }

  if (mqttClient.connected()) {
    publishState();
  }
}

void scheduleTenLoop(void *context) {
  schedule_function(std::bind(&tenLoop));
}

void initWIFI() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
/*
  delay(10);
  wifiManager.setTimeout(300);  // Time out after 5 mins.
  if (!wifiManager.autoConnect()) {
    delay(3000);
    ESP.reset();
    delay(5000); // Reboot. A.k.a. "Have you tried turning it Off and On again?"
  }
*/
}

void initMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  if (!mqttClient.connected()) {
    reconnectMQTT();
  }
}

void initOTA() {
  ArduinoOTA.begin();
  ArduinoOTA.onStart([]() {
    display->clear();
    display->setFont(ArialMT_Plain_10);
    display->setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display->drawString(display->getWidth() / 2, display->getHeight() / 2 - 10, "OTA Update");
    display->display();
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    display->drawProgressBar(4, 32, 120, 8, progress / (total / 100) );
    display->display();
  });

  ArduinoOTA.onEnd([]() {
    display->clear();
    display->setFont(ArialMT_Plain_10);
    display->setTextAlignment(TEXT_ALIGN_CENTER_BOTH);
    display->drawString(display->getWidth() / 2, display->getHeight() / 2, "Restart");
    display->display();
  });
}

void handleRoot() {
  server.send(200, "text/html",
              "<html><head><title>Climate MQTT server</title></head>"
              "<body>"
              "<center><h1>ESP8266 Climate MQTT Server</h1></center>"
              "<br><hr>"
              "<h3>Information</h3>"
              "<p>IP address: " + WiFi.localIP().toString() + "<br>"
              "<br><hr>"
              "<h3>Update IR Server firmware</h3><p>"
              "<b><mark>Warning:</mark></b><br> "
              "<i>Updating your firmware may screw up your access to the device. "
              "If you are going to use this, know what you are doing first "
              "(and you probably do).</i><br>"
              "<form method='POST' action='/update' enctype='multipart/form-data'>"
              "Firmware to upload: <input type='file' name='update'>"
              "<input type='submit' value='Update'>"
              "</form>"
              "</body></html>");
}

void handleReset() {
  server.send(200, "text/html",
              "<html><head><title>Reset Config</title></head>"
              "<body>"
              "<h1>Resetting the WiFiManager config back to defaults.</h1>"
              "<p>Device restarting. Try connecting in a few seconds.</p>"
              "</body></html>");
  // Do the reset.
  wifiManager.resetSettings();
  delay(10);
  ESP.restart();
  delay(1000);
}

void handleNotFound() {
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i < server.args(); i++)
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  server.send(404, "text/plain", message);
}

void initServer() {
  if (mdns.begin(HOSTNAME, WiFi.localIP())) {
    Serial.println("MDNS responder started");
  }

  // Setup the root web page.
  server.on("/", handleRoot);
  // Setup a reset page to cause WiFiManager information to be reset.
  server.on("/reset", handleReset);

  // Setup the URL to allow Over-The-Air (OTA) firmware updates.
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      WiFiUDP::stopAll();
      Serial.println("Update: " + upload.filename);
      uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) &
                                0xFFFFF000;
      if (!Update.begin(maxSketchSpace)) {  // start with max available size
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) !=
          upload.currentSize) {
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) {  // true to set the size to the current progress
        Serial.println("Update Success: " + (String) upload.totalSize +
              "\nRebooting...");
      }
    }
    yield();
  });

  // Set up an error page.
  server.onNotFound(handleNotFound);

  server.begin();
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  mqttClient.loop();

  int buttonState = digitalRead(buttonPin);
  if (buttonState == LOW) {
    lastBS = true;
  } else {
    if (lastBS) {
      displayInfo();
    }
    lastBS = false;
  }

  run_scheduled_functions();
}

void setup() {
  Serial.begin(9600);
  Serial.println("Booting");
  delay(10);

  pinMode(buttonPin, INPUT);
  pinMode(resetPin, INPUT);
  sleepMode = (digitalRead(resetPin) == HIGH);

  Serial.println("Setup settings");
  // Create a key-value Dictionary in emulated EEPROM (FLASH actually...)
  EEPROM.begin(SPI_FLASH_SEC_SIZE);
  Embedis::dictionary( 
      "EEPROM",
      SPI_FLASH_SEC_SIZE,
      [](size_t pos) -> char { return EEPROM.read(pos); },
      [](size_t pos, char value) { EEPROM.write(pos, value); },
      []() { EEPROM.commit(); }
  );

  AC_POWER = getSetting("power", AC_POWER);
  AC_MODE = getSetting("mode", AC_MODE);
  AC_FAN = getSetting("fan", AC_FAN);
  AC_TEMP = getSetting("temp", AC_TEMP);
  AC_VSWING = getSetting("vswing", AC_VSWING);
  AC_HSWING = getSetting("hswing", AC_HSWING);

  if (!sleepMode) {
    Serial.println("Setup TFT");

    display = new SSD1306Wire(0x3c, D6, D5);
    display->init();
    display->flipScreenVertically();
    display->setContrast(255);

    display->clear();
    display->setFont(ArialMT_Plain_10);
    display->setTextAlignment(TEXT_ALIGN_CENTER);
    display->drawString(64, 32, "Hello!");
    display->display();
  }

  Serial.println("Setup Climate Sensor");
  Wire.begin(D6, D5);
  bool status = bme.begin(0x76);
  if (!status) {
    Serial.println("Could not find a valid BME280 sensor, check wiring!");
  } else {
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                Adafruit_BME280::SAMPLING_X1, // temperature
                Adafruit_BME280::SAMPLING_X1, // pressure
                Adafruit_BME280::SAMPLING_X1, // humidity
                Adafruit_BME280::FILTER_OFF,
                Adafruit_BME280::STANDBY_MS_0_5);
  } 

  Serial.println("Setup WiFi");

  initWIFI();

  if (!sleepMode) {
    initOTA();
    initServer();
  }

  Serial.println("Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  if (!sleepMode) {
    Serial.println("Setup HVAC");
    heatpump = new MitsubishiHeavyZJHeatpumpIR();
  }

  Serial.println("Setup MQTT");
  initMQTT();

  // Read initial values and display them
  tenLoop();

  if (sleepMode) {
    Serial.println("Wait a second");
    delay(1000);

    // D0 is connected to RST: Go to sleep
    Serial.println("Going to sleep");
    ESP.deepSleep(600 * 1000000);
  } else {
    // D0 is connected to GND: Init loop timers
    Serial.println("Scheduling the loop");
    looper.attach_ms(60000, scheduleTenLoop, (void *)0);
  }
}
