
#include <WiFiUdp.h>
#ifndef DEBUG_UDP_SERVER
  #define DEBUG_UDP_SERVER "baradeb"
#endif
#ifndef DEBUG_UDP_PORT
  #define DEBUG_UDP_PORT 23
#endif
#if defined(DEBUG_PORT)
  #define DPRINT(...)    DEBUG_PORT.print(__VA_ARGS__)
  #define DPRINTLN(...)  DEBUG_PORT.println(__VA_ARGS__)
  #define DPRINTF(...)   DEBUG_PORT.printf(__VA_ARGS__)
#elif defined(DEBUG_UDP)
  #define DPRINT(...);    {DEBUG_UDP.beginPacket(DEBUG_UDP_SERVER,DEBUG_UDP_PORT);DEBUG_UDP.print(__VA_ARGS__);DEBUG_UDP.endPacket();}
  #define DPRINTLN(...);  {DEBUG_UDP.beginPacket(DEBUG_UDP_SERVER,DEBUG_UDP_PORT);DEBUG_UDP.println(__VA_ARGS__);DEBUG_UDP.endPacket();}
  #define DPRINTF(...);   {DEBUG_UDP.beginPacket(DEBUG_UDP_SERVER,DEBUG_UDP_PORT);DEBUG_UDP.printf(__VA_ARGS__);DEBUG_UDP.endPacket();}
  WiFiUDP DEBUG_UDP;
#else
  #define DPRINT(...)
  #define DPRINTLN(...)
  #define DPRINTF(...)
#endif