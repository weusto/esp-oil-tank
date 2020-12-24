#include <Arduino.h>
#include <ArduinoJson.h>
#include <Client.h>
#include <WiFiClientSecure.h>
#include <MQTT.h>
#include <CloudIoTCore.h>
#include <CloudIoTCoreMqtt.h>
#include "ca_crt.h"

#include <DNSServer.h>
#if defined(ESP8266)
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#define VARIANT "esp8266"
#else
#include <WebServer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#define VARIANT "esp32"
#endif

#include <WiFiManager.h>
#include <UniversalTelegramBot.h>

#define USE_SERIAL Serial

#define DEBUG //!< active le mode débogage

#define CURRENT_VERSION VERSION
#define CLOUD_FUNCTION_URL "https://us-central1-oil-tank-298920.cloudfunctions.net/getDownloadUrl"

// Périodes
#define PERIODE_ACQUISITION 500     //!< période d'acquisition en millisecondes pour la sonde
#define PERIODE_ENVOI       60000    //!< période d'envoi des données en millisecondes pour MQTT


WiFiClient client;
#if defined(ESP8266)
ESP8266WebServer server(80);
#else
WebServer server(80);
#endif

// Wifi
const char *ssid = "";
const char *password = "";

// Cloud IoT
const char *project_id = "weuiot";
const char *location = "us-central1";
const char *registry_id = "weuiot-registry";
const char *device_id = "esp-1";

// NTP
const char* ntp_primary = "pool.ntp.org";
const char* ntp_secondary = "time.nist.gov";

const char* private_key_str =
  "bc:fd:9d:07:38:7f:1f:b6:3a:1c:b7:5c:01:aa:c1:"
  "d6:fe:18:03:c1:da:0c:7d:a8:af:4e:56:c5:61:be:"
  "1c:4e";

const int jwt_exp_secs = 3600; // Maximum 24H (3600*24)
const int ex_num_topics = 0;
const char* ex_topics[ex_num_topics];

float TANK_HEIGHT_IN_CM = 0;
float TANK_LENGTH_IN_CM = 0;
float TANK_WIDTH_IN_CM = 0;
float FULL_VOLUME_IN_LITERS = 1;
String UNIT;


Client *netClient;
CloudIoTCoreDevice *device;
CloudIoTCoreMqtt *mqtt;
MQTTClient *mqttClient;
unsigned long iss = 0;
String jwt;
unsigned long lastMillis = 0;

String getDefaultSensor();
String getJwt();
void setupWifi();
void connectWifi();
void publishTelemetry(String data);
void publishTelemetry(String subfolder, String data);
void connect();
void setupCloudIoT();
void getTankLevel();

// Initialize Telegram BOT
#define BOTtoken "1475527759:AAEuQSvWrhafu8dNrzGzaHmBpQx-80TqT34"  // your Bot Token (Get from Botfather)

// Use @myidbot to find out the chat ID of an individual or a group
// Also note that you need to click "start" on a bot before it can
// message you
String CHAT_ID = "";

WiFiClientSecure clientSecure;
UniversalTelegramBot bot(BOTtoken, clientSecure);

bool telegramNotificationTankFullSent = false;
bool telegramNotificationTankEmptySent = false;

void messageReceived(String &topic, String &payload) 
{
  USE_SERIAL.println("<- " + topic + " - " + payload);
  
  if(topic == "/devices/" + String(device_id) + "/config") {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    JsonObject obj = doc.as<JsonObject>();

    FULL_VOLUME_IN_LITERS = obj["full_volume_in_liters"];
    TANK_HEIGHT_IN_CM = obj["tank_height_in_cm"];
    TANK_LENGTH_IN_CM = obj["tank_lenght_in_cm"];
    TANK_WIDTH_IN_CM = obj["tank_width_in_cm"];
    UNIT = (const char*)obj["unit"];

    CHAT_ID = (const char*)obj["telegram_chat_id"];
  }
}

/* 
 * Check if needs to update the device and returns the download url.
 */
String getDownloadUrl()
{
  HTTPClient http;
  String downloadUrl;
  USE_SERIAL.print("[HTTP] begin...\n");

  String url = CLOUD_FUNCTION_URL;
  url += String("?version=") + CURRENT_VERSION;
  url += String("&variant=") + VARIANT;
  http.begin(url);

  USE_SERIAL.print("[HTTP] GET...\n");
  // start connection and send HTTP header
  int httpCode = http.GET();

  // httpCode will be negative on error
  if (httpCode > 0)
  {
    // HTTP header has been send and Server response header has been handled
    USE_SERIAL.printf("[HTTP] GET... code: %d\n", httpCode);

    // file found at server
    if (httpCode == HTTP_CODE_OK)
    {
      String payload = http.getString();
      USE_SERIAL.println(payload);
      downloadUrl = payload;
    } else {
      USE_SERIAL.println("Device is up to date!");
    }
  }
  else
  {
    USE_SERIAL.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();

  return downloadUrl;
}

/* 
 * Download binary image and use Update library to update the device.
 */
bool downloadUpdate(String url)
{
  HTTPClient http;
  USE_SERIAL.print("[HTTP] Download begin...\n");

  http.begin(url);

  USE_SERIAL.print("[HTTP] GET...\n");
  // start connection and send HTTP header
  int httpCode = http.GET();
  if (httpCode > 0)
  {
    // HTTP header has been send and Server response header has been handled
    USE_SERIAL.printf("[HTTP] GET... code: %d\n", httpCode);

    // file found at server
    if (httpCode == HTTP_CODE_OK)
    {

      int contentLength = http.getSize();
      USE_SERIAL.println("contentLength : " + String(contentLength));

      if (contentLength > 0)
      {
        bool canBegin = Update.begin(contentLength);
        if (canBegin)
        {
          WiFiClient stream = http.getStream();
          USE_SERIAL.println("Begin OTA. This may take 2 - 5 mins to complete. Things might be quite for a while.. Patience!");
          size_t written = Update.writeStream(stream);

          if (written == contentLength)
          {
            USE_SERIAL.println("Written : " + String(written) + " successfully");
          }
          else
          {
            USE_SERIAL.println("Written only : " + String(written) + "/" + String(contentLength) + ". Retry?");
          }

          if (Update.end())
          {
            USE_SERIAL.println("OTA done!");
            if (Update.isFinished())
            {
              USE_SERIAL.println("Update successfully completed. Rebooting.");
              ESP.restart();
              return true;
            }
            else
            {
              USE_SERIAL.println("Update not finished? Something went wrong!");
              return false;
            }
          }
          else
          {
            USE_SERIAL.println("Error Occurred. Error #: " + String(Update.getError()));
            return false;
          }
        }
        else
        {
          USE_SERIAL.println("Not enough space to begin OTA");
          client.flush();
          return false;
        }
      }
      else
      {
        USE_SERIAL.println("There was no content in the response");
        client.flush();
        return false;
      }
    }
    else
    {
      return false;
    }
  }
  else
  {
    return false;
  }
}

const int trigPin = 16;   // trigger pin
const int echoPin = 17;   // echo pin

float duration;           // variable to store the duration as float
int waterFilllevel = 0;
float distance;           // variable to store the distance as float
int tank_volume = 0;
int tank_percent = 0;

/* 
 * Show current device version
 */
void handleRoot() {  
  server.send(200, "text/plain", "v" + String(CURRENT_VERSION));
}

void setup() {
  USE_SERIAL.begin(115200);
  USE_SERIAL.setDebugOutput(true);  
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(trigPin, OUTPUT);             // set the trigger pin as output
  pinMode(echoPin, INPUT);               // set the echo pin as input

  delay(3000);
  USE_SERIAL.println("\n Starting");
  // Setup Wifi Manager
  String version = String("<p>Current Version - v") + String(CURRENT_VERSION) + String("</p>");
  USE_SERIAL.println(version);
  
  WiFiManager wm;
  WiFiManagerParameter versionText(version.c_str());
  wm.addParameter(&versionText);    
    
  if (!wm.autoConnect()) {
    USE_SERIAL.println("failed to connect and hit timeout");
    //reset and try again, or maybe put it to deep sleep
    ESP.restart();
    delay(1000);
  }
 
  // Check if we need to download a new version
  String downloadUrl = getDownloadUrl();
  if (downloadUrl.length() > 0)
  {
    bool success = downloadUpdate(downloadUrl);
    if (!success)
    {
      USE_SERIAL.println("Error updating device");
    }
  }

  server.on("/", handleRoot);
  server.begin();
  USE_SERIAL.println("HTTP server started");

  setupCloudIoT();
  USE_SERIAL.print("IP address: ");
  USE_SERIAL.println(WiFi.localIP());
}

void loop() {
  mqttClient->loop();
  delay(100);  // <- fixes some issues with WiFi stability

  if (!mqttClient->connected())
  {
    connect();
  }

  if (millis() - lastMillis > PERIODE_ENVOI)
  {
    lastMillis = millis();

    getTankLevel();    
  }

  // Just chill
  server.handleClient();
}

void getTankLevel()
{
  // Clear the trigPin by setting it LOW:
  digitalWrite(trigPin, LOW);

  // wait 2ms to make sure the trigPin is LOW
  delayMicroseconds(2);

  // Trigger the sensor by setting the trigPin high for 10 microseconds:
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Read the echoPin. pulseIn() returns the duration (length of the pulse) in microseconds:
  duration = pulseIn(echoPin, HIGH);
  distance = (duration * 0.0343) / 2;  // calculate the distance based on the speed of sound
                                       // we need to divide by 2 since the sound travelled the distance twice

  USE_SERIAL.println("Distance #: " + String(distance) + "cm");

  waterFilllevel = TANK_HEIGHT_IN_CM - distance;

  USE_SERIAL.println("Calcul #: " + String((TANK_LENGTH_IN_CM * TANK_WIDTH_IN_CM * waterFilllevel) / 1000) + "liter");

  if (distance >= TANK_HEIGHT_IN_CM) {
    tank_volume = 0;
    tank_percent = 0;
  }

  if (distance < 10 && distance > 0 && !telegramNotificationTankFullSent) {
    tank_volume = FULL_VOLUME_IN_LITERS;
    tank_percent = 100;

    bot.sendMessage(CHAT_ID, "La citerne est pleine!", "");
    telegramNotificationTankFullSent = true;
    telegramNotificationTankEmptySent = false;
  }

  if (distance >= 10 && distance <= TANK_HEIGHT_IN_CM) {
    tank_volume = (TANK_LENGTH_IN_CM * TANK_WIDTH_IN_CM * waterFilllevel) / 1000;
    tank_percent = (((float)tank_volume / FULL_VOLUME_IN_LITERS) * 100);
  }

  if(tank_percent < 20 && !telegramNotificationTankEmptySent) {
    bot.sendMessage(CHAT_ID, "La citerne est vide!!!", "");
    telegramNotificationTankFullSent = false;
    telegramNotificationTankEmptySent = true;
  }

  String payload = /*String("{\"timestamp\":") + time(nullptr) +*/
                     String("{\"current_volume_in_liters\":") + tank_volume +
                     String(",\"current_volume_in_percent\":") + tank_percent +
                     String(",\"full_volume_in_liters\":") + FULL_VOLUME_IN_LITERS +
                     String(",\"on\":") + "true" +
                     String(",\"tank_height_in_cm\":") + TANK_HEIGHT_IN_CM +
                     String(",\"tank_lenght_in_cm\":") + TANK_LENGTH_IN_CM +
                     String(",\"tank_width_in_cm\":") + TANK_WIDTH_IN_CM +
                     String("}");
  publishTelemetry(payload);
  USE_SERIAL.println("publishTelemetry -> " + payload);
}

String getDefaultSensor()
{
  return  "Wifi: " + String(WiFi.RSSI()) + "db";
}

String getJwt()
{
  iss = time(nullptr);
  USE_SERIAL.println("Refreshing JWT");
  jwt = device->createJWT(iss, jwt_exp_secs);
  return jwt;
}

void setupWifi()
{
  WiFi.mode(WIFI_STA);
  // WiFi.setSleep(false); // May help with disconnect? Seems to have been removed from WiFi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(100);
  }

  configTime(0, 0, ntp_primary, ntp_secondary);
  while (time(nullptr) < 1510644967) 
  {
    delay(10);
  }
}

void connectWifi()
{
  while (WiFi.status() != WL_CONNECTED) 
  {
    //USE_SERIAL.print(".");
    delay(1000);
  }
}

///////////////////////////////
// Orchestrates various methods from preceeding code.
///////////////////////////////
void publishTelemetry(String data)
{
  mqtt->publishTelemetry(data);
}

void publishTelemetry(String subfolder, String data)
{
  mqtt->publishTelemetry(subfolder, data);
}

void connect()
{
  connectWifi();
  mqtt->mqttConnect();
}

void setupCloudIoT()
{
  device = new CloudIoTCoreDevice(project_id, location, registry_id, device_id, private_key_str);

  setupWifi();
  netClient = new WiFiClientSecure();
  mqttClient = new MQTTClient(512);
  mqttClient->setOptions(180, true, 1000); // keepAlive, cleanSession, timeout
  mqtt = new CloudIoTCoreMqtt(mqttClient, netClient, device);
  mqtt->startMQTT();
}
