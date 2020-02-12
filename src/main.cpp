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
#include <NTPClient.h>
#include <ArduinoJson.h>

#define CLK_PIN1      13
#define DT_PIN1       12
#define SW_PIN1       14
#define MOSFET_PIN1   4

#define CLK_PIN2      2
#define DT_PIN2       9
#define SW_PIN2       10
#define MOSFET_PIN2   15

#define INCREMENT     5     // %
#define MANUAL_SPEED  50    // %/s
#define STARTUP_SPEED 100
#define SUNRISE_DURA  1800  // s
#define ALARM_TIME_PROVIDER_URL "http://www.dd.9e.cz/php/requests/get_alarm_time.php"

const char* otaHostName = "WorkspaceLedStrip";
const char* otaPasotaPasSW_PIN1ord = "esp1901";
const char* ssid = "SkyNET";
const char* password = "18Kuskov!";

const long utcOffsetInSeconds = 3600;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);

ulong alarmTime;
double sunriseTargetVal;
uint sunriseDuration = SUNRISE_DURA; // s
bool sunriseStripEna[2];             // if true, the strip is enabled for sunrise
bool alarmTimeReq, sunriseEnable, sunriseEnabled;

struct Strip
{
  Encoder enc;
  ulong timeBuff;
  int mosfetPin;
  char name[5];
  double currentVal = 0.0;
  double targetVal = 100.0;
  double storedVal = 0.0;
  double oldVal = 0.0;
  double speed = STARTUP_SPEED;
  bool startUp = true;
} strip[2];

void InitializeStrips()
{
  strip[0].enc = Encoder(CLK_PIN1, DT_PIN1, SW_PIN1);
  strip[1].enc = Encoder(CLK_PIN2, DT_PIN2, SW_PIN2);
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
  DPRINTF("%s strip speed value: %.2f\n", strip[0].name, strip[0].speed);
}

void stripEncoderHandle(Strip& _strip)
{  
  auto change = false;
  if (_strip.enc.isRight() && !_strip.startUp)
  {
    if(_strip.speed == MANUAL_SPEED)
      _strip.targetVal += INCREMENT;
    else
      _strip.targetVal = _strip.currentVal + INCREMENT;
    change = true;
  }

  if (_strip.enc.isLeft() && !_strip.startUp)
  {
    if(_strip.speed == -MANUAL_SPEED)
      _strip.targetVal -= INCREMENT;
    else
      _strip.targetVal = _strip.currentVal - INCREMENT;
    change = true;
  }

  if(_strip.enc.isClick() && !_strip.startUp)
  {
    auto storedValBuff = _strip.storedVal;
    _strip.storedVal = _strip.currentVal > 0 ? _strip.currentVal : 0;
    _strip.targetVal = _strip.currentVal > 0 ? 0 : (storedValBuff > 0 ? storedValBuff : (100 / 2));
    change = true;
  }

  if(change)
    _strip.speed = MANUAL_SPEED * (_strip.targetVal > _strip.currentVal ? 1 : -1);

  _strip.targetVal = max(min(_strip.targetVal, 100.0), 0.0);
}

void stripValueHandle(Strip& _strip)
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
    DPRINTF("%s strip: new value = %.2f\n",_strip.name, _strip.currentVal);
  }

  _strip.oldVal = _strip.currentVal;
  stripEncoderHandle(_strip);
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

void sunriseHandle(ulong _now, ulong _alarmTime)
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

void GetSunriseParams(ulong& _alarmTime, uint& _duration, double& _value, bool& _enable1, bool& _enable2)
{
  HTTPClient http;
  WiFiClient client;
  String url = ALARM_TIME_PROVIDER_URL;
  DPRINTF("*** Requested url:\n%s\n", url.c_str());
  http.begin(client, url);
  auto httpCode = http.GET();
  if(httpCode == HTTP_CODE_OK)
  {
    auto payload = http.getString().c_str();
    DPRINTF("*** Payload:\n%s\n", payload);
    // payload example: {"alarm":1581119220,"duration":300,"value":100.0,"strip":[1,1]}
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, payload);
    _alarmTime        = doc["alarm"];
    _duration         = doc["duration"];
    _value            = doc["value"];
    _enable1          = doc["strip"][0];
    _enable2          = doc["strip"][1];
  }
  else
  {
    DPRINTF("Failed to get alarm time\n");
  }
  http.end();  
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
return
  timeClient.begin();
  timeClient.update();
  InitializeStrips();
}

void loop() 
{
  ArduinoOTA.handle(); // OTA
return;
  stripValueHandle(strip[0]);
  stripValueHandle(strip[1]);
  if(timeClient.getSeconds() == 0)
    alarmTimeReq = true;
  else if(alarmTimeReq)
  {
    timeClient.update();
    DPRINTLN(timeClient.getFormattedTime());
    GetSunriseParams(alarmTime, sunriseDuration, sunriseTargetVal, sunriseStripEna[0], sunriseStripEna[1]);
    sunriseHandle(timeClient.getEpochTime(), alarmTime);
    alarmTimeReq = false;
  }
}

