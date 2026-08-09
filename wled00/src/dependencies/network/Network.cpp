#include "Network.h"
#ifdef WLED_USE_PPP
#include "wled_ppp.h"
#endif
#ifdef WLED_USE_SLIP
#include "wled_slip.h"
#endif

IPAddress WLEDNetworkClass::localIP()
{
  IPAddress localIP;
#ifdef WLED_USE_PPP
  if (isPPP() && ppp_netif) {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(ppp_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
      return IPAddress(ip_info.ip.addr);
    }
  }
#endif
#ifdef WLED_USE_SLIP
  if (slip_connected) {
    IPAddress slipIP;
    slipIP.fromString(SLIP_OUR_IP);
    return slipIP;
  }
#endif
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  localIP = ETH.localIP();
  if (localIP[0] != 0) {
    return localIP;
  }
#endif
#if !defined(WLED_USE_SLIP)
  localIP = WiFi.localIP();
  if (localIP[0] != 0) {
    return localIP;
  }
#endif
  return INADDR_NONE;
}

IPAddress WLEDNetworkClass::subnetMask()
{
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  if (ETH.localIP()[0] != 0) {
    return ETH.subnetMask();
  }
#endif
#ifdef WLED_USE_SLIP
  return IPAddress(255, 255, 255, 0);
#else
  if (WiFi.localIP()[0] != 0) {
    return WiFi.subnetMask();
  }
  return IPAddress(255, 255, 255, 0);
#endif
}

IPAddress WLEDNetworkClass::gatewayIP()
{
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  if (ETH.localIP()[0] != 0) {
      return ETH.gatewayIP();
  }
#endif
#ifdef WLED_USE_SLIP
  IPAddress gw;
  gw.fromString(SLIP_THEIR_IP);
  return gw;
#else
  if (WiFi.localIP()[0] != 0) {
      return WiFi.gatewayIP();
  }
  return INADDR_NONE;
#endif
}

void WLEDNetworkClass::localMAC(uint8_t* MAC)
{
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  String macString = ETH.macAddress();
  char macChar[18];
  char * octetEnd = macChar;
  strlcpy(macChar, macString.c_str(), 18);
  for (uint8_t i = 0; i < 6; i++) {
    MAC[i] = (uint8_t)strtol(octetEnd, &octetEnd, 16);
    octetEnd++;
  }
  for (uint8_t i = 0; i < 6; i++) {
    if (MAC[i] != 0x00) {
      return;
    }
  }
#endif
#ifdef WLED_USE_SLIP
  WiFi.macAddress(MAC);
#else
  WiFi.macAddress(MAC);
#endif
  return;
}

bool WLEDNetworkClass::isConnected()
{
#ifdef WLED_USE_SLIP
  return slip_connected;
#elif defined(WLED_USE_PPP)
  return (WiFi.localIP()[0] != 0 && WiFi.status() == WL_CONNECTED) || isEthernet() || isPPP();
#else
  return (WiFi.localIP()[0] != 0 && WiFi.status() == WL_CONNECTED) || isEthernet();
#endif
}

bool WLEDNetworkClass::isEthernet()
{
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  return (ETH.localIP()[0] != 0) && ETH.linkUp();
#endif
  return false;
}

#ifdef WLED_USE_PPP
bool WLEDNetworkClass::isPPP()
{
  return ppp_connected;
}
#endif

WLEDNetworkClass WLEDNetwork;
