#include "tusb.h"
#include "class/audio/audio.h"
#include "class/audio/audio_device.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"

#include "usb_descriptors.h"

// -----------------------------------------------------------------------------
// VENDOR / PRODUCT IDs
// -----------------------------------------------------------------------------
#define USB_VID           0x303A // Espressif VID
#define USB_PID_CDC       0x4000 // Modo 0: Serial CDC / Flash Puro (Standby/Player)
#define USB_PID_MSC       0x4002 // Modo 1: Armazenamento USB Puro
#define USB_PID_DAC       0x4004 // Modo 2: USB DAC de Mesa Puro

// -----------------------------------------------------------------------------
// DESCRITOR HID (Media Control / Consumer)
// -----------------------------------------------------------------------------
uint8_t const desc_hid_report[] =
{
    TUD_HID_REPORT_DESC_CONSUMER()
};

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void) instance;
    return desc_hid_report;
}

// =============================================================================
// MODO 0: SERIAL CDC PURO / FLASH (STANDBY / PLAYER)
// =============================================================================

#define EPNUM_CDC_NOTIF       0x83
#define EPNUM_CDC_OUT         0x02
#define EPNUM_CDC_IN          0x82

#define CONFIG_CDC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

const tusb_desc_device_t desc_device_cdc =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID_CDC,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

uint8_t const desc_configuration_cdc[] =
{
    // Config number, interface count (2), string index (0), total length, attribute, power in mA (500)
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_CDC_TOTAL_LEN, 0x00, 500),

    // Interface CDC (Communication + Data)
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)
};

_Static_assert(sizeof(desc_configuration_cdc) == CONFIG_CDC_TOTAL_LEN, "CONFIG_CDC_TOTAL_LEN mismatch!");

const char* usb_manager_cdc_string_desc_arr[] =
{
    (const char[]) { 0x09, 0x04 }, // 0: English (0x0409)
    "MPS3 Custom",                 // 1: Manufacturer
    "MPS3 Serial / Flash",         // 2: Product
    "123456",                      // 3: Serial
    "MPS3 CDC Console",            // 4: CDC Interface
};
const int usb_manager_cdc_string_desc_count = 5;

// =============================================================================
// MODO 1: ARMAZENAMENTO USB PURO (MSC ONLY)
// =============================================================================

#define EPNUM_MSC_OUT_SOLO    0x01
#define EPNUM_MSC_IN_SOLO     0x81

const tusb_desc_device_t desc_device_msc =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00, // Classe especificada na interface MSC
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID_MSC,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

#define CONFIG_MSC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

uint8_t const desc_configuration_msc[] =
{
    // Config number, interface count (1), string index (0), total length, attribute, power in mA (500)
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_MSC_TOTAL_LEN, 0x00, 500),

    // Interface MSC (Mass Storage puro na interface 0)
    TUD_MSC_DESCRIPTOR(0, 4, EPNUM_MSC_OUT_SOLO, EPNUM_MSC_IN_SOLO, 64)
};

_Static_assert(sizeof(desc_configuration_msc) == CONFIG_MSC_TOTAL_LEN, "CONFIG_MSC_TOTAL_LEN mismatch!");

const char* usb_manager_msc_string_desc_arr[] =
{
    (const char[]) { 0x09, 0x04 }, // 0: English (0x0409)
    "MPS3 Custom",                 // 1: Manufacturer
    "MPS3 USB Storage",            // 2: Product
    "123456",                      // 3: Serial
    "MPS3 SD Card",                // 4: MSC Interface
};
const int usb_manager_msc_string_desc_count = 5;

// =============================================================================
// MODO 2: USB DAC DE MESA PURO (UAC2 + HID ONLY)
// =============================================================================

#define EPNUM_DAC_AUDIO_OUT   0x01
#define EPNUM_DAC_HID_IN      0x82

enum
{
    ITF_NUM_DAC_AUDIO_CONTROL = 0,
    ITF_NUM_DAC_AUDIO_STREAMING,
    ITF_NUM_DAC_HID,
    ITF_NUM_DAC_TOTAL
};

const tusb_desc_device_t desc_device_dac =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID_DAC,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

#define AUDIO_BYTES_PER_SAMPLE    4 // 24-bit em subslot de 32 bits
#define AUDIO_BITS_PER_SAMPLE     24 // resolucao de 24 bits
#define AUDIO_EP_OUT_SIZE         384 // Max 48kHz * 2 ch * 4 bytes = 384 bytes/ms

#define CONFIG_DAC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + \
                               TUD_AUDIO20_SPEAKER_STEREO_DESC_LEN + \
                               TUD_HID_DESC_LEN)

uint8_t const desc_configuration_dac[] =
{
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_DAC_TOTAL, 0, CONFIG_DAC_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),

    // Interface Audio UAC2 Adaptive (Control + Streaming puro, sem feedback EP para compatibilidade universal)
    TUD_AUDIO20_SPEAKER_STEREO_DESCRIPTOR(
        ITF_NUM_DAC_AUDIO_CONTROL, 
        4, // String index 4: "MPS3 DAC Audio"
        AUDIO_BYTES_PER_SAMPLE, 
        AUDIO_BITS_PER_SAMPLE, 
        EPNUM_DAC_AUDIO_OUT, 
        AUDIO_EP_OUT_SIZE
    ),

    // Interface HID (Consumer Control)
    TUD_HID_DESCRIPTOR(ITF_NUM_DAC_HID, 5, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_DAC_HID_IN, 16, 10)
};

_Static_assert(sizeof(desc_configuration_dac) == CONFIG_DAC_TOTAL_LEN, "CONFIG_DAC_TOTAL_LEN mismatch!");

const char* usb_manager_dac_string_desc_arr[] =
{
    (const char[]) { 0x09, 0x04 }, // 0: English (0x0409)
    "MPS3 Custom",                 // 1: Manufacturer
    "MPS3 USB DAC",                // 2: Product
    "123456",                      // 3: Serial
    "MPS3 DAC Audio",              // 4: Audio Interface
    "MPS3 Media Control",          // 5: HID Interface
};
const int usb_manager_dac_string_desc_count = 6;

// =============================================================================
// DEFAULT DESCRIPTORS (Inicializa como CDC Puro / Flash por padrao)
// =============================================================================
const tusb_desc_device_t desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID_CDC,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

const uint8_t desc_configuration[] =
{
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_CDC_TOTAL_LEN, 0x00, 500),
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64)
};

const char* usb_manager_string_desc_arr[] =
{
    (const char[]) { 0x09, 0x04 },
    "MPS3 Custom",
    "MPS3 Serial / Flash",
    "123456",
    "MPS3 CDC Console",
};
const int usb_manager_string_desc_count = 5;
