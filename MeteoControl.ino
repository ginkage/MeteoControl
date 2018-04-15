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

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ESP8266mDNS.h>

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
#define HTTP_PORT 80  // The port the HTTP server is listening on.
#define HOSTNAME "lrclimate"  // Name of the device you want in mDNS.

SSD1306Wire display(0x3c, D7, D5);
DHT dht(D6, DHT22);
WiFiClient espClient;
PubSubClient mqttClient(espClient);
IRSenderBitBang irSender(D8);
MitsubishiHeavyZJHeatpumpIR *heatpump;
Timer t;
float last_temp = sqrt(-1), last_hum = sqrt(-1);

ESP8266WebServer server(HTTP_PORT);
MDNSResponder mdns;
WiFiManager wifiManager;

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
  initServer();

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
  server.handleClient();
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
      " power: %d, mode: %d, fan: %d\n t: %d, vs: %d, hs: %d\n ip: %s",
      AC_POWER, AC_MODE, AC_FAN, AC_TEMP, AC_VSWING, AC_HSWING, WiFi.localIP().toString().c_str()
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
