// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  Generic Bluetooth USB driver
 *
 *  Copyright (C) 2005-2008  Marcel Holtmann <marcel@holtmann.org>
 */

#include <linux/dmi.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/quirks.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/suspend.h>
#include <linux/gpio/consumer.h>
#include <linux/debugfs.h>
#include <linux/unaligned.h>
#include <linux/completion.h>
#include <linux/atomic.h>
#include <linux/workqueue.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/hci_drv.h>

#include "btintel.h"
#include "btbcm.h"
#include "btrtl.h"
#include "btmtk.h"

#define VERSION "0.8"

static bool disable_scofix;
static bool force_scofix;
static bool enable_autosuspend = IS_ENABLED(CONFIG_BT_HCIBTUSB_AUTOSUSPEND);
static bool enable_poll_sync = IS_ENABLED(CONFIG_BT_HCIBTUSB_POLL_SYNC);
static bool reset = true;

static struct usb_driver btusb_driver;

#define BTUSB_IGNORE			BIT(0)
#define BTUSB_DIGIANSWER		BIT(1)
#define BTUSB_CSR			BIT(2)
#define BTUSB_SNIFFER			BIT(3)
#define BTUSB_BCM92035			BIT(4)
#define BTUSB_BROKEN_ISOC		BIT(5)
#define BTUSB_WRONG_SCO_MTU		BIT(6)
#define BTUSB_ATH3012			BIT(7)
#define BTUSB_INTEL_COMBINED		BIT(8)
#define BTUSB_INTEL_BOOT		BIT(9)
#define BTUSB_BCM_PATCHRAM		BIT(10)
#define BTUSB_MARVELL			BIT(11)
#define BTUSB_SWAVE			BIT(12)
#define BTUSB_AMP			BIT(13)
#define BTUSB_QCA_ROME			BIT(14)
#define BTUSB_BCM_APPLE			BIT(15)
#define BTUSB_REALTEK			BIT(16)
#define BTUSB_BCM2045			BIT(17)
#define BTUSB_IFNUM_2			BIT(18)
#define BTUSB_CW6622			BIT(19)
#define BTUSB_MEDIATEK			BIT(20)
#define BTUSB_WIDEBAND_SPEECH		BIT(21)
#define BTUSB_INVALID_LE_STATES		BIT(22)
#define BTUSB_QCA_WCN6855		BIT(23)
#define BTUSB_INTEL_BROKEN_SHUTDOWN_LED	BIT(24)
#define BTUSB_INTEL_BROKEN_INITIAL_NCMD BIT(25)
#define BTUSB_INTEL_NO_WBS_SUPPORT	BIT(26)
#define BTUSB_ACTIONS_SEMI		BIT(27)
#define BTUSB_BARROT			BIT(28)

static const struct usb_device_id btusb_table[] = {
	/* Generic Bluetooth USB device */
	{ USB_DEVICE_INFO(0xe0, 0x01, 0x01) },

	/* Generic Bluetooth AMP device */
	{ USB_DEVICE_INFO(0xe0, 0x01, 0x04), .driver_info = BTUSB_AMP },

	/* Generic Bluetooth USB interface */
	{ USB_INTERFACE_INFO(0xe0, 0x01, 0x01) },

	/* Apple-specific (Broadcom) devices */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x05ac, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_APPLE | BTUSB_IFNUM_2 },

	/* MediaTek MT76x0E */
	{ USB_DEVICE(0x0e8d, 0x763f) },

	/* Broadcom SoftSailing reporting vendor specific */
	{ USB_DEVICE(0x0a5c, 0x21e1) },

	/* Apple MacBookPro 7,1 */
	{ USB_DEVICE(0x05ac, 0x8213) },

	/* Apple iMac11,1 */
	{ USB_DEVICE(0x05ac, 0x8215) },

	/* Apple MacBookPro6,2 */
	{ USB_DEVICE(0x05ac, 0x8218) },

	/* Apple MacBookAir3,1, MacBookAir3,2 */
	{ USB_DEVICE(0x05ac, 0x821b) },

	/* Apple MacBookAir4,1 */
	{ USB_DEVICE(0x05ac, 0x821f) },

	/* Apple MacBookPro8,2 */
	{ USB_DEVICE(0x05ac, 0x821a) },

	/* Apple MacMini5,1 */
	{ USB_DEVICE(0x05ac, 0x8281) },

	/* AVM BlueFRITZ! USB v2.0 */
	{ USB_DEVICE(0x057c, 0x3800), .driver_info = BTUSB_SWAVE },

	/* Bluetooth Ultraport Module from IBM */
	{ USB_DEVICE(0x04bf, 0x030a) },

	/* ALPS Modules with non-standard id */
	{ USB_DEVICE(0x044e, 0x3001) },
	{ USB_DEVICE(0x044e, 0x3002) },

	/* Ericsson with non-standard id */
	{ USB_DEVICE(0x0bdb, 0x1002) },

	/* Canyon CN-BTU1 with HID interfaces */
	{ USB_DEVICE(0x0c10, 0x0000) },

	/* Broadcom BCM20702B0 (Dynex/Insignia) */
	{ USB_DEVICE(0x19ff, 0x0239), .driver_info = BTUSB_BCM_PATCHRAM },

	/* Broadcom BCM43142A0 (Foxconn/Lenovo) */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x105b, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* Broadcom BCM920703 (HTC Vive) */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x0bb4, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* Foxconn - Hon Hai */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x0489, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* Lite-On Technology - Broadcom based */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x04ca, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* Broadcom devices with vendor specific id */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x0a5c, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* ASUSTek Computer - Broadcom based */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x0b05, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* Belkin F8065bf - Broadcom based */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x050d, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* IMC Networks - Broadcom based */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x13d3, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* Dell Computer - Broadcom based  */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x413c, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* Toshiba Corp - Broadcom based */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x0930, 0xff, 0x01, 0x01),
	  .driver_info = BTUSB_BCM_PATCHRAM },

	/* Intel Bluetooth USB Bootloader (RAM module) */
	{ USB_DEVICE(0x8087, 0x0a5a),
	  .driver_info = BTUSB_INTEL_BOOT | BTUSB_BROKEN_ISOC },

	{ }	/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, btusb_table);

static const struct usb_device_id quirks_table[] = {
	/* CSR BlueCore devices */
	{ USB_DEVICE(0x0a12, 0x0001), .driver_info = BTUSB_CSR },

	/* Broadcom BCM2033 without firmware */
	{ USB_DEVICE(0x0a5c, 0x2033), .driver_info = BTUSB_IGNORE },

	/* Broadcom BCM2045 devices */
	{ USB_DEVICE(0x0a5c, 0x2045), .driver_info = BTUSB_BCM2045 },

	/* Atheros 3011 with sflash firmware */
	{ USB_DEVICE(0x0489, 0xe027), .driver_info = BTUSB_IGNORE },
	{ USB_DEVICE(0x0489, 0xe03d), .driver_info = BTUSB_IGNORE },
	{ USB_DEVICE(0x04f2, 0xaff1), .driver_info = BTUSB_IGNORE },
	{ USB_DEVICE(0x0930, 0x0215), .driver_info = BTUSB_IGNORE },
	{ USB_DEVICE(0x0cf3, 0x3002), .driver_info = BTUSB_IGNORE },
	{ USB_DEVICE(0x0cf3, 0xe019), .driver_info = BTUSB_IGNORE },
	{ USB_DEVICE(0x13d3, 0x3304), .driver_info = BTUSB_IGNORE },

	/* Atheros AR9285 Malbec with sflash firmware */
	{ USB_DEVICE(0x03f0, 0x311d), .driver_info = BTUSB_IGNORE },

	/* Atheros 3012 with sflash firmware */
	{ USB_DEVICE(0x0489, 0xe04d), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0489, 0xe04e), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0489, 0xe056), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0489, 0xe057), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0489, 0xe05f), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0489, 0xe076), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0489, 0xe078), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0489, 0xe095), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04c5, 0x1330), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x3004), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x3005), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x3006), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x3007), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x3008), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x300b), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x300d), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x300f), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x3010), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x3014), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x04ca, 0x3018), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0930, 0x0219), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0930, 0x021c), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0930, 0x0220), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0930, 0x0227), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0b05, 0x17d0), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0x0036), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0x3004), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0x3008), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0x311d), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0x311e), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0x311f), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0x3121), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0x817a), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0x817b), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0xe003), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0xe004), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0xe005), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0cf3, 0xe006), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3362), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3375), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3393), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3395), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3402), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3408), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3423), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3432), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3472), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3474), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3487), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x13d3, 0x3490), .driver_info = BTUSB_ATH3012 },

	/* Atheros AR5BBU12 with sflash firmware */
	{ USB_DEVICE(0x0489, 0xe02c), .driver_info = BTUSB_IGNORE },

	/* Atheros AR5BBU12 with sflash firmware */
	{ USB_DEVICE(0x0489, 0xe036), .driver_info = BTUSB_ATH3012 },
	{ USB_DEVICE(0x0489, 0xe03c), .driver_info = BTUSB_ATH3012 },

	/* QCA ROME chipset */
	{ USB_DEVICE(0x0cf3, 0x535b), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cf3, 0xe007), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cf3, 0xe009), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cf3, 0xe010), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cf3, 0xe300), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cf3, 0xe301), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cf3, 0xe360), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cf3, 0xe500), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe092), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe09f), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0a2), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3011), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3015), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3016), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x301a), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3021), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3491), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3496), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3501), .driver_info = BTUSB_QCA_ROME |
						     BTUSB_WIDEBAND_SPEECH },

	/* QCA WCN6855 chipset */
	{ USB_DEVICE(0x0489, 0xe0c7), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0c9), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0ca), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0cb), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0cc), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0ce), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0d0), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0d6), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0de), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0df), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0e1), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0e3), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0ea), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0ec), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3022), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3023), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3024), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3a22), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3a24), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3a26), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3a27), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cf3, 0xe600), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9108), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9109), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9208), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9209), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9308), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9309), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9408), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9409), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9508), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9509), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9608), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9609), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x10ab, 0x9f09), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x28de, 0x1401), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },

	/* QCA WCN785x chipset */
	{ USB_DEVICE(0x0cf3, 0xe700), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0fc), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0f3), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe100), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe103), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe10a), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe10d), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe11b), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe11c), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe11f), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe141), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe14a), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe14b), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe14d), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3623), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3624), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x2c7c, 0x0130), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x2c7c, 0x0131), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x2c7c, 0x0132), .driver_info = BTUSB_QCA_WCN6855 |
						     BTUSB_WIDEBAND_SPEECH },

	/* Broadcom BCM2035 */
	{ USB_DEVICE(0x0a5c, 0x2009), .driver_info = BTUSB_BCM92035 },
	{ USB_DEVICE(0x0a5c, 0x200a), .driver_info = BTUSB_WRONG_SCO_MTU },
	{ USB_DEVICE(0x0a5c, 0x2035), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* Broadcom BCM2045 */
	{ USB_DEVICE(0x0a5c, 0x2039), .driver_info = BTUSB_WRONG_SCO_MTU },
	{ USB_DEVICE(0x0a5c, 0x2101), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* IBM/Lenovo ThinkPad with Broadcom chip */
	{ USB_DEVICE(0x0a5c, 0x201e), .driver_info = BTUSB_WRONG_SCO_MTU },
	{ USB_DEVICE(0x0a5c, 0x2110), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* HP laptop with Broadcom chip */
	{ USB_DEVICE(0x03f0, 0x171d), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* Dell laptop with Broadcom chip */
	{ USB_DEVICE(0x413c, 0x8126), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* Dell Wireless 370 and 410 devices */
	{ USB_DEVICE(0x413c, 0x8152), .driver_info = BTUSB_WRONG_SCO_MTU },
	{ USB_DEVICE(0x413c, 0x8156), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* Belkin F8T012 and F8T013 devices */
	{ USB_DEVICE(0x050d, 0x0012), .driver_info = BTUSB_WRONG_SCO_MTU },
	{ USB_DEVICE(0x050d, 0x0013), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* Asus WL-BTD202 device */
	{ USB_DEVICE(0x0b05, 0x1715), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* Kensington Bluetooth USB adapter */
	{ USB_DEVICE(0x047d, 0x105e), .driver_info = BTUSB_WRONG_SCO_MTU },

	/* RTX Telecom based adapters with buggy SCO support */
	{ USB_DEVICE(0x0400, 0x0807), .driver_info = BTUSB_BROKEN_ISOC },
	{ USB_DEVICE(0x0400, 0x080a), .driver_info = BTUSB_BROKEN_ISOC },

	/* CONWISE Technology based adapters with buggy SCO support */
	{ USB_DEVICE(0x0e5e, 0x6622),
	  .driver_info = BTUSB_BROKEN_ISOC | BTUSB_CW6622},

	/* Roper Class 1 Bluetooth Dongle (Silicon Wave based) */
	{ USB_DEVICE(0x1310, 0x0001), .driver_info = BTUSB_SWAVE },

	/* Digianswer devices */
	{ USB_DEVICE(0x08fd, 0x0001), .driver_info = BTUSB_DIGIANSWER },
	{ USB_DEVICE(0x08fd, 0x0002), .driver_info = BTUSB_IGNORE },

	/* CSR BlueCore Bluetooth Sniffer */
	{ USB_DEVICE(0x0a12, 0x0002),
	  .driver_info = BTUSB_SNIFFER | BTUSB_BROKEN_ISOC },

	/* Frontline ComProbe Bluetooth Sniffer */
	{ USB_DEVICE(0x16d3, 0x0002),
	  .driver_info = BTUSB_SNIFFER | BTUSB_BROKEN_ISOC },

	/* Marvell Bluetooth devices */
	{ USB_DEVICE(0x1286, 0x2044), .driver_info = BTUSB_MARVELL },
	{ USB_DEVICE(0x1286, 0x2046), .driver_info = BTUSB_MARVELL },
	{ USB_DEVICE(0x1286, 0x204e), .driver_info = BTUSB_MARVELL },

	/* Intel Bluetooth devices */
	{ USB_DEVICE(0x8087, 0x0025), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0026), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0029), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0032), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0033), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0035), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0036), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0037), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0038), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0039), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x07da), .driver_info = BTUSB_CSR },
	{ USB_DEVICE(0x8087, 0x07dc), .driver_info = BTUSB_INTEL_COMBINED |
						     BTUSB_INTEL_NO_WBS_SUPPORT |
						     BTUSB_INTEL_BROKEN_INITIAL_NCMD |
						     BTUSB_INTEL_BROKEN_SHUTDOWN_LED },
	{ USB_DEVICE(0x8087, 0x0a2a), .driver_info = BTUSB_INTEL_COMBINED |
						     BTUSB_INTEL_NO_WBS_SUPPORT |
						     BTUSB_INTEL_BROKEN_SHUTDOWN_LED },
	{ USB_DEVICE(0x8087, 0x0a2b), .driver_info = BTUSB_INTEL_COMBINED },
	{ USB_DEVICE(0x8087, 0x0aa7), .driver_info = BTUSB_INTEL_COMBINED |
						     BTUSB_INTEL_BROKEN_SHUTDOWN_LED },
	{ USB_DEVICE(0x8087, 0x0aaa), .driver_info = BTUSB_INTEL_COMBINED },

	/* Other Intel Bluetooth devices */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x8087, 0xe0, 0x01, 0x01),
	  .driver_info = BTUSB_IGNORE },

	/* Realtek 8821CE Bluetooth devices */
	{ USB_DEVICE(0x13d3, 0x3529), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3533), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek 8822CE Bluetooth devices */
	{ USB_DEVICE(0x0bda, 0xb00c), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0bda, 0xc822), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek 8822CU Bluetooth devices */
	{ USB_DEVICE(0x13d3, 0x3549), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek 8851BE Bluetooth devices */
	{ USB_DEVICE(0x0bda, 0xb850), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3600), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3601), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x0489, 0xe112), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek 8851BU Bluetooth devices */
	{ USB_DEVICE(0x3625, 0x010b), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x2001, 0x332a), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x7392, 0xe611), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek 8852AE Bluetooth devices */
	{ USB_DEVICE(0x0bda, 0x2852), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0bda, 0xc852), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0bda, 0x385a), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0bda, 0x4852), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04c5, 0x165c), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x4006), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cb8, 0xc549), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek 8852CE Bluetooth devices */
	{ USB_DEVICE(0x04ca, 0x4007), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04c5, 0x1675), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cb8, 0xc558), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3587), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3586), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3592), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3612), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe122), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek 8852BE Bluetooth devices */
	{ USB_DEVICE(0x0cb8, 0xc559), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0bda, 0x4853), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0bda, 0x887b), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0bda, 0xb85b), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3570), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3571), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3572), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3591), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe123), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe125), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek 8852BT/8852BE-VT Bluetooth devices */
	{ USB_DEVICE(0x0bda, 0x8520), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe12f), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3618), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3619), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek 8922AE Bluetooth devices */
	{ USB_DEVICE(0x0bda, 0x8922), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3617), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3616), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe130), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Realtek Bluetooth devices */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x0bda, 0xe0, 0x01, 0x01),
	  .driver_info = BTUSB_REALTEK },

	/* MediaTek Bluetooth devices */
	{ USB_VENDOR_AND_INTERFACE_INFO(0x0e8d, 0xe0, 0x01, 0x01),
	  .driver_info = BTUSB_MEDIATEK |
			 BTUSB_WIDEBAND_SPEECH },

	/* Additional MediaTek MT7615E Bluetooth devices */
	{ USB_DEVICE(0x13d3, 0x3560), .driver_info = BTUSB_MEDIATEK},

	/* Additional MediaTek MT7663 Bluetooth devices */
	{ USB_DEVICE(0x043e, 0x310c), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3801), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Additional MediaTek MT7668 Bluetooth devices */
	{ USB_DEVICE(0x043e, 0x3109), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Additional MediaTek MT7920 Bluetooth devices */
	{ USB_DEVICE(0x0489, 0xe134), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe135), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3620), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3621), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3622), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe158), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Additional MediaTek MT7921 Bluetooth devices */
	{ USB_DEVICE(0x0489, 0xe0c8), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0cd), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0e0), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0f2), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3802), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0e8d, 0x0608), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3563), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3564), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3567), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3576), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3578), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3583), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3606), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* MediaTek MT7922 Bluetooth devices */
	{ USB_DEVICE(0x13d3, 0x3585), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3610), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* MediaTek MT7922A Bluetooth devices */
	{ USB_DEVICE(0x0489, 0xe0d8), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0d9), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0e2), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0e4), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0f1), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0f2), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0f5), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe0f6), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe102), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe152), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe153), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe170), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x3804), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04ca, 0x38e4), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3568), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3584), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3605), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3607), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3614), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3615), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3633), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x35f5, 0x7922), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Additional MediaTek MT7925 Bluetooth devices */
	{ USB_DEVICE(0x0489, 0xe111), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe113), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe118), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe11e), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe124), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe139), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe14e), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe14f), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe150), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0489, 0xe151), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3602), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3603), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3604), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3608), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3613), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3627), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3628), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3630), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x2c7c, 0x7009), .driver_info = BTUSB_MEDIATEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Additional Realtek 8723AE Bluetooth devices */
	{ USB_DEVICE(0x0930, 0x021d), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3394), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8723BE Bluetooth devices */
	{ USB_DEVICE(0x0489, 0xe085), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x0489, 0xe08b), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x04f2, 0xb49f), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3410), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3416), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3459), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3494), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8723BU Bluetooth devices */
	{ USB_DEVICE(0x7392, 0xa611), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x2c0a, 0x8761), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8723DE Bluetooth devices */
	{ USB_DEVICE(0x0bda, 0xb009), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x2ff8, 0xb011), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8761BUV Bluetooth devices */
	{ USB_DEVICE(0x2357, 0x0604), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0b05, 0x190e), .driver_info = BTUSB_REALTEK |
	  					     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x2550, 0x8761), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0bda, 0x8771), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x6655, 0x8771), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x7392, 0xc611), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x2b89, 0x8761), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x2b89, 0x6275), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Additional Realtek 8821AE Bluetooth devices */
	{ USB_DEVICE(0x0b05, 0x17dc), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3414), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3458), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3461), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x13d3, 0x3462), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8822BE Bluetooth devices */
	{ USB_DEVICE(0x13d3, 0x3526), .driver_info = BTUSB_REALTEK },
	{ USB_DEVICE(0x0b05, 0x185c), .driver_info = BTUSB_REALTEK },

	/* Additional Realtek 8822CE Bluetooth devices */
	{ USB_DEVICE(0x04ca, 0x4005), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x04c5, 0x161f), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0b05, 0x18ef), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3548), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3549), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3553), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x13d3, 0x3555), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x2ff8, 0x3051), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x1358, 0xc123), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0bda, 0xc123), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },
	{ USB_DEVICE(0x0cb5, 0xc547), .driver_info = BTUSB_REALTEK |
						     BTUSB_WIDEBAND_SPEECH },

	/* Barrot Technology Bluetooth devices */
	{ USB_DEVICE(0x33fa, 0x0010), .driver_info = BTUSB_BARROT },
	{ USB_DEVICE(0x33fa, 0x0012), .driver_info = BTUSB_BARROT },

	/* Actions Semiconductor ATS2851 based devices */
	{ USB_DEVICE(0x10d7, 0xb012), .driver_info = BTUSB_ACTIONS_SEMI },

	/* Silicon Wave based devices */
	{ USB_DEVICE(0x0c10, 0x0000), .driver_info = BTUSB_SWAVE },

	{ }	/* Terminating entry */
};

/* The Bluetooth USB module build into some devices needs to be reset on resume,
 * this is a problem with the platform (likely shutting off all power) not with
 * the module itself. So we use a DMI list to match known broken platforms.
 */
static const struct dmi_system_id btusb_needs_reset_resume_table[] = {
	{
		/* Dell OptiPlex 3060 (QCA ROME device 0cf3:e007) */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "OptiPlex 3060"),
		},
	},
	{
		/* Dell XPS 9360 (QCA ROME device 0cf3:e300) */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "XPS 13 9360"),
		},
	},
	{
		/* Dell Inspiron 5565 (QCA ROME device 0cf3:e009) */
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Inspiron 5565"),
		},
	},
	{}
};

struct qca_dump_info {
	/* fields for dump collection */
	u16 id_vendor;
	u16 id_product;
	u32 fw_version;
	u32 controller_id;
	u32 ram_dump_size;
	u16 ram_dump_seqno;
};

#define BTUSB_MAX_ISOC_FRAMES	10

#define BTUSB_INTR_RUNNING	0
#define BTUSB_BULK_RUNNING	1
#define BTUSB_ISOC_RUNNING	2
#define BTUSB_SUSPENDING	3
#define BTUSB_DID_ISO_RESUME	4
#define BTUSB_BOOTLOADER	5
#define BTUSB_DOWNLOADING	6
#define BTUSB_FIRMWARE_LOADED	7
#define BTUSB_FIRMWARE_FAILED	8
#define BTUSB_BOOTING		9
#define BTUSB_DIAG_RUNNING	10
#define BTUSB_OOB_WAKE_ENABLED	11
#define BTUSB_HW_RESET_ACTIVE	12
#define BTUSB_TX_WAIT_VND_EVT	13
#define BTUSB_WAKEUP_AUTOSUSPEND	14
#define BTUSB_USE_ALT3_FOR_WBS	15
#define BTUSB_ALT6_CONTINUOUS_TX	16
#define BTUSB_HW_SSR_ACTIVE	17

struct btusb_data {
	struct hci_dev       *hdev;
	struct usb_device    *udev;
	struct usb_interface *intf;
	struct usb_interface *isoc;
	struct usb_interface *diag;
	unsigned isoc_ifnum;

	unsigned long flags;

	/* LOCAL PATCH: this clone (0a12:0001) is broken in two ways:
	 *  - it never answers some opcodes (see fake_resp_opcodes()),
	 *  - it emits stray bytes on the interrupt endpoint (see
	 *    btusb_recv_intr()), which desynchronizes the event framing.
	 */
	bool csr_clone;

	/*
	 * LOCAL PATCH: the event-code filter below only runs for a confirmed clone,
	 * but csr_clone is set inside btusb_setup_csr() *after* the first request has
	 * been sent, so the first event of an open is unchecked. That is what cost the
	 * run of 02:12:48: one stray 0x0c byte arrived before the command complete for
	 * 0x1001, was taken as the first byte of an event header, the 0x0e that
	 * followed became its length, and the real answer for 0x1001 was swallowed -
	 * the core timed out (-110) although the firmware had answered 1 ms after the
	 * command. bcdDevice is known at probe time, so the known fake list can turn
	 * the filter on before the first request. It only enables the filter, not the
	 * fake responses.
	 */
	bool csr_suspect;

	/* Set while btusb_setup_csr() is running a command itself, so the RX
	 * path records the firmware's real answer instead of answering the
	 * command locally; the recorded payload is then replayed for the core.
	 */
	bool prefetching;

	/* LOCAL PATCH: the prefetch is a request we run ourselves, so it waits on
	 * its own completion for its own opcode. The core's request state cannot
	 * be used here: it matches an event against hdev->sent_cmd and
	 * hdev->req_opcode, which belong to whoever called __hci_cmd_sync_*, and
	 * its only timeout is HCI_CMD_TIMEOUT (2 s), which is shorter than this
	 * clone's first reply after power-up (up to 15 s for the first reset).
	 */
	struct completion csr_done;
	__u16 wait_opcode;

	/* LOCAL PATCH: the last opcode that btusb_send_frame() answered locally
	 * (see the duplicate-answer suppression in btusb_recv_intr()). Zero when
	 * nothing was injected.
	 */
	__u16 last_injected;

	/* LOCAL PATCH: target address of the last connection command that actually
	 * went on the wire. A connection event is only genuine if its address is the
	 * one the firmware was asked to connect to; if it matches an address that was
	 * never the target of a connection command, the firmware invented it.
	 */
	__u8 csr_last_conn_addr[6];
	__u16 csr_last_conn_opcode;
	unsigned long csr_last_conn_jiffies;

	/* LOCAL PATCH: the connection handle the connection complete reported, so the
	 * handle-based events (Remote Features / Remote Extended Features) can be compared
	 * with the connection the command target was made as. Those events carry a handle,
	 * not an address.
	 */
	__u16 csr_last_conn_handle;

	/* LOCAL PATCH: whether the connection complete for the handle above has already
	 * been handed to the core. Only one connection complete can exist for a handle,
	 * so a second one for the same handle is only genuine if a connection command
	 * was sent after the first - which is why this is cleared in
	 * csr_record_conn_target().
	 */
	bool csr_conn_complete_done;

	/* LOCAL PATCH: the last command that actually went on the wire and when. A read of
	 * the peer's features is a baseband transaction: the controller has to poll the peer
	 * and the peer has to answer, which takes tens of milliseconds. So the time between
	 * this stamp and the answer is what separates a canned reply from a real exchange.
	 */
	__u16 csr_last_cmd_opcode;
	unsigned long csr_last_cmd_jiffies;

	/* LOCAL PATCH: set once the connection probe has been sent (see
	 * csr_run_conn_probe()), so a reload does not send it twice.
	 */
	bool conn_probe_ran;

	/* LOCAL PATCH: an active reference held for the clone, so the runtime PM
	 * core never drops the last one and tries to suspend it. pm_runtime_forbid()
	 * only stops *new* idle suspends; the existing reference is what actually
	 * keeps the interrupt endpoint able to complete its URBs, so it has to be
	 * held explicitly and released in btusb_disconnect() to keep the count balanced.
	 */
	bool csr_pm_hold;

	/* LOCAL PATCH: the two halves of one diagnostic work item (see
	 * csr_run_probe()). Printing the state is not enough on its own, because
	 * hci_cmd_work() runs on an ordered workqueue: if a work item there is
	 * already running, nothing else on that queue can run, so a work item we
	 * queue on it would never print either. Printing from two different queues
	 * turns the absence of a print into an answer.
	 *   sys_probe  - queued on system_wq, which cannot be blocked by anything
	 *                the Bluetooth core is doing, so it always runs;
	 *   core_probe - queued on hdev->workqueue, the very queue hci_cmd_work()
	 *                uses. If it stops printing while sys_probe still reports a
	 *                pending request, that queue is why the command never left
	 *                the wire.
	 */
	struct delayed_work sys_probe;
	struct delayed_work core_probe;

	/* How many times each probe has printed, so the last line can compare them.
	 * Counting separately matters: a queue that cannot run never counts, so the
	 * difference between the two numbers is the measurement.
	 */
	int sys_probe_rounds;
	int core_probe_rounds;

	/* Set in btusb_disconnect() so the probes stop before hdev is freed. */
	bool probe_stopped;

	bool poll_sync;
	int intr_interval;
	struct work_struct  work;
	struct work_struct  waker;
	struct delayed_work rx_work;

	struct sk_buff_head acl_q;

	struct usb_anchor deferred;
	struct usb_anchor tx_anchor;
	int tx_in_flight;
	spinlock_t txlock;

	struct usb_anchor intr_anchor;
	struct usb_anchor bulk_anchor;
	struct usb_anchor isoc_anchor;
	struct usb_anchor diag_anchor;
	struct usb_anchor ctrl_anchor;
	spinlock_t rxlock;

	struct sk_buff *evt_skb;
	struct sk_buff *acl_skb;
	struct sk_buff *sco_skb;

	struct usb_endpoint_descriptor *intr_ep;
	struct usb_endpoint_descriptor *bulk_tx_ep;
	struct usb_endpoint_descriptor *bulk_rx_ep;
	struct usb_endpoint_descriptor *isoc_tx_ep;
	struct usb_endpoint_descriptor *isoc_rx_ep;
	struct usb_endpoint_descriptor *diag_tx_ep;
	struct usb_endpoint_descriptor *diag_rx_ep;

	struct gpio_desc *reset_gpio;

	__u8 cmdreq_type;
	__u8 cmdreq;

	unsigned int sco_num;
	unsigned int air_mode;
	bool usb_alt6_packet_flow;
	int isoc_altsetting;
	int suspend_count;

	int (*recv_event)(struct hci_dev *hdev, struct sk_buff *skb);
	int (*recv_acl)(struct hci_dev *hdev, struct sk_buff *skb);
	int (*recv_bulk)(struct btusb_data *data, void *buffer, int count);

	int (*setup_on_usb)(struct hci_dev *hdev);

	int (*suspend)(struct hci_dev *hdev);
	int (*resume)(struct hci_dev *hdev);
	int (*disconnect)(struct hci_dev *hdev);

	int oob_wake_irq;   /* irq for out-of-band wake-on-bt */

	struct qca_dump_info qca_dump;
};

static void btusb_reset(struct hci_dev *hdev)
{
	struct btusb_data *data;
	int err;

	data = hci_get_drvdata(hdev);
	/* This is not an unbalanced PM reference since the device will reset */
	err = usb_autopm_get_interface(data->intf);
	if (err) {
		bt_dev_err(hdev, "Failed usb_autopm_get_interface: %d", err);
		return;
	}

	bt_dev_err(hdev, "Resetting usb device.");
	usb_queue_reset_device(data->intf);
}

static void btusb_intel_reset(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct gpio_desc *reset_gpio = data->reset_gpio;
	struct btintel_data *intel_data = hci_get_priv(hdev);

	if (intel_data->acpi_reset_method) {
		if (test_and_set_bit(INTEL_ACPI_RESET_ACTIVE, intel_data->flags)) {
			bt_dev_err(hdev, "acpi: last reset failed ? Not resetting again");
			return;
		}

		bt_dev_err(hdev, "Initiating acpi reset method");
		/* If ACPI reset method fails, lets try with legacy GPIO
		 * toggling
		 */
		if (!intel_data->acpi_reset_method(hdev)) {
			return;
		}
	}

	if (!reset_gpio) {
		btusb_reset(hdev);
		return;
	}

	/*
	 * Toggle the hard reset line if the platform provides one. The reset
	 * is going to yank the device off the USB and then replug. So doing
	 * once is enough. The cleanup is handled correctly on the way out
	 * (standard USB disconnect), and the new device is detected cleanly
	 * and bound to the driver again like it should be.
	 */
	if (test_and_set_bit(BTUSB_HW_RESET_ACTIVE, &data->flags)) {
		bt_dev_err(hdev, "last reset failed? Not resetting again");
		return;
	}

	bt_dev_err(hdev, "Initiating HW reset via gpio");
	gpiod_set_value_cansleep(reset_gpio, 1);
	msleep(100);
	gpiod_set_value_cansleep(reset_gpio, 0);
}

#define RTK_DEVCOREDUMP_CODE_MEMDUMP		0x01
#define RTK_DEVCOREDUMP_CODE_HW_ERR		0x02
#define RTK_DEVCOREDUMP_CODE_CMD_TIMEOUT	0x03

#define RTK_SUB_EVENT_CODE_COREDUMP		0x34

struct rtk_dev_coredump_hdr {
	u8 type;
	u8 code;
	u8 reserved[2];
} __packed;

static inline void btusb_rtl_alloc_devcoredump(struct hci_dev *hdev,
		struct rtk_dev_coredump_hdr *hdr, u8 *buf, u32 len)
{
	struct sk_buff *skb;

	skb = alloc_skb(len + sizeof(*hdr), GFP_ATOMIC);
	if (!skb)
		return;

	skb_put_data(skb, hdr, sizeof(*hdr));
	if (len)
		skb_put_data(skb, buf, len);

	if (!hci_devcd_init(hdev, skb->len)) {
		hci_devcd_append(hdev, skb);
		hci_devcd_complete(hdev);
	} else {
		bt_dev_err(hdev, "RTL: Failed to generate devcoredump");
		kfree_skb(skb);
	}
}

static void btusb_rtl_reset(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct gpio_desc *reset_gpio = data->reset_gpio;
	struct rtk_dev_coredump_hdr hdr = {
		.type = RTK_DEVCOREDUMP_CODE_CMD_TIMEOUT,
	};

	btusb_rtl_alloc_devcoredump(hdev, &hdr, NULL, 0);

	if (!reset_gpio) {
		btusb_reset(hdev);
		return;
	}

	/* Toggle the hard reset line. The Realtek device is going to
	 * yank itself off the USB and then replug. The cleanup is handled
	 * correctly on the way out (standard USB disconnect), and the new
	 * device is detected cleanly and bound to the driver again like
	 * it should be.
	 */
	if (test_and_set_bit(BTUSB_HW_RESET_ACTIVE, &data->flags)) {
		bt_dev_err(hdev, "last reset failed? Not resetting again");
		return;
	}

	bt_dev_err(hdev, "Reset Realtek device via gpio");
	gpiod_set_value_cansleep(reset_gpio, 1);
	msleep(200);
	gpiod_set_value_cansleep(reset_gpio, 0);
}

static void btusb_rtl_hw_error(struct hci_dev *hdev, u8 code)
{
	struct rtk_dev_coredump_hdr hdr = {
		.type = RTK_DEVCOREDUMP_CODE_HW_ERR,
		.code = code,
	};

	bt_dev_err(hdev, "RTL: hw err, trigger devcoredump (%d)", code);

	btusb_rtl_alloc_devcoredump(hdev, &hdr, NULL, 0);
}

static void btusb_qca_reset(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct gpio_desc *reset_gpio = data->reset_gpio;

	if (test_bit(BTUSB_HW_SSR_ACTIVE, &data->flags)) {
		bt_dev_info(hdev, "Ramdump in progress, defer reset");
		return;
	}

	if (reset_gpio) {
		bt_dev_err(hdev, "Reset qca device via bt_en gpio");

		/* Toggle the hard reset line. The qca bt device is going to
		 * yank itself off the USB and then replug. The cleanup is handled
		 * correctly on the way out (standard USB disconnect), and the new
		 * device is detected cleanly and bound to the driver again like
		 * it should be.
		 */
		if (test_and_set_bit(BTUSB_HW_RESET_ACTIVE, &data->flags)) {
			bt_dev_err(hdev, "last reset failed? Not resetting again");
			return;
		}

		gpiod_set_value_cansleep(reset_gpio, 0);
		msleep(200);
		gpiod_set_value_cansleep(reset_gpio, 1);

		return;
	}

	btusb_reset(hdev);
}

static u8 btusb_classify_qca_pkt_type(struct hci_dev *hdev, struct sk_buff *skb)
{
	/* Some Qualcomm controllers, e.g., QCNFA765 with WCN6855 chip, send debug
	 * packets as ACL frames with connection handle 0x2EDC. These are not real
	 * ACL packets and should be reclassified as HCI_DIAG_PKT to prevent
	 * "ACL packet for unknown connection handle 3804" errors.
	 */
	if (skb->len >= 2) {
		u16 handle = get_unaligned_le16(skb->data);

		if (handle == 0x2EDC)
			return HCI_DIAG_PKT;
	}

	/* Use default packet type for other packets */
	return hci_skb_pkt_type(skb);
}

static inline void btusb_free_frags(struct btusb_data *data)
{
	unsigned long flags;

	spin_lock_irqsave(&data->rxlock, flags);

	dev_kfree_skb_irq(data->evt_skb);
	data->evt_skb = NULL;

	dev_kfree_skb_irq(data->acl_skb);
	data->acl_skb = NULL;

	dev_kfree_skb_irq(data->sco_skb);
	data->sco_skb = NULL;

	spin_unlock_irqrestore(&data->rxlock, flags);
}

static int btusb_recv_event(struct btusb_data *data, struct sk_buff *skb)
{
	if (data->intr_interval) {
		/* Trigger dequeue immediately if an event is received */
		schedule_delayed_work(&data->rx_work, 0);
	}

	return data->recv_event(data->hdev, skb);
}

/*
 * LOCAL PATCH (instrumentation): this clone answers some commands and stays
 * silent for others. Log every command we send, the runtime-PM state of the
 * USB device at that moment, and every packet that reaches an RX endpoint,
 * with a millisecond timestamp, so the reply latency becomes measurable.
 */
static void trace_rx(struct btusb_data *data, void *buffer, int count,
		     const char *ep)
{
	struct hci_dev *hdev = data->hdev;
	unsigned char *p = buffer;

	/* LOCAL PATCH: 6 bytes were not enough to decode anything. This clone
	 * delivers one event across several URBs, so the first byte of a URB is
	 * usually payload rather than an event code. Printing 16 shows whether a
	 * URB starts at an event header at all.
	 */
	bt_dev_info(hdev,
		    "RX %u ms %s len=%d %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
		    jiffies_to_msecs(jiffies), ep, count,
		    count > 0 ? p[0] : 0, count > 1 ? p[1] : 0,
		    count > 2 ? p[2] : 0, count > 3 ? p[3] : 0,
		    count > 4 ? p[4] : 0, count > 5 ? p[5] : 0,
		    count > 6 ? p[6] : 0, count > 7 ? p[7] : 0,
		    count > 8 ? p[8] : 0, count > 9 ? p[9] : 0,
		    count > 10 ? p[10] : 0, count > 11 ? p[11] : 0,
		    count > 12 ? p[12] : 0, count > 13 ? p[13] : 0,
		    count > 14 ? p[14] : 0, count > 15 ? p[15] : 0);
}

/*
 * LOCAL PATCH: resynchronize the event stream when a CSR clone emits a byte pair
 * that looks like event code 0x01 (Inquiry Complete) but has plen != 1. A valid
 * Inquiry Complete is always 01 01. If we let 01 <wrong plen> through, hci_event.c
 * treats it as Inquiry Complete and the following bytes are misinterpreted, which
 * prevents BlueZ from creating or pairing OpenRun.
 *
 * Priority #1 for event code 0x02 is the same class of framing bug: 02 <bad plen>
 * is not a valid Inquiry Result unless plen matches 1 + num * sizeof(inquiry_info).
 * When it does not match, do not deliver the malformed event to the core. Log the
 * raw interrupt buffer plus the expected plen, then resync from the 16-byte interrupt
 * stream to a real event header, usually 0x2f Extended Inquiry Result or 0x07 Remote
 * Name when present.
 */
static bool csr_contains_openrun(const unsigned char *p, int count)
{
	static const unsigned char needle[] = {
		0x09, 0x4f, 0x70, 0x65, 0x6e, 0x52, 0x75, 0x6e
	};
	int i, j;

	for (i = 0; i + (int)sizeof(needle) <= count; i++) {
		bool found = true;

		for (j = 0; j < (int)sizeof(needle); j++) {
			if (p[i + j] != needle[j]) {
				found = false;
				break;
			}
		}

		if (found)
			return true;
	}

	return false;
}

static int csr_expected_plen_inquiry_result(const unsigned char *payload, int len)
{
	if (len <= 0)
		return -1;

	return 1 + payload[0] * (int)sizeof(struct inquiry_info);
}

static bool csr_is_resync_target_header(const unsigned char *p, int n)
{
	if (n < 2)
		return false;

	switch (p[0]) {
	case HCI_EV_EXTENDED_INQUIRY_RESULT:
		/* num + fixed EIR fields, with a variable EIR payload. */
		return p[1] >= 15;
	case HCI_EV_REMOTE_NAME:
		/* status + bdaddr + at least one name byte. */
		return p[1] >= 7;
	default:
		return false;
	}
}

static int csr_find_real_event_header(const unsigned char *p, int n, int start)
{
	int i;

	for (i = max_t(int, start, 0); i + 1 < n; i++) {
		if (p[i] != 0x02 && csr_is_resync_target_header(p + i, n - i))
			return i;
	}

	return -1;
}

static bool csr_resync_bad_inquiry_complete(struct btusb_data *data,
                                           struct sk_buff *skb,
                                           void **buffer,
                                           int *count)
{
	struct hci_event_hdr *hdr = hci_event_hdr(skb);
	unsigned char raw[64];
	unsigned char *p = (unsigned char *)buffer;
	int n = *count;
	int raw_len;
	int header_len;
	int payload_len;
	int next;
	int skip;

	if (!data->csr_clone && !data->csr_suspect)
		return false;

	if (skb->len != HCI_EVENT_HDR_SIZE)
		return false;

	if (hdr->evt != HCI_EV_INQUIRY_COMPLETE)
		return false;

	if (hdr->plen == 1)
		return false;

	raw_len = min_t(int, HCI_EVENT_HDR_SIZE + n, (int)sizeof(raw));
	header_len = min_t(int, raw_len, (int)skb->len);
	payload_len = raw_len - header_len;

	memcpy(raw, skb->data, header_len);
	if (payload_len > 0)
		memcpy(raw + header_len, p, payload_len);

	bt_dev_warn(data->hdev,
		    "CSR clone: true event header 0x01 plen=%d raw buffer %d%s",
		    hdr->plen,
		    raw_len,
		    csr_contains_openrun(raw, raw_len) ? " contains OpenRun" : "");
	print_hex_dump(KERN_INFO, "CSR clone raw: ", DUMP_PREFIX_OFFSET, 16, 1,
		       raw, raw_len, false);

	next = hdr->plen;

	if (next + 1 < n && p[next] == HCI_EV_INQUIRY_COMPLETE && p[next + 1] == 1) {
		bt_dev_warn(data->hdev,
			    "CSR clone: resync to legitimate Inquiry Complete at byte %d",
			    next);
		*buffer += next;
		*count -= next;
		return true;
	}

	skip = min_t(int, n, next);
	*buffer += skip;
	*count -= skip;
	return true;
}

static bool csr_resync_bad_inquiry_result_event(struct btusb_data *data,
                                               struct sk_buff *skb,
                                               void **buffer,
                                               int *count)
{
	struct hci_event_hdr *hdr = hci_event_hdr(skb);
	unsigned char raw[64];
	unsigned char *p = (unsigned char *)buffer;
	int n = *count;
	int expected;
	int raw_len;
	int header_len;
	int payload_len;
	int next;
	int idx;
	int skip;

	if (!data->csr_clone && !data->csr_suspect)
		return false;

	if (skb->len != HCI_EVENT_HDR_SIZE)
		return false;

	if (hdr->evt != HCI_EV_INQUIRY_RESULT)
		return false;

	expected = csr_expected_plen_inquiry_result(p, n);
	if (expected < 0)
		expected = 1 + (int)sizeof(struct inquiry_info);

	if (hdr->plen == expected)
		return false;

	raw_len = min_t(int, HCI_EVENT_HDR_SIZE + n, (int)sizeof(raw));
	header_len = min_t(int, raw_len, (int)skb->len);
	payload_len = raw_len - header_len;

	memcpy(raw, skb->data, header_len);
	if (payload_len > 0)
		memcpy(raw + header_len, p, payload_len);

	bt_dev_warn(data->hdev,
		    "CSR clone: true event header 0x02 plen=%d is not a valid Inquiry Result; raw buffer %d%s",
		    hdr->plen,
		    raw_len,
		    csr_contains_openrun(raw, raw_len) ? " contains OpenRun" : "");
	bt_dev_warn(data->hdev,
		    "CSR clone: true event header 0x02 expected_plen=%d",
		    expected);
	print_hex_dump(KERN_INFO, "CSR clone raw: ", DUMP_PREFIX_OFFSET, 16, 1,
		       raw, raw_len, false);

	next = hdr->plen;

	idx = csr_find_real_event_header(p, n, next);
	if (idx < 0)
		idx = csr_find_real_event_header(p, n, 0);

	if (idx >= 0) {
		bt_dev_warn(data->hdev,
			    "CSR clone: dropping malformed 02 event and resync to real event header 0x%02x at byte %d",
			    p[idx],
			    idx);
		*buffer += idx;
		*count -= idx;
		return true;
	}

	bt_dev_warn(data->hdev,
		    "CSR clone: dropping malformed 02 event; no real 0x2f/0x07 header in this interrupt buffer");

	skip = min_t(int, n, next);
	*buffer += skip;
	*count -= skip;
	return true;
}

static bool csr_resync_bad_event_plen(struct btusb_data *data, void **buffer,
				      int *count)
{
	unsigned char *p = (unsigned char *)*buffer;
	int n = *count;
	int next;
	int skip;
	int idx;

	if (n < 2)
		return false;

	if (p[0] == HCI_EV_INQUIRY_RESULT) {
		int expected = csr_expected_plen_inquiry_result(p + 2, n - 2);

		if (expected < 0)
			expected = 1 + (int)sizeof(struct inquiry_info);

		if (p[1] == expected)
			return false;

		bt_dev_warn(data->hdev,
			    "CSR clone: event code 0x02 plen=%d is not a valid Inquiry Result; raw buffer %d",
			    p[1],
			    n);
		bt_dev_warn(data->hdev,
			    "CSR clone: event code 0x02 expected_plen=%d%s",
			    expected,
			    csr_contains_openrun(p, n) ? " contains OpenRun" : "");
		print_hex_dump(KERN_INFO, "CSR clone raw: ", DUMP_PREFIX_OFFSET, 16, 1,
			       p, n, false);

		next = 2 + p[1];

		if (next < n) {
			idx = csr_find_real_event_header(p, n, next);
			if (idx < 0)
				idx = csr_find_real_event_header(p, n, 0);
		} else {
			idx = csr_find_real_event_header(p, n, 2);
		}

		if (idx > 0) {
			bt_dev_warn(data->hdev,
				    "CSR clone: dropping malformed 02 event and resync to real event header 0x%02x at byte %d",
				    p[idx],
				    idx);
			*buffer += idx;
			*count -= idx;
			return true;
		}

		bt_dev_warn(data->hdev,
			    "CSR clone: dropping malformed 02 event; no real 0x2f/0x07 header in this interrupt buffer");

		skip = min_t(int, n, next);
		*buffer += skip;
		*count -= skip;
		return true;
	}

	return false;
}

static const char *pm_state(struct btusb_data *data)
{
	switch (data->udev->dev.power.runtime_status) {
	case RPM_ACTIVE:
		return "active";
	case RPM_SUSPENDED:
		return "suspended";
	default:
		return "other";
	}
}

/*
 * LOCAL PATCH: event codes this CSR clone can actually produce. The clone has
 * been seen sending a single stray byte on the interrupt endpoint - a 0x0c
 * (Remote Version Information) event code with no payload - immediately before
 * a perfectly good command-complete. The core reads the next byte as that
 * event's plen, then waits for a payload that never arrives and swallows the
 * real command-complete behind it, so the sync call times out and the framing
 * stays broken for everything that follows. Only accept at the start of an
 * event the codes this firmware is capable of emitting; drop anything else.
 */
static bool csr_event_code_ok(unsigned char code)
{
	switch (code) {
	case HCI_EV_INQUIRY_COMPLETE:
	case HCI_EV_INQUIRY_RESULT:
	case HCI_EV_INQUIRY_RESULT_WITH_RSSI:
	case HCI_EV_CONN_COMPLETE:
	case HCI_EV_CONN_REQUEST:
	case HCI_EV_DISCONN_COMPLETE:
	case HCI_EV_AUTH_COMPLETE:
	case HCI_EV_ENCRYPT_CHANGE:
	case HCI_EV_CMD_COMPLETE:
	case HCI_EV_CMD_STATUS:
	case HCI_EV_HARDWARE_ERROR:
	case HCI_EV_MODE_CHANGE:
	case HCI_EV_NUM_COMP_PKTS:
	case HCI_EV_REMOTE_NAME:
	case HCI_EV_EXTENDED_INQUIRY_RESULT:
	case HCI_EV_LE_META:
	/*
	 * LOCAL PATCH: the codes above are the ones the earlier traces actually
	 * saw. This block adds the codes the clone needs in order to complete a
	 * pairing, which the traces have never shown because they were dropped
	 * here before reaching the core.
	 *
	 * HCI_EV_REMOTE_FEATURES (0x0b) is the answer to HCI_OP_READ_REMOTE_FEATURES
	 * (0x041b). On 2026-11-06 23:13:31 the clone sent a spec-correct event for
	 * handle 0x0081 - "0b 0b 00 81 00 ff fe 8f fe db ff 5b 87", plen 11 =
	 * status + handle + 8 feature bytes - and this whitelist dropped it, so the
	 * pairing stalled with org.bluez.Error.InProgress. It is the only event code
	 * seen on the wire that was rejected purely because it was not listed here.
	 *
	 * The pairing events are listed so that if the clone can pair at all its
	 * requests are not silently eaten.
	 *
	 * Deliberately NOT added: HCI_EV_REMOTE_VERSION (0x0c). The clone emits a
	 * stray 0x0c with no payload, which would otherwise swallow the following
	 * command complete - the original reason this whitelist exists.
	 *
	 * Same for HCI_EV_ROLE_CHANGE (0x12): on 2026-11-09 19:08:11 a lone
	 * "12" byte arrived just before the Command Complete for LE Read Buffer
	 * Size (0x2001). Accepting it made plen=0x0e and ate that completion, so
	 * init timed out and hci0 stayed DOWN. Keep dropping bare 0x12.
	 *
	 * Max Slots Change is HCI event 0x1b (Bluetooth Core Spec). Linux headers
	 * do not define HCI_EV_MAX_SLOTS, but the clone emits a correct frame after
	 * a successful Create Connection - seen 2026-11-09 19:00:03 as
	 * "1b 03 81 00 05" (handle 0x0081, max slots 5). Dropping that byte left
	 * "03 81 00 05" as a fake Conn Complete with plen 0x81, which swallowed the
	 * Command Status for 0x041c and the Remote Ext Features event, then timed
	 * out the pairing. Accept 0x1b only when csr_event_start_ok() has seen
	 * plen==3 (see below); a lone 0x1b is treated as stray.
	 */
	case 0x1b: /* Max Slots Change - not named in hci.h; plen checked below */
	case HCI_EV_REMOTE_FEATURES:
	/*
	 * HCI_EV_REMOTE_EXT_FEATURES (0x23) is the answer to
	 * HCI_OP_READ_REMOTE_EXT_FEATURES (0x041c). On 2026-11-07 00:59:34 the clone sent
	 * a spec-correct frame for handle 0x0081 - "23 0d 00 81 00 01 02 03 00 00 00 00 00 00 00",
	 * plen 13 = exactly sizeof(struct hci_ev_remote_ext_features) - and it was dropped
	 * here only because this code was not listed, so pairing stalled. Add it.
	 */
	case HCI_EV_REMOTE_EXT_FEATURES:
	case HCI_EV_PIN_CODE_REQ:
	case HCI_EV_LINK_KEY_REQ:
	case HCI_EV_LINK_KEY_NOTIFY:
	case HCI_EV_KEY_REFRESH_COMPLETE:
	case HCI_EV_IO_CAPA_REQUEST:
	case HCI_EV_IO_CAPA_REPLY:
	case HCI_EV_USER_CONFIRM_REQUEST:
	case HCI_EV_USER_PASSKEY_REQUEST:
	case HCI_EV_REMOTE_OOB_DATA_REQUEST:
	case HCI_EV_SIMPLE_PAIR_COMPLETE:
		return true;
	}

	return false;
}

/*
 * LOCAL PATCH: decide whether the first byte of a new interrupt URB may start an
 * event. csr_event_code_ok() alone is not enough for codes the firmware also
 * emits as single stray bytes: accepting those without a plausible plen desyncs
 * the stream (see the 0x12 / 0x2001 failure of 19:08:11).
 *
 * Max Slots Change (0x1b) always has plen 3 (handle + LMP max slots). Require at
 * least the two-byte header and that plen before committing.
 */
static bool csr_event_start_ok(const unsigned char *p, int n)
{
	if (n < 1 || !csr_event_code_ok(p[0]))
		return false;

	if (p[0] == 0x1b) {
		if (n < 2 || p[1] != 3)
			return false;
	}

	return true;
}

/*
 * LOCAL PATCH: the payload size the kernel struct gives each connection event, so a
 * frame that claims a different one can be recognised as garbled rather than as a real
 * event with an unusual length. HCI_EV_CMD_STATUS and HCI_EV_REMOTE_NAME are here
 * because they are the other two codes that appear inside the frames, and for
 * 0x07 the kernel size is 255, which is the value the clone writes as its length.
 */
static int csr_expected_plen_conn_event(unsigned char code)
{
	switch (code) {
	case HCI_EV_CONN_COMPLETE:
		return (int)sizeof(struct hci_ev_conn_complete);
	case HCI_EV_CONN_REQUEST:
		return (int)sizeof(struct hci_ev_conn_request);
	case HCI_EV_DISCONN_COMPLETE:
		return (int)sizeof(struct hci_ev_disconn_complete);
	case HCI_EV_CMD_STATUS:
		return (int)sizeof(struct hci_ev_cmd_status);
	case HCI_EV_REMOTE_NAME:
		return (int)sizeof(struct hci_ev_remote_name);
	}

	return -1;
}

/*
 * LOCAL PATCH: rebuild one record from inside the clone's buffer and hand it to the
 * core as its own event.
 *
 * A record is usable only when it has its event code, the length the kernel struct for
 * that code has, and all of that content inside the frame. In the frames seen so far
 * exactly one record qualifies - "0f 04 00 01 19 04", Command Status with status 0x00
 * for the command the core had just sent - because the record for the name is truncated
 * to its header and the address.
 *
 * This is a behaviour change on data the core would otherwise reject, so it is gated by
 * a module parameter. It does not invent an answer: the caller still has no Remote Name
 * event to wait for, but the core can finish the request with status 0x00 instead of
 * failing on "unexpected event 0x03 length: 129 > 11", which turns org.bluez.Error.InProgress
 * into a clean timeout. Default on so the next run exercises it.
 */
static bool reconstruct_records = true;
module_param(reconstruct_records, bool, 0644);

static bool csr_reconstruct_record(struct btusb_data *data, const unsigned char *p,
				   int n, int i)
{
	struct sk_buff *nskb;
	unsigned char code = p[i];
	int expected = csr_expected_plen_conn_event(code);
	int plen;

	/* Byte 0 is the record the parser already read as this frame, so it is not a
	 * record inside the buffer.
	 */
	if (i == 0 || expected < 0 || !csr_event_code_ok(code))
		return false;

	plen = p[i + 1];

	if (plen != expected || i + 2 + plen > n)
		return false;

	/* A record boundary is only real if the byte after the content starts another
	 * record or the buffer ends there.
	 */
	if (i + 2 + plen < n && !csr_event_code_ok(p[i + 2 + plen]))
		return false;

	nskb = bt_skb_alloc(HCI_EVENT_HDR_SIZE + plen, GFP_ATOMIC);
	if (!nskb)
		return false;

	hci_skb_pkt_type(nskb) = HCI_EVENT_PKT;
	skb_put_data(nskb, p + i, HCI_EVENT_HDR_SIZE + plen);

	bt_dev_warn(data->hdev,
		    "CSR clone: rebuilding record at byte %d as event 0x%02x plen=%d for the core",
		    i, code, plen);

	btusb_recv_event(data, nskb);

	return true;
}

/*
 * LOCAL PATCH: what a garbled connection frame actually holds.
 *
 * Every genuine connection frame seen so far has had the spec plen for its code:
 * "03 0b 00 81 00 1d fa d9 e1 f5 a8 01 00" is event 0x03 with plen 11 = exactly
 * sizeof(struct hci_ev_conn_complete). The frames that are not like that claim a
 * plen far larger than the code can have (129 for event 0x03), so they are not
 * one event - they are the clone's buffer read as if its first record were the
 * whole frame.
 *
 * Reading the buffer as records gives three, and each is spec-shaped on its own:
 *
 *   03 81 | 00 05                  event 0x03, length byte 0x81 (129), then status 0x00
 *                                  and 0x05 where the handle should have been
 *   0f 04 | 00 01 19 04            Command Status, status 0x00, ncmd 0x01, opcode 0x0419
 *   07 ff | 0c 1d fa d9 e1 f5 a8   event 0x07, length byte 0xff (the kernel struct size
 *                                  for a Remote Name answer), then status 0x0c and the
 *                                  peer address, with no name bytes after it
 *
 * So the clone does answer HCI_OP_REMOTE_NAME_REQ, and its status for 0x0419 is 0x00:
 * the frame is not an error reply, it is a buffer whose record lengths were written as
 * the buffer size instead of the record size. The four garbled frames in boot -1
 * (13:24:38, 13:25:16, 13:26:20, 13:57:04) are byte-identical, which is why they read
 * as one template the firmware fills rather than as four separate mistakes.
 *
 * The status bytes are still printed without kernel names: the header defines neither
 * 0x04 nor 0x0c for these positions, and 0x07 is defined as Memory Exceeded, so a name
 * here would be a guess. Note 0x0c is HCI_ERROR_COMMAND_DISALLOWED only if 07 ff is read
 * as a record header, which is the reading above, not a proof.
 *
 * Such a frame is dropped rather than handed to the core as one event, because the core
 * can only print the length error. What it contains that is usable is rebuilt record by
 * record by csr_reconstruct_record(), which is where the behaviour choice lives.
 */
static bool csr_decode_garbled_conn_frame(struct btusb_data *data, const struct sk_buff *skb)
{
	const struct hci_event_hdr *hdr = (void *)skb->data;
	const unsigned char *p = (const unsigned char *)skb->data;
	int n = skb->len;
	int expected = csr_expected_plen_conn_event(hdr->evt);
	int i;

	if (expected < 0 || hdr->plen == expected)
		return false;

	/* The whole payload this time: the interesting bytes are the echoed command,
	 * which is not at a fixed offset, so the 24-byte cap in
	 * csr_log_event_frame() is not enough to see the header and the status byte.
	 */
	print_hex_dump(KERN_WARNING, "CSR garbled frame: ", DUMP_PREFIX_OFFSET, 16, 1,
		       p, min_t(int, n, HCI_MAX_EVENT_SIZE), false);

	if (!data->csr_last_cmd_opcode) {
		bt_dev_warn(data->hdev,
			    "CSR clone: garbled frame for event 0x%02x plen=%d expected=%d names no command",
			    hdr->evt, hdr->plen, expected);
		return true;
	}

	for (i = 0; i + 2 <= n; i++) {
		/* Any byte can be the start of a record, so a record is only handed over
		 * when it is complete inside this buffer (csr_reconstruct_record() checks
		 * that), which is why the zero padding cannot produce a false one.
		 */
		if (reconstruct_records)
			csr_reconstruct_record(data, p, n, i);

		if (p[i] == (data->csr_last_cmd_opcode & 0xff) &&
		    p[i + 1] == (data->csr_last_cmd_opcode >> 8)) {
			/* Which byte is the status depends on which layout is read: the kernel's
			 * hci_ev_cmd_status is status,ncmd,opcode, while the spec order is
			 * ncmd,opcode,status. The frames that do match a request are only
			 * consistent with the kernel order (0f 04 00 01 06 04 is status 0x00
			 * ncmd 0x01 opcode 0x0406), so the kernel order is printed first. The
			 * byte after the opcode is not a status byte in this frame: 0x07 is
			 * HCI_EV_REMOTE_NAME, the answer to HCI_OP_REMOTE_NAME_REQ, and the 0xff
			 * after it is that record's length byte, so this buffer reads as three
			 * records where the parser saw one oversized event
			 * and the matching frames do not.
			 */
			bt_dev_warn(data->hdev,
				    "CSR clone: garbled frame for event 0x%02x plen=%d expected=%d echoes cmd 0x%04x at byte %d; kernel status 0x%02x, spec-order status 0x%02x",
				    hdr->evt, hdr->plen, expected,
				    data->csr_last_cmd_opcode, i,
				    i >= 2 ? p[i - 2] : 0,
				    i + 2 < n ? p[i + 2] : 0);
			return true;
		}
	}

	bt_dev_warn(data->hdev,
		    "CSR clone: garbled frame for event 0x%02x plen=%d expected=%d does not echo the last command 0x%04x",
		    hdr->evt, hdr->plen, expected, data->csr_last_cmd_opcode);

	return true;
}

/*
 * LOCAL PATCH: duplicate connection complete, dropped on the RX side. Only one
 * connection complete can exist for a handle. This can only fire for a frame that has
 * the spec length for its code, because a garbled frame is decoded and dropped before
 * it reaches here - and that is what the attempts on 2026-11-07 showed: the second frame
 * for handle 0x0081 was the 129-byte buffer, not a second well-formed connection
 * complete, so this counter stayed at 0 and hci_conn_complete() ran once per attempt.
 * The local abort (reason 0x16) in those traces came 30 s after the attempt, which is
 * the caller timing out and closing the connection, not the core reacting to a double
 * delivery. The send policy is left alone; a duplicate is simply not handed over.
 */
static bool csr_is_duplicate_conn_complete(struct btusb_data *data, const struct sk_buff *skb)
{
	const struct hci_event_hdr *hdr = (void *)skb->data;
	const unsigned char *p = (const unsigned char *)skb->data;
	int hoff;
	__u16 handle;

	if (hdr->evt == HCI_EV_CONN_COMPLETE) {
		if (hdr->plen != (int)sizeof(struct hci_ev_conn_complete))
			return false;

		hoff = HCI_EVENT_HDR_SIZE + 1;
	} else if (hdr->evt == HCI_EV_LE_META) {
		unsigned char sub = p[HCI_EVENT_HDR_SIZE];

		if (sub != HCI_EV_LE_CONN_COMPLETE &&
		    sub != HCI_EV_LE_ENHANCED_CONN_COMPLETE)
			return false;

		if (hdr->plen != 1 + (int)sizeof(struct hci_ev_le_conn_complete) &&
		    hdr->plen != 1 + (int)sizeof(struct hci_ev_le_enh_conn_complete))
			return false;

		hoff = HCI_EVENT_HDR_SIZE + 2;
	} else {
		return false;
	}

	if (skb->len < hoff + 2)
		return false;

	handle = p[hoff] | (p[hoff + 1] << 8);

	if (!data->csr_conn_complete_done) {
		data->csr_conn_complete_done = true;
		return false;
	}

	bt_dev_warn(data->hdev,
		    "CSR clone: connection complete for handle 0x%04x was already delivered; this one for the same handle has no connection command behind it, so it is dropped here instead of reaching hci_conn_complete()",
		    handle);

	return true;
}

/*
 * LOCAL PATCH: is this the event the prefetch is waiting for? The prefetch sends
 * its own command, so the core has no request outstanding for its opcode and
 * would only print "unexpected event for opcode 0x%4.4x" if it saw the answer.
 * The answer must therefore be taken here, where it also fills the cache the
 * core is replayed from later.
 */
static bool csr_event_is_waited(struct btusb_data *data, const struct sk_buff *skb)
{
	const struct hci_event_hdr *hdr = (void *)skb->data;

	if (hci_skb_pkt_type(skb) != HCI_EVENT_PKT ||
	    skb->len < HCI_EVENT_HDR_SIZE)
		return false;

	if (hdr->evt == HCI_EV_CMD_COMPLETE) {
		const struct hci_ev_cmd_complete *cc;

		if (skb->len < HCI_EVENT_HDR_SIZE + sizeof(*cc))
			return false;

		cc = (void *)(skb->data + HCI_EVENT_HDR_SIZE);
		return le16_to_cpu(cc->opcode) == data->wait_opcode;
	}

	if (hdr->evt == HCI_EV_CMD_STATUS) {
		const struct hci_ev_cmd_status *cs;

		if (skb->len < HCI_EVENT_HDR_SIZE + sizeof(*cs))
			return false;

		cs = (void *)(skb->data + HCI_EVENT_HDR_SIZE);
		return le16_to_cpu(cs->opcode) == data->wait_opcode;
	}

	return false;
}

/*
 * LOCAL PATCH: log a complete event frame with its payload. The RX lines alone cannot
 * show the event boundaries, because this clone delivers one event across several URBs:
 * the 16+16+16+11 group that summed to 59 is exactly one 0x3e frame with plen 57, so the
 * bytes that looked like stray event codes were payload. Dumping the frame at the moment
 * it is complete is the only way to see whether a connection event (0x03/0x04/0x05) is
 * ever produced, and whether the codes csr_event_code_ok() accepts are codes hci_event.c
 * can interpret. The counter caps the LE meta dumps so a scan does not flood the log.
 */
static bool log_events = true;
module_param(log_events, bool, 0644);

static int csr_le_meta_dumps;

/*
 * LOCAL PATCH: record which device a connection command asked for, so a later
 * connection event can be compared with it. Only commands that actually go on
 * the wire are recorded, so a replayed cache answer cannot create a target.
 */
static void csr_record_conn_target(struct btusb_data *data, __u16 opcode,
				   const unsigned char *param, int plen)
{
	if (plen < 6)
		return;

	memcpy(data->csr_last_conn_addr, param, 6);
	data->csr_last_conn_opcode = opcode;
	data->csr_last_conn_jiffies = jiffies;

	/* A new connection command starts a new connection, so the next connection
	 * complete for this handle is allowed to reach the core.
	 */
	data->csr_conn_complete_done = false;
}

/*
 * LOCAL PATCH: the decisive connection probe, declared here because
 * csr_log_conn_provenance() reports its result. It sends
 * HCI_OP_LE_EXT_CREATE_CONN (0x2043) to an address that no device can have, so a
 * connection event naming that address can only have been invented by the
 * firmware. The send itself is csr_run_conn_probe(), further down.
 */
static bool csr_conn_probe;

static const u8 csr_probe_peer[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

/*
 * LOCAL PATCH: connection provenance. A connection event is only meaningful if
 * it reports the device a connection command was sent to. If an event reports an
 * address that was never the target of a connection command, the firmware made it
 * up (or is echoing the last thing it saw on the air, which is what the recurring
 * 0x2f frames look like). The address is searched anywhere in the payload rather
 * than read at a fixed offset, so a mis-framed frame can still be compared.
 */
static void csr_log_conn_provenance(struct btusb_data *data, const struct sk_buff *skb)
{
	const struct hci_event_hdr *hdr = (void *)skb->data;
	const unsigned char *p = (const unsigned char *)skb->data;
	int n = skb->len;
	int expected = 0;
	int i, match = -1;
	const unsigned char *a;

	if (hdr->evt == HCI_EV_LE_META) {
		/* The LE meta event is 0x3e with a subevent byte after the header,
		 * so its payload length counts that byte. Only the two connection
		 * subevents are compared with the command target.
		 */
		if (n < HCI_EVENT_HDR_SIZE + 1)
			return;

		switch (p[HCI_EVENT_HDR_SIZE]) {
		case HCI_EV_LE_CONN_COMPLETE:
			expected = 1 + sizeof(struct hci_ev_le_conn_complete);
			break;
		case HCI_EV_LE_ENHANCED_CONN_COMPLETE:
			expected = 1 + sizeof(struct hci_ev_le_enh_conn_complete);
			break;
		default:
			return;
		}
	} else {
		switch (hdr->evt) {
		case HCI_EV_CONN_COMPLETE:
			expected = sizeof(struct hci_ev_conn_complete);
			break;
		case HCI_EV_CONN_REQUEST:
			expected = sizeof(struct hci_ev_conn_request);
			break;
		case HCI_EV_DISCONN_COMPLETE:
			expected = sizeof(struct hci_ev_disconn_complete);
			break;
		case HCI_EV_REMOTE_FEATURES:
			expected = sizeof(struct hci_ev_remote_features);
			break;
		case HCI_EV_REMOTE_EXT_FEATURES:
			expected = sizeof(struct hci_ev_remote_ext_features);
			break;
		default:
			return;
		}
	}

	/* LOCAL PATCH: the two feature events name a connection handle, not an address, so
	 * the address search below only finds the handle bytes - the "addr
	 * 00:81:00:ff:fe:8f" line at 00:59:34 is handle 0x0081 followed by the feature
	 * bytes. Record the handle the connection complete reports, compare the feature
	 * events with it, and print how long the answer took: on that same trace the answer
	 * for 0x041b arrived 1 ms after the command, which no transaction with a peer can
	 * do, so the handle was right but the features were not the peer's.
	 */
	if (n >= HCI_EVENT_HDR_SIZE + 3) {
		int hoff = HCI_EVENT_HDR_SIZE + (hdr->evt == HCI_EV_LE_META ? 2 : 1);
		__u16 handle = p[hoff] | (p[hoff + 1] << 8);

		if (hdr->evt == HCI_EV_CONN_COMPLETE ||
		    (hdr->evt == HCI_EV_LE_META &&
		     (p[HCI_EVENT_HDR_SIZE] == HCI_EV_LE_CONN_COMPLETE ||
		      p[HCI_EVENT_HDR_SIZE] == HCI_EV_LE_ENHANCED_CONN_COMPLETE)))
			data->csr_last_conn_handle = handle;

		if (hdr->evt == HCI_EV_REMOTE_FEATURES ||
		    hdr->evt == HCI_EV_REMOTE_EXT_FEATURES) {
			unsigned int ms = data->csr_last_cmd_opcode ?
				jiffies_to_msecs(jiffies - data->csr_last_cmd_jiffies) : 0;

			bt_dev_warn(data->hdev,
				    "CSR clone: event 0x%02x status 0x%02x handle 0x%04x %s; answered %u ms after cmd 0x%04x%s",
				    hdr->evt, p[HCI_EVENT_HDR_SIZE], handle,
				    handle == data->csr_last_conn_handle ?
					    "handle is the connection made for the command target" :
					    "handle is not the connection made for the command target",
				    ms, data->csr_last_cmd_opcode,
				    data->csr_last_cmd_opcode && ms < 5 ?
					    " (too fast for a transaction with a peer, so this is a canned answer)" :
				    "");

			return;
		}
	}

	if (!data->csr_last_conn_opcode)
		return;

	for (i = 0; i + 6 <= n; i++) {
		if (!memcmp(p + i, data->csr_last_conn_addr, 6)) {
			match = i;
			break;
		}
	}

	/* LOCAL PATCH: print the address the event reports, not the offset where it
	 * was found, so a mis-framed frame can still be read. When the probe is the
	 * target, any match is an invention: no device exists at that address, so
	 * nothing on the air can name it.
	 */
	a = match >= 0 ? p + match : p + HCI_EVENT_HDR_SIZE +
		(hdr->evt == HCI_EV_LE_META ? 1 : 0);

	bt_dev_warn(data->hdev,
		    "CSR clone: event 0x%02x plen=%d expected=%d %s; addr %02x:%02x:%02x:%02x:%02x:%02x %s%s last conn cmd 0x%04x (%u ms ago)",
		    hdr->evt, hdr->plen, expected,
		    hdr->plen == expected ? "plen ok" : "plen mismatch",
		    a[0], a[1], a[2], a[3], a[4], a[5],
		    match >= 0 ? "matches the command target" : "does NOT match",
		    match >= 0 && !memcmp(p + match, csr_probe_peer, 6) ?
			" [PROBE ADDRESS - cannot be genuine]" : "",
		    data->csr_last_conn_opcode,
		    jiffies_to_msecs(jiffies - data->csr_last_conn_jiffies));
}

static void csr_log_event_frame(struct btusb_data *data, const struct sk_buff *skb)
{
	const struct hci_event_hdr *hdr = (void *)skb->data;
	int n = min_t(int, skb->len, 24);

	if (!log_events)
		return;

	switch (hdr->evt) {
	case HCI_EV_CONN_COMPLETE:
	case HCI_EV_CONN_REQUEST:
	case HCI_EV_DISCONN_COMPLETE:
	case HCI_EV_AUTH_COMPLETE:
	case HCI_EV_ENCRYPT_CHANGE:
	case HCI_EV_MODE_CHANGE:
	case HCI_EV_HARDWARE_ERROR:
	case HCI_EV_REMOTE_FEATURES:
	case HCI_EV_REMOTE_EXT_FEATURES:
		bt_dev_warn(data->hdev, "CSR clone: connection event code=0x%02x plen=%d len=%d",
			    hdr->evt, hdr->plen, skb->len);
		print_hex_dump(KERN_WARNING, "CSR conn event: ", DUMP_PREFIX_OFFSET, 16, 1,
			       skb->data, n, false);
		csr_log_conn_provenance(data, skb);
		break;
	case HCI_EV_LE_META:
		/* The connection subevents are the ones the probe can be checked
		 * against, so provenance is printed even after the dump cap is
		 * reached - a scan can use up all 20 dumps before a connection.
		 */
		csr_log_conn_provenance(data, skb);

		if (csr_le_meta_dumps++ < 20) {
			bt_dev_info(data->hdev, "CSR clone: LE meta event plen=%d len=%d",
				    hdr->plen, skb->len);
			print_hex_dump(KERN_INFO, "CSR LE meta: ", DUMP_PREFIX_OFFSET, 16, 1,
				       skb->data, n, false);
		}
		break;
	}
}

/*
 * LOCAL PATCH: duplicate answers. When btusb_send_frame() answers an opcode locally, the
 * core is told the command finished at that moment; if the firmware answers the same
 * opcode afterwards, hci_rx_evt() prints "unexpected event for opcode 0x%04x". That is
 * cosmetic - handle_cmd_cnt_and_timer() sets cmd_cnt to 1 rather than decrementing it, so
 * the clone is not credited twice - but it makes the trace hard to read, and it hands the
 * core a reply for a request it no longer holds. Suppress it here, on the RX side, rather
 * than changing what btusb_send_frame() does: remember the last opcode we injected and
 * drop the answer that matches it.
 */
static bool suppress_dupes = true;
module_param(suppress_dupes, bool, 0644);

static bool csr_is_duplicate_answer(struct btusb_data *data, const struct sk_buff *skb)
{
	const struct hci_event_hdr *hdr = (void *)skb->data;
	__u16 opcode;

	if (!data->last_injected || hci_skb_pkt_type(skb) != HCI_EVENT_PKT)
		return false;

	if (hdr->evt == HCI_EV_CMD_COMPLETE) {
		const struct hci_ev_cmd_complete *cc;

		if (skb->len < HCI_EVENT_HDR_SIZE + sizeof(*cc))
			return false;

		cc = (void *)(skb->data + HCI_EVENT_HDR_SIZE);
		opcode = le16_to_cpu(cc->opcode);
	} else if (hdr->evt == HCI_EV_CMD_STATUS) {
		const struct hci_ev_cmd_status *cs;

		if (skb->len < HCI_EVENT_HDR_SIZE + sizeof(*cs))
			return false;

		cs = (void *)(skb->data + HCI_EVENT_HDR_SIZE);
		opcode = le16_to_cpu(cs->opcode);
	} else {
		return false;
	}

	return opcode == data->last_injected;
}

/*
 * LOCAL PATCH: the contradiction in the trace of 11:56:28 - the clone answered
 * 0x0c1a three milliseconds after it was sent, but two seconds later the caller
 * for that opcode got -110 and hdev->sent_cmd still held 0x2005. Only hci_rx_evt()
 * completes a request and it matches the event against hdev->req_skb, so a reply
 * whose opcode is not the pending request can never wake the caller. Printing the
 * reply opcode next to what the core thinks is pending is the measurement that
 * settles it: if they differ, the reply for this command was not the one a caller
 * was waiting for, which is what "command 0x2005 tx timeout" means.
 *
 * This runs from an URB completion, so hdev_lock() cannot be taken; the values are
 * an unlocked snapshot. It only prints for command complete/status, so an idle scan
 * (LE meta events) stays quiet.
 */
static void csr_check_request_match(struct btusb_data *data, const struct sk_buff *skb)
{
	struct hci_dev *hdev = data->hdev;
	const struct hci_event_hdr *hdr = (void *)skb->data;
	__u16 opcode = 0;
	__u8 ncmd = 0;
	__u8 status = 0;
	__u16 req_opcode = 0;

	if (hci_skb_pkt_type(skb) != HCI_EVENT_PKT || skb->len < HCI_EVENT_HDR_SIZE)
		return;

	if (hdr->evt == HCI_EV_CMD_COMPLETE) {
		const struct hci_ev_cmd_complete *cc;

		if (skb->len < HCI_EVENT_HDR_SIZE + sizeof(*cc))
			return;

		cc = (void *)(skb->data + HCI_EVENT_HDR_SIZE);
		opcode = le16_to_cpu(cc->opcode);
		ncmd = cc->ncmd;

		/* The byte after the opcode is the return status, and that is what the
		 * core turns into a mgmt status - which is where the 0x05 at 11:56:30
		 * would have come from.
		 */
		if (skb->len >= HCI_EVENT_HDR_SIZE + sizeof(*cc) + 1)
			status = *((__u8 *)(skb->data + HCI_EVENT_HDR_SIZE + sizeof(*cc)));
	} else if (hdr->evt == HCI_EV_CMD_STATUS) {
		const struct hci_ev_cmd_status *cs;

		if (skb->len < HCI_EVENT_HDR_SIZE + sizeof(*cs))
			return;

		cs = (void *)(skb->data + HCI_EVENT_HDR_SIZE);
		opcode = le16_to_cpu(cs->opcode);
		status = cs->status;
	} else {
		return;
	}

	if (hdev->req_skb && hdev->req_skb->len >= HCI_COMMAND_HDR_SIZE)
		req_opcode = hci_skb_opcode(hdev->req_skb);

	bt_dev_info(hdev,
		    "CSR clone: reply for 0x%04x (ncmd 0x%02x status 0x%02x): core pending req 0x%04x "
		    "req_status %u sent 0x%04x cmd_cnt %d cmd_q %d cmd_pending %d%s",
		    opcode, ncmd, status, req_opcode, hdev->req_status,
		    (hdev->sent_cmd && hdev->sent_cmd->len >= HCI_COMMAND_HDR_SIZE) ?
			hci_skb_opcode(hdev->sent_cmd) : 0,
		    atomic_read(&hdev->cmd_cnt), skb_queue_len(&hdev->cmd_q),
		    hci_dev_test_flag(hdev, HCI_CMD_PENDING),
		    hci_dev_test_flag(hdev, HCI_CMD_PENDING) &&
		    hdev->req_status == HCI_REQ_PEND && req_opcode == opcode ?
			" -> completes this request" : "");
}

/*
 * LOCAL PATCH: the clone's real answers for the opcodes we answer locally are
 * worth remembering, so the core can be handed the firmware's own reply instead
 * of a made-up one. Defined together with fake_resp_opcodes() below.
 */
struct csr_fake_resp;
static void csr_cache_response(struct btusb_data *data, const struct sk_buff *skb);
static void csr_replenish_cmd_cnt(struct hci_dev *hdev);

/* hci_sync.h is not exported to drivers, so restate the one value this patch
 * needs to tell whether a __hci_cmd_sync* caller is blocked. */
#ifndef HCI_REQ_PEND
#define HCI_REQ_PEND 1
#endif

static void csr_dump_state(struct btusb_data *data, const char *tag);
static bool csr_core_waiting_for(struct btusb_data *data, __u16 opcode);
static void csr_fix_ncmd(struct btusb_data *data, struct sk_buff *skb);

/*
 * LOCAL PATCH: the diagnostic work item. Printing from btusb_recv_intr() or from
 * btusb_setup_csr() cannot answer why a core request stalls, because both those
 * contexts are outside hdev->workqueue, and a request that is waiting has already
 * returned control to the core. The only way to see what hci_cmd_work() would have
 * seen is to run a work item on a queue that is guaranteed to execute.
 */
/*
 * The window has to stay open long enough to cover the core's own first request,
 * which is issued after btusb_setup_csr() returns. In the last trace the -110 came
 * 8 s after setup started, and the prefetch inside setup can itself take a minute
 * when the clone is silent, so the window is 300 rounds of 100 ms (30 s) and prints
 * only the states that matter, not every round.
 */
#define CSR_PROBE_ROUNDS 300
#define CSR_PROBE_INTERVAL msecs_to_jiffies(100)

static void csr_run_probe(struct btusb_data *data, struct workqueue_struct *wq,
			  struct delayed_work *probe, const char *tag, int *rounds);
static void csr_sys_probe(struct work_struct *work);
static void csr_core_probe(struct work_struct *work);
static void csr_arm_probe(struct btusb_data *data);

static int btusb_recv_intr(struct btusb_data *data, void *buffer, int count)
{
	struct sk_buff *skb;
	unsigned long flags;
	int err = 0;

	trace_rx(data, buffer, count, "intr");

	spin_lock_irqsave(&data->rxlock, flags);
	skb = data->evt_skb;

	while (count) {
		int len;

		if (!skb) {
			if ((data->csr_clone || data->csr_suspect) &&
			    csr_resync_bad_event_plen(data, &buffer, &count))
				continue;

			if ((data->csr_clone || data->csr_suspect) &&
			    !csr_event_start_ok(buffer, count)) {
				bt_dev_warn(data->hdev,
					    "CSR clone: dropping stray event code 0x%02x",
					    ((unsigned char *)buffer)[0]);
				buffer += 1;
				count -= 1;
				continue;
			}

			skb = bt_skb_alloc(HCI_MAX_EVENT_SIZE, GFP_ATOMIC);
			if (!skb) {
				err = -ENOMEM;
				break;
			}

			hci_skb_pkt_type(skb) = HCI_EVENT_PKT;
			hci_skb_expect(skb) = HCI_EVENT_HDR_SIZE;
		}

		len = min_t(uint, hci_skb_expect(skb), count);
		skb_put_data(skb, buffer, len);

		count -= len;
		buffer += len;
		hci_skb_expect(skb) -= len;

		if (skb->len == HCI_EVENT_HDR_SIZE) {
			if ((data->csr_clone || data->csr_suspect) &&
			    csr_resync_bad_inquiry_result_event(data, skb, &buffer, &count)) {
				kfree_skb(skb);
				skb = NULL;
				continue;
			}

			if ((data->csr_clone || data->csr_suspect) &&
			    csr_resync_bad_inquiry_complete(data, skb, &buffer, &count)) {
				kfree_skb(skb);
				skb = NULL;
				continue;
			}

			/* Complete event header */
			hci_skb_expect(skb) = hci_event_hdr(skb)->plen;

			if (skb_tailroom(skb) < hci_skb_expect(skb)) {
				kfree_skb(skb);
				skb = NULL;

				err = -EILSEQ;
				break;
			}
		}

		if (!hci_skb_expect(skb)) {
			/* Each chunk should correspond to at least 1 or more
			 * events so if there are still bytes left that doesn't
			 * constitute a new event this is likely a bug in the
			 * controller.
			 */
			if (count && count < HCI_EVENT_HDR_SIZE) {
				bt_dev_warn(data->hdev,
					"Unexpected continuation: %d bytes",
					count);
				count = 0;
			}

			/* Complete frame */
			if (data->csr_clone) {
				csr_cache_response(data, skb);

				/* Fix ncmd before deciding what to do with the event,
				 * so the event handed to the core is fixed too.
				 */
				csr_fix_ncmd(data, skb);

				/* LOCAL PATCH: print what hci_rx_evt() will see, i.e.
				 * whether this reply can complete the pending request at
				 * all. Do it before duplicate suppression, because a
				 * dropped reply is exactly the case where the caller is
				 * left waiting for an event that did arrive.
				 */
				csr_check_request_match(data, skb);

				/* LOCAL PATCH: drop the firmware's answer for an
				 * opcode we already answered locally, then log what is
				 * handed to the core.
				 */
				if (suppress_dupes && csr_is_duplicate_answer(data, skb)) {
					bt_dev_info(data->hdev,
						    "CSR clone: duplicate answer for 0x%04x, dropping it",
						    data->last_injected);
					data->last_injected = 0;
					kfree_skb(skb);
					skb = NULL;
					continue;
				}

				csr_log_event_frame(data, skb);

				/* LOCAL PATCH: RX-side suppression. A frame whose
				 * plen does not match its event code is the clone's
				 * buffer, not an event: decode what it echoes, then do
				 * not hand it to the core, which can only print the
				 * length error. A second connection complete for a
				 * handle that was already completed is dropped too.
				 */
				if (data->csr_clone && csr_decode_garbled_conn_frame(data, skb)) {
					kfree_skb(skb);
					skb = NULL;
					continue;
				}

				if (data->csr_clone && suppress_dupes &&
				    csr_is_duplicate_conn_complete(data, skb)) {
					kfree_skb(skb);
					skb = NULL;
					continue;
				}

				if (data->prefetching && data->wait_opcode &&
				    csr_event_is_waited(data, skb)) {
					bool core_waits = csr_core_waiting_for(data,
									    data->wait_opcode);

					bt_dev_info(data->hdev,
						    "CSR clone: prefetch answer for opcode 0x%04x",
						    data->wait_opcode);

					csr_dump_state(data, "prefetch answer");

					complete(&data->csr_done);

					/* LOCAL PATCH: if a __hci_cmd_sync* caller is
					 * blocked on this very opcode, the event must
					 * reach the core. Only hci_rx_evt() can set
					 * hdev->req_rsp and wake hdev->req_wait_q, so
					 * consuming it here leaves the caller waiting the
					 * whole HCI_CMD_TIMEOUT and it returns -110 even
					 * though the firmware answered. csr_cache_response()
					 * has already copied the payload, so the skb is free
					 * to hand over, and handle_cmd_cnt_and_timer() in the
					 * core sets cmd_cnt from the ncmd byte fixed above -
					 * no csr_replenish_cmd_cnt() in that case, or the
					 * clone would get a credit twice.
					 */
					if (core_waits) {
						bt_dev_info(data->hdev,
							    "CSR clone: a caller is waiting for 0x%04x, handing the event to the core",
							    data->wait_opcode);

						btusb_recv_event(data, skb);
						skb = NULL;
						continue;
					}

					/* The core never sees this event, so nothing
					 * else can replenish cmd_cnt for the command
					 * it has already sent (or still holds in cmd_q).
					 */
					csr_replenish_cmd_cnt(data->hdev);

					kfree_skb(skb);
					skb = NULL;
					continue;
				}

				/* LOCAL PATCH: ncmd is fixed earlier, in csr_fix_ncmd(). */
			}

			btusb_recv_event(data, skb);
			skb = NULL;
		}
	}

	data->evt_skb = skb;
	spin_unlock_irqrestore(&data->rxlock, flags);

	return err;
}

static int btusb_recv_acl(struct btusb_data *data, struct sk_buff *skb)
{
	/* Only queue ACL packet if intr_interval is set as it means
	 * force_poll_sync has been enabled.
	 */
	if (!data->intr_interval)
		return data->recv_acl(data->hdev, skb);

	skb_queue_tail(&data->acl_q, skb);
	schedule_delayed_work(&data->rx_work, data->intr_interval);

	return 0;
}

static int btusb_recv_bulk(struct btusb_data *data, void *buffer, int count)
{
	struct sk_buff *skb;
	unsigned long flags;
	int err = 0;

	trace_rx(data, buffer, count, "bulk");

	spin_lock_irqsave(&data->rxlock, flags);
	skb = data->acl_skb;

	while (count) {
		int len;

		if (!skb) {
			skb = bt_skb_alloc(HCI_MAX_FRAME_SIZE, GFP_ATOMIC);
			if (!skb) {
				err = -ENOMEM;
				break;
			}

			hci_skb_pkt_type(skb) = HCI_ACLDATA_PKT;
			hci_skb_expect(skb) = HCI_ACL_HDR_SIZE;
		}

		len = min_t(uint, hci_skb_expect(skb), count);
		skb_put_data(skb, buffer, len);

		count -= len;
		buffer += len;
		hci_skb_expect(skb) -= len;

		if (skb->len == HCI_ACL_HDR_SIZE) {
			__le16 dlen = hci_acl_hdr(skb)->dlen;

			/* Complete ACL header */
			hci_skb_expect(skb) = __le16_to_cpu(dlen);

			if (skb_tailroom(skb) < hci_skb_expect(skb)) {
				kfree_skb(skb);
				skb = NULL;

				err = -EILSEQ;
				break;
			}
		}

		if (!hci_skb_expect(skb)) {
			/* Complete frame */
			btusb_recv_acl(data, skb);
			skb = NULL;
		}
	}

	data->acl_skb = skb;
	spin_unlock_irqrestore(&data->rxlock, flags);

	return err;
}

static bool btusb_validate_sco_handle(struct hci_dev *hdev,
				      struct hci_sco_hdr *hdr)
{
	__u16 handle;

	if (hci_dev_test_flag(hdev, HCI_USER_CHANNEL))
		// Can't validate, userspace controls everything.
		return true;

	/*
	 * USB isochronous transfers are not designed to be reliable and may
	 * lose fragments.  When this happens, the next first fragment
	 * encountered might actually be a continuation fragment.
	 * Validate the handle to detect it and drop it, or else the upper
	 * layer will get garbage for a while.
	 */

	handle = hci_handle(__le16_to_cpu(hdr->handle));

	switch (hci_conn_lookup_type(hdev, handle)) {
	case SCO_LINK:
	case ESCO_LINK:
		return true;
	default:
		return false;
	}
}

static int btusb_recv_isoc(struct btusb_data *data, void *buffer, int count)
{
	struct sk_buff *skb;
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&data->rxlock, flags);
	skb = data->sco_skb;

	while (count) {
		int len;

		if (!skb) {
			skb = bt_skb_alloc(HCI_MAX_SCO_SIZE, GFP_ATOMIC);
			if (!skb) {
				err = -ENOMEM;
				break;
			}

			hci_skb_pkt_type(skb) = HCI_SCODATA_PKT;
			hci_skb_expect(skb) = HCI_SCO_HDR_SIZE;
		}

		len = min_t(uint, hci_skb_expect(skb), count);
		skb_put_data(skb, buffer, len);

		count -= len;
		buffer += len;
		hci_skb_expect(skb) -= len;

		if (skb->len == HCI_SCO_HDR_SIZE) {
			/* Complete SCO header */
			struct hci_sco_hdr *hdr = hci_sco_hdr(skb);

			hci_skb_expect(skb) = hdr->dlen;

			if (skb_tailroom(skb) < hci_skb_expect(skb) ||
			    !btusb_validate_sco_handle(data->hdev, hdr)) {
				kfree_skb(skb);
				skb = NULL;

				err = -EILSEQ;
				break;
			}
		}

		if (!hci_skb_expect(skb)) {
			/* Complete frame */
			hci_recv_frame(data->hdev, skb);
			skb = NULL;
		}
	}

	data->sco_skb = skb;
	spin_unlock_irqrestore(&data->rxlock, flags);

	return err;
}

static void btusb_intr_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return;

	if (urb->status == 0) {
		hdev->stat.byte_rx += urb->actual_length;

		if (btusb_recv_intr(data, urb->transfer_buffer,
				    urb->actual_length) < 0) {
			bt_dev_err(hdev, "corrupted event packet");
			hdev->stat.err_rx++;
		}
	} else if (urb->status == -ENOENT) {
		/* Avoid suspend failed when usb_kill_urb */
		return;
	}

	if (!test_bit(BTUSB_INTR_RUNNING, &data->flags))
		return;

	usb_mark_last_busy(data->udev);
	usb_anchor_urb(urb, &data->intr_anchor);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* -EPERM: urb is being killed;
		 * -ENODEV: device got disconnected
		 */
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p failed to resubmit (%d)",
				   urb, -err);
		if (err != -EPERM)
			hci_cmd_sync_cancel(hdev, -err);
		usb_unanchor_urb(urb);
	}
}

static int btusb_submit_intr_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size;

	BT_DBG("%s", hdev->name);

	if (!data->intr_ep)
		return -ENODEV;

	urb = usb_alloc_urb(0, mem_flags);
	if (!urb)
		return -ENOMEM;

	if (le16_to_cpu(data->udev->descriptor.idVendor)  == 0x0a12 &&
	    le16_to_cpu(data->udev->descriptor.idProduct) == 0x0001)
		/* Fake CSR devices don't seem to support sort-transter */
		size = le16_to_cpu(data->intr_ep->wMaxPacketSize);
	else
		/* Use maximum HCI Event size so the USB stack handles
		 * ZPL/short-transfer automatically.
		 */
		size = HCI_MAX_EVENT_SIZE;

	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvintpipe(data->udev, data->intr_ep->bEndpointAddress);

	usb_fill_int_urb(urb, data->udev, pipe, buf, size,
			 btusb_intr_complete, hdev, data->intr_ep->bInterval);

	urb->transfer_flags |= URB_FREE_BUFFER;

	usb_anchor_urb(urb, &data->intr_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		if (err != -EPERM)
			hci_cmd_sync_cancel(hdev, -err);
		usb_unanchor_urb(urb);
	}

	/* Only initialize intr_interval if URB poll sync is enabled */
	if (!data->poll_sync)
		goto done;

	/* The units are frames (milliseconds) for full and low speed devices,
	 * and microframes (1/8 millisecond) for highspeed and SuperSpeed
	 * devices.
	 *
	 * This is done once on open/resume so it shouldn't change even if
	 * force_poll_sync changes.
	 */
	switch (urb->dev->speed) {
	case USB_SPEED_SUPER_PLUS:
	case USB_SPEED_SUPER:	/* units are 125us */
		data->intr_interval = usecs_to_jiffies(urb->interval * 125);
		break;
	default:
		data->intr_interval = msecs_to_jiffies(urb->interval);
		break;
	}

done:
	usb_free_urb(urb);

	return err;
}

static void btusb_bulk_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return;

	if (urb->status == 0) {
		hdev->stat.byte_rx += urb->actual_length;

		if (data->recv_bulk(data, urb->transfer_buffer,
				    urb->actual_length) < 0) {
			bt_dev_err(hdev, "corrupted ACL packet");
			hdev->stat.err_rx++;
		}
	} else if (urb->status == -ENOENT) {
		/* Avoid suspend failed when usb_kill_urb */
		return;
	}

	if (!test_bit(BTUSB_BULK_RUNNING, &data->flags))
		return;

	usb_anchor_urb(urb, &data->bulk_anchor);
	usb_mark_last_busy(data->udev);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* -EPERM: urb is being killed;
		 * -ENODEV: device got disconnected
		 */
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p failed to resubmit (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}
}

static int btusb_submit_bulk_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size = HCI_MAX_FRAME_SIZE;

	BT_DBG("%s", hdev->name);

	if (!data->bulk_rx_ep)
		return -ENODEV;

	urb = usb_alloc_urb(0, mem_flags);
	if (!urb)
		return -ENOMEM;

	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvbulkpipe(data->udev, data->bulk_rx_ep->bEndpointAddress);

	usb_fill_bulk_urb(urb, data->udev, pipe, buf, size,
			  btusb_bulk_complete, hdev);

	urb->transfer_flags |= URB_FREE_BUFFER;

	usb_mark_last_busy(data->udev);
	usb_anchor_urb(urb, &data->bulk_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);

	return err;
}

static void btusb_isoc_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct btusb_data *data = hci_get_drvdata(hdev);
	int i, err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return;

	if (urb->status == 0) {
		for (i = 0; i < urb->number_of_packets; i++) {
			unsigned int offset = urb->iso_frame_desc[i].offset;
			unsigned int length = urb->iso_frame_desc[i].actual_length;

			if (urb->iso_frame_desc[i].status)
				continue;

			hdev->stat.byte_rx += length;

			if (btusb_recv_isoc(data, urb->transfer_buffer + offset,
					    length) < 0) {
				bt_dev_err(hdev, "corrupted SCO packet");
				hdev->stat.err_rx++;
			}
		}
	} else if (urb->status == -ENOENT) {
		/* Avoid suspend failed when usb_kill_urb */
		return;
	}

	if (!test_bit(BTUSB_ISOC_RUNNING, &data->flags))
		return;

	usb_anchor_urb(urb, &data->isoc_anchor);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* -EPERM: urb is being killed;
		 * -ENODEV: device got disconnected
		 */
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p failed to resubmit (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}
}

static inline void __fill_isoc_descriptor_msbc(struct urb *urb, int len,
					       int mtu, struct btusb_data *data)
{
	int i = 0, offset = 0;
	unsigned int interval;

	BT_DBG("len %d mtu %d", len, mtu);

	/* For mSBC ALT 6 settings some chips need to transmit the data
	 * continuously without the zero length of USB packets.
	 */
	if (test_bit(BTUSB_ALT6_CONTINUOUS_TX, &data->flags))
		goto ignore_usb_alt6_packet_flow;

	/* For mSBC ALT 6 setting the host will send the packet at continuous
	 * flow. As per core spec 5, vol 4, part B, table 2.1. For ALT setting
	 * 6 the HCI PACKET INTERVAL should be 7.5ms for every usb packets.
	 * To maintain the rate we send 63bytes of usb packets alternatively for
	 * 7ms and 8ms to maintain the rate as 7.5ms.
	 */
	if (data->usb_alt6_packet_flow) {
		interval = 7;
		data->usb_alt6_packet_flow = false;
	} else {
		interval = 6;
		data->usb_alt6_packet_flow = true;
	}

	for (i = 0; i < interval; i++) {
		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = offset;
	}

ignore_usb_alt6_packet_flow:
	if (len && i < BTUSB_MAX_ISOC_FRAMES) {
		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = len;
		i++;
	}

	urb->number_of_packets = i;
}

static inline void __fill_isoc_descriptor(struct urb *urb, int len, int mtu)
{
	int i, offset = 0;

	BT_DBG("len %d mtu %d", len, mtu);

	for (i = 0; i < BTUSB_MAX_ISOC_FRAMES && len >= mtu;
					i++, offset += mtu, len -= mtu) {
		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = mtu;
	}

	if (len && i < BTUSB_MAX_ISOC_FRAMES) {
		urb->iso_frame_desc[i].offset = offset;
		urb->iso_frame_desc[i].length = len;
		i++;
	}

	urb->number_of_packets = i;
}

static int btusb_submit_isoc_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size;

	BT_DBG("%s", hdev->name);

	if (!data->isoc_rx_ep)
		return -ENODEV;

	urb = usb_alloc_urb(BTUSB_MAX_ISOC_FRAMES, mem_flags);
	if (!urb)
		return -ENOMEM;

	size = le16_to_cpu(data->isoc_rx_ep->wMaxPacketSize) *
						BTUSB_MAX_ISOC_FRAMES;

	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvisocpipe(data->udev, data->isoc_rx_ep->bEndpointAddress);

	usb_fill_int_urb(urb, data->udev, pipe, buf, size, btusb_isoc_complete,
			 hdev, data->isoc_rx_ep->bInterval);

	urb->transfer_flags = URB_FREE_BUFFER | URB_ISO_ASAP;

	__fill_isoc_descriptor(urb, size,
			       le16_to_cpu(data->isoc_rx_ep->wMaxPacketSize));

	usb_anchor_urb(urb, &data->isoc_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);

	return err;
}

static void btusb_diag_complete(struct urb *urb)
{
	struct hci_dev *hdev = urb->context;
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (urb->status == 0) {
		struct sk_buff *skb;

		skb = bt_skb_alloc(urb->actual_length, GFP_ATOMIC);
		if (skb) {
			skb_put_data(skb, urb->transfer_buffer,
				     urb->actual_length);
			hci_recv_diag(hdev, skb);
		}
	} else if (urb->status == -ENOENT) {
		/* Avoid suspend failed when usb_kill_urb */
		return;
	}

	if (!test_bit(BTUSB_DIAG_RUNNING, &data->flags))
		return;

	usb_anchor_urb(urb, &data->diag_anchor);
	usb_mark_last_busy(data->udev);

	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err < 0) {
		/* -EPERM: urb is being killed;
		 * -ENODEV: device got disconnected
		 */
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p failed to resubmit (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}
}

static int btusb_submit_diag_urb(struct hci_dev *hdev, gfp_t mem_flags)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned char *buf;
	unsigned int pipe;
	int err, size = HCI_MAX_FRAME_SIZE;

	BT_DBG("%s", hdev->name);

	if (!data->diag_rx_ep)
		return -ENODEV;

	urb = usb_alloc_urb(0, mem_flags);
	if (!urb)
		return -ENOMEM;

	buf = kmalloc(size, mem_flags);
	if (!buf) {
		usb_free_urb(urb);
		return -ENOMEM;
	}

	pipe = usb_rcvbulkpipe(data->udev, data->diag_rx_ep->bEndpointAddress);

	usb_fill_bulk_urb(urb, data->udev, pipe, buf, size,
			  btusb_diag_complete, hdev);

	urb->transfer_flags |= URB_FREE_BUFFER;

	usb_mark_last_busy(data->udev);
	usb_anchor_urb(urb, &data->diag_anchor);

	err = usb_submit_urb(urb, mem_flags);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		usb_unanchor_urb(urb);
	}

	usb_free_urb(urb);

	return err;
}

static void btusb_tx_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	struct hci_dev *hdev = (struct hci_dev *)skb->dev;
	struct btusb_data *data = hci_get_drvdata(hdev);
	unsigned long flags;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		goto done;

	if (!urb->status) {
		hdev->stat.byte_tx += urb->transfer_buffer_length;
	} else {
		if (hci_skb_pkt_type(skb) == HCI_COMMAND_PKT)
			hci_cmd_sync_cancel(hdev, -urb->status);
		hdev->stat.err_tx++;
	}

done:
	spin_lock_irqsave(&data->txlock, flags);
	data->tx_in_flight--;
	spin_unlock_irqrestore(&data->txlock, flags);

	kfree(urb->setup_packet);

	kfree_skb(skb);
}

static void btusb_isoc_tx_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;
	struct hci_dev *hdev = (struct hci_dev *)skb->dev;

	BT_DBG("%s urb %p status %d count %d", hdev->name, urb, urb->status,
	       urb->actual_length);

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		goto done;

	if (!urb->status)
		hdev->stat.byte_tx += urb->transfer_buffer_length;
	else
		hdev->stat.err_tx++;

done:
	kfree(urb->setup_packet);

	kfree_skb(skb);
}

static int btusb_open(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s", hdev->name);

	err = usb_autopm_get_interface(data->intf);
	if (err < 0)
		return err;

	/* Patching USB firmware files prior to starting any URBs of HCI path
	 * It is more safe to use USB bulk channel for downloading USB patch
	 */
	if (data->setup_on_usb) {
		err = data->setup_on_usb(hdev);
		if (err < 0)
			goto setup_fail;
	}

	data->intf->needs_remote_wakeup = 1;

	if (test_and_set_bit(BTUSB_INTR_RUNNING, &data->flags))
		goto done;

	err = btusb_submit_intr_urb(hdev, GFP_KERNEL);
	if (err < 0)
		goto failed;

	err = btusb_submit_bulk_urb(hdev, GFP_KERNEL);
	if (err < 0) {
		usb_kill_anchored_urbs(&data->intr_anchor);
		goto failed;
	}

	set_bit(BTUSB_BULK_RUNNING, &data->flags);
	btusb_submit_bulk_urb(hdev, GFP_KERNEL);

	if (data->diag) {
		if (!btusb_submit_diag_urb(hdev, GFP_KERNEL))
			set_bit(BTUSB_DIAG_RUNNING, &data->flags);
	}

done:
	usb_autopm_put_interface(data->intf);
	return 0;

failed:
	clear_bit(BTUSB_INTR_RUNNING, &data->flags);
setup_fail:
	usb_autopm_put_interface(data->intf);
	return err;
}

static void btusb_stop_traffic(struct btusb_data *data)
{
	usb_kill_anchored_urbs(&data->intr_anchor);
	usb_kill_anchored_urbs(&data->bulk_anchor);
	usb_kill_anchored_urbs(&data->isoc_anchor);
	usb_kill_anchored_urbs(&data->diag_anchor);
	usb_kill_anchored_urbs(&data->ctrl_anchor);
}

static int btusb_close(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	BT_DBG("%s", hdev->name);

	cancel_delayed_work(&data->rx_work);
	cancel_work_sync(&data->work);
	cancel_work_sync(&data->waker);

	skb_queue_purge(&data->acl_q);

	clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
	clear_bit(BTUSB_BULK_RUNNING, &data->flags);
	clear_bit(BTUSB_INTR_RUNNING, &data->flags);
	clear_bit(BTUSB_DIAG_RUNNING, &data->flags);

	btusb_stop_traffic(data);
	btusb_free_frags(data);

	err = usb_autopm_get_interface(data->intf);
	if (err < 0)
		goto failed;

	data->intf->needs_remote_wakeup = 0;

	/* Enable remote wake up for auto-suspend */
	if (test_bit(BTUSB_WAKEUP_AUTOSUSPEND, &data->flags))
		data->intf->needs_remote_wakeup = 1;

	usb_autopm_put_interface(data->intf);

failed:
	usb_scuttle_anchored_urbs(&data->deferred);
	return 0;
}

static int btusb_flush(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);

	BT_DBG("%s", hdev->name);

	cancel_delayed_work(&data->rx_work);

	skb_queue_purge(&data->acl_q);

	usb_kill_anchored_urbs(&data->tx_anchor);
	btusb_free_frags(data);

	return 0;
}

static struct urb *alloc_ctrl_urb(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct usb_ctrlrequest *dr;
	struct urb *urb;
	unsigned int pipe;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return ERR_PTR(-ENOMEM);

	dr = kmalloc_obj(*dr);
	if (!dr) {
		usb_free_urb(urb);
		return ERR_PTR(-ENOMEM);
	}

	dr->bRequestType = data->cmdreq_type;
	dr->bRequest     = data->cmdreq;
	dr->wIndex       = 0;
	dr->wValue       = 0;
	dr->wLength      = __cpu_to_le16(skb->len);

	pipe = usb_sndctrlpipe(data->udev, 0x00);

	usb_fill_control_urb(urb, data->udev, pipe, (void *)dr,
			     skb->data, skb->len, btusb_tx_complete, skb);

	skb->dev = (void *)hdev;

	return urb;
}

static struct urb *alloc_bulk_urb(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned int pipe;

	if (!data->bulk_tx_ep)
		return ERR_PTR(-ENODEV);

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return ERR_PTR(-ENOMEM);

	pipe = usb_sndbulkpipe(data->udev, data->bulk_tx_ep->bEndpointAddress);

	usb_fill_bulk_urb(urb, data->udev, pipe,
			  skb->data, skb->len, btusb_tx_complete, skb);

	skb->dev = (void *)hdev;

	return urb;
}

static struct urb *alloc_isoc_urb(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;
	unsigned int pipe;

	if (!data->isoc_tx_ep)
		return ERR_PTR(-ENODEV);

	urb = usb_alloc_urb(BTUSB_MAX_ISOC_FRAMES, GFP_KERNEL);
	if (!urb)
		return ERR_PTR(-ENOMEM);

	pipe = usb_sndisocpipe(data->udev, data->isoc_tx_ep->bEndpointAddress);

	usb_fill_int_urb(urb, data->udev, pipe,
			 skb->data, skb->len, btusb_isoc_tx_complete,
			 skb, data->isoc_tx_ep->bInterval);

	urb->transfer_flags  = URB_ISO_ASAP;

	if (data->isoc_altsetting == 6)
		__fill_isoc_descriptor_msbc(urb, skb->len,
					    le16_to_cpu(data->isoc_tx_ep->wMaxPacketSize),
					    data);
	else
		__fill_isoc_descriptor(urb, skb->len,
				       le16_to_cpu(data->isoc_tx_ep->wMaxPacketSize));
	skb->dev = (void *)hdev;

	return urb;
}

static int submit_tx_urb(struct hci_dev *hdev, struct urb *urb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	usb_anchor_urb(urb, &data->tx_anchor);

	err = usb_submit_urb(urb, GFP_KERNEL);
	if (err < 0) {
		if (err != -EPERM && err != -ENODEV)
			bt_dev_err(hdev, "urb %p submission failed (%d)",
				   urb, -err);
		kfree(urb->setup_packet);
		usb_unanchor_urb(urb);
	} else {
		usb_mark_last_busy(data->udev);
	}

	usb_free_urb(urb);
	return err;
}

static int submit_or_queue_tx_urb(struct hci_dev *hdev, struct urb *urb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	unsigned long flags;
	bool suspending;

	spin_lock_irqsave(&data->txlock, flags);
	suspending = test_bit(BTUSB_SUSPENDING, &data->flags);
	if (!suspending)
		data->tx_in_flight++;
	spin_unlock_irqrestore(&data->txlock, flags);

	if (!suspending)
		return submit_tx_urb(hdev, urb);

	usb_anchor_urb(urb, &data->deferred);
	schedule_work(&data->waker);

	usb_free_urb(urb);
	return 0;
}

static int inject_cmd_complete(struct hci_dev *hdev, __u16 opcode);

/*
 * LOCAL PATCH: this clone (manufacturer 2279, HCI rev 0x0087) answers the
 * setup commands and the init commands fine - its RX path works, so the
 * Barrot 8041a02 receive-issue workaround is useless for it (it cannot even
 * be suspended: sysfs rejects "suspend" for this device).
 *
 * What breaks is that the core always sends HCI Read Buffer Size (0x1005)
 * during the BR/EDR init stage 2, and this firmware never replies to it, so
 * __hci_cmd_sync_status() times out and hci0 never comes up.
 *
 * The traced timestamps showed the clone answers everything else in ~3 ms, so
 * latency is not a general problem here. But its first HCI Reset after power-up
 * took 2245 ms - more than HCI_CMD_TIMEOUT (2000 ms) - while the same command
 * answered in 3 ms on the following attempt. That is the cold-boot path, so the
 * command is sent by csr_prefetch() from btusb_setup_csr(), where a long timeout
 * applies, and the firmware's answer is captured there. For HCI Reset the core's
 * own later call is no longer replayed from the cache: btusb_send_frame() puts it
 * on the wire and waits for the real event, which answers in ~500 ms once the boot
 * is done. The cache entry for 0x0c03 is now only a record of the cold-boot reply.
 *
 * The third entry is a firmware claim that is simply false: the clone reports
 * LMP sync-train support (feature bit LMP_SYNC_TRAIN) in its Read Local
 * Supported Commands/LMP features reply, so hci_read_sync_train_params_sync()
 * sends HCI_OP_READ_SYNC_TRAIN_PARAMS (0x0c77) during init stage 3, and the
 * firmware answers "Unknown HCI Command" (status 0x01, which the core turns
 * into -EBADRQC = -56). That aborts __hci_init() and hci0 stays DOWN even
 * though everything before it succeeded. A Primary controller should never
 * claim an AMP-only feature, so answer the command ourselves with status 0.
 *
 * Answer those commands locally here instead of waiting for a reply that
 * never comes. The device keeps default (sane) buffer sizes.
 */
struct csr_fake_resp {
	__u16 opcode;
	/* Is the command still worth handing to the controller? The reset has
	 * to reach the firmware for the boot to finish, and Read Buffer Size is
	 * answered late but correctly. For opcodes the firmware cannot execute
	 * at all there is no point: sending them only produces a stray error
	 * reply the core reports as "unexpected event for opcode 0x%4.4x".
	 */
	bool to_firmware;
	/* Run the command from btusb_setup_csr() with our own long timeout
	 * (csr_prefetch_timeout, 5 s for this test) instead of leaving it to the
	 * core, which only waits HCI_CMD_TIMEOUT (2 s). See csr_prefetch().
	 */
	bool prefetch;
	/* The firmware's own answer, captured by btusb_recv_intr() and replayed
	 * when the core asks for the same opcode again.
	 */
	int len;
	u8 data[16];
};

static struct csr_fake_resp fake_resp_opcodes[] = {
	/* Prefetch order matters: reset first, then features, then buffer size. */
	/* 0x0c03: 2.2 s on the first attempt, the firmware still needs it. Since
	 * the quick fix this entry is not replayed - HCI Reset always goes on the
	 * wire - it only records what the cold-boot reply was.
	 */
	{ .opcode = HCI_OP_RESET,		.to_firmware = true, .prefetch = true },
	/* 0x1003: answered correctly but 0.5-2.3 s after power-up, which is
	 * more than HCI_CMD_TIMEOUT, so it is prefetched and then replayed
	 * from the cache. Its handler wants 9 return parameters, so a bare
	 * status byte is not enough - the cached reply is what makes this work.
	 */
	{ .opcode = HCI_OP_READ_LOCAL_FEATURES,	.to_firmware = true, .prefetch = true },
	/* 0x1005: on a cold boot the clone often misses HCI_CMD_TIMEOUT (2 s).
	 * When it does answer (warm, ~1-2 ms) the payload is acl 1021:9 /
	 * sco 255:4. Prefetch it here with csr_prefetch_timeout so the core
	 * never races a local inject against a late real complete ("unexpected
	 * event for opcode 0x1005"). On prefetch timeout, inject_read_buffer_size()
	 * falls back to those known Barrot sizes.
	 */
	{ .opcode = HCI_OP_READ_BUFFER_SIZE,	.to_firmware = true, .prefetch = true },
	/* 0x0c77: claims support, rejects the command - nothing to send */
	{ .opcode = HCI_OP_READ_SYNC_TRAIN_PARAMS, .to_firmware = false },
};

static struct csr_fake_resp *needs_fake_response(__u16 opcode)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fake_resp_opcodes); i++)
		if (fake_resp_opcodes[i].opcode == opcode)
			return &fake_resp_opcodes[i];

	return NULL;
}

/*
 * LOCAL PATCH: remember what the firmware actually answered for an opcode we
 * answer locally. btusb_recv_intr() calls this with a complete event, so the
 * bytes here are the real reply (the clone pads its events with zeros up to the
 * length it claims, which is why the cache is capped at what was received).
 * skb->data is the event header; the command-complete body is ncmd + opcode,
 * and the return parameters follow it.
 */
static void csr_cache_response(struct btusb_data *data, const struct sk_buff *skb)
{
	const struct hci_event_hdr *hdr = (void *)skb->data;
	const struct hci_ev_cmd_complete *cc;
	struct csr_fake_resp *fake;
	int len;

	if (hdr->evt != HCI_EV_CMD_COMPLETE ||
	    skb->len < HCI_EVENT_HDR_SIZE + sizeof(*cc))
		return;

	cc = (void *)(skb->data + HCI_EVENT_HDR_SIZE);
	fake = needs_fake_response(le16_to_cpu(cc->opcode));
	if (!fake)
		return;

	len = skb->len - HCI_EVENT_HDR_SIZE - sizeof(*cc);
	if (len <= 0)
		return;

	fake->len = min_t(int, len, (int)sizeof(fake->data));
	memcpy(fake->data, (void *)(skb->data + HCI_EVENT_HDR_SIZE + sizeof(*cc)),
	       fake->len);
}

/*
 * LOCAL PATCH: do what handle_cmd_cnt_and_timer() would have done for an event
 * we took for ourselves.
 *
 * hci_cmd_work() only transmits a command when hdev->cmd_cnt is non-zero, and
 * the core replenishes that counter from the ncmd byte of a command-complete
 * event it has actually seen. Two paths here break that bookkeeping:
 *   - btusb_recv_intr() consumes the event the prefetch is waiting for, so it
 *     never reaches hci_cmd_complete_evt();
 *   - btusb_send_frame() answers commands locally.
 * When nothing replenishes the counter, the next command the core queues stays
 * in hdev->cmd_q forever and its sync fails with -ETIMEDOUT (-110). Note that
 * the prefetch sends do not go through hci_send_cmd_sync(), so they never
 * decrement the counter either - the decrement only happens for commands the
 * core sent itself, which is why the counter ends up at 0 after the first
 * command of the init sequence.
 */
static void csr_replenish_cmd_cnt(struct hci_dev *hdev)
{
	if (!hdev)
		return;

	bt_dev_info(hdev,
		    "CSR clone: cmd_cnt %d -> 1, reset flag %d, cmd_q %d",
		    atomic_read(&hdev->cmd_cnt),
		    test_bit(HCI_RESET, &hdev->flags),
		    skb_queue_len(&hdev->cmd_q));

	atomic_set(&hdev->cmd_cnt, 1);

	if (!skb_queue_empty(&hdev->cmd_q))
		queue_work(hdev->workqueue, &hdev->cmd_work);
}

/*
 * LOCAL PATCH: diagnostic for the -110 the core's own commands get. The three
 * values below are all the core needs to make a request progress, so printing
 * them at the points where the patch diverges from the core tells which one is
 * missing:
 *   cmd_cnt    - hci_cmd_work() transmits only when this is non-zero, so a
 *                request whose command sits in cmd_q with cmd_cnt 0 can only
 *                time out;
 *   cmd_q      - the queued commands of the core (the prefetch path never uses it);
 *   req_status - HCI_REQ_PEND (1) means a __hci_cmd_sync* caller is blocked on
 *                hdev->req_wait_q right now;
 *   req_opcode - the opcode that request issued; hci_rx_evt() matches an event
 *                against this, so an event with a different opcode cannot complete
 *                it and the caller waits the whole timeout;
 *   cmd_pending - HCI_CMD_PENDING is set by hci_send_cmd_sync() when it arms
 *                hdev->req_skb for a request, so a pending request with this 0 means
 *                hci_send_cmd_sync() never ran for it, i.e. the command never left
 *                the wire;
 *   sent_opcode - the opcode hdev->sent_cmd still holds. It is only ever set by
 *                hci_send_cmd_sync(), so if a request is pending for opcode A while
 *                sent_cmd holds opcode B, the pending request was not the one sent;
 *   running    - hci_send_frame() refuses to call hdev->send() when this is 0 and
 *                instead returns -EINVAL, which a __hci_cmd_sync* caller reports as
 *                -22, not -110, so a -110 cannot come from this gate;
 *   on_cmd_work - whether this code runs on hdev->workqueue. That workqueue is
 *                ordered (alloc_ordered_workqueue), so while a work item runs on
 *                it hci_cmd_work() cannot run at all; a request issued from that
 *                context would never see its own command leave the wire.
 *   on_rx_work - the event path: hci_rx_work() runs on the same ordered
 *                workqueue and is where the event is matched against
 *                hdev->req_skb, so a bt_dev_info from an URB completion prints
 *                both as 0 and is not itself a work item on that queue. A print
 *                from the probe also shows both as 0, because the probe is its own
 *                work item; the tag in front of it says which queue executed it.
 */
static void csr_dump_state(struct btusb_data *data, const char *tag)
{
	struct hci_dev *hdev = data->hdev;
	__u16 req_opcode = 0;
	__u16 sent_opcode = 0;

	if (hdev->req_skb && hdev->req_skb->len >= HCI_COMMAND_HDR_SIZE)
		req_opcode = hci_skb_opcode(hdev->req_skb);

	if (hdev->sent_cmd && hdev->sent_cmd->len >= HCI_COMMAND_HDR_SIZE)
		sent_opcode = hci_skb_opcode(hdev->sent_cmd);

	bt_dev_info(hdev,
		    "CSR clone: %s: cmd_cnt %d cmd_q %d req_status %u req_opcode 0x%04x "
		    "sent 0x%04x wait 0x%04x running %d cmd_pending %d reset %d init %d up %d "
		    "on_cmd_work %d on_rx_work %d",
		    tag, atomic_read(&hdev->cmd_cnt), skb_queue_len(&hdev->cmd_q),
		    hdev->req_status, req_opcode, sent_opcode, data->wait_opcode,
		    test_bit(HCI_RUNNING, &hdev->flags),
		    hci_dev_test_flag(hdev, HCI_CMD_PENDING),
		    test_bit(HCI_RESET, &hdev->flags),
		    test_bit(HCI_INIT, &hdev->flags),
		    test_bit(HCI_UP, &hdev->flags),
		    current_work() == &hdev->cmd_work,
		    current_work() == &hdev->rx_work);
}

/*
 * LOCAL PATCH: one round of the diagnostic probe. Print only the two states that
 * can explain a stalled request, so the trace stays short:
 *   req_status == HCI_REQ_PEND  - a __hci_cmd_sync* caller is blocked right now;
 *   cmd_cnt == 0 with cmd_q > 0 - the request's command is queued but hci_cmd_work()
 *                                 will not transmit it. This is the only way a request
 *                                 can time out without a "tx timeout" line, because
 *                                 hci_cmd_work() arms the command timer only after a
 *                                 send, so a 0 counter returns silently and the caller
 *                                 is left to the full timeout.
 * Both copies of the work item come through here, and the tag says which queue ran
 * it. If hdev->workqueue were blocked, its own copy could not print, so comparing
 * the two counters is the measurement; a request pending with cmd_cnt 1 and cmd_q 1
 * while only system_wq printed means the ordered queue could not reach hci_cmd_work().
 */
static void csr_run_probe(struct btusb_data *data, struct workqueue_struct *wq,
			  struct delayed_work *probe, const char *tag, int *rounds)
{
	struct hci_dev *hdev = data->hdev;

	if (data->probe_stopped || !hdev)
		return;

	if (hdev->req_status == HCI_REQ_PEND ||
	    (atomic_read(&hdev->cmd_cnt) == 0 && !skb_queue_empty(&hdev->cmd_q)))
		csr_dump_state(data, tag);

	(*rounds)++;

	if (data->sys_probe_rounds >= CSR_PROBE_ROUNDS) {
		bt_dev_info(hdev,
			    "CSR clone: probe window closed: system_wq printed %d, "
			    "hdev->workqueue printed %d",
			    data->sys_probe_rounds, data->core_probe_rounds);
		return;
	}

	queue_delayed_work(wq, probe, CSR_PROBE_INTERVAL);
}

static void csr_sys_probe(struct work_struct *work)
{
	struct btusb_data *data =
		container_of(work, struct btusb_data, sys_probe.work);

	csr_run_probe(data, system_wq, &data->sys_probe, "probe system_wq",
		      &data->sys_probe_rounds);
}

static void csr_core_probe(struct work_struct *work)
{
	struct btusb_data *data =
		container_of(work, struct btusb_data, core_probe.work);
	struct hci_dev *hdev = data->hdev;

	if (data->probe_stopped || !hdev)
		return;

	csr_run_probe(data, hdev->workqueue, &data->core_probe,
		      "probe hdev->workqueue", &data->core_probe_rounds);
}

/*
 * LOCAL PATCH: open the probe window. It has to start before the core's first
 * command and stay open past btusb_setup_csr(), because the request that fails is
 * issued after setup returns, during the core's init stages, where this patch has no
 * hook at all.
 */
static void csr_arm_probe(struct btusb_data *data)
{
	struct hci_dev *hdev = data->hdev;

	if (!hdev || data->probe_stopped)
		return;

	data->sys_probe_rounds = 0;
	data->core_probe_rounds = 0;

	/* The delay gives the request time to be queued and reach hci_cmd_work()
	 * before the first print, so the first round shows the state a send would
	 * have seen rather than the state at the moment of the call. */
	queue_delayed_work(system_wq, &data->sys_probe, CSR_PROBE_INTERVAL);
	queue_delayed_work(hdev->workqueue, &data->core_probe, CSR_PROBE_INTERVAL);
}

/*
 * LOCAL PATCH: is a __hci_cmd_sync* caller blocked right now waiting for this
 * exact opcode? If it is, only an event delivered to the core can wake it:
 * hci_rx_evt() compares the event with hdev->req_skb, so an event consumed by the
 * prefetch path leaves the caller waiting the full HCI_CMD_TIMEOUT and it returns
 * -ETIMEDOUT (-110) even though the firmware answered.
 */
static bool csr_core_waiting_for(struct btusb_data *data, __u16 opcode)
{
	struct hci_dev *hdev = data->hdev;

	/* hci_dev_lock() cannot be taken from an URB completion, so this is a
	 * best-effort snapshot of what the core has pending. hdev->req_skb is only
	 * armed by hci_cmd_work() when a command actually leaves the wire
	 * (hdev->req_skb = skb_get(hdev->sent_cmd)), and never by the skb the
	 * prefetch sends itself - so a request whose command is still sitting in
	 * cmd_q shows up here as no request at all, which is itself the answer.
	 */
	if (!hdev || hdev->req_status != HCI_REQ_PEND || !hdev->req_skb)
		return false;

	if (!hci_dev_test_flag(hdev, HCI_CMD_PENDING))
		return false;

	if (hdev->req_skb->len < HCI_COMMAND_HDR_SIZE)
		return false;

	return hci_skb_opcode(hdev->req_skb) == opcode;
}

/*
 * LOCAL PATCH: the ncmd byte comes from the clone's firmware. When it is 0 the
 * core arms the ncmd timer (4 s) and resets the controller, so always claim the
 * one packet we know the core has just waited for. It has to happen before the
 * event is either consumed or handed over, which is why it is a function and not
 * a block inside btusb_recv_intr().
 */
static void csr_fix_ncmd(struct btusb_data *data, struct sk_buff *skb)
{
	struct hci_event_hdr *hdr = (void *)skb->data;
	struct hci_ev_cmd_complete *cc;

	if (skb->len < HCI_EVENT_HDR_SIZE + sizeof(struct hci_ev_cmd_complete))
		return;

	cc = (void *)(skb->data + HCI_EVENT_HDR_SIZE);

	if (hdr->evt == HCI_EV_CMD_COMPLETE && !cc->ncmd)
		cc->ncmd = 0x01;
}

/*
 * LOCAL PATCH: a command-complete holding only a status byte is not enough -
 * the core compares the event payload against the length its handler expects
 * (hci_cc_read_buffer_size wants 8 return parameters) and rejects anything
 * shorter with -EREMOTE ("unexpected cc 0x1005 length: 1 < 8"). Build the
 * whole reply here. On a cold boot this firmware often never answers 0x1005 in
 * time, but when it does (warm, ~1-2 ms) the payload is:
 *   acl_mtu=1021 acl_max_pkt=9 sco_mtu=255 sco_max_pkt=4
 * Injecting the old 31:1 defaults caused ACL credit starvation and A2DP
 * micro-dropouts until a late unexpected complete overwrote them. Prefer those
 * known-good sizes on cache miss; hci_cc_read_buffer_size() still rejects a
 * zero acl_mtu / acl_max_pkt.
 */
static int inject_read_buffer_size(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	const struct csr_fake_resp *fake = needs_fake_response(HCI_OP_READ_BUFFER_SIZE);
	bool cached = fake && fake->len >= (int)sizeof(struct hci_rp_read_buffer_size);
	int len = cached ? fake->len : (int)sizeof(struct hci_rp_read_buffer_size);
	struct sk_buff *skb;
	struct hci_event_hdr *hdr;
	struct hci_ev_cmd_complete *ec;
	struct hci_rp_read_buffer_size *rp;

	skb = bt_skb_alloc(sizeof(*hdr) + sizeof(*ec) + len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	/* LOCAL PATCH: this is the -22 of the run of 02:12:40. The packet type was
	 * only set on the cache-miss path below, so an event built from a cache hit
	 * reached hci_recv_frame() with an undefined type, the core's switch fell
	 * through to its default and returned -EINVAL
	 * ("sending frame failed (-22)", "Opcode 0x1005 failed: -22"). Both paths must
	 * set it, otherwise the -22 comes back as soon as 0x1005 has been answered once
	 * by the firmware and cached.
	 */
	hci_skb_pkt_type(skb) = HCI_EVENT_PKT;

	/* LOCAL PATCH: see the duplicate-answer suppression in btusb_recv_intr(). */
	data->last_injected = HCI_OP_READ_BUFFER_SIZE;

	hdr = skb_put(skb, sizeof(*hdr));
	hdr->evt = HCI_EV_CMD_COMPLETE;
	hdr->plen = sizeof(*ec) + len;

	ec = skb_put(skb, sizeof(*ec));
	ec->ncmd = 0x01;
	ec->opcode = cpu_to_le16(HCI_OP_READ_BUFFER_SIZE);

	rp = skb_put(skb, sizeof(*rp));

	if (cached) {
		/* The firmware did answer this once - replay what it said */
		bt_dev_info(hdev, "CSR clone: cache hit 0x%04x, inject %d bytes",
			    HCI_OP_READ_BUFFER_SIZE, len);
		memcpy(rp, fake->data, sizeof(*rp));
		return hci_recv_frame(hdev, skb);
	}

	bt_dev_info(hdev,
		    "CSR clone: cache miss 0x%04x, inject Barrot 8041-class buffer sizes",
		    HCI_OP_READ_BUFFER_SIZE);

	rp->status = 0x00;
	rp->acl_mtu = cpu_to_le16(1021);
	rp->sco_mtu = 255;
	rp->acl_max_pkt = cpu_to_le16(9);
	rp->sco_max_pkt = cpu_to_le16(4);

	/* hci_skb_pkt_type() is set once at the top of the function, for both paths */

	return hci_recv_frame(hdev, skb);
}

static int btusb_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;

	BT_DBG("%s", hdev->name);

	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT: {
		struct hci_command_hdr *cmd = (void *)skb->data;
		__u16 opcode = le16_to_cpu(cmd->opcode);
		struct csr_fake_resp *fake;
		int err;

		/* LOCAL PATCH: printed for every command before any decision is
		 * taken, so the trace shows which of the three paths answered it -
		 * the cache, the prefetch (this is the call from csr_prefetch()), or
		 * the plain wire path. It is deliberately before the reset branch and
		 * before needs_fake_response(), so the normal path is logged too.
		 */
		bt_dev_info(hdev, "CSR clone: send_frame opcode 0x%04x prefetching=%d",
			    opcode, data->prefetching);

		bt_dev_info(hdev, "TX %u ms cmd 0x%04x pm=%s cnt=%d reset=%d",
			    jiffies_to_msecs(jiffies), opcode, pm_state(data),
			    atomic_read(&hdev->cmd_cnt),
			    test_bit(HCI_RESET, &hdev->flags));

		/* LOCAL PATCH: a new command makes the previous injected answer stale,
		 * so start the duplicate check fresh for this opcode.
		 */
		data->last_injected = 0;

		/* LOCAL PATCH: stamp every command that goes on the wire so its answer can
		 * be measured against it (see csr_log_conn_provenance()).
		 */
		data->csr_last_cmd_opcode = opcode;
		data->csr_last_cmd_jiffies = jiffies;

		/* LOCAL PATCH: remember the target of a connection command so the
		 * matching event can be checked against it (csr_log_conn_provenance()).
		 * The peer address is not the first parameter for all three connection
		 * commands, so take its offset from the struct definitions.
		 */
		if (opcode == HCI_OP_CREATE_CONN ||
		    opcode == HCI_OP_LE_CREATE_CONN ||
		    opcode == HCI_OP_LE_EXT_CREATE_CONN) {
			const unsigned char *param = (const unsigned char *)skb->data +
				HCI_COMMAND_HDR_SIZE;
			int off = 0;

			if (opcode == HCI_OP_LE_CREATE_CONN)
				off = offsetof(struct hci_cp_le_create_conn, peer_addr);
			else if (opcode == HCI_OP_LE_EXT_CREATE_CONN)
				off = offsetof(struct hci_cp_le_ext_create_conn, peer_addr);

			if (cmd->plen >= off + 6) {
				const unsigned char *addr = param + off;

				csr_record_conn_target(data, opcode, addr, 6);

				bt_dev_warn(hdev,
					    "CSR clone: connection command 0x%04x target %02x:%02x:%02x:%02x:%02x:%02x",
					    opcode, addr[0], addr[1], addr[2],
					    addr[3], addr[4], addr[5]);
			}
		}

		/* LOCAL PATCH (quick fix): never serve HCI_Reset from the cache.
		 * Confirmed on the trace of 02:06:13 (device power-cycled):
		 *   prefetch 0x0c03  TX 86606074 -> RX 86606573 = 499 ms
		 *   core's own 0x0c03 TX 86606575 -> RX 86607076 = 501 ms
		 * so the real event arrives well inside HCI_CMD_TIMEOUT (2 s) and the
		 * core can wait for it. Replaying a cached reset here is wrong in
		 * itself: it tells the core the controller is ready while the firmware
		 * may still be booting, and the real command-complete then arrives as
		 * a stray "unexpected event for opcode 0x0c03". Let it go on the wire.
		 */
		if (opcode == HCI_OP_RESET) {
			bt_dev_info(hdev,
				    "CSR clone: HCI Reset sent to the firmware (no cache replay)");
			goto send_to_usb;
		}

		/* During the prefetch the command must reach the firmware,
		 * otherwise there is nothing to cache and the core would get a
		 * reply with no content.
		 */
		if (data->csr_clone && !data->prefetching &&
		    (fake = needs_fake_response(opcode))) {
			bt_dev_warn(hdev,
				    "CSR clone: fake response for opcode 0x%04x",
				    opcode);

			if (fake->to_firmware) {
				/* Still hand the command to the controller: the
				 * firmware needs the reset to finish its boot,
				 * it is only the reply that is too late.
				 * Ownership of skb passes to the tx path here
				 * (btusb_tx_complete frees it), so do not
				 * kfree_skb() in this branch.
				 */
				urb = alloc_ctrl_urb(hdev, skb);
				if (IS_ERR(urb))
					return PTR_ERR(urb);

				hdev->stat.cmd_tx++;

				err = submit_or_queue_tx_urb(hdev, urb);
				if (err < 0)
					return err;
			} else {
				/* The firmware cannot execute this command at
				 * all, so keep it off the wire: a real reply
				 * would only be a stray error event. Free the
				 * skb here because nothing owns it any more.
				 */
				kfree_skb(skb);
			}

			if (opcode == HCI_OP_READ_BUFFER_SIZE)
				return inject_read_buffer_size(hdev);

			return inject_cmd_complete(hdev, opcode);
		}

send_to_usb:
		urb = alloc_ctrl_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		hdev->stat.cmd_tx++;
		return submit_or_queue_tx_urb(hdev, urb);
	}

	case HCI_ACLDATA_PKT:
		urb = alloc_bulk_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		hdev->stat.acl_tx++;
		return submit_or_queue_tx_urb(hdev, urb);

	case HCI_SCODATA_PKT:
		if (!hci_dev_test_flag(hdev, HCI_USER_CHANNEL) &&
		    hci_conn_num(hdev, SCO_LINK) < 1)
			return -ENODEV;

		urb = alloc_isoc_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		hdev->stat.sco_tx++;
		return submit_tx_urb(hdev, urb);

	case HCI_ISODATA_PKT:
		urb = alloc_bulk_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		return submit_or_queue_tx_urb(hdev, urb);
	}

	return -EILSEQ;
}

static void btusb_notify(struct hci_dev *hdev, unsigned int evt)
{
	struct btusb_data *data = hci_get_drvdata(hdev);

	BT_DBG("%s evt %d", hdev->name, evt);

	if (hci_conn_num(hdev, SCO_LINK) != data->sco_num) {
		data->sco_num = hci_conn_num(hdev, SCO_LINK);
		data->air_mode = evt;
		schedule_work(&data->work);
	}
}

static inline int __set_isoc_interface(struct hci_dev *hdev, int altsetting)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct usb_interface *intf = data->isoc;
	struct usb_endpoint_descriptor *ep_desc;
	int i, err;

	if (!data->isoc)
		return -ENODEV;

	err = usb_set_interface(data->udev, data->isoc_ifnum, altsetting);
	if (err < 0) {
		bt_dev_err(hdev, "setting interface failed (%d)", -err);
		return err;
	}

	data->isoc_altsetting = altsetting;

	data->isoc_tx_ep = NULL;
	data->isoc_rx_ep = NULL;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		ep_desc = &intf->cur_altsetting->endpoint[i].desc;

		if (!data->isoc_tx_ep && usb_endpoint_is_isoc_out(ep_desc)) {
			data->isoc_tx_ep = ep_desc;
			continue;
		}

		if (!data->isoc_rx_ep && usb_endpoint_is_isoc_in(ep_desc)) {
			data->isoc_rx_ep = ep_desc;
			continue;
		}
	}

	if (!data->isoc_tx_ep || !data->isoc_rx_ep) {
		bt_dev_err(hdev, "invalid SCO descriptors");
		return -ENODEV;
	}

	return 0;
}

static int btusb_switch_alt_setting(struct hci_dev *hdev, int new_alts)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	int err;

	if (data->isoc_altsetting != new_alts) {
		unsigned long flags;

		clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
		usb_kill_anchored_urbs(&data->isoc_anchor);

		/* When isochronous alternate setting needs to be
		 * changed, because SCO connection has been added
		 * or removed, a packet fragment may be left in the
		 * reassembling state. This could lead to wrongly
		 * assembled fragments.
		 *
		 * Clear outstanding fragment when selecting a new
		 * alternate setting.
		 */
		spin_lock_irqsave(&data->rxlock, flags);
		dev_kfree_skb_irq(data->sco_skb);
		data->sco_skb = NULL;
		spin_unlock_irqrestore(&data->rxlock, flags);

		err = __set_isoc_interface(hdev, new_alts);
		if (err < 0)
			return err;
	}

	if (!test_and_set_bit(BTUSB_ISOC_RUNNING, &data->flags)) {
		if (btusb_submit_isoc_urb(hdev, GFP_KERNEL) < 0)
			clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
		else
			btusb_submit_isoc_urb(hdev, GFP_KERNEL);
	}

	return 0;
}

static struct usb_host_interface *btusb_find_altsetting(struct btusb_data *data,
							int alt)
{
	struct usb_interface *intf = data->isoc;
	int i;

	BT_DBG("Looking for Alt no :%d", alt);

	if (!intf)
		return NULL;

	for (i = 0; i < intf->num_altsetting; i++) {
		if (intf->altsetting[i].desc.bAlternateSetting == alt)
			return &intf->altsetting[i];
	}

	return NULL;
}

static void btusb_work(struct work_struct *work)
{
	struct btusb_data *data = container_of(work, struct btusb_data, work);
	struct hci_dev *hdev = data->hdev;
	int new_alts = 0;
	int err;

	if (data->sco_num > 0) {
		if (!test_bit(BTUSB_DID_ISO_RESUME, &data->flags)) {
			err = usb_autopm_get_interface(data->isoc ? data->isoc : data->intf);
			if (err < 0) {
				clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
				usb_kill_anchored_urbs(&data->isoc_anchor);
				return;
			}

			set_bit(BTUSB_DID_ISO_RESUME, &data->flags);
		}

		if (data->air_mode == HCI_NOTIFY_ENABLE_SCO_CVSD) {
			if (hdev->voice_setting & 0x0020) {
				static const int alts[3] = { 2, 4, 5 };
				unsigned int sco_idx;

				sco_idx = min_t(unsigned int, data->sco_num - 1,
						ARRAY_SIZE(alts) - 1);
				new_alts = alts[sco_idx];
			} else {
				new_alts = data->sco_num;
			}
		} else if (data->air_mode == HCI_NOTIFY_ENABLE_SCO_TRANSP) {
			/* Bluetooth USB spec recommends alt 6 (63 bytes), but
			 * many adapters do not support it.  Alt 1 appears to
			 * work for all adapters that do not have alt 6, and
			 * which work with WBS at all.  Some devices prefer
			 * alt 3 (HCI payload >= 60 Bytes let air packet
			 * data satisfy 60 bytes), requiring
			 * MTU >= 3 (packets) * 25 (size) - 3 (headers) = 72
			 * see also Core spec 5, vol 4, B 2.1.1 & Table 2.1.
			 */
			if (btusb_find_altsetting(data, 6))
				new_alts = 6;
			else if (btusb_find_altsetting(data, 3) &&
				 hdev->sco_mtu >= 72 &&
				 test_bit(BTUSB_USE_ALT3_FOR_WBS, &data->flags))
				new_alts = 3;
			else
				new_alts = 1;
		}

		if (btusb_switch_alt_setting(hdev, new_alts) < 0)
			bt_dev_err(hdev, "set USB alt:(%d) failed!", new_alts);
	} else {
		usb_kill_anchored_urbs(&data->isoc_anchor);

		if (test_and_clear_bit(BTUSB_ISOC_RUNNING, &data->flags))
			__set_isoc_interface(hdev, 0);

		if (test_and_clear_bit(BTUSB_DID_ISO_RESUME, &data->flags))
			usb_autopm_put_interface(data->isoc ? data->isoc : data->intf);
	}
}

static void btusb_waker(struct work_struct *work)
{
	struct btusb_data *data = container_of(work, struct btusb_data, waker);
	int err;

	err = usb_autopm_get_interface(data->intf);
	if (err < 0)
		return;

	usb_autopm_put_interface(data->intf);
}

static void btusb_rx_work(struct work_struct *work)
{
	struct btusb_data *data = container_of(work, struct btusb_data,
					       rx_work.work);
	struct sk_buff *skb;

	/* Dequeue ACL data received during the interval */
	while ((skb = skb_dequeue(&data->acl_q)))
		data->recv_acl(data->hdev, skb);
}

static int btusb_setup_bcm92035(struct hci_dev *hdev)
{
	struct sk_buff *skb;
	u8 val = 0x00;

	BT_DBG("%s", hdev->name);

	skb = __hci_cmd_sync(hdev, 0xfc3b, 1, &val, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb))
		bt_dev_err(hdev, "BCM92035 command failed (%ld)", PTR_ERR(skb));
	else
		kfree_skb(skb);

	return 0;
}

/*
 * LOCAL PATCH: send one command ourselves and wait for the firmware's answer on
 * our own completion. The core's request machinery cannot be used from here:
 * __hci_cmd_sync_status() waits HCI_CMD_TIMEOUT (2 s), which is shorter than the
 * first reply this clone gives after power-up (500 ms - 2.3 s, and 15 s for the
 * first HCI Reset), and the answer cannot be matched anyway because hdev->sent_cmd
 * and hdev->req_opcode belong to the caller's request, not to ours - the trace
 * showed a correct reset reply arriving 503 ms after the TX that still ended in
 * -110, because the core reported it as an unexpected event instead of completing
 * the request.
 *
 * Sending the control URB from here and completing in btusb_recv_intr() bypasses
 * cmd_cnt, the command timer, req_lock and the opcode matching entirely. What the
 * firmware answers is cached by csr_cache_response(), so the core's own call for
 * the same opcode is replayed instantly from the cache with the real payload.
 */
/*
 * LOCAL PATCH: the timeout the prefetch waits for the firmware's answer to its own
 * command. It was a fixed 60 s, which is longer than one open cycle: a reply that
 * arrives that late lands after the open has already moved on, and the next open
 * chains on the leftover event. Shortened to 5 s for this test, and made a module
 * parameter so it can be changed without rebuilding:
 *   insmod btusb.ko csr_prefetch_timeout=5000
 *
 * Trade-off to watch in the trace: the first answer after power-up is measured at
 * 2.2-2.3 s, and some of these clones take up to 15 s for the first reset, so a
 * short timeout can cut the cache fill short. If the trace shows
 * "prefetch 0x1003 failed: -110", the core then gets only a status byte, which is
 * too short for its handler, and the -110 comes back; put the value back to
 * something above HCI_CMD_TIMEOUT with margin (10000) for that case.
 */
static unsigned int csr_prefetch_timeout = 5000;
module_param(csr_prefetch_timeout, uint, 0644);

/*
 * LOCAL PATCH: the connection probe, off by default so the normal behaviour is
 * unchanged. Load with csr_conn_probe=1 to run it.
 */
module_param(csr_conn_probe, bool, 0644);

static int csr_prefetch(struct hci_dev *hdev, struct csr_fake_resp *fake)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct hci_command_hdr *cmd;
	struct sk_buff *skb;
	int err;

	skb = bt_skb_alloc(HCI_COMMAND_HDR_SIZE, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	cmd = skb_put(skb, HCI_COMMAND_HDR_SIZE);
	cmd->opcode = cpu_to_le16(fake->opcode);
	cmd->plen = 0;

	hci_skb_pkt_type(skb) = HCI_COMMAND_PKT;

	bt_dev_info(hdev, "prefetch 0x%04x: cmd_cnt %d cmd_q %d timeout %u ms",
		    fake->opcode, atomic_read(&hdev->cmd_cnt),
		    skb_queue_len(&hdev->cmd_q), csr_prefetch_timeout);

	csr_dump_state(data, "prefetch send");

	reinit_completion(&data->csr_done);
	data->wait_opcode = fake->opcode;

	/* Ownership of the skb passes to the tx path here */
	err = btusb_send_frame(hdev, skb);
	if (err)
		return err;

	if (!wait_for_completion_timeout(&data->csr_done,
					 msecs_to_jiffies(csr_prefetch_timeout))) {
		csr_dump_state(data, "prefetch timeout");
		return -ETIMEDOUT;
	}

	return 0;
}

/*
 * LOCAL PATCH: the decisive experiment for whether this clone can connect at all.
 *
 * Every reply seen so far could have been produced without a radio: cached
 * payloads, canned event frames, and invented status bytes. A connection event is
 * different, because the firmware cannot know the address of a device that does
 * not exist. So send HCI_OP_LE_EXT_CREATE_CONN (0x2043) to an address no device
 * can have, and read what comes back:
 *
 *   - Command status 0 only means the command was accepted. It is not a connection.
 *   - A connection event naming csr_probe_peer is invented: nothing on the air can
 *     name an address that was never advertised. That is the proof of fabrication.
 *   - A connection event naming a different address is the recurring 0x2f-style
 *     frame - the last thing the receiver saw on the air - so the RX path is real
 *     but its events are not tied to the command that was sent.
 *   - No event within csr_prefetch_timeout means connection commands are not
 *     executed at all, which is the same conclusion as the earlier "no event code
 *     0x03/0x04/0x05 was ever produced" observation.
 *
 * The target is recorded by btusb_send_frame() itself, so csr_log_conn_provenance()
 * compares the answer against this address and marks it as the probe address.
 */
static int csr_run_conn_probe(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct hci_command_hdr *cmd;
	struct hci_cp_le_ext_create_conn *cp;
	struct sk_buff *skb;
	int plen = sizeof(*cp) + sizeof(struct hci_cp_le_ext_conn_param);
	int err;

	if (!csr_conn_probe || data->conn_probe_ran)
		return 0;

	data->conn_probe_ran = true;

	skb = bt_skb_alloc(HCI_COMMAND_HDR_SIZE + plen, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	cmd = skb_put(skb, HCI_COMMAND_HDR_SIZE);
	cmd->opcode = cpu_to_le16(HCI_OP_LE_EXT_CREATE_CONN);
	cmd->plen = plen;

	cp = skb_put(skb, plen);
	memset(cp, 0, plen);
	memcpy(&cp->peer_addr, csr_probe_peer, 6);
	cp->phys = 0x01; /* LE 1M PHY, the rest of the parameter block stays zero */

	hci_skb_pkt_type(skb) = HCI_COMMAND_PKT;

	bt_dev_warn(hdev,
		    "CSR clone: CONN PROBE sending 0x%04x to %02x:%02x:%02x:%02x:%02x:%02x. "
		    "No device can answer this address, so a connection event naming it is invented.",
		    HCI_OP_LE_EXT_CREATE_CONN,
		    csr_probe_peer[0], csr_probe_peer[1], csr_probe_peer[2],
		    csr_probe_peer[3], csr_probe_peer[4], csr_probe_peer[5]);

	/* Run it through the same send path the core uses, so the answer is read
	 * from the wire rather than from the cache.
	 */
	reinit_completion(&data->csr_done);
	data->wait_opcode = HCI_OP_LE_EXT_CREATE_CONN;

	err = btusb_send_frame(hdev, skb);
	if (err)
		return err;

	if (!wait_for_completion_timeout(&data->csr_done,
					 msecs_to_jiffies(csr_prefetch_timeout))) {
		csr_dump_state(data, "conn probe timeout");
		bt_dev_warn(hdev,
			    "CSR clone: CONN PROBE no command status for 0x%04x within %u ms - "
			    "connection commands are not executed",
			    HCI_OP_LE_EXT_CREATE_CONN, csr_prefetch_timeout);
		return -ETIMEDOUT;
	}

	bt_dev_warn(hdev,
		    "CSR clone: CONN PROBE command status received. Watch the provenance lines "
		    "for the next few seconds: an event naming %02x:%02x:%02x:%02x:%02x:%02x is fabricated.",
		    csr_probe_peer[0], csr_probe_peer[1], csr_probe_peer[2],
		    csr_probe_peer[3], csr_probe_peer[4], csr_probe_peer[5]);

	/* The connection event, if there is one, arrives after the status, so stop
	 * waiting here and let it reach the core: it is only visible if the core is
	 * handed the event.
	 */
	data->wait_opcode = 0;

	return 0;
}

/*
 * LOCAL PATCH: what setup returns and what the cache holds at the moment it
 * returns. The core's init stages run after this, so this is the last line that
 * explains whether a later request had a real payload to replay or only a status
 * byte - which is the difference between a cache hit and a -110.
 */
static int csr_log_setup_exit(struct hci_dev *hdev, int ret)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	size_t i, prefetched = 0, cached = 0;

	for (i = 0; i < ARRAY_SIZE(fake_resp_opcodes); i++) {
		if (!fake_resp_opcodes[i].prefetch)
			continue;

		prefetched++;
		if (fake_resp_opcodes[i].len)
			cached++;
	}

	bt_dev_info(hdev,
		    "CSR clone: setup exit %d: clone %d prefetching %d, "
		    "cache %zu/%zu opcodes, prefetch timeout %u ms",
		    ret, data->csr_clone, data->prefetching, cached, prefetched,
		    csr_prefetch_timeout);

	return ret;
}

static int btusb_setup_csr(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	u16 bcdDevice = le16_to_cpu(data->udev->descriptor.bcdDevice);
	struct hci_rp_read_local_version *rp;
	struct sk_buff *skb;
	bool is_fake = false;
	size_t i;
	int ret = 0;

	BT_DBG("%s", hdev->name);

	/* LOCAL PATCH: the state the core will act on for its first command.
	 * This is the point where the -110 is decided: if cmd_cnt is 0 here, the
	 * command is queued but never transmitted and only the command timer can
	 * end the request.
	 */
	csr_dump_state(data, "setup entry");

	/* LOCAL PATCH: open the probe window here rather than in btusb_probe(),
	 * because the open path is where a core request is actually pending, and the
	 * request that fails is issued after this function returns.
	 */
	csr_arm_probe(data);

	skb = __hci_cmd_sync(hdev, HCI_OP_READ_LOCAL_VERSION, 0, NULL,
			     HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		int err = PTR_ERR(skb);
		bt_dev_err(hdev, "CSR: Local version failed (%d)", err);
		return csr_log_setup_exit(hdev, err);
	}

	rp = skb_pull_data(skb, sizeof(*rp));
	if (!rp) {
		bt_dev_err(hdev, "CSR: Local version length mismatch");
		kfree_skb(skb);
		return csr_log_setup_exit(hdev, -EIO);
	}

	bt_dev_info(hdev, "CSR: Setting up dongle with HCI ver=%u rev=%04x",
		    rp->hci_ver, le16_to_cpu(rp->hci_rev));

	bt_dev_info(hdev, "LMP ver=%u subver=%04x; manufacturer=%u",
		    rp->lmp_ver, le16_to_cpu(rp->lmp_subver),
		    le16_to_cpu(rp->manufacturer));

	/* Detect a wide host of Chinese controllers that aren't CSR.
	 *
	 * Known fake bcdDevices: 0x0100, 0x0134, 0x1915, 0x2520, 0x7558, 0x8891
	 *
	 * The main thing they have in common is that these are really popular low-cost
	 * options that support newer Bluetooth versions but rely on heavy VID/PID
	 * squatting of this poor old Bluetooth 1.1 device. Even sold as such.
	 *
	 * We detect actual CSR devices by checking that the HCI manufacturer code
	 * is Cambridge Silicon Radio (10) and ensuring that LMP sub-version and
	 * HCI rev values always match. As they both store the firmware number.
	 */
	if (le16_to_cpu(rp->manufacturer) != 10 ||
	    le16_to_cpu(rp->hci_rev) != le16_to_cpu(rp->lmp_subver))
		is_fake = true;

	/* Known legit CSR firmware build numbers and their supported BT versions:
	 * - 1.1 (0x1) -> 0x0073, 0x020d, 0x033c, 0x034e
	 * - 1.2 (0x2) ->                 0x04d9, 0x0529
	 * - 2.0 (0x3) ->         0x07a6, 0x07ad, 0x0c5c
	 * - 2.1 (0x4) ->         0x149c, 0x1735, 0x1899 (0x1899 is a BlueCore4-External)
	 * - 4.0 (0x6) ->         0x1d86, 0x2031, 0x22bb
	 *
	 * e.g. Real CSR dongles with LMP subversion 0x73 are old enough that
	 *      support BT 1.1 only; so it's a dead giveaway when some
	 *      third-party BT 4.0 dongle reuses it.
	 */
	else if (le16_to_cpu(rp->lmp_subver) <= 0x034e &&
		 rp->hci_ver > BLUETOOTH_VER_1_1)
		is_fake = true;

	else if (le16_to_cpu(rp->lmp_subver) <= 0x0529 &&
		 rp->hci_ver > BLUETOOTH_VER_1_2)
		is_fake = true;

	else if (le16_to_cpu(rp->lmp_subver) <= 0x0c5c &&
		 rp->hci_ver > BLUETOOTH_VER_2_0)
		is_fake = true;

	else if (le16_to_cpu(rp->lmp_subver) <= 0x1899 &&
		 rp->hci_ver > BLUETOOTH_VER_2_1)
		is_fake = true;

	else if (le16_to_cpu(rp->lmp_subver) <= 0x22bb &&
		 rp->hci_ver > BLUETOOTH_VER_4_0)
		is_fake = true;

	/* Other clones which beat all the above checks */
	else if (bcdDevice == 0x0134 &&
		 le16_to_cpu(rp->lmp_subver) == 0x0c5c &&
		 rp->hci_ver == BLUETOOTH_VER_2_0)
		is_fake = true;

	if (is_fake) {
		bt_dev_warn(hdev, "CSR: Unbranded CSR clone detected; adding workarounds and force-suspending once...");

		/* LOCAL PATCH: this firmware is broken in two ways that both
		 * stop the core from finishing its init:
		 *  - it never answers HCI Read Buffer Size (0x1005);
		 *  - its first HCI Reset (0x0c03) takes 2.2 s, longer than
		 *    HCI_CMD_TIMEOUT (2 s);
		 * and it emits stray bytes on the interrupt endpoint that
		 * desynchronize the event framing. Let btusb_send_frame()
		 * answer the first two locally and btusb_recv_intr() drop the
		 * stray bytes. See the comments near fake_resp_opcodes().
		 */
		data->csr_clone = true;

		/* Generally these clones have big discrepancies between
		 * advertised features and what's actually supported.
		 * Probably will need to be expanded in the future;
		 * without these the controller will lock up.
		 */
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_STORED_LINK_KEY);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_ERR_DATA_REPORTING);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_FILTER_CLEAR_ALL);
		hci_set_quirk(hdev, HCI_QUIRK_NO_SUSPEND_NOTIFIER);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_READ_VOICE_SETTING);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_READ_PAGE_SCAN_TYPE);

		/* LOCAL PATCH: setup is persistent by default, so btusb_setup_csr()
		 * runs once - at the first open after insmod - and never again. That is
		 * why trace.sh prints no setup lines while reload.sh does, and why the
		 * cache is never refreshed from the firmware's real post-power-cycle
		 * answers. Realtek already does this below (HCI_QUIRK_NON_PERSISTENT_SETUP
		 * in the Realtek block); for this clone it also means the state the
		 * firmware has after a power cycle is re-read on every open.
		 */
		hci_set_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);

		/* Clear the reset quirk since this is not an actual
		 * early Bluetooth 1.1 device from CSR.
		 */
		hci_clear_quirk(hdev, HCI_QUIRK_RESET_ON_CLOSE);
		hci_clear_quirk(hdev, HCI_QUIRK_SIMULTANEOUS_DISCOVERY);

		/*
		 * Special workaround for these BT 4.0 chip clones, and potentially more:
		 *
		 * - 0x0134: a Barrot 8041a02                 (HCI rev: 0x0810 sub: 0x1012)
		 * - 0x7558: IC markings FR3191AHAL 749H15143 (HCI rev/sub-version: 0x0709)
		 *
		 * These controllers are really messed-up.
		 *
		 * 1. Their bulk RX endpoint will never report any data unless
		 *    the device was suspended at least once (yes, really).
		 * 2. They will not wakeup when autosuspended and receiving data
		 *    on their bulk RX endpoint from e.g. a keyboard or mouse
		 *    (IOW remote-wakeup support is broken for the bulk endpoint).
		 *
		 * To fix 1. enable runtime-suspend, force-suspend the
		 * HCI and then wake-it up by disabling runtime-suspend.
		 *
		 * To fix 2. clear the HCI's can_wake flag, this way the HCI
		 * will still be autosuspended when it is not open.
		 *
		 * --
		 *
		 * Because these are widespread problems we prefer generic solutions; so
		 * apply this initialization quirk to every controller that gets here,
		 * it should be harmless. The alternative is to not work at all.
		 */
		/* LOCAL PATCH: this clone does not support USB suspend at all
		 * (writing "suspend" to its sysfs power/level returns -EINVAL),
		 * so the receive-issue workaround can never be applied and the
		 * device stays dead: every following HCI command times out.
		 * Skipping the suspend block is what makes it work.
		 */
		ret = 0;

		device_set_wakeup_capable(&data->udev->dev, false);

		/* LOCAL PATCH: the RX endpoint goes deaf when this clone is
		 * runtime-suspended, so it must stay awake - that is what the
		 * "force-suspend then wake it up" trick above was meant to
		 * achieve here, and it is also the reason the workaround cannot
		 * work at all on this device.
		 *
		 * Note that usb_disable_autosuspend() does not mean "never
		 * suspend": it sets autosuspend_delay to -1, which the PM core
		 * reads as "suspend immediately when the last reference is
		 * dropped" (that is what the sysfs file showed:
		 * autosuspend_delay_ms = 0, and the device was suspended again
		 * as soon as hci0 was closed). Forbid runtime PM instead, which
		 * is what this clone actually needs - it cannot be suspended
		 * meaningfully anyway.
		 */
		pm_runtime_forbid(&data->udev->dev);

		/* LOCAL PATCH: do not re-enable autosuspend here. With
		 * enable_autosuspend (the default for this build) the re-enable made
		 * the device suspend again as soon as hci0 was closed, and the RX
		 * endpoint goes deaf on this clone - that is exactly the -110 the
		 * earlier traces showed. Forbid it on the interface as well: the
		 * interrupt endpoint belongs to it, so its runtime_status is what
		 * decides whether RX URBs can complete at all.
		 */
		if (data->intf)
			pm_runtime_forbid(&data->intf->dev);

		/* LOCAL PATCH: hold one active reference for the whole lifetime of
		 * the clone. With runtime PM forbidden nothing schedules an idle
		 * suspend, but the device would still be suspended if the reference
		 * count reached zero (hci0 closed, no URB in flight). Holding it is
		 * what keeps runtime_status active, and btusb_disconnect() releases it.
		 */
		data->csr_pm_hold = true;
		pm_runtime_get_noresume(&data->udev->dev);

		/* LOCAL PATCH: do the commands whose first reply is slow here,
		 * before the core's init stages, because this is the only place
		 * where a long timeout applies - HCI_INIT_TIMEOUT, not the
		 * HCI_CMD_TIMEOUT (2 s) the core uses for its own init commands,
		 * which cannot be raised from btusb. The trace showed the first
		 * post-power-up answers taking 500 ms - 2.3 s: Read Local LMP
		 * Features (0x1003) failed with -110 (ETIMEDOUT) even though the
		 * firmware answered 2285 ms after the TX, and HCI Reset (0x0c03)
		 * took 2245 ms on its first attempt and 3 ms afterwards.
		 *
		 * Whatever the firmware answers here is cached by
		 * csr_cache_response(), so the core's own commands for the same
		 * opcodes get an immediate, correct replay - including the real
		 * feature bits, which is why nothing is fabricated for 0x1003.
		 */
		data->prefetching = true;

		for (i = 0; i < ARRAY_SIZE(fake_resp_opcodes); i++) {
			struct csr_fake_resp *fake = &fake_resp_opcodes[i];
			int err;

			if (!fake->prefetch)
				continue;

			err = csr_prefetch(hdev, fake);
			if (err) {
				bt_dev_warn(hdev,
					    "CSR clone: prefetch 0x%04x failed: %d",
					    fake->opcode, err);
				continue;
			}

			bt_dev_info(hdev,
				    "CSR clone: prefetched 0x%04x (%d bytes cached)",
				    fake->opcode, fake->len);
		}

		data->prefetching = false;
		data->wait_opcode = 0;

		/* LOCAL PATCH: the connection probe. It has to run while
		 * prefetching is still on, because the answer is matched by
		 * csr_event_is_waited() on this path only - and it has to run before
		 * setup returns, so the event it provokes is logged while the flag
		 * that made the send possible is still set.
		 */
		data->prefetching = true;
		csr_run_conn_probe(hdev);
		data->wait_opcode = 0;
		data->prefetching = false;
	}

	kfree_skb(skb);

	return csr_log_setup_exit(hdev, ret);
}

static int inject_cmd_complete(struct hci_dev *hdev, __u16 opcode)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	const struct csr_fake_resp *fake = needs_fake_response(opcode);
	int len = fake && fake->len ? fake->len : 1;
	struct sk_buff *skb;
	struct hci_event_hdr *hdr;
	struct hci_ev_cmd_complete *evt;

	/* LOCAL PATCH: remember what we answered, so btusb_recv_intr() can drop the
	 * firmware's late answer for the same opcode instead of letting the core see
	 * "unexpected event for opcode 0x%04x".
	 */
	data->last_injected = opcode;

	if (fake && fake->len)
		bt_dev_info(hdev, "CSR clone: cache hit 0x%04x, inject %d bytes",
			    opcode, len);
	else
		bt_dev_info(hdev,
			    "CSR clone: cache miss 0x%04x, inject a status byte",
			    opcode);

	skb = bt_skb_alloc(sizeof(*hdr) + sizeof(*evt) + len, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	hdr = skb_put(skb, sizeof(*hdr));
	hdr->evt = HCI_EV_CMD_COMPLETE;
	hdr->plen = sizeof(*evt) + len;

	evt = skb_put(skb, sizeof(*evt));
	evt->ncmd = 0x01;
	evt->opcode = cpu_to_le16(opcode);

	if (fake && fake->len)
		skb_put_data(skb, fake->data, fake->len);
	else
		skb_put_u8(skb, 0x00);

	hci_skb_pkt_type(skb) = HCI_EVENT_PKT;

	return hci_recv_frame(hdev, skb);
}

static int btusb_recv_bulk_intel(struct btusb_data *data, void *buffer,
				 int count)
{
	struct hci_dev *hdev = data->hdev;

	/* When the device is in bootloader mode, then it can send
	 * events via the bulk endpoint. These events are treated the
	 * same way as the ones received from the interrupt endpoint.
	 */
	if (btintel_test_flag(hdev, INTEL_BOOTLOADER))
		return btusb_recv_intr(data, buffer, count);

	return btusb_recv_bulk(data, buffer, count);
}

static int btusb_send_frame_intel(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct urb *urb;

	BT_DBG("%s", hdev->name);

	switch (hci_skb_pkt_type(skb)) {
	case HCI_COMMAND_PKT:
		if (btintel_test_flag(hdev, INTEL_BOOTLOADER)) {
			struct hci_command_hdr *cmd = (void *)skb->data;
			__u16 opcode = le16_to_cpu(cmd->opcode);

			/* When in bootloader mode and the command 0xfc09
			 * is received, it needs to be send down the
			 * bulk endpoint. So allocate a bulk URB instead.
			 */
			if (opcode == 0xfc09)
				urb = alloc_bulk_urb(hdev, skb);
			else
				urb = alloc_ctrl_urb(hdev, skb);

			/* When the BTINTEL_HCI_OP_RESET command is issued to
			 * boot into the operational firmware, it will actually
			 * not send a command complete event. To keep the flow
			 * control working inject that event here.
			 */
			if (opcode == BTINTEL_HCI_OP_RESET)
				inject_cmd_complete(hdev, opcode);
		} else {
			urb = alloc_ctrl_urb(hdev, skb);
		}
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		hdev->stat.cmd_tx++;
		return submit_or_queue_tx_urb(hdev, urb);

	case HCI_ACLDATA_PKT:
		urb = alloc_bulk_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		hdev->stat.acl_tx++;
		return submit_or_queue_tx_urb(hdev, urb);

	case HCI_SCODATA_PKT:
		if (!hci_dev_test_flag(hdev, HCI_USER_CHANNEL) &&
		    hci_conn_num(hdev, SCO_LINK) < 1)
			return -ENODEV;

		urb = alloc_isoc_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		hdev->stat.sco_tx++;
		return submit_tx_urb(hdev, urb);

	case HCI_ISODATA_PKT:
		urb = alloc_bulk_urb(hdev, skb);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		return submit_or_queue_tx_urb(hdev, urb);
	}

	return -EILSEQ;
}

static int btusb_setup_realtek(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	int ret;

	ret = btrtl_setup_realtek(hdev);

	if (btrealtek_test_flag(data->hdev, REALTEK_ALT6_CONTINUOUS_TX_CHIP))
		set_bit(BTUSB_ALT6_CONTINUOUS_TX, &data->flags);

	return ret;
}

static int btusb_recv_event_realtek(struct hci_dev *hdev, struct sk_buff *skb)
{
	if (skb->data[0] == HCI_VENDOR_PKT && skb->data[2] == RTK_SUB_EVENT_CODE_COREDUMP) {
		struct rtk_dev_coredump_hdr hdr = {
			.code = RTK_DEVCOREDUMP_CODE_MEMDUMP,
		};

		bt_dev_dbg(hdev, "RTL: received coredump vendor evt, len %u",
			skb->len);

		btusb_rtl_alloc_devcoredump(hdev, &hdr, skb->data, skb->len);
		kfree_skb(skb);

		return 0;
	}

	return hci_recv_frame(hdev, skb);
}

static void btusb_mtk_claim_iso_intf(struct btusb_data *data)
{
	struct btmtk_data *btmtk_data;
	int err;

	if (!data->hdev)
		return;

	btmtk_data = hci_get_priv(data->hdev);
	if (!btmtk_data)
		return;

	if (!btmtk_data->isopkt_intf) {
		bt_dev_err(data->hdev, "Can't claim NULL iso interface");
		return;
	}

	/*
	 * The function usb_driver_claim_interface() is documented to need
	 * locks held if it's not called from a probe routine. The code here
	 * is called from the hci_power_on workqueue, so grab the lock.
	 */
	device_lock(&btmtk_data->isopkt_intf->dev);
	err = usb_driver_claim_interface(&btusb_driver,
					 btmtk_data->isopkt_intf, data);
	device_unlock(&btmtk_data->isopkt_intf->dev);
	if (err < 0) {
		btmtk_data->isopkt_intf = NULL;
		bt_dev_err(data->hdev, "Failed to claim iso interface: %d", err);
		return;
	}

	set_bit(BTMTK_ISOPKT_OVER_INTR, &btmtk_data->flags);
	init_usb_anchor(&btmtk_data->isopkt_anchor);
}

static void btusb_mtk_release_iso_intf(struct hci_dev *hdev)
{
	struct btmtk_data *btmtk_data;

	if (!hdev)
		return;

	btmtk_data = hci_get_priv(hdev);
	if (!btmtk_data)
		return;

	if (test_bit(BTMTK_ISOPKT_OVER_INTR, &btmtk_data->flags)) {
		usb_kill_anchored_urbs(&btmtk_data->isopkt_anchor);
		clear_bit(BTMTK_ISOPKT_RUNNING, &btmtk_data->flags);

		if (btmtk_data->isopkt_skb) {
			dev_kfree_skb_irq(btmtk_data->isopkt_skb);
			btmtk_data->isopkt_skb = NULL;
		}

		if (btmtk_data->isopkt_intf) {
			usb_set_intfdata(btmtk_data->isopkt_intf, NULL);
			usb_driver_release_interface(&btusb_driver,
						     btmtk_data->isopkt_intf);
			btmtk_data->isopkt_intf = NULL;
		}
	}

	clear_bit(BTMTK_ISOPKT_OVER_INTR, &btmtk_data->flags);
}

static int btusb_mtk_disconnect(struct hci_dev *hdev)
{
	/* This function describes the specific additional steps taken by MediaTek
	 * when Bluetooth usb driver's resume function is called.
	 */
	btusb_mtk_release_iso_intf(hdev);

	return 0;
}

static int btusb_mtk_reset(struct hci_dev *hdev, void *rst_data)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct btmtk_data *btmtk_data = hci_get_priv(hdev);
	int err;

	/* It's MediaTek specific bluetooth reset mechanism via USB */
	if (test_and_set_bit(BTMTK_HW_RESET_ACTIVE, &btmtk_data->flags)) {
		bt_dev_err(hdev, "last reset failed? Not resetting again");
		return -EBUSY;
	}

	err = usb_autopm_get_interface(data->intf);
	if (err < 0)
		return err;

	/* Release MediaTek ISO data interface */
	btusb_mtk_release_iso_intf(hdev);

	btusb_stop_traffic(data);
	usb_kill_anchored_urbs(&data->tx_anchor);

	/* Toggle the hard reset line. The MediaTek device is going to
	 * yank itself off the USB and then replug. The cleanup is handled
	 * correctly on the way out (standard USB disconnect), and the new
	 * device is detected cleanly and bound to the driver again like
	 * it should be.
	 */
	if (data->reset_gpio) {
		gpiod_set_value_cansleep(data->reset_gpio, 1);
		msleep(200);
		gpiod_set_value_cansleep(data->reset_gpio, 0);
		return 0;
	}

	err = btmtk_usb_subsys_reset(hdev, btmtk_data->dev_id);

	usb_queue_reset_device(data->intf);
	clear_bit(BTMTK_HW_RESET_ACTIVE, &btmtk_data->flags);

	return err;
}

static int btusb_send_frame_mtk(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct urb *urb;

	BT_DBG("%s", hdev->name);

	if (hci_skb_pkt_type(skb) == HCI_ISODATA_PKT) {
		urb = alloc_mtk_intr_urb(hdev, skb, btusb_tx_complete);
		if (IS_ERR(urb))
			return PTR_ERR(urb);

		return submit_or_queue_tx_urb(hdev, urb);
	} else {
		return btusb_send_frame(hdev, skb);
	}
}

static int btusb_mtk_setup(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct btmtk_data *btmtk_data = hci_get_priv(hdev);

	/* MediaTek WMT vendor cmd requiring below USB resources to
	 * complete the handshake.
	 */
	btmtk_data->drv_name = btusb_driver.name;
	btmtk_data->intf = data->intf;
	btmtk_data->udev = data->udev;
	btmtk_data->ctrl_anchor = &data->ctrl_anchor;
	btmtk_data->reset_sync = btusb_mtk_reset;

	/* Claim ISO data interface and endpoint */
	if (!test_bit(BTMTK_ISOPKT_OVER_INTR, &btmtk_data->flags)) {
		btmtk_data->isopkt_intf = usb_ifnum_to_if(data->udev, MTK_ISO_IFNUM);
		btusb_mtk_claim_iso_intf(data);
	}

	return btmtk_usb_setup(hdev);
}

static int btusb_mtk_shutdown(struct hci_dev *hdev)
{
	int ret;

	ret = btmtk_usb_shutdown(hdev);

	/* Release MediaTek iso interface after shutdown */
	btusb_mtk_release_iso_intf(hdev);

	return ret;
}

#ifdef CONFIG_PM
/* Configure an out-of-band gpio as wake-up pin, if specified in device tree */
static int marvell_config_oob_wake(struct hci_dev *hdev)
{
	struct sk_buff *skb;
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct device *dev = &data->udev->dev;
	u16 pin, gap, opcode;
	int ret;
	u8 cmd[5];

	/* Move on if no wakeup pin specified */
	if (of_property_read_u16(dev->of_node, "marvell,wakeup-pin", &pin) ||
	    of_property_read_u16(dev->of_node, "marvell,wakeup-gap-ms", &gap))
		return 0;

	/* Vendor specific command to configure a GPIO as wake-up pin */
	opcode = hci_opcode_pack(0x3F, 0x59);
	cmd[0] = opcode & 0xFF;
	cmd[1] = opcode >> 8;
	cmd[2] = 2; /* length of parameters that follow */
	cmd[3] = pin;
	cmd[4] = gap; /* time in ms, for which wakeup pin should be asserted */

	skb = bt_skb_alloc(sizeof(cmd), GFP_KERNEL);
	if (!skb) {
		bt_dev_err(hdev, "%s: No memory", __func__);
		return -ENOMEM;
	}

	skb_put_data(skb, cmd, sizeof(cmd));
	hci_skb_pkt_type(skb) = HCI_COMMAND_PKT;

	ret = btusb_send_frame(hdev, skb);
	if (ret) {
		bt_dev_err(hdev, "%s: configuration failed", __func__);
		kfree_skb(skb);
		return ret;
	}

	return 0;
}
#endif

static int btusb_set_bdaddr_marvell(struct hci_dev *hdev,
				    const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;
	u8 buf[8];
	long ret;

	buf[0] = 0xfe;
	buf[1] = sizeof(bdaddr_t);
	memcpy(buf + 2, bdaddr, sizeof(bdaddr_t));

	skb = __hci_cmd_sync(hdev, 0xfc22, sizeof(buf), buf, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		ret = PTR_ERR(skb);
		bt_dev_err(hdev, "changing Marvell device address failed (%ld)",
			   ret);
		return ret;
	}
	kfree_skb(skb);

	return 0;
}

static int btusb_set_bdaddr_ath3012(struct hci_dev *hdev,
				    const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;
	u8 buf[10];
	long ret;

	buf[0] = 0x01;
	buf[1] = 0x01;
	buf[2] = 0x00;
	buf[3] = sizeof(bdaddr_t);
	memcpy(buf + 4, bdaddr, sizeof(bdaddr_t));

	skb = __hci_cmd_sync(hdev, 0xfc0b, sizeof(buf), buf, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		ret = PTR_ERR(skb);
		bt_dev_err(hdev, "Change address command failed (%ld)", ret);
		return ret;
	}
	kfree_skb(skb);

	return 0;
}

static int btusb_set_bdaddr_wcn6855(struct hci_dev *hdev,
				const bdaddr_t *bdaddr)
{
	struct sk_buff *skb;
	u8 buf[6];
	long ret;

	memcpy(buf, bdaddr, sizeof(bdaddr_t));

	skb = __hci_cmd_sync_ev(hdev, 0xfc14, sizeof(buf), buf,
				HCI_EV_CMD_COMPLETE, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		ret = PTR_ERR(skb);
		bt_dev_err(hdev, "Change address command failed (%ld)", ret);
		return ret;
	}
	kfree_skb(skb);

	return 0;
}

#define QCA_MEMDUMP_ACL_HANDLE 0x2EDD
#define QCA_MEMDUMP_SIZE_MAX  0x100000
#define QCA_MEMDUMP_VSE_CLASS 0x01
#define QCA_MEMDUMP_MSG_TYPE 0x08
#define QCA_MEMDUMP_PKT_SIZE 248
#define QCA_LAST_SEQUENCE_NUM 0xffff

struct qca_dump_hdr {
	u8 vse_class;
	u8 msg_type;
	__le16 seqno;
	u8 reserved;
	union {
		u8 data[0];
		struct {
			__le32 ram_dump_size;
			u8 data0[0];
		} __packed;
	};
} __packed;


static void btusb_dump_hdr_qca(struct hci_dev *hdev, struct sk_buff *skb)
{
	char buf[128];
	struct btusb_data *btdata = hci_get_drvdata(hdev);

	snprintf(buf, sizeof(buf), "Controller Name: 0x%x\n",
			btdata->qca_dump.controller_id);
	skb_put_data(skb, buf, strlen(buf));

	snprintf(buf, sizeof(buf), "Firmware Version: 0x%x\n",
			btdata->qca_dump.fw_version);
	skb_put_data(skb, buf, strlen(buf));

	snprintf(buf, sizeof(buf), "Driver: %s\nVendor: qca\n",
			btusb_driver.name);
	skb_put_data(skb, buf, strlen(buf));

	snprintf(buf, sizeof(buf), "VID: 0x%x\nPID:0x%x\n",
			btdata->qca_dump.id_vendor, btdata->qca_dump.id_product);
	skb_put_data(skb, buf, strlen(buf));

	snprintf(buf, sizeof(buf), "Lmp Subversion: 0x%x\n",
			hdev->lmp_subver);
	skb_put_data(skb, buf, strlen(buf));
}

static void btusb_coredump_qca(struct hci_dev *hdev)
{
	int err;
	static const u8 param[] = { 0x26 };

	err = __hci_cmd_send(hdev, 0xfc0c, 1, param);
	if (err < 0)
		bt_dev_err(hdev, "%s: triggle crash failed (%d)", __func__, err);
}

/* Return: 0 on success, negative errno on failure. */
static int handle_dump_pkt_qca(struct hci_dev *hdev, struct sk_buff *skb)
{
	int ret = 0;
	unsigned int skip = 0;
	u8 pkt_type;
	u16 seqno;
	u32 dump_size;

	struct qca_dump_hdr *dump_hdr;
	struct btusb_data *btdata = hci_get_drvdata(hdev);
	struct usb_device *udev = btdata->udev;

	pkt_type = hci_skb_pkt_type(skb);
	skip = sizeof(struct hci_event_hdr);
	if (pkt_type == HCI_ACLDATA_PKT)
		skip += sizeof(struct hci_acl_hdr);

	skb_pull(skb, skip);
	dump_hdr = (struct qca_dump_hdr *)skb->data;

	seqno = le16_to_cpu(dump_hdr->seqno);
	if (seqno == 0) {
		set_bit(BTUSB_HW_SSR_ACTIVE, &btdata->flags);
		dump_size = le32_to_cpu(dump_hdr->ram_dump_size);
		if (!dump_size || (dump_size > QCA_MEMDUMP_SIZE_MAX)) {
			ret = -EILSEQ;
			bt_dev_err(hdev, "Invalid memdump size(%u)",
				   dump_size);
			goto out;
		}

		ret = hci_devcd_init(hdev, dump_size);
		if (ret < 0) {
			bt_dev_err(hdev, "memdump init error(%d)", ret);
			goto out;
		}

		btdata->qca_dump.ram_dump_size = dump_size;
		btdata->qca_dump.ram_dump_seqno = 0;

		skb_pull(skb, offsetof(struct qca_dump_hdr, data0));

		usb_disable_autosuspend(udev);
		bt_dev_info(hdev, "%s memdump size(%u)\n",
			    (pkt_type == HCI_ACLDATA_PKT) ? "ACL" : "event",
			    dump_size);
	} else {
		skb_pull(skb, offsetof(struct qca_dump_hdr, data));
	}

	if (!btdata->qca_dump.ram_dump_size) {
		ret = -EINVAL;
		bt_dev_err(hdev, "memdump is not active");
		goto out;
	}

	if ((seqno > btdata->qca_dump.ram_dump_seqno + 1) && (seqno != QCA_LAST_SEQUENCE_NUM)) {
		dump_size = QCA_MEMDUMP_PKT_SIZE * (seqno - btdata->qca_dump.ram_dump_seqno - 1);
		hci_devcd_append_pattern(hdev, 0x0, dump_size);
		bt_dev_err(hdev,
			   "expected memdump seqno(%u) is not received(%u)\n",
			   btdata->qca_dump.ram_dump_seqno, seqno);
		btdata->qca_dump.ram_dump_seqno = seqno;
		kfree_skb(skb);
		return ret;
	}

	hci_devcd_append(hdev, skb);
	btdata->qca_dump.ram_dump_seqno++;
	if (seqno == QCA_LAST_SEQUENCE_NUM) {
		bt_dev_info(hdev,
				"memdump done: pkts(%u), total(%u)\n",
				btdata->qca_dump.ram_dump_seqno, btdata->qca_dump.ram_dump_size);

		hci_devcd_complete(hdev);
		goto out;
	}
	return ret;

out:
	if (btdata->qca_dump.ram_dump_size)
		usb_enable_autosuspend(udev);
	btdata->qca_dump.ram_dump_size = 0;
	btdata->qca_dump.ram_dump_seqno = 0;
	clear_bit(BTUSB_HW_SSR_ACTIVE, &btdata->flags);

	if (ret < 0)
		kfree_skb(skb);
	return ret;
}

/* Return: true if the ACL packet is a dump packet, false otherwise. */
static bool acl_pkt_is_dump_qca(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct hci_event_hdr *event_hdr;
	struct hci_acl_hdr *acl_hdr;
	struct qca_dump_hdr *dump_hdr;
	struct sk_buff *clone = skb_clone(skb, GFP_ATOMIC);
	bool is_dump = false;

	if (!clone)
		return false;

	acl_hdr = skb_pull_data(clone, sizeof(*acl_hdr));
	if (!acl_hdr || (le16_to_cpu(acl_hdr->handle) != QCA_MEMDUMP_ACL_HANDLE))
		goto out;

	event_hdr = skb_pull_data(clone, sizeof(*event_hdr));
	if (!event_hdr || (event_hdr->evt != HCI_VENDOR_PKT))
		goto out;

	dump_hdr = skb_pull_data(clone, sizeof(*dump_hdr));
	if (!dump_hdr || (dump_hdr->vse_class != QCA_MEMDUMP_VSE_CLASS) ||
	   (dump_hdr->msg_type != QCA_MEMDUMP_MSG_TYPE))
		goto out;

	is_dump = true;
out:
	consume_skb(clone);
	return is_dump;
}

/* Return: true if the event packet is a dump packet, false otherwise. */
static bool evt_pkt_is_dump_qca(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct hci_event_hdr *event_hdr;
	struct qca_dump_hdr *dump_hdr;
	struct sk_buff *clone = skb_clone(skb, GFP_ATOMIC);
	bool is_dump = false;

	if (!clone)
		return false;

	event_hdr = skb_pull_data(clone, sizeof(*event_hdr));
	if (!event_hdr || (event_hdr->evt != HCI_VENDOR_PKT))
		goto out;

	dump_hdr = skb_pull_data(clone, sizeof(*dump_hdr));
	if (!dump_hdr || (dump_hdr->vse_class != QCA_MEMDUMP_VSE_CLASS) ||
	   (dump_hdr->msg_type != QCA_MEMDUMP_MSG_TYPE))
		goto out;

	is_dump = true;
out:
	consume_skb(clone);
	return is_dump;
}

static int btusb_recv_acl_qca(struct hci_dev *hdev, struct sk_buff *skb)
{
	if (acl_pkt_is_dump_qca(hdev, skb))
		return handle_dump_pkt_qca(hdev, skb);
	return hci_recv_frame(hdev, skb);
}

static int btusb_recv_evt_qca(struct hci_dev *hdev, struct sk_buff *skb)
{
	if (evt_pkt_is_dump_qca(hdev, skb))
		return handle_dump_pkt_qca(hdev, skb);
	return hci_recv_frame(hdev, skb);
}


#define QCA_DFU_PACKET_LEN	4096

#define QCA_GET_TARGET_VERSION	0x09
#define QCA_CHECK_STATUS	0x05
#define QCA_DFU_DOWNLOAD	0x01

#define QCA_SYSCFG_UPDATED	0x40
#define QCA_PATCH_UPDATED	0x80
#define QCA_DFU_TIMEOUT		3000
#define QCA_FLAG_MULTI_NVM      0x80
#define QCA_BT_RESET_WAIT_MS    100

#define WCN6855_2_0_RAM_VERSION_GF 0x400c1200
#define WCN6855_2_1_RAM_VERSION_GF 0x400c1211

struct qca_version {
	__le32	rom_version;
	__le32	patch_version;
	__le32	ram_version;
	__u8	chip_id;
	__u8	platform_id;
	__le16	flag;
	__u8	reserved[4];
} __packed;

struct qca_rampatch_version {
	__le16	rom_version_high;
	__le16  rom_version_low;
	__le16	patch_version;
} __packed;

struct qca_device_info {
	u32	rom_version;
	u8	rampatch_hdr;	/* length of header in rampatch */
	u8	nvm_hdr;	/* length of header in NVM */
	u8	ver_offset;	/* offset of version structure in rampatch */
};

struct qca_custom_firmware {
	u32 rom_version;
	u16 board_id;
	const char *subdirectory;
};

static const struct qca_device_info qca_devices_table[] = {
	{ 0x00000100, 20, 4,  8 }, /* Rome 1.0 */
	{ 0x00000101, 20, 4,  8 }, /* Rome 1.1 */
	{ 0x00000200, 28, 4, 16 }, /* Rome 2.0 */
	{ 0x00000201, 28, 4, 16 }, /* Rome 2.1 */
	{ 0x00000300, 28, 4, 16 }, /* Rome 3.0 */
	{ 0x00000302, 28, 4, 16 }, /* Rome 3.2 */
	{ 0x00130100, 40, 4, 16 }, /* WCN6855 1.0 */
	{ 0x00130200, 40, 4, 16 }, /* WCN6855 2.0 */
	{ 0x00130201, 40, 4, 16 }, /* WCN6855 2.1 */
	{ 0x00190200, 40, 4, 16 }, /* WCN785x 2.0 */
};

static const struct qca_custom_firmware qca_custom_btfws[] = {
	{ 0x00130201, 0x030A, "QCA2066" },
	{ 0x00130201, 0x030B, "QCA2066" },
	{ },
};

static u16 qca_extract_board_id(const struct qca_version *ver)
{
	u16 flag = le16_to_cpu(ver->flag);
	u16 board_id = 0;

	if (((flag >> 8) & 0xff) == QCA_FLAG_MULTI_NVM) {
		/* The board_id should be split into two bytes
		 * The 1st byte is chip ID, and the 2nd byte is platform ID
		 * For example, board ID 0x010A, 0x01 is platform ID. 0x0A is chip ID
		 * we have several platforms, and platform IDs are continuously added
		 * Platform ID:
		 * 0x00 is for Mobile
		 * 0x01 is for X86
		 * 0x02 is for Automotive
		 * 0x03 is for Consumer electronic
		 */
		board_id = (ver->chip_id << 8) + ver->platform_id;
	}

	/* Take 0xffff as invalid board ID */
	if (board_id == 0xffff)
		board_id = 0;

	return board_id;
}

static const char *qca_get_fw_subdirectory(const struct qca_version *ver)
{
	const struct qca_custom_firmware *ptr;
	u32 rom_ver;
	u16 board_id;

	rom_ver = le32_to_cpu(ver->rom_version);
	board_id = qca_extract_board_id(ver);
	if (!board_id)
		return NULL;

	for (ptr = qca_custom_btfws; ptr->rom_version; ptr++) {
		if (ptr->rom_version == rom_ver &&
		    ptr->board_id == board_id)
			return ptr->subdirectory;
	}

	return NULL;
}

static int btusb_qca_send_vendor_req(struct usb_device *udev, u8 request,
				     void *data, u16 size)
{
	int pipe, err;
	u8 *buf;

	buf = kmalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Found some of USB hosts have IOT issues with ours so that we should
	 * not wait until HCI layer is ready.
	 */
	pipe = usb_rcvctrlpipe(udev, 0);
	err = usb_control_msg(udev, pipe, request, USB_TYPE_VENDOR | USB_DIR_IN,
			      0, 0, buf, size, USB_CTRL_GET_TIMEOUT);
	if (err < 0) {
		dev_err(&udev->dev, "Failed to access otp area (%d)", err);
		goto done;
	}

	memcpy(data, buf, size);

done:
	kfree(buf);

	return err;
}

static int btusb_setup_qca_download_fw(struct hci_dev *hdev,
				       const struct firmware *firmware,
				       size_t hdr_size)
{
	struct btusb_data *btdata = hci_get_drvdata(hdev);
	struct usb_device *udev = btdata->udev;
	size_t count, size, sent = 0;
	int pipe, len, err;
	u8 *buf;

	buf = kmalloc(QCA_DFU_PACKET_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	count = firmware->size;

	size = min_t(size_t, count, hdr_size);
	memcpy(buf, firmware->data, size);

	/* USB patches should go down to controller through USB path
	 * because binary format fits to go down through USB channel.
	 * USB control path is for patching headers and USB bulk is for
	 * patch body.
	 */
	pipe = usb_sndctrlpipe(udev, 0);
	err = usb_control_msg(udev, pipe, QCA_DFU_DOWNLOAD, USB_TYPE_VENDOR,
			      0, 0, buf, size, USB_CTRL_SET_TIMEOUT);
	if (err < 0) {
		bt_dev_err(hdev, "Failed to send headers (%d)", err);
		goto done;
	}

	sent += size;
	count -= size;

	/* ep2 need time to switch from function acl to function dfu,
	 * so we add 20ms delay here.
	 */
	msleep(20);

	while (count) {
		size = min_t(size_t, count, QCA_DFU_PACKET_LEN);

		memcpy(buf, firmware->data + sent, size);

		pipe = usb_sndbulkpipe(udev, 0x02);
		err = usb_bulk_msg(udev, pipe, buf, size, &len,
				   QCA_DFU_TIMEOUT);
		if (err < 0) {
			bt_dev_err(hdev, "Failed to send body at %zd of %zd (%d)",
				   sent, firmware->size, err);
			break;
		}

		if (size != len) {
			bt_dev_err(hdev, "Failed to get bulk buffer");
			err = -EILSEQ;
			break;
		}

		sent  += size;
		count -= size;
	}

done:
	kfree(buf);
	return err;
}

static int btusb_setup_qca_load_rampatch(struct hci_dev *hdev,
					 struct qca_version *ver,
					 const struct qca_device_info *info)
{
	struct qca_rampatch_version *rver;
	const struct firmware *fw;
	const char *fw_subdir;
	u32 ver_rom, ver_patch, rver_rom;
	u16 rver_rom_low, rver_rom_high, rver_patch;
	char fwname[80];
	int err;

	ver_rom = le32_to_cpu(ver->rom_version);
	ver_patch = le32_to_cpu(ver->patch_version);

	fw_subdir = qca_get_fw_subdirectory(ver);
	if (fw_subdir)
		snprintf(fwname, sizeof(fwname), "qca/%s/rampatch_usb_%08x.bin",
			 fw_subdir, ver_rom);
	else
		snprintf(fwname, sizeof(fwname), "qca/rampatch_usb_%08x.bin",
			 ver_rom);

	err = request_firmware(&fw, fwname, &hdev->dev);
	if (err) {
		bt_dev_err(hdev, "failed to request rampatch file: %s (%d)",
			   fwname, err);
		return err;
	}

	bt_dev_info(hdev, "using rampatch file: %s", fwname);

	rver = (struct qca_rampatch_version *)(fw->data + info->ver_offset);
	rver_rom_low = le16_to_cpu(rver->rom_version_low);
	rver_patch = le16_to_cpu(rver->patch_version);

	if (ver_rom & ~0xffffU) {
		rver_rom_high = le16_to_cpu(rver->rom_version_high);
		rver_rom = rver_rom_high << 16 | rver_rom_low;
	} else {
		rver_rom = rver_rom_low;
	}

	bt_dev_info(hdev, "QCA: patch rome 0x%x build 0x%x, "
		    "firmware rome 0x%x build 0x%x",
		    rver_rom, rver_patch, ver_rom, ver_patch);

	/* Allow rampatch when the patch version equals the firmware version.
	 * A firmware download may be aborted by a transient USB error (e.g.
	 * disconnect) after the controller updates version info but before
	 * completion.
	 * Allowing equal versions enables re-flashing during recovery.
	 */
	if (rver_rom != ver_rom || rver_patch < ver_patch) {
		bt_dev_err(hdev, "rampatch file version did not match with firmware");
		err = -EINVAL;
		goto done;
	}

	err = btusb_setup_qca_download_fw(hdev, fw, info->rampatch_hdr);

done:
	release_firmware(fw);

	return err;
}

static void btusb_generate_qca_nvm_name(char *fwname, size_t max_size,
					const struct qca_version *ver)
{
	u32 rom_version = le32_to_cpu(ver->rom_version);
	const char *variant, *fw_subdir;
	int len;
	u16 board_id;

	fw_subdir = qca_get_fw_subdirectory(ver);
	board_id = qca_extract_board_id(ver);

	switch (le32_to_cpu(ver->ram_version)) {
	case WCN6855_2_0_RAM_VERSION_GF:
	case WCN6855_2_1_RAM_VERSION_GF:
		variant = "_gf";
		break;
	default:
		variant = NULL;
		break;
	}

	if (fw_subdir)
		len = snprintf(fwname, max_size, "qca/%s/nvm_usb_%08x",
			       fw_subdir, rom_version);
	else
		len = snprintf(fwname, max_size, "qca/nvm_usb_%08x",
			       rom_version);
	if (variant)
		len += snprintf(fwname + len, max_size - len, "%s", variant);
	if (board_id)
		len += snprintf(fwname + len, max_size - len, "_%04x", board_id);
	len += snprintf(fwname + len, max_size - len, ".bin");
}

static int btusb_setup_qca_load_nvm(struct hci_dev *hdev,
				    struct qca_version *ver,
				    const struct qca_device_info *info)
{
	const struct firmware *fw;
	char fwname[80];
	int err;

	btusb_generate_qca_nvm_name(fwname, sizeof(fwname), ver);

	err = request_firmware(&fw, fwname, &hdev->dev);
	if (err) {
		bt_dev_err(hdev, "failed to request NVM file: %s (%d)",
			   fwname, err);
		return err;
	}

	bt_dev_info(hdev, "using NVM file: %s", fwname);

	err = btusb_setup_qca_download_fw(hdev, fw, info->nvm_hdr);

	release_firmware(fw);

	return err;
}

/* identify the ROM version and check whether patches are needed */
static bool btusb_qca_need_patch(struct usb_device *udev)
{
	struct qca_version ver;

	if (btusb_qca_send_vendor_req(udev, QCA_GET_TARGET_VERSION, &ver,
				      sizeof(ver)) < 0)
		return false;
	/* only low ROM versions need patches */
	return !(le32_to_cpu(ver.rom_version) & ~0xffffU);
}

static int btusb_setup_qca(struct hci_dev *hdev)
{
	struct btusb_data *btdata = hci_get_drvdata(hdev);
	struct usb_device *udev = btdata->udev;
	const struct qca_device_info *info = NULL;
	struct qca_version ver;
	u32 ver_rom;
	u8 status;
	int i, err;

	err = btusb_qca_send_vendor_req(udev, QCA_GET_TARGET_VERSION, &ver,
					sizeof(ver));
	if (err < 0)
		return err;

	ver_rom = le32_to_cpu(ver.rom_version);

	for (i = 0; i < ARRAY_SIZE(qca_devices_table); i++) {
		if (ver_rom == qca_devices_table[i].rom_version)
			info = &qca_devices_table[i];
	}
	if (!info) {
		/* If the rom_version is not matched in the qca_devices_table
		 * and the high ROM version is not zero, we assume this chip no
		 * need to load the rampatch and nvm.
		 */
		if (ver_rom & ~0xffffU)
			return 0;

		bt_dev_err(hdev, "don't support firmware rome 0x%x", ver_rom);
		return -ENODEV;
	}

	err = btusb_qca_send_vendor_req(udev, QCA_CHECK_STATUS, &status,
					sizeof(status));
	if (err < 0)
		return err;

	if (!(status & QCA_PATCH_UPDATED)) {
		err = btusb_setup_qca_load_rampatch(hdev, &ver, info);
		if (err < 0)
			return err;
	}

	err = btusb_qca_send_vendor_req(udev, QCA_GET_TARGET_VERSION, &ver,
					sizeof(ver));
	if (err < 0)
		return err;

	btdata->qca_dump.fw_version = le32_to_cpu(ver.patch_version);
	btdata->qca_dump.controller_id = le32_to_cpu(ver.rom_version);

	if (!(status & QCA_SYSCFG_UPDATED)) {
		err = btusb_setup_qca_load_nvm(hdev, &ver, info);
		if (err < 0)
			return err;

		/* WCN6855 2.1 and later will reset to apply firmware downloaded here, so
		 * wait ~100ms for reset Done then go ahead, otherwise, it maybe
		 * cause potential enable failure.
		 */
		if (info->rom_version >= 0x00130201)
			msleep(QCA_BT_RESET_WAIT_MS);
	}

	/* Mark HCI_OP_ENHANCED_SETUP_SYNC_CONN as broken as it doesn't seem to
	 * work with the likes of HSP/HFP mSBC.
	 */
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN);

	return 0;
}

static inline int __set_diag_interface(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct usb_interface *intf = data->diag;
	int i;

	if (!data->diag)
		return -ENODEV;

	data->diag_tx_ep = NULL;
	data->diag_rx_ep = NULL;

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep_desc;

		ep_desc = &intf->cur_altsetting->endpoint[i].desc;

		if (!data->diag_tx_ep && usb_endpoint_is_bulk_out(ep_desc)) {
			data->diag_tx_ep = ep_desc;
			continue;
		}

		if (!data->diag_rx_ep && usb_endpoint_is_bulk_in(ep_desc)) {
			data->diag_rx_ep = ep_desc;
			continue;
		}
	}

	if (!data->diag_tx_ep || !data->diag_rx_ep) {
		bt_dev_err(hdev, "invalid diagnostic descriptors");
		return -ENODEV;
	}

	return 0;
}

static struct urb *alloc_diag_urb(struct hci_dev *hdev, bool enable)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct sk_buff *skb;
	struct urb *urb;
	unsigned int pipe;

	if (!data->diag_tx_ep)
		return ERR_PTR(-ENODEV);

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return ERR_PTR(-ENOMEM);

	skb = bt_skb_alloc(2, GFP_KERNEL);
	if (!skb) {
		usb_free_urb(urb);
		return ERR_PTR(-ENOMEM);
	}

	skb_put_u8(skb, 0xf0);
	skb_put_u8(skb, enable);

	pipe = usb_sndbulkpipe(data->udev, data->diag_tx_ep->bEndpointAddress);

	usb_fill_bulk_urb(urb, data->udev, pipe,
			  skb->data, skb->len, btusb_tx_complete, skb);

	skb->dev = (void *)hdev;

	return urb;
}

static int btusb_bcm_set_diag(struct hci_dev *hdev, bool enable)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct urb *urb;

	if (!data->diag)
		return -ENODEV;

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		return -ENETDOWN;

	urb = alloc_diag_urb(hdev, enable);
	if (IS_ERR(urb))
		return PTR_ERR(urb);

	return submit_or_queue_tx_urb(hdev, urb);
}

#ifdef CONFIG_PM
static irqreturn_t btusb_oob_wake_handler(int irq, void *priv)
{
	struct btusb_data *data = priv;

	pm_wakeup_event(&data->udev->dev, 0);
	pm_system_wakeup();

	/* Disable only if not already disabled (keep it balanced) */
	if (test_and_clear_bit(BTUSB_OOB_WAKE_ENABLED, &data->flags)) {
		disable_irq_nosync(irq);
		disable_irq_wake(irq);
	}
	return IRQ_HANDLED;
}

static const struct of_device_id btusb_match_table[] = {
	{ .compatible = "usb1286,204e" },
	{ .compatible = "usbcf3,e300" }, /* QCA6174A */
	{ .compatible = "usb4ca,301a" }, /* QCA6174A (Lite-On) */
	{ }
};
MODULE_DEVICE_TABLE(of, btusb_match_table);

/* Use an oob wakeup pin? */
static int btusb_config_oob_wake(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);
	struct device *dev = &data->udev->dev;
	int irq, ret;

	clear_bit(BTUSB_OOB_WAKE_ENABLED, &data->flags);

	if (!of_match_device(btusb_match_table, dev))
		return 0;

	/* Move on if no IRQ specified */
	irq = of_irq_get_byname(dev->of_node, "wakeup");
	if (irq <= 0) {
		bt_dev_dbg(hdev, "%s: no OOB Wakeup IRQ in DT", __func__);
		return 0;
	}

	irq_set_status_flags(irq, IRQ_NOAUTOEN);
	ret = devm_request_irq(&hdev->dev, irq, btusb_oob_wake_handler,
			       0, "OOB Wake-on-BT", data);
	if (ret) {
		bt_dev_err(hdev, "%s: IRQ request failed", __func__);
		return ret;
	}

	ret = device_init_wakeup(dev, true);
	if (ret) {
		bt_dev_err(hdev, "%s: failed to init_wakeup", __func__);
		return ret;
	}

	data->oob_wake_irq = irq;
	bt_dev_info(hdev, "OOB Wake-on-BT configured at IRQ %u", irq);
	return 0;
}
#endif

static void btusb_check_needs_reset_resume(struct usb_interface *intf)
{
	if (dmi_check_system(btusb_needs_reset_resume_table))
		interface_to_usbdev(intf)->quirks |= USB_QUIRK_RESET_RESUME;
}

static bool btusb_wakeup(struct hci_dev *hdev)
{
	struct btusb_data *data = hci_get_drvdata(hdev);

	return device_may_wakeup(&data->udev->dev);
}

static int btusb_shutdown_qca(struct hci_dev *hdev)
{
	struct sk_buff *skb;

	skb = __hci_cmd_sync(hdev, HCI_OP_RESET, 0, NULL, HCI_INIT_TIMEOUT);
	if (IS_ERR(skb)) {
		bt_dev_err(hdev, "HCI reset during shutdown failed");
		return PTR_ERR(skb);
	}
	kfree_skb(skb);

	return 0;
}

static ssize_t force_poll_sync_read(struct file *file, char __user *user_buf,
				    size_t count, loff_t *ppos)
{
	struct btusb_data *data = file->private_data;
	char buf[3];

	buf[0] = data->poll_sync ? 'Y' : 'N';
	buf[1] = '\n';
	buf[2] = '\0';
	return simple_read_from_buffer(user_buf, count, ppos, buf, 2);
}

static ssize_t force_poll_sync_write(struct file *file,
				     const char __user *user_buf,
				     size_t count, loff_t *ppos)
{
	struct btusb_data *data = file->private_data;
	bool enable;
	int err;

	err = kstrtobool_from_user(user_buf, count, &enable);
	if (err)
		return err;

	/* Only allow changes while the adapter is down */
	if (test_bit(HCI_UP, &data->hdev->flags))
		return -EPERM;

	if (data->poll_sync == enable)
		return -EALREADY;

	data->poll_sync = enable;

	return count;
}

static const struct file_operations force_poll_sync_fops = {
	.owner		= THIS_MODULE,
	.open		= simple_open,
	.read		= force_poll_sync_read,
	.write		= force_poll_sync_write,
	.llseek		= default_llseek,
};

#define BTUSB_HCI_DRV_OP_SUPPORTED_ALTSETTINGS \
		hci_opcode_pack(HCI_DRV_OGF_DRIVER_SPECIFIC, 0x0000)
#define BTUSB_HCI_DRV_SUPPORTED_ALTSETTINGS_SIZE	0
struct btusb_hci_drv_rp_supported_altsettings {
	__u8	num;
	__u8	altsettings[];
} __packed;

#define BTUSB_HCI_DRV_OP_SWITCH_ALTSETTING \
		hci_opcode_pack(HCI_DRV_OGF_DRIVER_SPECIFIC, 0x0001)
#define BTUSB_HCI_DRV_SWITCH_ALTSETTING_SIZE		1
struct btusb_hci_drv_cmd_switch_altsetting {
	__u8	altsetting;
} __packed;

static const struct {
	u16 opcode;
	const char *desc;
} btusb_hci_drv_supported_commands[] = {
	/* Common commands */
	{ HCI_DRV_OP_READ_INFO, "Read Info" },

	/* Driver specific commands */
	{ BTUSB_HCI_DRV_OP_SUPPORTED_ALTSETTINGS, "Supported Altsettings" },
	{ BTUSB_HCI_DRV_OP_SWITCH_ALTSETTING,     "Switch Altsetting" },
};
static int btusb_hci_drv_read_info(struct hci_dev *hdev, void *data,
				   u16 data_len)
{
	struct hci_drv_rp_read_info *rp;
	size_t rp_size;
	int err, i;
	u16 opcode, num_supported_commands =
		ARRAY_SIZE(btusb_hci_drv_supported_commands);

	rp_size = sizeof(*rp) + num_supported_commands * 2;

	rp = kmalloc(rp_size, GFP_KERNEL);
	if (!rp)
		return -ENOMEM;

	strscpy_pad(rp->driver_name, btusb_driver.name);

	rp->num_supported_commands = cpu_to_le16(num_supported_commands);
	for (i = 0; i < num_supported_commands; i++) {
		opcode = btusb_hci_drv_supported_commands[i].opcode;
		bt_dev_info(hdev,
			    "Supported HCI Drv command (0x%02x|0x%04x): %s",
			    hci_opcode_ogf(opcode),
			    hci_opcode_ocf(opcode),
			    btusb_hci_drv_supported_commands[i].desc);
		rp->supported_commands[i] = cpu_to_le16(opcode);
	}

	err = hci_drv_cmd_complete(hdev, HCI_DRV_OP_READ_INFO,
				   HCI_DRV_STATUS_SUCCESS, rp, rp_size);

	kfree(rp);
	return err;
}

static int btusb_hci_drv_supported_altsettings(struct hci_dev *hdev, void *data,
					       u16 data_len)
{
	struct btusb_data *drvdata = hci_get_drvdata(hdev);
	struct btusb_hci_drv_rp_supported_altsettings *rp;
	size_t rp_size;
	int err;
	u8 i;

	/* There are at most 7 alt (0 - 6) */
	rp = kmalloc(sizeof(*rp) + 7, GFP_KERNEL);
	if (!rp)
		return -ENOMEM;

	rp->num = 0;
	if (!drvdata->isoc)
		goto done;

	for (i = 0; i <= 6; i++) {
		if (btusb_find_altsetting(drvdata, i))
			rp->altsettings[rp->num++] = i;
	}

done:
	rp_size = sizeof(*rp) + rp->num;

	err = hci_drv_cmd_complete(hdev, BTUSB_HCI_DRV_OP_SUPPORTED_ALTSETTINGS,
				   HCI_DRV_STATUS_SUCCESS, rp, rp_size);
	kfree(rp);
	return err;
}

static int btusb_hci_drv_switch_altsetting(struct hci_dev *hdev, void *data,
					   u16 data_len)
{
	struct btusb_hci_drv_cmd_switch_altsetting *cmd = data;
	u8 status;

	if (cmd->altsetting > 6) {
		status = HCI_DRV_STATUS_INVALID_PARAMETERS;
	} else {
		if (btusb_switch_alt_setting(hdev, cmd->altsetting))
			status = HCI_DRV_STATUS_UNSPECIFIED_ERROR;
		else
			status = HCI_DRV_STATUS_SUCCESS;
	}

	return hci_drv_cmd_status(hdev, BTUSB_HCI_DRV_OP_SWITCH_ALTSETTING,
				  status);
}

static const struct hci_drv_handler btusb_hci_drv_common_handlers[] = {
	{ btusb_hci_drv_read_info,	HCI_DRV_READ_INFO_SIZE },
};

static const struct hci_drv_handler btusb_hci_drv_specific_handlers[] = {
	{ btusb_hci_drv_supported_altsettings,
				BTUSB_HCI_DRV_SUPPORTED_ALTSETTINGS_SIZE },
	{ btusb_hci_drv_switch_altsetting,
				BTUSB_HCI_DRV_SWITCH_ALTSETTING_SIZE },
};

static struct hci_drv btusb_hci_drv = {
	.common_handler_count	= ARRAY_SIZE(btusb_hci_drv_common_handlers),
	.common_handlers	= btusb_hci_drv_common_handlers,
	.specific_handler_count	= ARRAY_SIZE(btusb_hci_drv_specific_handlers),
	.specific_handlers	= btusb_hci_drv_specific_handlers,
};

static int btusb_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct usb_endpoint_descriptor *ep_desc;
	struct gpio_desc *reset_gpio;
	struct btusb_data *data;
	struct hci_dev *hdev;
	unsigned ifnum_base;
	int i, err, priv_size;

	BT_DBG("intf %p id %p", intf, id);

	if ((id->driver_info & BTUSB_IFNUM_2) &&
	    (intf->cur_altsetting->desc.bInterfaceNumber != 0) &&
	    (intf->cur_altsetting->desc.bInterfaceNumber != 2))
		return -ENODEV;

	ifnum_base = intf->cur_altsetting->desc.bInterfaceNumber;

	if (!id->driver_info) {
		const struct usb_device_id *match;

		match = usb_match_id(intf, quirks_table);
		if (match)
			id = match;
	}

	if (id->driver_info == BTUSB_IGNORE)
		return -ENODEV;

	if (id->driver_info & BTUSB_ATH3012) {
		struct usb_device *udev = interface_to_usbdev(intf);

		/* Old firmware would otherwise let ath3k driver load
		 * patch and sysconfig files
		 */
		if (le16_to_cpu(udev->descriptor.bcdDevice) <= 0x0001 &&
		    !btusb_qca_need_patch(udev))
			return -ENODEV;
	}

	data = kzalloc_obj(*data);
	if (!data)
		return -ENOMEM;

	/* LOCAL PATCH: the known fake bcdDevices listed in btusb_setup_csr()
	 * (0x0100, 0x0134, 0x1915, 0x2520, 0x7558, 0x8891 - this dongle is 0x8891) are
	 * readable here, before any HCI request has been sent. Setting this only
	 * enables the stray-event-code filter in btusb_recv_intr() for the first event
	 * of the open; the clone is still confirmed by its reply to
	 * HCI Read Local Version, which is what turns the fake responses on.
	 */
	switch (le16_to_cpu(interface_to_usbdev(intf)->descriptor.bcdDevice)) {
	case 0x0100:
	case 0x0134:
	case 0x1915:
	case 0x2520:
	case 0x7558:
	case 0x8891:
		data->csr_suspect = true;
		break;
	}

	for (i = 0; i < intf->cur_altsetting->desc.bNumEndpoints; i++) {
		ep_desc = &intf->cur_altsetting->endpoint[i].desc;

		if (!data->intr_ep && usb_endpoint_is_int_in(ep_desc)) {
			data->intr_ep = ep_desc;
			continue;
		}

		if (!data->bulk_tx_ep && usb_endpoint_is_bulk_out(ep_desc)) {
			data->bulk_tx_ep = ep_desc;
			continue;
		}

		if (!data->bulk_rx_ep && usb_endpoint_is_bulk_in(ep_desc)) {
			data->bulk_rx_ep = ep_desc;
			continue;
		}
	}

	if (!data->intr_ep || !data->bulk_tx_ep || !data->bulk_rx_ep) {
		kfree(data);
		return -ENODEV;
	}

	if (id->driver_info & BTUSB_AMP) {
		data->cmdreq_type = USB_TYPE_CLASS | 0x01;
		data->cmdreq = 0x2b;
	} else {
		data->cmdreq_type = USB_TYPE_CLASS;
		data->cmdreq = 0x00;
	}

	data->udev = interface_to_usbdev(intf);
	data->intf = intf;

	INIT_WORK(&data->work, btusb_work);
	INIT_WORK(&data->waker, btusb_waker);
	INIT_DELAYED_WORK(&data->rx_work, btusb_rx_work);

	/* LOCAL PATCH: the diagnostic probe, queued by btusb_setup_csr() */
	INIT_DELAYED_WORK(&data->sys_probe, csr_sys_probe);
	INIT_DELAYED_WORK(&data->core_probe, csr_core_probe);

	skb_queue_head_init(&data->acl_q);

	init_completion(&data->csr_done);

	init_usb_anchor(&data->deferred);
	init_usb_anchor(&data->tx_anchor);
	spin_lock_init(&data->txlock);

	init_usb_anchor(&data->intr_anchor);
	init_usb_anchor(&data->bulk_anchor);
	init_usb_anchor(&data->isoc_anchor);
	init_usb_anchor(&data->diag_anchor);
	init_usb_anchor(&data->ctrl_anchor);
	spin_lock_init(&data->rxlock);

	priv_size = 0;

	data->recv_event = hci_recv_frame;
	data->recv_bulk = btusb_recv_bulk;

	if (id->driver_info & BTUSB_INTEL_COMBINED) {
		/* Allocate extra space for Intel device */
		priv_size += sizeof(struct btintel_data);

		/* Override the rx handlers */
		data->recv_event = btintel_recv_event;
		data->recv_bulk = btusb_recv_bulk_intel;
	} else if (id->driver_info & BTUSB_REALTEK) {
		/* Allocate extra space for Realtek device */
		priv_size += sizeof(struct btrealtek_data);

		data->recv_event = btusb_recv_event_realtek;
	} else if (id->driver_info & BTUSB_MEDIATEK) {
		/* Allocate extra space for Mediatek device */
		priv_size += sizeof(struct btmtk_data);
	}

	data->recv_acl = hci_recv_frame;

	hdev = hci_alloc_dev_priv(priv_size);
	if (!hdev) {
		kfree(data);
		return -ENOMEM;
	}

	hdev->bus = HCI_USB;
	hci_set_drvdata(hdev, data);

	data->hdev = hdev;

	SET_HCIDEV_DEV(hdev, &intf->dev);

	reset_gpio = gpiod_get_optional(&data->udev->dev, "reset",
					GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio)) {
		err = PTR_ERR(reset_gpio);
		goto out_free_dev;
	} else if (reset_gpio) {
		data->reset_gpio = reset_gpio;
	}

	hdev->open    = btusb_open;
	hdev->close   = btusb_close;
	hdev->flush   = btusb_flush;
	hdev->send    = btusb_send_frame;
	hdev->notify  = btusb_notify;
	hdev->wakeup  = btusb_wakeup;
	hdev->hci_drv = &btusb_hci_drv;

#ifdef CONFIG_PM
	err = btusb_config_oob_wake(hdev);
	if (err)
		goto out_free_dev;

	/* Marvell devices may need a specific chip configuration */
	if (id->driver_info & BTUSB_MARVELL && data->oob_wake_irq) {
		err = marvell_config_oob_wake(hdev);
		if (err)
			goto out_free_dev;
	}
#endif
	if (id->driver_info & BTUSB_CW6622)
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_STORED_LINK_KEY);

	if (id->driver_info & BTUSB_BCM2045)
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_STORED_LINK_KEY);

	if (id->driver_info & BTUSB_BCM92035)
		hdev->setup = btusb_setup_bcm92035;

	if (IS_ENABLED(CONFIG_BT_HCIBTUSB_BCM) &&
	    (id->driver_info & BTUSB_BCM_PATCHRAM)) {
		hdev->manufacturer = 15;
		hdev->setup = btbcm_setup_patchram;
		hdev->set_diag = btusb_bcm_set_diag;
		hdev->set_bdaddr = btbcm_set_bdaddr;

		/* Broadcom LM_DIAG Interface numbers are hardcoded */
		data->diag = usb_ifnum_to_if(data->udev, ifnum_base + 2);
	}

	if (IS_ENABLED(CONFIG_BT_HCIBTUSB_BCM) &&
	    (id->driver_info & BTUSB_BCM_APPLE)) {
		hdev->manufacturer = 15;
		hdev->setup = btbcm_setup_apple;
		hdev->set_diag = btusb_bcm_set_diag;

		/* Broadcom LM_DIAG Interface numbers are hardcoded */
		data->diag = usb_ifnum_to_if(data->udev, ifnum_base + 2);
	}

	/* Combined Intel Device setup to support multiple setup routine */
	if (id->driver_info & BTUSB_INTEL_COMBINED) {
		err = btintel_configure_setup(hdev, btusb_driver.name);
		if (err)
			goto out_free_dev;

		/* Transport specific configuration */
		hdev->send = btusb_send_frame_intel;
		hdev->reset = btusb_intel_reset;

		if (id->driver_info & BTUSB_INTEL_NO_WBS_SUPPORT)
			btintel_set_flag(hdev, INTEL_ROM_LEGACY_NO_WBS_SUPPORT);

		if (id->driver_info & BTUSB_INTEL_BROKEN_INITIAL_NCMD)
			btintel_set_flag(hdev, INTEL_BROKEN_INITIAL_NCMD);

		if (id->driver_info & BTUSB_INTEL_BROKEN_SHUTDOWN_LED)
			btintel_set_flag(hdev, INTEL_BROKEN_SHUTDOWN_LED);
	}

	if (id->driver_info & BTUSB_MARVELL)
		hdev->set_bdaddr = btusb_set_bdaddr_marvell;

	if (IS_ENABLED(CONFIG_BT_HCIBTUSB_MTK) &&
	    (id->driver_info & BTUSB_MEDIATEK)) {
		hdev->setup = btusb_mtk_setup;
		hdev->shutdown = btusb_mtk_shutdown;
		hdev->manufacturer = 70;
		hdev->reset = btmtk_reset_sync;
		hdev->set_bdaddr = btmtk_set_bdaddr;
		hdev->send = btusb_send_frame_mtk;
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_ENHANCED_SETUP_SYNC_CONN);
		hci_set_quirk(hdev, HCI_QUIRK_NON_PERSISTENT_SETUP);
		data->recv_acl = btmtk_usb_recv_acl;
		data->suspend = btmtk_usb_suspend;
		data->resume = btmtk_usb_resume;
		data->disconnect = btusb_mtk_disconnect;
	}

	if (id->driver_info & BTUSB_SWAVE) {
		hci_set_quirk(hdev, HCI_QUIRK_FIXUP_INQUIRY_MODE);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_LOCAL_COMMANDS);
	}

	if (id->driver_info & BTUSB_INTEL_BOOT) {
		hdev->manufacturer = 2;
		hci_set_quirk(hdev, HCI_QUIRK_RAW_DEVICE);
	}

	if (id->driver_info & BTUSB_ATH3012) {
		data->setup_on_usb = btusb_setup_qca;
		hdev->set_bdaddr = btusb_set_bdaddr_ath3012;
		hci_set_quirk(hdev, HCI_QUIRK_SIMULTANEOUS_DISCOVERY);
		hci_set_quirk(hdev, HCI_QUIRK_STRICT_DUPLICATE_FILTER);
	}

	if (id->driver_info & BTUSB_QCA_ROME) {
		data->setup_on_usb = btusb_setup_qca;
		hdev->shutdown = btusb_shutdown_qca;
		hdev->set_bdaddr = btusb_set_bdaddr_ath3012;
		hdev->reset = btusb_qca_reset;
		hci_set_quirk(hdev, HCI_QUIRK_SIMULTANEOUS_DISCOVERY);
		btusb_check_needs_reset_resume(intf);
	}

	if (id->driver_info & BTUSB_QCA_WCN6855) {
		data->qca_dump.id_vendor = id->idVendor;
		data->qca_dump.id_product = id->idProduct;
		data->recv_event = btusb_recv_evt_qca;
		data->recv_acl = btusb_recv_acl_qca;
		hci_devcd_register(hdev, btusb_coredump_qca, btusb_dump_hdr_qca, NULL);
		data->setup_on_usb = btusb_setup_qca;
		hdev->classify_pkt_type = btusb_classify_qca_pkt_type;
		hdev->shutdown = btusb_shutdown_qca;
		hdev->set_bdaddr = btusb_set_bdaddr_wcn6855;
		hdev->reset = btusb_qca_reset;
		hci_set_quirk(hdev, HCI_QUIRK_SIMULTANEOUS_DISCOVERY);
		hci_set_msft_opcode(hdev, 0xFD70);
	}

	if (id->driver_info & BTUSB_AMP) {
		/* AMP controllers do not support SCO packets */
		data->isoc = NULL;
	} else {
		/* Interface orders are hardcoded in the specification */
		data->isoc = usb_ifnum_to_if(data->udev, ifnum_base + 1);
		data->isoc_ifnum = ifnum_base + 1;
	}

	if (IS_ENABLED(CONFIG_BT_HCIBTUSB_RTL) &&
	    (id->driver_info & BTUSB_REALTEK)) {
		btrtl_set_driver_name(hdev, btusb_driver.name);
		hdev->setup = btusb_setup_realtek;
		hdev->shutdown = btrtl_shutdown_realtek;
		hdev->reset = btusb_rtl_reset;
		hdev->hw_error = btusb_rtl_hw_error;

		/* Realtek devices need to set remote wakeup on auto-suspend */
		set_bit(BTUSB_WAKEUP_AUTOSUSPEND, &data->flags);
		set_bit(BTUSB_USE_ALT3_FOR_WBS, &data->flags);
	}

	if (id->driver_info & BTUSB_ACTIONS_SEMI) {
		/* Support is advertised, but not implemented */
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_ERR_DATA_REPORTING);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_READ_TRANSMIT_POWER);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_SET_RPA_TIMEOUT);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_EXT_SCAN);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_READ_ENC_KEY_SIZE);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_EXT_CREATE_CONN);
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_WRITE_AUTH_PAYLOAD_TIMEOUT);
	}

	if (!reset)
		hci_set_quirk(hdev, HCI_QUIRK_RESET_ON_CLOSE);

	if (force_scofix || id->driver_info & BTUSB_WRONG_SCO_MTU) {
		if (!disable_scofix)
			hci_set_quirk(hdev, HCI_QUIRK_FIXUP_BUFFER_SIZE);
	}

	if (id->driver_info & BTUSB_BROKEN_ISOC)
		data->isoc = NULL;

	if (id->driver_info & BTUSB_WIDEBAND_SPEECH)
		hci_set_quirk(hdev, HCI_QUIRK_WIDEBAND_SPEECH_SUPPORTED);

	if (id->driver_info & BTUSB_INVALID_LE_STATES)
		hci_set_quirk(hdev, HCI_QUIRK_BROKEN_LE_STATES);

	if (id->driver_info & BTUSB_DIGIANSWER) {
		data->cmdreq_type = USB_TYPE_VENDOR;
		hci_set_quirk(hdev, HCI_QUIRK_RESET_ON_CLOSE);
	}

	if (id->driver_info & BTUSB_CSR) {
		struct usb_device *udev = data->udev;
		u16 bcdDevice = le16_to_cpu(udev->descriptor.bcdDevice);

		/* Old firmware would otherwise execute USB reset */
		if (bcdDevice < 0x117)
			hci_set_quirk(hdev, HCI_QUIRK_RESET_ON_CLOSE);

		/* This must be set first in case we disable it for fakes */
		hci_set_quirk(hdev, HCI_QUIRK_SIMULTANEOUS_DISCOVERY);

		/* Fake CSR devices with broken commands */
		if (le16_to_cpu(udev->descriptor.idVendor)  == 0x0a12 &&
		    le16_to_cpu(udev->descriptor.idProduct) == 0x0001)
			hdev->setup = btusb_setup_csr;
	}

	if (id->driver_info & BTUSB_SNIFFER) {
		struct usb_device *udev = data->udev;

		/* New sniffer firmware has crippled HCI interface */
		if (le16_to_cpu(udev->descriptor.bcdDevice) > 0x997)
			hci_set_quirk(hdev, HCI_QUIRK_RAW_DEVICE);
	}

	if (id->driver_info & BTUSB_INTEL_BOOT) {
		/* A bug in the bootloader causes that interrupt interface is
		 * only enabled after receiving SetInterface(0, AltSetting=0).
		 */
		err = usb_set_interface(data->udev, 0, 0);
		if (err < 0) {
			BT_ERR("failed to set interface 0, alt 0 %d", err);
			goto out_free_dev;
		}
	}

	if (data->isoc) {
		err = usb_driver_claim_interface(&btusb_driver,
						 data->isoc, data);
		if (err < 0)
			goto out_free_dev;
	}

	if (IS_ENABLED(CONFIG_BT_HCIBTUSB_BCM) && data->diag) {
		if (!usb_driver_claim_interface(&btusb_driver,
						data->diag, data))
			__set_diag_interface(hdev);
		else
			data->diag = NULL;
	}

	if (enable_autosuspend)
		usb_enable_autosuspend(data->udev);

	data->poll_sync = enable_poll_sync;

	err = hci_register_dev(hdev);
	if (err < 0)
		goto out_free_dev;

	usb_set_intfdata(intf, data);

	debugfs_create_file("force_poll_sync", 0644, hdev->debugfs, data,
			    &force_poll_sync_fops);

	return 0;

out_free_dev:
	if (data->reset_gpio)
		gpiod_put(data->reset_gpio);
	hci_free_dev(hdev);
	kfree(data);
	return err;
}

static void btusb_disconnect(struct usb_interface *intf)
{
	struct btusb_data *data = usb_get_intfdata(intf);
	struct hci_dev *hdev;

	BT_DBG("intf %p", intf);

	if (!data)
		return;

	hdev = data->hdev;
	usb_set_intfdata(data->intf, NULL);

	if (data->isoc)
		usb_set_intfdata(data->isoc, NULL);

	if (data->diag)
		usb_set_intfdata(data->diag, NULL);

	if (data->disconnect)
		data->disconnect(hdev);

	/* LOCAL PATCH: stop the probe before hci_unregister_dev() can free hdev.
	 * The flag is checked by the handler too, so a round that is already running
	 * does not queue itself again; the window for a work item that passed that
	 * check and then found hdev gone is a few instructions wide, which is
	 * acceptable for a diagnostic. cancel_delayed_work_sync() would close it
	 * completely, but it has to wait for hdev->workqueue, and if that queue is
	 * the thing that is stuck the unload would hang here - which would itself be
	 * the answer, at the cost of not being able to unload.
	 */
	data->probe_stopped = true;
	cancel_delayed_work(&data->sys_probe);
	cancel_delayed_work(&data->core_probe);

	hci_unregister_dev(hdev);

	if (data->oob_wake_irq)
		device_init_wakeup(&data->udev->dev, false);
	if (data->reset_gpio)
		gpiod_put(data->reset_gpio);

	/* LOCAL PATCH: release the active reference held for the clone
	 * (see btusb_data::csr_pm_hold).
	 */
	if (data->csr_pm_hold)
		pm_runtime_put_noidle(&data->udev->dev);

	if (intf == data->intf) {
		if (data->isoc)
			usb_driver_release_interface(&btusb_driver, data->isoc);
		if (data->diag)
			usb_driver_release_interface(&btusb_driver, data->diag);
	} else if (intf == data->isoc) {
		if (data->diag)
			usb_driver_release_interface(&btusb_driver, data->diag);
		usb_driver_release_interface(&btusb_driver, data->intf);
	} else if (intf == data->diag) {
		if (data->isoc)
			usb_driver_release_interface(&btusb_driver, data->isoc);
		usb_driver_release_interface(&btusb_driver, data->intf);
	}

	hci_free_dev(hdev);
	kfree(data);
}

static int btusb_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct btusb_data *data = usb_get_intfdata(intf);

	BT_DBG("intf %p", intf);

	/* Don't auto-suspend if there are connections or discovery in
	 * progress; external suspend calls shall never fail.
	 */
	if (PMSG_IS_AUTO(message) &&
	    (hci_conn_count(data->hdev) || hci_discovery_active(data->hdev)))
		return -EBUSY;

	if (data->suspend_count++)
		return 0;

	spin_lock_irq(&data->txlock);
	if (!(PMSG_IS_AUTO(message) && data->tx_in_flight)) {
		set_bit(BTUSB_SUSPENDING, &data->flags);
		spin_unlock_irq(&data->txlock);
	} else {
		spin_unlock_irq(&data->txlock);
		data->suspend_count--;
		return -EBUSY;
	}

	cancel_work_sync(&data->work);

	if (data->suspend)
		data->suspend(data->hdev);

	btusb_stop_traffic(data);
	usb_kill_anchored_urbs(&data->tx_anchor);

	if (data->oob_wake_irq && device_may_wakeup(&data->udev->dev)) {
		set_bit(BTUSB_OOB_WAKE_ENABLED, &data->flags);
		enable_irq_wake(data->oob_wake_irq);
		enable_irq(data->oob_wake_irq);
	}

	/* For global suspend, Realtek devices lose the loaded fw
	 * in them. But for autosuspend, firmware should remain.
	 * Actually, it depends on whether the usb host sends
	 * set feature (enable wakeup) or not.
	 */
	if (test_bit(BTUSB_WAKEUP_AUTOSUSPEND, &data->flags)) {
		if (PMSG_IS_AUTO(message) &&
		    device_can_wakeup(&data->udev->dev))
			data->udev->do_remote_wakeup = 1;
		else if (!PMSG_IS_AUTO(message) &&
			 !device_may_wakeup(&data->udev->dev)) {
			data->udev->do_remote_wakeup = 0;
			data->udev->reset_resume = 1;
		}
	}

	return 0;
}

static void play_deferred(struct btusb_data *data)
{
	struct urb *urb;
	int err;

	while ((urb = usb_get_from_anchor(&data->deferred))) {
		usb_anchor_urb(urb, &data->tx_anchor);

		err = usb_submit_urb(urb, GFP_ATOMIC);
		if (err < 0) {
			if (err != -EPERM && err != -ENODEV)
				BT_ERR("%s urb %p submission failed (%d)",
				       data->hdev->name, urb, -err);
			kfree(urb->setup_packet);
			usb_unanchor_urb(urb);
			usb_free_urb(urb);
			break;
		}

		data->tx_in_flight++;
		usb_free_urb(urb);
	}

	/* Cleanup the rest deferred urbs. */
	while ((urb = usb_get_from_anchor(&data->deferred))) {
		kfree(urb->setup_packet);
		usb_free_urb(urb);
	}
}

static int btusb_resume(struct usb_interface *intf)
{
	struct btusb_data *data = usb_get_intfdata(intf);
	struct hci_dev *hdev = data->hdev;
	int err = 0;

	BT_DBG("intf %p", intf);

	if (--data->suspend_count)
		return 0;

	/* Disable only if not already disabled (keep it balanced) */
	if (test_and_clear_bit(BTUSB_OOB_WAKE_ENABLED, &data->flags)) {
		disable_irq(data->oob_wake_irq);
		disable_irq_wake(data->oob_wake_irq);
	}

	if (!test_bit(HCI_RUNNING, &hdev->flags))
		goto done;

	if (test_bit(BTUSB_INTR_RUNNING, &data->flags)) {
		err = btusb_submit_intr_urb(hdev, GFP_NOIO);
		if (err < 0) {
			clear_bit(BTUSB_INTR_RUNNING, &data->flags);
			goto failed;
		}
	}

	if (test_bit(BTUSB_BULK_RUNNING, &data->flags)) {
		err = btusb_submit_bulk_urb(hdev, GFP_NOIO);
		if (err < 0) {
			clear_bit(BTUSB_BULK_RUNNING, &data->flags);
			goto failed;
		}

		btusb_submit_bulk_urb(hdev, GFP_NOIO);
	}

	if (test_bit(BTUSB_ISOC_RUNNING, &data->flags)) {
		if (btusb_submit_isoc_urb(hdev, GFP_NOIO) < 0)
			clear_bit(BTUSB_ISOC_RUNNING, &data->flags);
		else
			btusb_submit_isoc_urb(hdev, GFP_NOIO);
	}

	if (data->resume)
		data->resume(hdev);

	spin_lock_irq(&data->txlock);
	play_deferred(data);
	clear_bit(BTUSB_SUSPENDING, &data->flags);
	spin_unlock_irq(&data->txlock);
	schedule_work(&data->work);

	return 0;

failed:
	usb_scuttle_anchored_urbs(&data->deferred);
done:
	spin_lock_irq(&data->txlock);
	clear_bit(BTUSB_SUSPENDING, &data->flags);
	spin_unlock_irq(&data->txlock);

	return err;
}

#ifdef CONFIG_DEV_COREDUMP
static void btusb_coredump(struct device *dev)
{
	struct btusb_data *data = dev_get_drvdata(dev);
	struct hci_dev *hdev = data->hdev;

	if (hdev->dump.coredump)
		hdev->dump.coredump(hdev);
}
#endif

static struct usb_driver btusb_driver = {
	.name		= "btusb",
	.probe		= btusb_probe,
	.disconnect	= btusb_disconnect,
	.suspend	= pm_ptr(btusb_suspend),
	.resume		= pm_ptr(btusb_resume),
	.id_table	= btusb_table,
	.supports_autosuspend = 1,
	.disable_hub_initiated_lpm = 1,

#ifdef CONFIG_DEV_COREDUMP
	.driver = {
		.coredump = btusb_coredump,
	},
#endif
};

module_usb_driver(btusb_driver);

module_param(disable_scofix, bool, 0644);
MODULE_PARM_DESC(disable_scofix, "Disable fixup of wrong SCO buffer size");

module_param(force_scofix, bool, 0644);
MODULE_PARM_DESC(force_scofix, "Force fixup of wrong SCO buffers size");

module_param(enable_autosuspend, bool, 0644);
MODULE_PARM_DESC(enable_autosuspend, "Enable USB autosuspend by default");

module_param(reset, bool, 0644);
MODULE_PARM_DESC(reset, "Send HCI reset command on initialization");

MODULE_AUTHOR("Marcel Holtmann <marcel@holtmann.org>");
MODULE_DESCRIPTION("Generic Bluetooth USB driver ver " VERSION);
MODULE_VERSION(VERSION);
MODULE_LICENSE("GPL");
