#include "usb_descriptors.h"

#include "board_config.h"

#include <stdio.h>
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"

// Sublicensed from MCS Electronics, who allocate individual PIDs under their
// own VID. Not usable for USB-IF logo certification — that needs a VID of your
// own — but this device is not certified and will not be.
#define USB_VID 0x16d0
#define USB_PID 0x1551
#define USB_BCD 0x0200

#define ITF_NUM_CCID 0
#define ITF_NUM_HID 1
#define ITF_NUM_CDC 2
#define ITF_NUM_CDC_DATA 3
#define ITF_NUM_TOTAL 4

#define EPNUM_CCID_OUT 0x01
#define EPNUM_CCID_IN 0x81
#define EPNUM_HID 0x82
#define EPNUM_CDC_NOTIF 0x83
#define EPNUM_CDC_OUT 0x04
#define EPNUM_CDC_IN 0x84

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 9 + 54 + 7 + 7 + TUD_HID_DESC_LEN + TUD_CDC_DESC_LEN)

uint8_t const smart_card_hid_report_descriptor[] = {
  TUD_HID_REPORT_DESC_KEYBOARD()
};

static const tusb_desc_device_t device_descriptor = {
  .bLength = sizeof(tusb_desc_device_t),
  .bDescriptorType = TUSB_DESC_DEVICE,
  .bcdUSB = USB_BCD,
  .bDeviceClass = 0x00,
  .bDeviceSubClass = 0x00,
  .bDeviceProtocol = 0x00,
  .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor = USB_VID,
  .idProduct = USB_PID,
  .bcdDevice = 0x0100,
  .iManufacturer = 0x01,
  .iProduct = 0x02,
  .iSerialNumber = 0x03,
  .bNumConfigurations = 0x01,
};

static const uint8_t configuration_descriptor[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),

  // Interface 0: CCID (USB class 0x0b), two bulk endpoints, no interrupt IN.
  9, TUSB_DESC_INTERFACE, ITF_NUM_CCID, 0, 2, 0x0b, 0x00, 0x00, 0,

  // CCID class descriptor. Declares T=1, 5 V only, short and extended APDU
  // support, and the exchange level macOS expects. Byte-for-byte as upstream:
  // this is the part that took someone else a lot of trial and error.
  54, 0x21,
  0x10, 0x01,
  0x00,
  0x07,
  0x02, 0x00, 0x00, 0x00,
  0x80, 0x25, 0x00, 0x00,
  0x80, 0x25, 0x00, 0x00,
  0x00,
  0x80, 0x25, 0x00, 0x00,
  0x80, 0x25, 0x00, 0x00,
  0x00,
  0xfe, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00,
  0x3e, 0x00, 0x02, 0x00,
  0x00, 0x08, 0x00, 0x00,
  0x00,        // bClassGetResponse
  0x00,        // bClassEnvelope
  0x00, 0x00,  // wLcdLayout: no display
  0x01,        // bPINSupport: PIN verification, i.e. we are a pinpad reader.
               //
               // CryptoTokenKit reads this byte straight out of the descriptor
               // to identify pinpad readers. Apple's built-in pivtoken ignores
               // it — measured: with 0x01 declared it never sent a single
               // PC_to_RDR_Secure. Our own token driver in macos/ is the one
               // that uses it, via TKTokenSmartCardPINAuthOperation's
               // APDUTemplate, which is what moves PIN entry onto the button
               // and removes the on-screen PIN field entirely.
               //
               // usb_ccid.c implements PC_to_RDR_Secure with CCID time
               // extensions, so the host waits for a human rather than timing
               // out.
  0x01,        // bMaxCCIDBusySlots

  7, TUSB_DESC_ENDPOINT, EPNUM_CCID_OUT, TUSB_XFER_BULK, 64, 0x00, 0,
  7, TUSB_DESC_ENDPOINT, EPNUM_CCID_IN, TUSB_XFER_BULK, 64, 0x00, 0,

  // Interface 1: HID keyboard, used only for the dummy PIV PIN.
  TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_KEYBOARD,
                     sizeof(smart_card_hid_report_descriptor), EPNUM_HID, 8, 10),

  // Interfaces 2 and 3: CDC, used only by the provisioning console.
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 0, EPNUM_CDC_NOTIF, 8,
                     EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

// "openhanko.io:" + 12 hex digits + NUL = 26 bytes. Sized with room to spare;
// tud_descriptor_string_cb caps the descriptor at 31 code units regardless.
static char serial_string[32] = "openhanko.io";

void smart_card_init_serial(void) {
  pico_unique_board_id_t id;
  pico_get_unique_board_id(&id);
  // Bytes 0-1 are the flash vendor's, identical across boards from one supplier,
  // so the low six carry what actually distinguishes one device from another.
  snprintf(serial_string, sizeof(serial_string),
           "openhanko.io:%02X%02X%02X%02X%02X%02X",
           id.id[2], id.id[3], id.id[4], id.id[5], id.id[6], id.id[7]);
}

static const char *string_descriptors[] = {
  NULL,               // 0: language, handled specially below
  // macOS builds its reader name by concatenating these two, so identical
  // strings read as "OpenHanko OpenHanko" in the pairing dialog.
  DEVICE_NAME,        // 1: manufacturer
  "Smart Card",       // 2: product
  serial_string,      // 3: serial
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&device_descriptor;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return configuration_descriptor;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  // Descriptor header plus up to 31 UTF-16 code units.
  static uint16_t descriptor[32];
  uint8_t count;

  if (index == 0) {
    descriptor[1] = 0x0409;  // English (US)
    count = 1;
  } else {
    if (index >= sizeof(string_descriptors) / sizeof(string_descriptors[0])) return NULL;
    const char *text = string_descriptors[index];
    if (!text) return NULL;

    size_t length = strlen(text);
    if (length > 31) length = 31;
    for (size_t i = 0; i < length; i++) descriptor[1 + i] = text[i];
    count = (uint8_t)length;
  }

  descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
  return descriptor;
}
