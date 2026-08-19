/*
 * USB descriptors for CDC-NCM Ethernet device.
 * Based on TinyUSB net_lwip_webserver example.
 * Includes BOS + MS OS 2.0 descriptors for Windows auto-driver.
 */
#include "tusb.h"
#include "pico/unique_id.h"

uint8_t tud_network_mac_address[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_INTERFACE,
    STRID_MAC,
};

enum {
    ITF_NUM_CDC = 0,
    ITF_NUM_DATA,
    ITF_NUM_TOTAL,
};

#define EPNUM_NET_NOTIF   0x81
#define EPNUM_NET_OUT     0x02
#define EPNUM_NET_IN      0x82

static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0201,        /* USB 2.1 required for BOS/NCM */
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,        /* Raspberry Pi */
    .idProduct          = 0x00F1,        /* Custom NCM bridge */
    .bcdDevice          = 0x0100,
    .iManufacturer      = STRID_MANUFACTURER,
    .iProduct           = STRID_PRODUCT,
    .iSerialNumber      = STRID_SERIAL,
    .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

#define NCM_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_NCM_DESC_LEN)

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, NCM_CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_NCM_DESCRIPTOR(ITF_NUM_CDC, STRID_INTERFACE, STRID_MAC,
                           EPNUM_NET_NOTIF, 64,
                           EPNUM_NET_OUT, EPNUM_NET_IN, 64,
                           CFG_TUD_NET_MTU),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

static char mac_string[13];  /* "AABBCCDDEEFF\0" */

static void mac_to_string(void) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < 6; i++) {
        mac_string[i * 2]     = hex[tud_network_mac_address[i] >> 4];
        mac_string[i * 2 + 1] = hex[tud_network_mac_address[i] & 0x0F];
    }
    mac_string[12] = '\0';
}

/* Convert char string to UTF-16 descriptor on the fly */
static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    switch (index) {
    case STRID_LANGID:
        _desc_str[1] = 0x0409;  /* English */
        chr_count = 1;
        break;
    case STRID_MANUFACTURER: {
        const char *s = "WLED";
        chr_count = (uint8_t)strlen(s);
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = s[i];
        break;
    }
    case STRID_PRODUCT: {
        const char *s = "WLED NCM-PPP Bridge";
        chr_count = (uint8_t)strlen(s);
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = s[i];
        break;
    }
    case STRID_SERIAL: {
        pico_unique_board_id_t id;
        pico_get_unique_board_id(&id);
        chr_count = 16;
        static const char hex[] = "0123456789ABCDEF";
        for (int i = 0; i < 8; i++) {
            _desc_str[1 + i * 2]     = hex[id.id[i] >> 4];
            _desc_str[1 + i * 2 + 1] = hex[id.id[i] & 0x0F];
        }
        break;
    }
    case STRID_INTERFACE: {
        const char *s = "WLED USB Ethernet";
        chr_count = (uint8_t)strlen(s);
        for (uint8_t i = 0; i < chr_count; i++) _desc_str[1 + i] = s[i];
        break;
    }
    case STRID_MAC:
        mac_to_string();
        chr_count = 12;
        for (uint8_t i = 0; i < 12; i++) _desc_str[1 + i] = mac_string[i];
        break;
    default:
        return NULL;
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

/* BOS descriptor (required for NCM on Windows) */
#define BOS_TOTAL_LEN      (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

#define MS_OS_20_DESC_LEN  0xB2

static uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, 1),
};

uint8_t const *tud_descriptor_bos_cb(void) {
    return desc_bos;
}

/* MS OS 2.0 descriptor  -- tells Windows to load the NCM driver */
static uint8_t const desc_ms_os_20[] = {
    /* Header */
    0x0A, 0x00,             /* wLength */
    0x00, 0x00,             /* MS_OS_20_SET_HEADER_DESCRIPTOR */
    0x00, 0x00, 0x03, 0x06, /* dwWindowsVersion: Win 8.1+ */
    (MS_OS_20_DESC_LEN & 0xFF), ((MS_OS_20_DESC_LEN >> 8) & 0xFF),

    /* Configuration subset header */
    0x08, 0x00,
    0x01, 0x00,             /* MS_OS_20_SUBSET_HEADER_CONFIGURATION */
    0x00,                   /* bConfigurationValue */
    0x00,
    (MS_OS_20_DESC_LEN - 0x0A) & 0xFF, ((MS_OS_20_DESC_LEN - 0x0A) >> 8) & 0xFF,

    /* Function subset header */
    0x08, 0x00,
    0x02, 0x00,             /* MS_OS_20_SUBSET_HEADER_FUNCTION */
    ITF_NUM_CDC,
    0x00,
    (MS_OS_20_DESC_LEN - 0x0A - 0x08) & 0xFF,
    ((MS_OS_20_DESC_LEN - 0x0A - 0x08) >> 8) & 0xFF,

    /* Compatible ID: WINNCM */
    0x14, 0x00,
    0x03, 0x00,             /* MS_OS_20_FEATURE_COMPATIBLE_ID */
    'W', 'I', 'N', 'N', 'C', 'M', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    /* Registry property: DeviceInterfaceGUIDs */
    0x84, 0x00,
    0x04, 0x00,             /* MS_OS_20_FEATURE_REG_PROPERTY */
    0x07, 0x00,             /* REG_MULTI_SZ */
    0x2A, 0x00,             /* wPropertyNameLength */
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    0x50, 0x00,             /* wPropertyDataLength */
    '{', 0, 'C', 0, 'E', 0, '5', 0, '0', 0, '2', 0, '4', 0, 'E', 0, 'E', 0,
    '-', 0, 'A', 0, '0', 0, '3', 0, 'D', 0,
    '-', 0, '4', 0, '4', 0, '6', 0, 'E', 0,
    '-', 0, 'B', 0, '1', 0, '6', 0, '5', 0,
    '-', 0, 'C', 0, 'A', 0, 'E', 0, '5', 0, 'D', 0, '0', 0, 'A', 0, '0', 0,
    'B', 0, 'B', 0, 'C', 0, '5', 0, '}', 0, 0, 0, 0, 0,
};

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
        request->bRequest == 1 &&
        request->wIndex == 7) {
        return tud_control_xfer(rhport, request,
                                (void *)(uintptr_t)desc_ms_os_20,
                                sizeof(desc_ms_os_20));
    }
    return false;
}

void usb_descriptors_init(void) {
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);

    tud_network_mac_address[0] = 0x02;  /* locally administered, unicast */
    tud_network_mac_address[1] = id.id[3];
    tud_network_mac_address[2] = id.id[4];
    tud_network_mac_address[3] = id.id[5];
    tud_network_mac_address[4] = id.id[6];
    tud_network_mac_address[5] = id.id[7];
}
