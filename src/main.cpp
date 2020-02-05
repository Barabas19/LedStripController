#define DDEBUG   //If you comment this line, the DPRINT & DPRINTLN lines are defined as blank.
#ifdef DDEBUG    //Macros are usually in all capital letters.
  #define DPRINT(...)    Serial.print(__VA_ARGS__)     //DPRINT is a macro, debug print
  #define DPRINTLN(...)  Serial.println(__VA_ARGS__)   //DPRINTLN is a macro, debug print with new line
  #define DPRINTF(...)   Serial.printf(__VA_ARGS__)
#else
  #define DPRINT(...)     //now defines a blank line
  #define DPRINTLN(...)   //now defines a blank line
  #define DPRINTF(...)
#endif

#include <Arduino.h>
#include <GyverEncoder.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <Time.h>
#include <NTPClient.h>

#define CLK       13
#define DT        12
#define SW        14
#define MOSFET    4
#define INCREMENT 51

#define SUNRISE_VALUE   50        // %, at this value sunrise is finished
#define SUNRISE_SPEED   50.0/1    // %/s, in this time light value reaches the sunrise value


const char* otaHostName = "WorkspaceLedStrip";
const char* otaPassword = "esp1901";
const char* ssid = "SkyNET";
const char* password = "18Kuskov!";

const long utcOffsetInSeconds = 3600;
char daysOfTheWeek[7][12] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

Encoder enc1(CLK, DT, SW);
int currentValue = 0, oldValue = 0, storedValue = 0;
ulong sunriseTime = 0;
int sunriseValueRel = SUNRISE_VALUE;
float sunriseSpeedRel = SUNRISE_SPEED;
bool sunrise, startSunrise;

void OTAini()
{
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(otaHostName);
  ArduinoOTA.setPassword(otaPassword);

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    DPRINTLN("Start updating " + type);
  });
   ArduinoOTA.onEnd([]() {
    DPRINTLN("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    DPRINTF("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    DPRINTF("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) DPRINTLN("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) DPRINTLN("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) DPRINTLN("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) DPRINTLN("Receive Failed");
    else if (error == OTA_END_ERROR) DPRINTLN("End Failed");
  });
  ArduinoOTA.begin();

}

time_t timeSyncNTP()
{
  timeClient.update();
  DPRINTLN(timeClient.getFormattedTime());
  return timeClient.getEpochTime();
}

bool sunRiseHandle(bool _sunRise, int& lightValue)
{
  int newValue;
  if(!_sunRise)
    return false;
  auto now = millis();
  if(sunriseTime < now)
    return true;
  if(sunriseValueRel * 1024 / 100 > lightValue)
  {
    newValue = (sunriseValueRel - (sunriseTime - now) * sunriseSpeedRel / 1000) * 1024 / 100;
    lightValue = max(lightValue, newValue);
  }
  else
  {
    newValue = (sunriseValueRel + (sunriseTime - now) * sunriseSpeedRel / 1000) * 1024 / 100;
    lightValue = min(lightValue, newValue);
  }
  lightValue = max(min(lightValue, 1024), 0);
  return false;  
}

void setup() 
{
  Serial.begin(115200);
  DPRINTLN("Booting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    DPRINTLN("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
  OTAini();
  
  DPRINTLN("Ready");
  DPRINTLN("IP address: ");
  DPRINTLN(WiFi.localIP());

  enc1.setTickMode(AUTO);
  pinMode(MOSFET, OUTPUT);

  timeClient.begin();
  timeSyncNTP();
  setSyncProvider(timeSyncNTP);
  setSyncInterval(10);
  startSunrise = true;
  sunriseSpeedRel = 100.0;
  sunriseValueRel = 100;
  sunriseTime = millis() + 1000;
}

void loop() {
  // OTA
  ArduinoOTA.handle();

  if(startSunrise)
  {
    if(sunRiseHandle(startSunrise, currentValue))
      startSunrise = false;
  }


  

  if (enc1.isRight())
    currentValue += INCREMENT;

  if (enc1.isLeft())
    currentValue -= INCREMENT;

  if(enc1.isClick())
  {
    storedValue = currentValue > 0 ? currentValue : 0;
    currentValue = currentValue > 0 ? 0 : (storedValue > 0 ? storedValue : (1024 / 2));
  }

  currentValue = max(min(currentValue, 1023), 0);

  if(currentValue != oldValue)
  {
    analogWrite(MOSFET, currentValue);
    DPRINTLN("New value = " + String(currentValue));
  }

  oldValue = currentValue;


}

