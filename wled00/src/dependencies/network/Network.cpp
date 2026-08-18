#include "Network.h"
#ifdef WLED_USE_PPP
#include "wled_ppp.h"
#endif

IPAddress WLEDNetworkClass::localIP()
{
  IPAddress localIP;
#ifdef WLED_USE_PPP
  if (isPPP()) {
  #ifdef WLED_USE_PPP_UART
    if (ppp_netif_uart) {
      esp_netif_ip_info_t ip_info;
      if (esp_netif_get_ip_info(ppp_netif_uart, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
        return IPAddress(ip_info.ip.addr);
    }
  #endif
  }
#endif
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  localIP = ETH.localIP();
  if (localIP[0] != 0) {
    return localIP;
  }
#endif
  localIP = WiFi.localIP();
  if (localIP[0] != 0) {
    return localIP;
  }

  return INADDR_NONE;
}

IPAddress WLEDNetworkClass::subnetMask()
{
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  if (ETH.localIP()[0] != 0) {
    return ETH.subnetMask();
  }
#endif
  if (WiFi.localIP()[0] != 0) {
    return WiFi.subnetMask();
  }
  return IPAddress(255, 255, 255, 0);
}

IPAddress WLEDNetworkClass::gatewayIP()
{
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  if (ETH.localIP()[0] != 0) {
      return ETH.gatewayIP();
  }
#endif
  if (WiFi.localIP()[0] != 0) {
      return WiFi.gatewayIP();
  }
  return INADDR_NONE;
}

void WLEDNetworkClass::localMAC(uint8_t* MAC)
{
#if defined(ARDUINO_ARCH_ESP32) && defined(WLED_USE_ETHERNET)
  // ETH.macAddress(MAC); // Does not work because of missing ETHClass:: in ETH.ccp

  // Start work around
  String macString = ETH.macAddress();
  char macChar[18];
  char * octetEnd = macChar;

  strlcpy(macChar, macString.c_str(), 18);

  for (uint8_t i = 0; i < 6; i++) {
    MAC[i] = (uint8_t)strtol(octetEnd, &octetEnd, 16);
    octetEnd++;
  }
  // End work around

  for (uint8_t i = 0; i < 6; i++) {
    if (MAC[i] != 0x00) {
      return;
    }
  }
#endif
  WiFi.macAddress(MAC);
  return;
}

bool WLEDNetworkClass::isConnected()
{
#ifdef WLED_USE_PPP
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
  #ifdef WLED_USE_PPP_UART
  if (ppp_netif_uart) {
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(ppp_netif_uart, &ip) == ESP_OK && ip.ip.addr != 0)
      return true;
  }
  #endif
  return false;
}
#endif

WLEDNetworkClass WLEDNetwork;