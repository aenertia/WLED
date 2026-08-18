#ifdef ESP8266
  #include <ESP8266WiFi.h>
#else // ESP32
  #include <WiFi.h>
  #ifndef WLED_USE_SLIP
  #include <ETH.h>
  #endif
#endif

#ifndef Network_h
#define Network_h

class WLEDNetworkClass
{
public:
  IPAddress localIP();
  IPAddress subnetMask();
  IPAddress gatewayIP();
  void localMAC(uint8_t* MAC);
  bool isConnected();
  bool isEthernet();
#ifdef WLED_USE_PPP
  bool isPPP();
#endif
};

extern WLEDNetworkClass WLEDNetwork;

#endif