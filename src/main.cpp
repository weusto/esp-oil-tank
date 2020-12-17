#include <Arduino.h>

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

#define USE_SERIAL Serial

#define CURRENT_VERSION VERSION
#define CLOUD_FUNCTION_URL "https://us-central1-oil-tank-298920.cloudfunctions.net/getDownloadUrl"

WiFiClient client;
#if defined(ESP8266)
ESP8266WebServer server(80);
#else
WebServer server(80);
#endif

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
float distance;           // variable to store the distance as float

/* 
 * Show current device version
 */
void handleRoot() {  
  server.send(200, "text/plain", "v" + String(CURRENT_VERSION));
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);  
  pinMode(LED_BUILTIN, OUTPUT);

  delay(3000);
  Serial.println("\n Starting");
  // Setup Wifi Manager
  String version = String("<p>Current Version - v") + String(CURRENT_VERSION) + String("</p>");
  USE_SERIAL.println(version);
  
  WiFiManager wm;
  WiFiManagerParameter versionText(version.c_str());
  wm.addParameter(&versionText);    
    
  if (!wm.autoConnect()) {
    Serial.println("failed to connect and hit timeout");
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

  pinMode(trigPin, OUTPUT);             // set the trigger pin as output
  pinMode(echoPin, INPUT);               // set the echo pin as input
}

void loop() {
  digitalWrite(trigPin, LOW);            // set the trigPin to LOW
  delayMicroseconds(2);                  // wait 2ms to make sure the trigPin is LOW

  digitalWrite(trigPin, HIGH);           // set the trigPin to HIGH to start sending ultrasonic sound
  delayMicroseconds(10);                 // pause 10ms
  digitalWrite(trigPin, LOW);            // set the trigPin to LOW to stop sending ultrasonic sound

  duration = pulseIn(echoPin, HIGH);     // request how long the echoPin has been HIGH
  distance = (duration * 0.0343) / 2;    // calculate the distance based on the speed of sound
                                         // we need to divide by 2 since the sound travelled the distance twice
  USE_SERIAL.println("Distance #: " + String(distance));           // Print the result to the serial monitor

  delay(100);                            // pause 100ms till the next measurement

  // Just chill
  server.handleClient();
}