//#define DDEBUG   //If you comment this line, the DPRINT & DPRINTLN lines are defined as blank.
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
#include <WiFiClient.h>
#include <TM1637Display.h>

#define CLK_PIN1      3  // RX  WH
#define DT_PIN1       5  // D1  GN
#define SW_PIN1       2  // D4  BR
#define MOSFET_PIN1   15 // D8

#define CLK_PIN2      13 // D7  YE
#define DT_PIN2       12 // D6  BU
#define SW_PIN2       14 // D5  OR
#define MOSFET_PIN2   4  // D2  

#define DISP_CLK_PIN  0  // D3
#define DISP_DIO_PIN  1  // TX

#define INCREMENT     5     // %
#define MANUAL_SPEED  50    // %/s
#define STARTUP_SPEED 100
#define STARTUP_VALUE 50
#define SUNRISE_DURA  1800  // s
#define ALARM_TIME_PROVIDER_URL   "http://www.dd.9e.cz/php/requests/get_alarm_time.php"
#define SUNRISE_PROVIDER_URL      "http://api.sunrise-sunset.org/json?lat=49.7447811&lng=13.3764689"
#define WEATHER_PROVIDER_URL      "http://api.openweathermap.org/data/2.5/weather?q=Plzen&units=metric&appid=2340d4e1dea5f52590c8421f9b472f93"

const char* otaHostName = "WorkspaceLedStrip";
const char* otaPassword = "esp1901";
const char* ssid        = "SkyNET";
const char* password    = "18Kuskov!";

const long utcOffsetInSeconds = 3600;
WiFiUDP ntpUDP;
WiFiClient client;  // !!!!!!! Must be a global variable
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
StaticJsonDocument<1024> alarmJsonDoc, sunriseJsonDoc, weaterJsonDoc;

bool use_display = true;
TM1637Display display(DISP_CLK_PIN, DISP_DIO_PIN);
uint8_t display_data[] = { 0xff, 0xff, 0xff, 0xff };
int sec_buff = 0;
bool show_dot = false;

ulong alarmTime;
double sunriseTargetVal;
uint sunriseDuration = SUNRISE_DURA; // s
bool sunriseStripEna[2];             // if true, the strip is enabled for sunrise
bool alarmTimeReq, sunriseEnable, sunriseEnabled, doubleClick, twilightReq, weatherReq;
int sunrise_begin, sunrise_end, sunset_begin, sunset_end;

int temperature;

struct Strip
{
  Encoder enc;
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
}

void stripEncoderHandle(Strip& _strip)
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
      _strip.targetVal = _strip.targetVal + INCREMENT;
    change = true;
  }

  if (isLeft && !_strip.startUp)
  {
    if(_strip.speed == -MANUAL_SPEED)
      _strip.targetVal -= INCREMENT;
    else
      _strip.targetVal = _strip.targetVal - INCREMENT;
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

  if(isRight)
    DPRINTF("%s is right.\n", _strip.name);
  if(isLeft)
    DPRINTF("%s is left.\n", _strip.name);
  if(isSingleClick)
    DPRINTF("%s is click.\n", _strip.name);
  if(change)
    DPRINTF("%s speed = %.2f, target_value = %.2f.\n", _strip.name, _strip.speed, _strip.targetVal);
  
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
    // DPRINTF("%s strip: new value = %.2f\n",_strip.name, _strip.currentVal);
  }

  _strip.oldVal = _strip.currentVal;
  stripEncoderHandle(_strip);
}

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
    #ifdef DDEBUG
      Serial.end();
    #endif
  });
   ArduinoOTA.onEnd([]() {
    #ifdef DDEBUG
      Serial.begin(115200);
      DPRINTLN("\nEnd");
    #endif
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

String GetHttp(String _url)
{
  String payload = "";
  DPRINTF("*** Requested url:\n%s\n", _url.c_str());
  HTTPClient http;
  if(http.begin(client, _url))
  {
    if(http.connected())
      DPRINTLN("http connected.");
    delay(500);
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
  return payload;
}

void UpdateSunriseParams()
{
  String payload = GetHttp(ALARM_TIME_PROVIDER_URL);
  if(payload != "")
  {
    // payload example: {"alarm":1581119220,"duration":300,"value":100.0,"strip":{"upper":true,"lower":true}}
    DeserializationError error = deserializeJson(alarmJsonDoc, payload);
    if (error) {
      DPRINTF("deserializeJson() failed: %s\n", error.c_str());
      return;
    }
    alarmTime         = alarmJsonDoc["alarm"];
    sunriseDuration   = alarmJsonDoc["duration"];
    sunriseTargetVal  = alarmJsonDoc["value"];
    sunriseStripEna[0]= alarmJsonDoc["strip"]["upper"];
    sunriseStripEna[1]= alarmJsonDoc["strip"]["lower"];
    DPRINTF("Json deserialization:\n alarm=%lu\n duration=%d\n value=%.2f\n upper=%d\n lower=%d\n", alarmTime, sunriseDuration, sunriseTargetVal, sunriseStripEna[0], sunriseStripEna[1]);
  }  
}

void DisplayHandle()
{
  auto seconds = timeClient.getSeconds();
  auto minutes = timeClient.getMinutes();
  auto hours   = timeClient.getHours();
  auto time = hours * 100 + minutes;
  
  if(seconds % 10 < 7) // in every 0-6 second show time, else show temperature
  {
    if(seconds != sec_buff)
    {
      show_dot = !show_dot;
    }
    sec_buff = seconds;

    display.showNumberDecEx(time, show_dot ? 0b01000000 : 0b00000000, time < 100, time < 100 ? 3 : 4, 0);
  }
  else
  {
    String temp = String(temperature) + "C";
    uint8_t data[] = { 0x00, 0x00, 0x00, 0x00 };
    uint8_t minus = 0b01000000; // '-'
    uint8_t C = 0b00111001; // 'C'
    if(temperature >= 10 || temperature <= -10)
      data[1] = temperature > 0 ? display.encodeDigit(temperature / 10) : display.encodeDigit(temperature / -10);
    data[2] = temperature > 0 ? display.encodeDigit(temperature % 10) : display.encodeDigit(temperature % -10);
    data[3] = C;
    if(temperature < 0)
      data[temperature <= -10 ? 0 : 1] = minus;
    display.setSegments(data);    
  }

  uint8_t brightness = 1;
  if((time > sunrise_end && time < sunset_begin) || strip[0].currentVal > 10 || strip[1].currentVal > 10)
  {
    brightness = 7;
  }
  else if(time > sunrise_begin && time < sunrise_end)
  {
    auto sunrise_length = sunrise_end - sunrise_begin;
    brightness = (time - sunrise_begin) / (sunrise_length / 7);
  }
  else if(time > sunset_begin && time < sunset_end)
  {
    auto sunset_length = sunset_end - sunset_begin;
    brightness = (time - sunset_begin) / (sunset_length / 7);
  }
  brightness = max(min(brightness, (uint8_t)7), (uint8_t)0);

  display.setBrightness(brightness);
}

void UpdateTwilight()
{
  String payload = GetHttp(SUNRISE_PROVIDER_URL);
  if(payload != "")
  {
    // payload example: { "results":{"sunrise":"5:32:38 AM","sunset":"5:01:25 PM","solar_noon":"11:17:01 AM","day_length":"11:28:47",
    //                    "civil_twilight_begin":"5:00:42 AM","civil_twilight_end":"5:33:20 PM",
    //                    "nautical_twilight_begin":"4:23:27 AM","nautical_twilight_end":"6:10:35 PM",
    //                    "astronomical_twilight_begin":"3:45:34 AM","astronomical_twilight_end":"6:48:28 PM"},"status":"OK"}
    DeserializationError error = deserializeJson(sunriseJsonDoc, payload);
    if (error) {
      DPRINTF("deserializeJson() failed: %s\n", error.c_str());
      return;
    }
    String twilight_begin = sunriseJsonDoc["results"]["civil_twilight_begin"];
    String sunrise        = sunriseJsonDoc["results"]["sunrise"];
    String sunset         = sunriseJsonDoc["results"]["sunset"];
    String twilight_end   = sunriseJsonDoc["results"]["civil_twilight_end"];

    DPRINTF("Json deserialization:\n twilight_begin=%s\n sunrise=%s\n sunset=%s\n twilight_end=%s\n", 
      twilight_begin.c_str(), sunrise.c_str(), sunset.c_str(), twilight_end.c_str());

    sunrise_begin   = twilight_begin.substring(0,1).toInt() * 100 + twilight_begin.substring(2,4).toInt();
    sunrise_end     = sunrise.substring(0,1).toInt() * 100 + sunrise.substring(2,4).toInt();
    if(sunset.length() > 10)
      sunset_begin = sunset.substring(0,2).toInt() * 100 + sunset.substring(3,5).toInt();
    else
      sunset_begin = sunset.substring(0,1).toInt() * 100 + sunset.substring(2,4).toInt();
    if(twilight_end.length() > 10)
      sunset_end = twilight_end.substring(0,2).toInt() * 100 + twilight_end.substring(3,5).toInt();
    else
      sunset_end = twilight_end.substring(0,1).toInt() * 100 + twilight_end.substring(2,4).toInt();

    sunset_begin += 1200;
    sunset_end += 1200;

    // Correct time zone
    sunset_begin += 100;
    sunset_end += 100;
    sunrise_begin += 100;
    sunrise_end += 100;
    
    DPRINTF("Times convertion:\n sunrise_begin=%d\n sunrise_end=%d\n sunset_begin=%d\n sunset_end=%d\n", 
      sunrise_begin, sunrise_end, sunset_begin, sunset_end);
  } 
}

void UpdateCurrentTemperature()
{
  String payload = GetHttp(WEATHER_PROVIDER_URL);
  if(payload != "")
  {
    // payload example: {"coord":{"lon":13.38,"lat":49.75},"weather":[{"id":800,"main":"Clear","description":"clear sky","icon":"01d"}],
    //                    "base":"stations","main":{"temp":12.11,"feels_like":4.58,"temp_min":12,"temp_max":12.22,"pressure":1019,"humidity":40},
    //                    "visibility":10000,"wind":{"speed":7.7,"deg":110},"clouds":{"all":7},"dt":1585304554,"sys":{"type":1,"id":6839,
    //                    "country":"CZ","sunrise":1585284805,"sunset":1585330192},"timezone":3600,"id":3068160,"name":"Pilsen","cod":200}
    DeserializationError error = deserializeJson(weaterJsonDoc, payload);
    if (error) {
      DPRINTF("deserializeJson() failed: %s\n", error.c_str());
      return;
    }
    String temp = weaterJsonDoc["main"]["temp"];
    temperature = (int)temp.toFloat();
    DPRINTF("Json deserialization:\n alarm=%lu\n duration=%d\n value=%.2f\n upper=%d\n lower=%d\n", alarmTime, sunriseDuration, sunriseTargetVal, sunriseStripEna[0], sunriseStripEna[1]);
  }  
}

void setup() 
{
  #ifdef DDEBUG
    Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
    use_display = false;
  #endif
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
  DPRINTLN("done");

  timeClient.begin();
  timeClient.update();
  InitializeStrips();
  UpdateSunriseParams();
  UpdateTwilight();
  UpdateCurrentTemperature();

  if(use_display)
  {
    display.setBrightness(0x0f);
    display.setSegments(display_data);
  }
}

void loop() 
{
  ArduinoOTA.handle(); // OTA

  stripValueHandle(strip[0]);
  stripValueHandle(strip[1]);
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
    UpdateSunriseParams();
    sunriseHandle(timeClient.getEpochTime() - 3600, alarmTime);
    alarmTimeReq = false;
  }

  if(timeClient.getMinutes() % 10 == 0)
    weatherReq = true;
  else if(weatherReq)
  {
    UpdateCurrentTemperature();
    weatherReq = false;
  }

  if(timeClient.getMinutes() == 0)
    twilightReq = true;
  else if(twilightReq)
  {
    UpdateTwilight();
    twilightReq = false;
  }

  if(use_display)
    DisplayHandle();
}

