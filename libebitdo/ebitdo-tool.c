/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <gusb.h>

#include "ebitdo-common.h"
#include "ebitdo-device.h"

int
main (int argc, char **argv)
{
	gsize len;
	guint i;
	g_autofree guint8 *data = NULL;
	g_autoptr(EbitdoDevice) dev = NULL;
	g_autoptr(GBytes) fw = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GUsbContext) usb_ctx = NULL;

	g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

	/* require filename */
	if (argc != 2) {
		g_print ("USAGE: %s <filename>\n", argv[0]);
		return 1;
	}

	/* get the device */
	usb_ctx = g_usb_context_new (&error);
	if (usb_ctx == NULL) {
		g_print ("Failed to open USB devices: %s\n", error->message);
		return 1;
	}
	g_usb_context_enumerate (usb_ctx);
	devices = g_usb_context_get_devices (usb_ctx);
	for (i = 0; i < devices->len; i++) {
		GUsbDevice *usb_dev_tmp = g_ptr_array_index (devices, i);
		g_autoptr(EbitdoDevice) dev_tmp = ebitdo_device_new (usb_dev_tmp);
		if (ebitdo_device_get_kind (dev_tmp) != EBITDO_DEVICE_KIND_UNKNOWN) {
			dev = g_object_ref (dev_tmp);
			break;
		}
	}

	/* nothing supported */
	if (dev == NULL) {
		g_print ("No supported device plugged in!\n");
		return 1;
	}
	g_debug ("found %s [%04x:%04x]",
		 ebitdo_device_kind_to_string (ebitdo_device_get_kind (dev)),
		 g_usb_device_get_vid (ebitdo_device_get_usb_device (dev)),
		 g_usb_device_get_pid (ebitdo_device_get_usb_device (dev)));

	/* open device */
	if (!ebitdo_device_open (dev, &error)) {
		g_print ("Failed to open USB device: %s\n", error->message);
		return 1;
	}
	g_print ("Device Firmware Ver: %s\n", ebitdo_device_get_version (dev));
	g_print ("Device Verification ID:\n");
	for (i = 0; i < 9; i++)
		g_print ("\t%u = 0x%08x\n", i, ebitdo_device_get_serial(dev)[i]);

	/* not in bootloader mode, so print what to do */
	if (ebitdo_device_get_kind (dev) != EBITDO_DEVICE_KIND_BOOTLOADER) {
		g_print ("1. Disconnect the controller\n");
		switch (ebitdo_device_get_kind (dev)) {
		case EBITDO_DEVICE_KIND_FC30:
		case EBITDO_DEVICE_KIND_NES30:
		case EBITDO_DEVICE_KIND_SFC30:
		case EBITDO_DEVICE_KIND_SNES30:
			g_print ("2. Hold down L+R+START for 3 seconds until "
				 "both LED lights flashing.\n");
			break;
		case EBITDO_DEVICE_KIND_FC30PRO:
		case EBITDO_DEVICE_KIND_NES30PRO:
			g_print ("2. Hold down RETURN+POWER for 3 seconds until "
				 "both LED lights flashing.\n");
			break;
		case EBITDO_DEVICE_KIND_FC30_ARCADE:
			g_print ("2. Hold down L1+R1+HOME for 3 seconds until "
				 "both blue LED and green LED blink.\n");
			break;
		default:
			g_print ("2. Do what it says in the manual.\n");
			break;
		}
		g_print ("3. Connect controller\n");
		return 1;
	}

	/* load firmware file */
	if (!g_file_get_contents (argv[1], (gchar **) &data, &len, &error)) {
		g_print ("Failed to load file: %s\n", error->message);
		return 1;
	}

	/* update with data blob */
	fw = g_bytes_new (data, len);
	if (!ebitdo_device_write_firmware (dev, fw, &error)) {
		g_print ("Failed to write firmware: %s\n", error->message);
		return 1;
	}

	/* close device */
	if (!ebitdo_device_close (dev, &error)) {
		g_print ("Failed to close USB device: %s\n", error->message);
		return 1;
	}

	/* success */
	g_print ("Now turn off the controlled with the power button.\n");

	return 0;
}
