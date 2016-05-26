/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Mario Limonciello <mario_limonciello@dell.com>
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

#ifndef __FU_PROVIDER_DELL_H
#define __FU_PROVIDER_DELL_H

#include <glib-object.h>
#include <efivar.h>
#include <gusb.h>
#include "fu-device.h"
#include "fu-provider.h"

G_BEGIN_DECLS

#define FU_TYPE_PROVIDER_DELL (fu_provider_dell_get_type ())
G_DECLARE_DERIVABLE_TYPE (FuProviderDell, fu_provider_dell, FU, PROVIDER_DELL, FuProvider)

struct _FuProviderDellClass
{
	FuProviderClass			 parent_class;
};

FuProvider	*fu_provider_dell_new		(void);
void
fu_provider_dell_inject_fake_data (FuProviderDell *provider_dell,
				   guint32 *output, guint16 vid, guint16 pid,
				   guint8 *buf);
gboolean
fu_provider_dell_detect_tpm (FuProvider *provider, GError **error);

void
fu_provider_dell_device_added_cb (GUsbContext *ctx,
				  GUsbDevice *device,
				  FuProviderDell *provider_dell);

void
fu_provider_dell_device_removed_cb (GUsbContext *ctx,
				    GUsbDevice *device,
				    FuProviderDell *provider_dell);

G_END_DECLS

/* These are used to indicate the status of a previous DELL flash */
#define DELL_SUCCESS			0x0000
#define DELL_CONSISTENCY_FAIL		0x0001
#define DELL_FLASH_MEMORY_FAIL		0x0002
#define DELL_FLASH_NOT_READY		0x0003
#define DELL_FLASH_DISABLED		0x0004
#define DELL_BATTERY_MISSING		0x0005
#define DELL_BATTERY_DEAD		0x0006
#define DELL_AC_MISSING			0x0007
#define DELL_CANT_SET_12V		0x0008
#define DELL_CANT_UNSET_12V		0x0009
#define DELL_FAILURE_BLOCK_ERASE	0x000A
#define DELL_GENERAL_FAILURE		0x000B
#define DELL_DATA_MISCOMPARE		0x000C
#define DELL_IMAGE_MISSING		0x000D
#define DELL_DID_NOTHING		0xFFFF

/* These are nodes that will indicate information about
 * the TPM status
 */
struct tpm_status {
	guint32 ret;
	guint32 fw_version;
	guint32 status;
	guint32 flashes_left;
};
#define TPM_EN_MASK	0x0001
#define TPM_OWN_MASK	0x0004
#define TPM_TYPE_MASK	0x0F00
#define TPM_1_2_MODE	0x0001
#define TPM_2_0_MODE	0x0002

/* These are DACI class/select needed for
 * flash capability queries
 */
#define DACI_FLASH_INTERFACE_CLASS	7
#define DACI_FLASH_INTERFACE_SELECT	3
#define DACI_FLASH_ARG_TPM		2

/* These are for dock query capabilities */
struct dock_count_in {
	guint32 argument;
	guint32 reserved1;
	guint32 reserved2;
	guint32 reserved3;
};

struct dock_count_out {
	guint32 ret;
	guint32 count;
	guint32 location;
	guint32 reserved;
};

/* Dock Info version 1 */
#pragma pack(1)
#define MAX_COMPONENTS 5
typedef struct _DOCK_DESCRIPTION
{
	const efi_guid_t	guid;
	const gchar *		query;
	const gchar *		desc;
} DOCK_DESCRIPTION;

typedef struct _COMPONENTS {
	gchar		description[80];
	guint32		fw_version; 		/* BCD format: 0x00XXYYZZ */
} COMPONENTS;

typedef struct _DOCK_INFO {
	gchar		dock_description[80];
	guint32		flash_pkg_version;	/* BCD format: 0x00XXYYZZ */
	guint32		cable_type;		/* bit0-7 cable type, bit7-31 set to 0 */
	guint8		location;		/* Location of the dock */
	guint8		reserved;
	guint8		component_count;
	COMPONENTS	components[MAX_COMPONENTS];	/* number of component_count */
} DOCK_INFO;

typedef struct _DOCK_INFO_HEADER {
	guint8		dir_version;  		/* version 1, 2 … */
	guint8		dock_type;
	guint16		reserved;
} DOCK_INFO_HEADER;

typedef struct _DOCK_INFO_RECORD {
	DOCK_INFO_HEADER	dock_info_header; /* dock version specific definition */
	DOCK_INFO		dock_info;
} DOCK_INFO_RECORD;

typedef union _INFO_UNION{
	guint8 *buf;
	DOCK_INFO_RECORD *record;
} INFO_UNION;
#pragma pack()

typedef enum _DOCK_TYPE
{
	DOCK_TYPE_NONE,
	DOCK_TYPE_TB15,
	DOCK_TYPE_WD15
} DOCK_TYPE;

typedef enum _CABLE_TYPE
{
	CABLE_TYPE_NONE,
	CABLE_TYPE_TBT,
	CABLE_TYPE_OTHER,
	CABLE_TYPE_LEGACY
} CABLE_TYPE;

#define DACI_DOCK_CLASS			17
#define DACI_DOCK_SELECT		22
#define DACI_DOCK_ARG_COUNT		0
#define DACI_DOCK_ARG_INFO		1

#define DOCK_NIC_VID		0x0bda
#define DOCK_NIC_PID		0x8153

/* These are for matching the components */
#define WD15_EC_STR		"2 0 2 2 0"
#define TB15_EC_STR		"2 0 2 1 0"
#define TB15_PC2_STR		"2 1 0 1 1"
#define TB15_PC1_STR		"2 1 0 1 0"
#define WD15_PC1_STR		"2 1 0 2 0"
#define LEGACY_CBL_STR		"2 2 2 1 0"
#define OTHER_CBL_STR		"2 2 2 2 0"
#define TBT_CBL_STR		"2 2 2 3 0"

#define WD15_EC_GUID		EFI_GUID (0xE8445370, 0x0211, 0x449D, 0x9FAA, 0x10, 0x79, 0x06, 0xAB, 0x18, 0x9F)
#define TB15_EC_GUID		EFI_GUID (0x33CC8870, 0xB1FC, 0x4EC7, 0x948A, 0xC0, 0x74, 0x96, 0x87, 0x4F, 0xAF)
#define TB15_PC2_GUID		EFI_GUID (0x1B52C630, 0x86F6, 0x4AEE, 0x9F0C, 0x47, 0x4D, 0xC6, 0xBE, 0x49, 0xB6)
#define TB15_PC1_GUID		EFI_GUID (0x8FE183DA, 0xC94E, 0x4804, 0xB319, 0x0F, 0x1B, 0xA5, 0x45, 0x7A, 0x69)
#define WD15_PC1_GUID		EFI_GUID (0x8BA2B709, 0x6F97, 0x47FC, 0xB7E7, 0x6A, 0x87, 0xB5, 0x78, 0xFE, 0x25)
#define LEGACY_CBL_GUID		EFI_GUID (0xFECE1537, 0xD683, 0x4EA8, 0xB968, 0x15, 0x45, 0x30, 0xBB, 0x6F, 0x73)
#define OTHER_CBL_GUID		EFI_GUID (0xE2BF3AAD, 0x61A3, 0x44BF, 0x91EF, 0x34, 0x9B, 0x39, 0x51, 0x5D, 0x29)
#define TBT_CBL_GUID		EFI_GUID (0x6DC832FC, 0x5BB0, 0x4E63, 0xA2FF, 0x02, 0xAA, 0xBA, 0x5B, 0xC1, 0xDC)

#define EC_DESC			"EC"
#define PC1_DESC		"Port Controller 1"
#define PC2_DESC		"Port Controller 2"
#define LEGACY_CBL_DESC		"Passive Cable"
#define OTHER_CBL_DESC		"Other Cable"
#define TBT_CBL_DESC		"Thunderbolt Cable"

#endif /* __FU_PROVIDER_DELL_H */
