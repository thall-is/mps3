#include "tusb.h"
#include "class/audio/audio.h"
#include "class/audio/audio_device.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"

#include "uac2_speaker_fb_desc.h"
// -----------------------------------------------------------------------------
#define EPNUM_AUDIO_OUT   0x01
#define EPNUM_AUDIO_IN    0x81 // Usado para feedback do clock no UAC2

#define EPNUM_MSC_OUT     0x02
#define EPNUM_MSC_IN      0x82

#define EPNUM_HID_IN      0x83

// -----------------------------------------------------------------------------
// DEFINICOES DE INTERFACES
// -----------------------------------------------------------------------------
enum
{
    ITF_NUM_AUDIO_CONTROL = 0,
    ITF_NUM_AUDIO_STREAMING,
    ITF_NUM_MSC,
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

// -----------------------------------------------------------------------------
// DEVICE DESCRIPTOR
// -----------------------------------------------------------------------------
#define USB_VID   0x303A // Espressif VID
#define USB_PID   0x4002 // Custom PID

const tusb_desc_device_t desc_device =
{
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    // Use Interface Association Descriptor (IAD) for Audio
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,

    .bNumConfigurations = 0x01
};

// -----------------------------------------------------------------------------
// DESCRITOR HID (Media Control / Consumer)
// -----------------------------------------------------------------------------
// Report descriptor for Consumer Control (Volume, Play/Pause, Next, Prev)
uint8_t const desc_hid_report[] =
{
    TUD_HID_REPORT_DESC_CONSUMER()
};

uint8_t const * tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void) instance;
    return desc_hid_report;
}

// -----------------------------------------------------------------------------
// CONFIGURATION DESCRIPTOR
// -----------------------------------------------------------------------------

#include "usb_descriptors.h"

#define CONFIG_TOTAL_LEN    (TUD_CONFIG_DESC_LEN + \
                             TUD_AUDIO20_SPEAKER_STEREO_DESC_LEN + \
                             TUD_MSC_DESC_LEN + \
                             TUD_HID_DESC_LEN)

// Audio Resolution
#define AUDIO_BYTES_PER_SAMPLE    2 // 16-bits
#define AUDIO_BITS_PER_SAMPLE     16
#define AUDIO_EP_OUT_SIZE         192

// Descritor da configuracao inteira
uint8_t const desc_configuration[] =
{
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),

    // Interface Audio (UAC2 Stereo Speaker with Feedback)
    TUD_AUDIO20_SPEAKER_STEREO_DESCRIPTOR(
        ITF_NUM_AUDIO_CONTROL, 
        0, 
        AUDIO_BYTES_PER_SAMPLE, 
        AUDIO_BITS_PER_SAMPLE, 
        EPNUM_AUDIO_OUT, 
        AUDIO_EP_OUT_SIZE
    ),

    // Interface MSC (Mass Storage)
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 0, EPNUM_MSC_OUT, EPNUM_MSC_IN, 64),

    // Interface HID (Consumer Control)
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID_IN, 16, 10)
};

// Se isso falhar na compilacao, significa que CONFIG_TOTAL_LEN esta calculado errado e o Windows ira rejeitar!
_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN, "ERRO: O tamanho do descritor de configuracao nao bate com CONFIG_TOTAL_LEN!");

// -----------------------------------------------------------------------------
// CONFIGURATION DESCRIPTOR
// -----------------------------------------------------------------------------
// Callbacks are implemented by esp_tinyusb. We only export the descriptors.
const char* usb_manager_string_desc_arr[] =
{
    (const char[]) { 0x09, 0x04 }, // 0: is supported language is English (0x0409)
    "MPS3 Custom",                 // 1: Manufacturer
    "MPS3 Audio Interface",        // 2: Product
    "123456",                      // 3: Serials
    "MPS3 DAC",                    // 4: Audio Interface
    "MPS3 SD Card",                // 5: MSC Interface
    "MPS3 Media Control",          // 6: HID Interface
};
const int usb_manager_string_desc_count = 7;
