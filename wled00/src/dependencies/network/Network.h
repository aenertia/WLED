#ifdef ESP8266
  #include <ESP8266WiFi.h>
#elif defined(WLED_USE_SLIP)
  #include <Arduino.h>
  #include <esp_mac.h>
#else // ESP32
  #include <WiFi.h>
  #include <ETH.h>
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