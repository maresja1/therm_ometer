#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>

extern "C" {
    #include "user_interface.h"
}

#include <Wire.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include "thermo_wifi.h"
#include <Adafruit_SSD1306.h>
#include <Adafruit_BMP280.h>

WiFiClient espClient;
PubSubClient client(espClient);

char mqtt_broker[40] = "unset";
char mqtt_port[6] = "8080";
char mqtt_api_token[34] = "YOUR_API_TOKEN";
const uint32_t BUFFER_MAX_SIZE = 8*1024;
char buffer[BUFFER_MAX_SIZE] = "";
DynamicJsonDocument json(1024);

// The extra parameters to be configured (can be either global or just in the setup)
// After connecting, parameter.getValue() will get you the configured value
// id/name placeholder/prompt default length
WiFiManagerParameter *custom_mqtt_server;
WiFiManagerParameter *custom_mqtt_port;
WiFiManagerParameter *custom_api_token;

float roomTemp = 0.0f;
float pressure = 0.0f;

const String &topicBase = String("/therm_ometer_") + EspClass::getChipId();
const String &generalTopicBase = "homeassistant/sensor" + topicBase;

void saveConfig()
{
    //read updated parameters
    strcpy(mqtt_broker, custom_mqtt_server->getValue());
    strcpy(mqtt_port, custom_mqtt_port->getValue());
    strcpy(mqtt_api_token, custom_api_token->getValue());

	Serial.println("saving config");
	json["mqtt_server"] = mqtt_broker;
	json["mqtt_port"] = mqtt_port;
	json["api_token"] = mqtt_api_token;

	File configFile = LittleFS.open("/config.json", "w");
	if (!configFile) {
		Serial.println("failed to open config file for writing");
	}
	serializeJson(json, Serial);
	serializeJson(json, configFile);
	configFile.flush();
	configFile.close();
}

void switchToConfigMode(WiFiManager &wifiManager, DynamicJsonDocument &pJson) {
    wifiManager.startConfigPortal();
    EspClass::restart();
    delay(1000);
}

void jsonDiscoverPreset(JsonDocument &json);

void readAndPrintTemp();

#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 32 // OLED display height, in pixels
#define SCREEN_ADDRESS 0x3C ///< See datasheet for Address; 0x3D for 128x64, 0x3C for 128x32
Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire);
Adafruit_BMP280 bmp;
void setup() {
    Wire.begin();
    // Set software serial baud to 115200;
    Serial.begin(115200);
    for (int i = 0; i < 10 && !Serial; ++i) {
        // wait for serial port to connect. Needed for native USB port only
        delay(10);
    }

    Serial.println("before display init...");

    if(!oled.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        for(;;); // Don't proceed, loop forever
    }

    // Show initial display buffer contents on the screen --
    // the library initializes this with an Adafruit splash screen.
    oled.display();
    delay(2000); // Pause for 2 seconds

    if (!bmp.begin(BMP280_ADDRESS_ALT)) {
        Serial.println("Could not find a valid BMP280 sensor, check wiring!");
        for(;;); // Don't proceed, loop forever
    }

    /* Default settings from datasheet. */
    bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     /* Operating Mode. */
                    Adafruit_BMP280::SAMPLING_X2,     /* Temp. oversampling */
                    Adafruit_BMP280::SAMPLING_X2,    /* Pressure oversampling */
                    Adafruit_BMP280::FILTER_X16,      /* Filtering. */
                    Adafruit_BMP280::STANDBY_MS_500); /* Standby time. */
    delay(2000);
    readAndPrintTemp();

    //read configuration from FS json
    Serial.println("mounting FS...");

    if (LittleFS.begin()) {
        Serial.println("mounted file system");
        if (LittleFS.exists("/config.json")) {
            //file exists, reading and loading
            Serial.println("reading config file");
            File configFile = LittleFS.open("/config.json", "r");
            if (configFile && configFile.size() > BUFFER_MAX_SIZE) {
                Serial.println("FS or file corrupt");
            } else if (configFile) {
                Serial.println("opened config file");
                size_t size = configFile.size();
                configFile.readBytes(buffer, size);
                auto deserializeError = deserializeJson(json, buffer);
                serializeJson(json, Serial);
                if ( ! deserializeError ) {
                    Serial.println("\nparsed json");
                    strcpy(mqtt_broker, json["mqtt_server"]);
                    strcpy(mqtt_port, json["mqtt_port"]);
                    strcpy(mqtt_api_token, json["api_token"]);
                    Serial.print("The values in the file are: ");
                    Serial.print("\tmqtt_broker : " + String(mqtt_broker));
                    Serial.print("\tmqtt_port : " + String(mqtt_port));
                    Serial.println("\tmqtt_api_token : " + String(mqtt_api_token));
                } else {
                    Serial.println("failed to load json config");
                }
                configFile.close();
            }
        }
    } else {
        Serial.println("failed to mount FS");
    }
    //end read

    //connecting to a mqtt broker
    WiFiManager wifiManager;
#ifndef DEBUG
    wifiManager.setDebugOutput(false);
#endif
	wifiManager.setConfigPortalTimeout(300);

    //set callback that gets called when connecting to previous WiFi fails, and enters Access Point mode
    wifiManager.setAPCallback(configModeCallback);
    wifiManager.setSaveConfigCallback(saveConfig);

    custom_mqtt_server = new WiFiManagerParameter("server", "mqtt server", mqtt_broker, 40);
    custom_mqtt_port = new WiFiManagerParameter("port", "mqtt port", mqtt_port, 6);
    custom_api_token = new WiFiManagerParameter("apikey", "API token", mqtt_api_token, 32);

    wifiManager.addParameter(custom_mqtt_server);
    wifiManager.addParameter(custom_mqtt_port);
    wifiManager.addParameter(custom_api_token);

    wifi_softap_dhcps_start();
    if(!wifiManager.autoConnect("therm_ometer")) {
        Serial.println("failed to connect and hit timeout");
        //reset and try again, or maybe put it to deep sleep
        EspClass::restart();
        delay(1000);
    }
    wifi_softap_dhcps_stop();

    if (!MDNS.begin("therm_ometer")) {
        Serial.println("Error setting up MDNS responder!");
    }

    IPAddress remote_addr;
    if (!WiFi.hostByName(mqtt_broker, remote_addr)) {
        Serial.println("Error resolving mqtt_broker");
    };
    client.setServer(mqtt_broker, strtol(mqtt_port, nullptr, 10));
    client.setCallback(mqttDataCallback);
    uint16_t failures = 0;
    while (!client.connected()) {
		if (++failures > 10) {
			switchToConfigMode(wifiManager, json);
			failures = 0;
		}
        const String client_id("esp8266-client-" + String(EspClass::getChipId()));
        if (client.connect(client_id.c_str(), "therm_ometer", mqtt_api_token)) {
#ifdef DEBUG
            Serial.printf("The client %s (%s) connected to the mqtt broker\r\n", client_id.c_str(), mqtt_api_token);
#endif
        } else {
#ifdef DEBUG
            Serial.printf("The client %s (%s) ", client_id.c_str(), mqtt_api_token);
#endif
			// state > 0 means more attempts won't help, reset
            Serial.printf("failed to connect to mqtt, state: %tmp\r\n", client.state());
            if (client.state() > 0) {
				switchToConfigMode(wifiManager, json);
			}
            delay(2000);
        }
    }

    // no save allowed pass here
    delete custom_api_token, delete custom_mqtt_port, delete custom_mqtt_server;
    custom_api_token = custom_mqtt_port = custom_mqtt_server = nullptr;

    jsonDiscoverPreset(json);
    json["name"] = "Therm oMeter Temperature";
    json["device_class"] = "temperature";
    json["unit_of_measurement"] = "°C";
    json["value_template"] = "{{ value_json.roomTemp }}";
    json["unique_id"] = topicBase.substring(1) + "-temp";
    client.beginPublish((generalTopicBase + "-roomTemp/config").c_str(), measureJson(json), false);
    serializeJson(json, client);
    client.endPublish();

    jsonDiscoverPreset(json);
    json["name"] = "Therm oMeter Pressure";
    json["device_class"] = "pressure";
    json["unit_of_measurement"] = "hPa";
    json["value_template"] = "{{ value_json.pressure }}";
    json["unique_id"] = topicBase.substring(1) + "-pressure";
    client.beginPublish((generalTopicBase + "-pressure/config").c_str(), measureJson(json), false);
    serializeJson(json, client);
    client.endPublish();

    json.clear();

    Serial.println("State topic: " + generalTopicBase + "/state");
    // publish and subscribe
//    client.subscribe((generalTopicBase + heatNeededSetTopic).c_str());
}

void jsonDiscoverPreset(JsonDocument &json) {
    json.clear();
    const JsonObject &device = json.createNestedObject("device");
    device["identifiers"] = String(EspClass::getChipId());
    device["name"] = "Therm oMeter";
    json["~"] = generalTopicBase;
    json["stat_t"] = "~/state";
}

void loop() {
    MDNS.update();

    if (!client.connected()) {
        EspClass::restart();
    }
    client.loop();

    readAndPrintTemp();
    sendState();
    delay(2000);
}

void readAndPrintTemp()
{
    roomTemp = bmp.readTemperature();
    pressure = bmp.readPressure() / 100;
#ifdef DEBUG
    Serial.print("temp: ");
    Serial.println(roomTemp);
    Serial.print("pressure: ");
    Serial.println(pressure);
#endif

    oled.clearDisplay();
    oled.setTextSize(2);
    oled.setTextColor(WHITE);
    oled.setCursor(0, 0);
    oled.print(roomTemp);
    oled.println(" \xF7""C");
    oled.print(pressure, 1);
    oled.println(" hPa");
    oled.display();
}

void sendState() {
    json.clear();
    json["roomTemp"] = serialized(String(roomTemp, 2));
    json["pressure"] = serialized(String(pressure, 2));

    const String &topicBaseState = generalTopicBase + "/state";
//    Serial.println(topicBaseState);
    client.beginPublish(topicBaseState.c_str(), measureJson(json), false);
    serializeJson(json, client);
    client.endPublish();
//    serializeJson(doc, Serial);
//    Serial.println();
}


void mqttDataCallback(char* topic, const uint8_t* payload, unsigned int length) {
    String topicStr = String(topic);

    if (!topicStr.startsWith(generalTopicBase)) return;

//    Serial.print("Message arrived [");
//    Serial.print(topic);
//    Serial.print("] ");
    char payloadCStr[length + 1];
    for (unsigned int i = 0; i < length; i++) {
        payloadCStr[i] = (char)payload[i];
    }
    payloadCStr[length] = '\0';
//    Serial.print(payloadCStr);
//    Serial.println();

//    if (topicStr.equals(generalTopicBase + heatNeededSetTopic)) {
//        const bool lastHeatNeeded = strcasecmp(payloadCStr, "on") == 0;
//        if (lastHeatNeeded != heatNeeded) {
//            sendCmdHeatOverride(true, lastHeatNeeded);
//        }
//    }
}

void configModeCallback (WiFiManager *myWiFiManager) {
    Serial.println("Entered config mode");
    Serial.println(WiFi.softAPIP());
    //if you used auto generated SSID, print it
    Serial.println(myWiFiManager->getConfigPortalSSID());
}
