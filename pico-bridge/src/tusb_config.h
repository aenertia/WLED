/*
 * TinyUSB configuration for Pi Pico NCM-PPP Bridge
 * CDC-NCM network device (zero-config USB Ethernet on Linux/macOS/Win10+)
 */

#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "lwipopts.h"

/* Board config */
#ifndef BOARD_TUD_RHPORT
  #define BOARD_TUD_RHPORT 0
#endif

#ifndef BOARD_TUD_MAX_SPEED
  #define BOARD_TUD_MAX_SPEED OPT_MODE_DEFAULT_SPEED
#endif

/* Common config - CFG_TUSB_MCU defined by Pico SDK cmake */
#ifndef CFG_TUSB_MCU
  #error CFG_TUSB_MCU must be defined
#endif

#ifndef CFG_TUSB_OS
  #define CFG_TUSB_OS OPT_OS_NONE
#endif

#ifndef CFG_TUSB_DEBUG
  #define CFG_TUSB_DEBUG 0
#endif

#define CFG_TUD_ENABLED       1
#define CFG_TUD_MAX_SPEED     BOARD_TUD_MAX_SPEED

#ifndef CFG_TUSB_MEM_SECTION
  #define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
  #define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

/* Endpoint 0 size */
#ifndef CFG_TUD_ENDPOINT0_SIZE
  #define CFG_TUD_ENDPOINT0_SIZE 64
#endif

/* Network class: NCM only (not ECM/RNDIS) */
#define CFG_TUD_ECM_RNDIS     0
#define CFG_TUD_NCM           1

/* NCM buffer sizes - must be >> MTU */
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE   (2 * TCP_MSS + 100)
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE  (2 * TCP_MSS + 100)

#ifndef CFG_TUD_NCM_OUT_NTB_N
  #define CFG_TUD_NCM_OUT_NTB_N 1
#endif

#ifndef CFG_TUD_NCM_IN_NTB_N
  #define CFG_TUD_NCM_IN_NTB_N  1
#endif

#ifdef __cplusplus
}
#endif

#endif /* TUSB_CONFIG_H_ */
