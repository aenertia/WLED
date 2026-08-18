/*
 * TinyUSB configuration for Pi Pico CDC-NCM bridge.
 * Host sees a USB Ethernet adapter (CDC-NCM). Zero driver install on
 * Linux/macOS; Windows needs BOS + MS OS 2.0 descriptors (included).
 */
#ifndef TUSB_CONFIG_H_
#define TUSB_CONFIG_H_

/* ---- Board ---- */
#define CFG_TUSB_MCU            OPT_MCU_RP2040
#define CFG_TUSB_OS             OPT_OS_NONE
#define CFG_TUSB_DEBUG          0

/* ---- Device stack ---- */
#define CFG_TUD_ENABLED         1
#define CFG_TUD_MAX_SPEED       OPT_MODE_FULL_SPEED
#define CFG_TUD_ENDPOINT0_SIZE  64

/* ---- Network class: NCM only (not ECM/RNDIS) ---- */
#define CFG_TUD_ECM_RNDIS       0
#define CFG_TUD_NCM             1

/* ---- NCM NTB buffer tuning ----
 * RP2040 has 264KB SRAM. These are modest — tune up if throughput matters.
 * At 5Mbps PPP the USB side is never the bottleneck (12Mbps FS USB).
 */
#define CFG_TUD_NCM_IN_NTB_MAX_SIZE    2048
#define CFG_TUD_NCM_OUT_NTB_MAX_SIZE   4096
#define CFG_TUD_NCM_IN_NTB_N           2
#define CFG_TUD_NCM_OUT_NTB_N          2
#define CFG_TUD_NCM_IN_MAX_DATAGRAMS_PER_NTB   8
#define CFG_TUD_NCM_OUT_MAX_DATAGRAMS_PER_NTB  6

/* ---- No other device classes ---- */
#define CFG_TUD_CDC             0
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#endif /* TUSB_CONFIG_H_ */
