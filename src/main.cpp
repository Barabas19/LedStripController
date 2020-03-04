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
// #include <GyverEncoder.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <NTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClient.h>
#include <Adafruit_MCP23017.h>
#include <TM1637.h>
#include "MCP_Encoder.h"

// first encoder
#define CLK_PIN1      13 // D7  YE
#define DT_PIN1       12 // D6  BU
#define SW_PIN1       14 // D5  OR
#define MOSFET_PIN1   4  // D2  

// second encoder
#define CLK_PIN2      3  // RX  WH
#define DT_PIN2       5  // D1  GN
#define SW_PIN2       10 // SD3 BR
#define MOSFET_PIN2   15 // D8

// display
#define CLK_PIN_DISP  8
#define DIO_PIN_DISP  9

#define INCREMENT     5     // %
#define MANUAL_SPEED  50    // %/s
#define STARTUP_SPEED 100
#define STARTUP_VALUE 50
#define SUNRISE_DURA  1800  // s
#define ALARM_TIME_PROVIDER_URL "http://www.dd.9e.cz/php/requests/get_alarm_time.php"

const char* otaHostName = "WorkspaceLedStrip";
const char* otaPasotaPasSW_PIN1ord = "esp1901";
const char* ssid = "SkyNET";
const char* password = "18Kuskov!";

const long utcOffsetInSeconds = 3600;
WiFiUDP ntpUDP;
HTTPClient http;    // !!!!!!! Must be a global variable
WiFiClient client;  // !!!!!!! Must be a global variable
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
StaticJsonDocument<1024> doc;
Adafruit_MCP23017 mcp;
TM1637 display(CLK_PIN_DISP, DIO_PIN_DISP);
int8_t digits[] = {0, 1, 2, 3};
bool showSecondDots = false;
int buff_seconds;

ulong alarmTime;
double sunriseTargetVal;
uint sunriseDuration = SUNRISE_DURA; // s
bool sunriseStripEna[2];             // if true, the strip is enabled for sunrise
bool alarmTimeReq, sunriseEnable, sunriseEnabled, doubleClick;

struct Strip
{
  MCP_Encoder enc;
  ulong timeBuff;
  int mosfetPin;
  char name[5];
  double currentVal = 0.0;
  double targetVal = STARTUP_VALUE;
  double storedVal = 0.0;
  double oldVal = 0.0;
  double speed = STARTUP_SPEED;
  bool startUp = true;
  bool encoderDoubleClicked = false;
} strip[2];

void InitializeStrips()
{
  strip[0].enc = MCP_Encoder(mcp, CLK_PIN1, DT_PIN1, SW_PIN1);
  strip[1].enc = MCP_Encoder(mcp, CLK_PIN2, DT_PIN2, SW_PIN2);
  strip[0].enc.setTickMode(AUTO);
  strip[1].enc.setTickMode(AUTO);

  strip[0].timeBuff = millis();
  strip[1].timeBuff = millis();

  strip[0].mosfetPin = MOSFET_PIN1;
  strip[1].mosfetPin = MOSFET_PIN2;
  pinMode(strip[0].mosfetPin, OUTPUT);
  pinMode(strip[1].mosfetPin, OUTPUT);

  strcpy(strip[0].name, "upper");
  strcpy(strip[1].name, "lower");
  DPRINTF("%s and %s strips initialized\n", strip[0].name, strip[1].name);
}

void StripEncoderHandle(Strip& _strip)
{  
  auto isRight = _strip.enc.isRight();
  auto isLeft = _strip.enc.isLeft();
  auto isSingleClick = _strip.enc.isSingle();
  auto isDoubleClick = _strip.enc.isDouble();
  
  auto change = false;
  if (isRight && !_strip.startUp)
  {
    if(_strip.speed == MANUAL_SPEED)
      _strip.targetVal += INCREMENT;
    else
      _strip.targetVal = _strip.currentVal + INCREMENT;
    change = true;
  }

  if (isLeft && !_strip.startUp)
  {
    if(_strip.speed == -MANUAL_SPEED)
      _strip.targetVal -= INCREMENT;
    else
      _strip.targetVal = _strip.currentVal - INCREMENT;
    change = true;
  }

  if((isSingleClick || _strip.encoderDoubleClicked) && !_strip.startUp)
  {
    DPRINTF("click\n stored value = %.2f\n current value = %.2f\n", _strip.storedVal, _strip.currentVal);
    auto storedValBuff = _strip.storedVal;
    _strip.storedVal = _strip.currentVal > 0 ? _strip.currentVal : 0;
    _strip.targetVal = _strip.currentVal > 0 ? 0 : (storedValBuff > 0 ? storedValBuff : (100 / 2));
    change = true;
    _strip.encoderDoubleClicked = false;
  }
  
  if(isDoubleClick)
    doubleClick = true;

  if(change)
    _strip.speed = MANUAL_SPEED * (_strip.targetVal > _strip.currentVal ? 1 : -1);

  _strip.targetVal = max(min(_strip.targetVal, 100.0), 0.0);
}

void StripValueHandle(Strip& _strip)
{
  auto now = millis();
  auto cycleTime = now - _strip.timeBuff;
  _strip.timeBuff = now;

  auto valueReached = (_strip.speed > 0 && _strip.currentVal > _strip.targetVal) 
    || (_strip.speed < 0 && _strip.currentVal < _strip.targetVal)
    || _strip.currentVal == _strip.targetVal;

  if(valueReached)
  {
    _strip.speed = 0;
    if(_strip.startUp && _strip.targetVal)
    {
      _strip.speed = -STARTUP_SPEED;
      _strip.targetVal = 0.0;
    }
    else if(_strip.startUp && !_strip.targetVal)
    {
      _strip.startUp = false;
    }
  }
  
  if(_strip.speed != 0)
  {
    auto newVal = _strip.currentVal + _strip.speed * cycleTime / 1000;
    _strip.currentVal = max(min(newVal, 100.0), 0.0);
  }

  if(_strip.currentVal != _strip.oldVal)
  {
    analogWrite(_strip.mosfetPin, _strip.currentVal * 1024 / 100);
    //DPRINTF("%s strip: new value = %.2f\n",_strip.name, _strip.currentVal);
  }

  _strip.oldVal = _strip.currentVal;
  StripEncoderHandle(_strip);
}

void OTAini()
{
  ArduinoOTA.setPort(8266);
  ArduinoOTA.setHostname(otaHostName);
  ArduinoOTA.setPassword(otaPasotaPasSW_PIN1ord);

  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    DPRINTLN("Start updating " + type);
    Serial.end();
  });
   ArduinoOTA.onEnd([]() {
    Serial.begin(115200);
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

void SunriseHandle(ulong _now, ulong _alarmTime)
{
  sunriseEnable = _now <= _alarmTime && _now >= _alarmTime - sunriseDuration;

  if(sunriseEnable && !sunriseEnabled)
  {
    for(int i=0; i<2; i++)
    {
      if(sunriseStripEna[i])
      {
        strip[i].speed = sunriseTargetVal / sunriseDuration;;
        strip[i].targetVal = sunriseTargetVal;
      }
    }
  }
  sunriseEnabled = sunriseEnable;
}

void GetSunriseParams()
{
  
  String payload = "";
  String url = ALARM_TIME_PROVIDER_URL;
  DPRINTF("*** Requested url:\n%s\n", url.c_str());
  if(http.begin(client, url))
  {
    auto httpCode = http.GET();
    DPRINTF("[HTTP] GET... code: %d\n", httpCode);
    
    if (httpCode > 0) 
    {
      DPRINTF("[HTTP] GET... code: %d\n", httpCode);
      
      // file found at server
      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) 
      {
        payload = http.getString();
        DPRINTLN(payload);
      }
    } 
    else 
    {
      DPRINTF("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  } 
  else 
  {
    DPRINTF("[HTTP} Unable to connect\n");
  }
  
  if(payload != "")
  {
    // payload example: {"alarm":1581119220,"duration":300,"value":100.0,"strip":{"upper":true,"lower":true}}
    DeserializationError error = deserializeJson(doc, payload);
    if (error) {
      DPRINTF("deserializeJson() failed: %s\n", error.c_str());
      return;
    }
    alarmTime         = doc["alarm"];
    sunriseDuration   = doc["duration"];
    sunriseTargetVal  = doc["value"];
    sunriseStripEna[0]= doc["strip"]["upper"];
    sunriseStripEna[1]= doc["strip"]["lower"];
    DPRINTF("Json deserialization:\n alarm=%lu\n duration=%d\n value=%.2f\n upper=%d\n lower=%d\n", alarmTime, sunriseDuration, sunriseTargetVal, sunriseStripEna[0], sunriseStripEna[1]);
  }  
}

void DisplayHandle()
{
  auto hours = timeClient.getHours();
  auto minutes = timeClient.getMinutes();
  auto seconds = timeClient.getSeconds();
  digits[0] = minutes % 10;
  digits[1] = minutes / 10;
  digits[2] = hours % 10;
  digits[3] = hours / 10;

  if(buff_seconds != seconds)
  {
    showSecondDots = !showSecondDots;
    display.point(showSecondDots);
  }
  buff_seconds = seconds;

  display.display(digits);
}

void setup() 
{
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  DPRINTLN("\nBooting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) 
  {
    DPRINTLN("Connection Failed! Rebooting...");
    delay(5000);
    ESP.restart();
  }
  OTAini();
  
  DPRINTLN("Ready");
  DPRINTLN("IP address: ");
  DPRINTLN(WiFi.localIP());
  // DPRINTLN("done");

  timeClient.begin();
  timeClient.update();
  InitializeStrips();

  display.set();
  display.init();
}

void loop() 
{
  ArduinoOTA.handle(); // OTA

  StripValueHandle(strip[0]);
  StripValueHandle(strip[1]);
  DisplayHandle();
  if(doubleClick)
  {
    strip[0].encoderDoubleClicked = true;
    strip[1].encoderDoubleClicked = true;
    doubleClick = false;
  }

  if(timeClient.getSeconds() == 0)
    alarmTimeReq = true;
  else if(alarmTimeReq)
  {
    timeClient.update();
    DPRINTF("Formated time: %s\n", timeClient.getFormattedTime().c_str());
    DPRINTF("Epoch time: %lu\n", timeClient.getEpochTime() - 3600);
    GetSunriseParams();
    SunriseHandle(timeClient.getEpochTime() - 3600, alarmTime);
    alarmTimeReq = false;
  }
}

