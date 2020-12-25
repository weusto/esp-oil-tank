#include <Arduino.h>
#include <ArduinoJson.h>

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
#include "google-cloud-iot-arduino/universal-mqtt.h"
#include <UniversalTelegramBot.h>

#define USE_SERIAL Serial

#define CURRENT_VERSION VERSION
#define CLOUD_FUNCTION_URL "https://us-central1-oil-tank-298920.cloudfunctions.net/getDownloadUrl"

WiFiClient client;
#if defined(ESP8266)
ESP8266WebServer server(80);
#else
WebServer server(80);
#endif

// Périodes
#define PERIODE_ACQUISITION 500     //!< période d'acquisition en millisecondes pour la sonde
#define PERIODE_ENVOI       60000    //!< période d'envoi des données en millisecondes pour MQTT
unsigned long lastMillis = 0;
int ledState = LOW;

float TANK_HEIGHT_IN_CM = 0;
float TANK_LENGTH_IN_CM = 0;
float TANK_WIDTH_IN_CM = 0;
float FULL_VOLUME_IN_LITERS = 1;
String UNIT;

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

  USE_SERIAL.print("IP address: ");
  USE_SERIAL.println(WiFi.localIP());

  setupCloudIoT();

  pinMode(trigPin, OUTPUT); // set the trigger pin as output
  pinMode(echoPin, INPUT);  // set the echo pin as input
}



void loop() {
  mqtt->loop();
  delay(10);  // <- fixes some issues with WiFi stability
  if (!mqttClient->connected()) {
    connect();
  }

  if (millis() - lastMillis > PERIODE_ENVOI)
  {
    lastMillis = millis();
    ledState = ledState == LOW ? HIGH : LOW;
    digitalWrite( BUILTIN_LED, ledState );

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
