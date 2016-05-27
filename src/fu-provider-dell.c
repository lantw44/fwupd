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

#include "config.h"

#include <fwupd.h>
#include <fwup.h>
#include <glib-object.h>
#include <gusb.h>
#include <appstream-glib.h>
#include <glib/gstdio.h>
#include <smbios_c/system_info.h>
#include <smbios_c/smbios.h>
#include <smbios_c/smi.h>
#include <smbios_c/obj/smi.h>
#include <unistd.h>
#include <fcntl.h>

#include "fu-quirks.h"
#include "fu-device.h"
#include "fu-provider-dell.h"

static void	fu_provider_dell_finalize	(GObject	*object);

 /**
  * FuProviderDellPrivate:
  **/
typedef struct {
	GHashTable		*devices;	/* DeviceKey:FuProviderDellDockItem */
	GUsbContext		*usb_ctx;
	gboolean		fake_smbios;
	guint32			fake_output[4];
} FuProviderDellPrivate;

typedef struct {
	FuDevice		*device;
	FuProviderDell		*provider_dell;
} FuProviderDellDockItem;

G_DEFINE_TYPE_WITH_PRIVATE (FuProviderDell, fu_provider_dell, FU_TYPE_PROVIDER)
#define GET_PRIVATE(o) (fu_provider_dell_get_instance_private (o))

/**
 * fu_provider_dell_get_name:
 **/
static const gchar *
fu_provider_dell_get_name (FuProvider *provider)
{
	return "Dell";
}

/**
 * fu_provider_dell_match_dock_component:
 **/
static gboolean
fu_provider_dell_match_dock_component(const gchar *query_str,
				 const efi_guid_t **guid_out,
				 const gchar **name_out)
{
	guint i;
	const DOCK_DESCRIPTION list[] = {
		{WD15_EC_GUID, WD15_EC_STR, EC_DESC},
		{TB15_EC_GUID, TB15_EC_STR, EC_DESC},
		{WD15_PC1_GUID, WD15_PC1_STR, PC1_DESC},
		{TB15_PC1_GUID, TB15_PC1_STR, PC1_DESC},
		{TB15_PC2_GUID, TB15_PC2_STR, PC2_DESC},
		{TBT_CBL_GUID, TBT_CBL_STR, TBT_CBL_DESC},
		{OTHER_CBL_GUID, OTHER_CBL_STR, OTHER_CBL_DESC},
		{LEGACY_CBL_GUID, LEGACY_CBL_STR, LEGACY_CBL_DESC},
	};

	for (i = 0; i < G_N_ELEMENTS(list); i++) {
		if (g_strcmp0 (query_str,
			       list[i].query) == 0) {
			*guid_out = &list[i].guid;
			*name_out = list[i].desc;
			return TRUE;
		}
	}
	return FALSE;
}

/*
 * fu_provider_dell_inject_fake_data:
 **/
void
fu_provider_dell_inject_fake_data (FuProviderDell *provider_dell,
				   guint32 *output)
{
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);
	guint i;

	if (!priv->fake_smbios)
		return;
	for (i = 0; i < 4; i++)
		priv->fake_output[i] = output[i];
}

/**
 * fu_provider_dell_execute_smi:
 **/
static gboolean
fu_provider_dell_execute_smi (FuProviderDell *provider_dell, struct dell_smi_obj *smi)
{
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);
	guint ret;

	if (priv->fake_smbios)
		return TRUE;

	ret = dell_smi_obj_execute (smi);
	if (ret != 0) {
		g_debug ("Dell: SMI execution failed: %d", ret);
		dell_smi_obj_free (smi);
		return FALSE;
	}
	return TRUE;
}

/**
 * fu_provider_dell_execute_simple_smi:
 **/
static gboolean
fu_provider_dell_execute_simple_smi (FuProviderDell *provider_dell,
				     guint32 class, guint32 select,
				     guint32  *args, guint32 *out)
{
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);
	guint ret;
	guint i;

	if (priv->fake_smbios) {
		for (i = 0; i < 4; i++)
			out[i] = priv->fake_output[i];
		return TRUE;
	}
	ret = dell_simple_ci_smi (class, select, args, out);
	if (ret) {
		g_debug ("Dell: failed to run query %d/%d", class, select);
		return FALSE;
	}
	return TRUE;
}

/**
 * fu_provider_dell_get_res:
 **/
static guint32
fu_provider_dell_get_res (FuProviderDell *provider_dell,
			  struct dell_smi_obj *smi, guint32 arg)
{
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);

	if (priv->fake_smbios)
		return priv->fake_output[arg];

	return dell_smi_obj_get_res (smi, arg);
}

/**
 * fu_provider_dell_detect_dock:
 **/
static gboolean
fu_provider_dell_detect_dock (FuProviderDell *provider_dell, guint32 *location)
{
	g_autofree struct dock_count_in *count_args;
	g_autofree struct dock_count_out *count_out;

	/* look up dock count */
	count_args = g_malloc0 (sizeof(struct dock_count_in));
	count_out  = g_malloc0 (sizeof(struct dock_count_out));
	count_args->argument = DACI_DOCK_ARG_COUNT;
	if (!fu_provider_dell_execute_simple_smi (provider_dell,
						  DACI_DOCK_CLASS,
						  DACI_DOCK_SELECT,
						  (guint32 *) count_args,
						  (guint32 *) count_out))
		return FALSE;
	if (count_out->ret != 0) {
		g_debug ("Dell: Failed to query system for dock count: "
			 "(%d)", count_out->ret);
		return FALSE;
	}
	if (count_out->count < 1) {
		g_debug ("Dell: no dock plugged in");
		return FALSE;
	}
	*location = count_out->location;
	g_debug ("Dell: Dock count %u, location %u.",
		 count_out->count, *location);
	return TRUE;

}

/**
 * fu_provider_dell_device_free:
 * Used for clearing an item created by a dock.
 **/
static void
fu_provider_dell_device_free (FuProviderDellDockItem *item)
{
	g_object_unref (item->device);
	g_object_unref (item->provider_dell);
}

/**
 * fu_provider_dell_get_version_format:
 **/
static AsVersionParseFlag
fu_provider_dell_get_version_format (void)
{
	guint i;
	g_autofree gchar *content = NULL;

	/* any vendors match */
	if (!g_file_get_contents ("/sys/class/dmi/id/sys_vendor",
				  &content, NULL, NULL))
		return AS_VERSION_PARSE_FLAG_USE_TRIPLET;
	g_strchomp (content);
	for (i = 0; quirk_table[i].sys_vendor != NULL; i++) {
		if (g_strcmp0 (content, quirk_table[i].sys_vendor) == 0)
			return quirk_table[i].flags;
	}

	/* fall back */
	return AS_VERSION_PARSE_FLAG_USE_TRIPLET;
}

/**
 * fu_provider_dell_get_dock_key:
 **/
static gchar *
fu_provider_dell_get_dock_key (GUsbDevice *device, const gchar *guid)
{
	return g_strdup_printf ("%s_%s",
				g_usb_device_get_platform_id (device),
				guid);
}

/**
 * fu_provider_dell_device_added_cb:
 **/
static void
fu_provider_dell_device_added_cb (GUsbContext *ctx,
				  GUsbDevice *device,
				  FuProviderDell *provider_dell)
{
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);
	FuProviderDellDockItem *item;
	AsVersionParseFlag parse_flags;
	guint16 pid;
	guint16 vid;
	const gchar *query_str;
	const gchar *dock_type = "";
	const gchar *component_name = NULL;
	INFO_UNION buf;
	DOCK_INFO *dock_info;
	guint buf_size;
	struct dell_smi_obj *smi;
	gint result;
	gint i;
	guint32 location;
	const efi_guid_t *guid_raw = NULL;

	g_autofree gchar *dock_key = NULL;
	g_autofree gchar *fw_str = NULL;
	g_autofree gchar *guid_str = NULL;
	g_autofree gchar *dock_id = NULL;
	g_autofree gchar *dock_name = NULL;

	/* don't look up immediately if a dock is connected as that would
	   mean a SMI on every USB device that showed up on the system */
	vid = g_usb_device_get_vid (device);
	pid = g_usb_device_get_pid (device);

	/* we're going to match on the Realtek NIC in the dock */
	if (!(vid == DOCK_NIC_VID && pid == DOCK_NIC_PID))
		return;
	if (!fu_provider_dell_detect_dock (provider_dell, &location))
		return;

	/* look up more information on dock */
	smi = dell_smi_factory (DELL_SMI_DEFAULTS);
	if (!smi) {
		g_debug ("Dell: failure initializing SMI");
		return;
	}
	dell_smi_obj_set_class (smi, DACI_DOCK_CLASS);
	dell_smi_obj_set_select (smi, DACI_DOCK_SELECT);
	dell_smi_obj_set_arg (smi, cbARG1, DACI_DOCK_ARG_INFO);
	dell_smi_obj_set_arg (smi, cbARG2, location);
	buf_size = sizeof(DOCK_INFO_RECORD);
	buf.buf = dell_smi_obj_make_buffer_frombios_auto (smi, cbARG3, buf_size);
	if (!buf.buf) {
		g_debug ("Dell: failed to initialize buffer");
		dell_smi_obj_free (smi);
		return;
	}
	if (!fu_provider_dell_execute_smi (provider_dell, smi))
		return;
	result = fu_provider_dell_get_res (provider_dell, smi, cbARG1);
	if (result != 0) {
		if (result == -6) {
			g_debug ("Dell: Invalid buffer size, sent %d, needed %d",
				 buf_size,
				 fu_provider_dell_get_res (provider_dell, smi, cbARG2));
		} else {
			g_debug ("Dell: SMI execution returned error: %d",
				 result);
		}
		dell_smi_obj_free (smi);
		return;
	}

	if (buf.record->dock_info_header.dir_version != 1) {
		dell_smi_obj_free (smi);
		g_debug ("Dell: Dock info header version unknown: %d",
			 buf.record->dock_info_header.dir_version);
		return;
	}
	switch (buf.record->dock_info_header.dock_type) {
		case DOCK_TYPE_TB15:
			dock_type = "TB15";
			break;
		case DOCK_TYPE_WD15:
			dock_type = "WD15";
			break;
		default:
			g_debug ("Dell: Dock type %d unknown",
				 buf.record->dock_info_header.dock_type);
			return;
	}

	dock_info = &buf.record->dock_info;
	g_debug ("Dell: dock description: %s", dock_info->dock_description);
	/* Note: fw package version is deprecated, look at components instead */
	g_debug ("Dell: dock flash pkg ver: 0x%x", dock_info->flash_pkg_version);
	g_debug ("Dell: dock cable type: %d", dock_info->cable_type);
	g_debug ("Dell: dock location: %d", dock_info->location);
	g_debug ("Dell: dock component count: %d", dock_info->component_count);

	for (i = 0; i < dock_info->component_count; i++) {
		if (i > MAX_COMPONENTS) {
			g_debug ("Dell: Too many components.  Invalid: #%d", i);
			break;
		}
		g_debug ("Dell: dock component %d: %s (version 0x%x)", i,
			 dock_info->components[i].description,
			 dock_info->components[i].fw_version);
		query_str = g_strrstr (dock_info->components[i].description,
				       "Query ") + 6;
		if (!fu_provider_dell_match_dock_component (query_str, &guid_raw,
							    &component_name)) {
			g_debug("Dell: unable to match dock component %s",
				query_str);
			return;
		}

		guid_str = g_strdup ("00000000-0000-0000-0000-000000000000");
		if (efi_guid_to_str (guid_raw, &guid_str) < 0) {
			g_debug ("Dell: Failed to convert GUID.");
			return;
		}
		dock_key = fu_provider_dell_get_dock_key (device, guid_str);
		item = g_hash_table_lookup (priv->devices, dock_key);
		if (item != NULL) {
			g_debug ("Dell: Item %s is already registered.",
				 dock_key);
			return;
		}

		item = g_new0 (FuProviderDellDockItem, 1);
		item->provider_dell = g_object_ref (provider_dell);
		item->device = fu_device_new();
		dock_id = g_strdup_printf ("DELL-%s" G_GUINT64_FORMAT, guid_str);
		dock_name = g_strdup_printf ("Dell %s %s", dock_type,
					     component_name);
		fu_device_set_id (item->device, dock_id);
		fu_device_set_name (item->device, dock_name);
		fu_device_add_guid (item->device, guid_str);
		fu_device_add_flag(item->device, FU_DEVICE_FLAG_REQUIRE_AC);

		/* Dock EC hasn't been updated yet */
		if (dock_info->flash_pkg_version == 0x00ffffff) {
			g_debug ("Dell: dock flash package version %x invalid",
				 dock_info->flash_pkg_version);
			/* Set the EC as the only updatable component */
			if (g_strcmp0 (component_name, EC_DESC) == 0) {
				fu_device_add_flag (item->device,
						    FU_DEVICE_FLAG_ALLOW_OFFLINE);
				fu_device_set_version (item->device, "0.0.0.0");
			}
		}
		else {
			parse_flags = fu_provider_dell_get_version_format ();
			fw_str = as_utils_version_from_uint32 (dock_info->components[i].fw_version,
							       parse_flags);
			fu_device_set_version (item->device, fw_str);
			fu_device_add_flag (item->device,
					    FU_DEVICE_FLAG_ALLOW_OFFLINE);
		}
		g_hash_table_insert (priv->devices, g_strdup (dock_key), item);

		fu_provider_device_add (FU_PROVIDER (provider_dell), item->device);

	}

	/* FIXME: g_autoptr on the smi object */
	dell_smi_obj_free (smi);
}

/**
 * fu_provider_dell_device_removed_cb:
 **/
static void
fu_provider_dell_device_removed_cb (GUsbContext *ctx,
				    GUsbDevice *device,
				    FuProviderDell *provider_dell)
{
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);
	FuProviderDellDockItem *item;
	g_autofree gchar *dock_key = NULL;
	const efi_guid_t guids[] = { WD15_EC_GUID, TB15_EC_GUID, TB15_PC2_GUID,
				     TB15_PC1_GUID, WD15_PC1_GUID,
				     LEGACY_CBL_GUID, OTHER_CBL_GUID,
				     TBT_CBL_GUID};
	const efi_guid_t *guid_raw;
	guint16 pid;
	guint16 vid;
	guint i;

	g_autofree gchar *guid_str = NULL;

	vid = g_usb_device_get_vid (device);
	pid = g_usb_device_get_pid (device);

	/* we're going to match on the Realtek NIC in the dock */
	if (!(vid == DOCK_NIC_VID && pid == DOCK_NIC_PID))
		return;

	/* remove any components already in database? */
	for (i = 0; i < G_N_ELEMENTS(guids); i++) {
		guid_raw = &guids[i];
		guid_str = g_strdup ("00000000-0000-0000-0000-000000000000");
		efi_guid_to_str (guid_raw, &guid_str);
		dock_key = fu_provider_dell_get_dock_key (device, guid_str);
		item = g_hash_table_lookup (priv->devices, dock_key);
		if (item) {
			fu_provider_device_remove (FU_PROVIDER (provider_dell),
						   item->device);
			g_hash_table_remove (priv->devices, dock_key);
		}
	}
}

/**
 * fu_provider_dell_get_results:
 **/
static gboolean
fu_provider_dell_get_results (FuProvider *provider, FuDevice *device, GError **error)
{
	struct smbios_struct *de_table;
	guint16 completion_code = 0xFFFF;
	const gchar *tmp = NULL;

	/* look at offset 0x06 for identifier meaning completion code */
	de_table = smbios_get_next_struct_by_type (0, 0xDE);
	smbios_struct_get_data (de_table, &(completion_code), 0x06, sizeof(guint16));

	if (completion_code == DELL_SUCCESS) {
		fu_device_set_update_state (device, FWUPD_UPDATE_STATE_SUCCESS);
	} else {
		fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED);
		switch (completion_code) {
		case DELL_CONSISTENCY_FAIL:
			tmp = "The image failed one or more consistency checks.";
			break;
		case DELL_FLASH_MEMORY_FAIL:
			tmp = "The BIOS could not access the flash-memory device.";
			break;
		case DELL_FLASH_NOT_READY:
			tmp = "The flash-memory device was not ready when an erase was attempted.";
			break;
		case DELL_FLASH_DISABLED:
			tmp = "Flash programming is currently disabled on the system, or the voltage is low.";
			break;
		case DELL_BATTERY_MISSING:
			tmp = "A battery must be installed for the operation to complete.";
			break;
		case DELL_BATTERY_DEAD:
			tmp = "A fully-charged battery must be present for the operation to complete.";
			break;
		case DELL_AC_MISSING:
			tmp = "An external power adapter must be connected for the operation to complete.";
			break;
		case DELL_CANT_SET_12V:
			tmp = "The 12V required to program the flash-memory could not be set.";
			break;
		case DELL_CANT_UNSET_12V:
			tmp = "The 12V required to program the flash-memory could not be removed.";
			break;
		case DELL_FAILURE_BLOCK_ERASE :
			tmp = "A flash-memory failure occurred during a block-erase operation.";
			break;
		case DELL_GENERAL_FAILURE:
			tmp = "A general failure occurred during the flash programming.";
			break;
		case DELL_DATA_MISCOMPARE:
			tmp = "A data miscompare error occurred during the flash programming.";
			break;
		case DELL_IMAGE_MISSING:
			tmp = "The image could not be found in memory, i.e. the header could not be located.";
			break;
		case DELL_DID_NOTHING:
			tmp = "No update operation has been performed on the system.";
			break;
		default:
			break;
		}
		if (tmp != NULL)
			fu_device_set_update_error (device, tmp);
	}

	return TRUE;
}

/**
 * fu_provider_dell_detect_tpm:
 **/
gboolean
fu_provider_dell_detect_tpm (FuProvider *provider, GError **error)
{
	FuProviderDell *provider_dell = FU_PROVIDER_DELL (provider);
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);
	const gchar *tpm_mode;
	const gchar *tpm_mode_alt;
	guint16 system_id = 0;
	g_autofree gchar *pretty_tpm_name_alt = NULL;
	g_autofree gchar *pretty_tpm_name = NULL;
	g_autofree gchar *product_name = NULL;
	g_autofree gchar *tpm_guid_raw_alt = NULL;
	g_autofree gchar *tpm_guid_alt = NULL;
	g_autofree gchar *tpm_guid = NULL;
	g_autofree gchar *tpm_guid_raw = NULL;
	g_autofree gchar *tpm_id_alt = NULL;
	g_autofree gchar *tpm_id = NULL;
	g_autofree gchar *version_str = NULL;
	g_autofree guint32 *args;
	g_autofree struct tpm_status *out;
	g_autoptr(FuDevice) dev_alt = NULL;
	g_autoptr(FuDevice) dev = NULL;

	args = g_malloc0 (sizeof(guint32) *4);
	out = g_malloc0 (sizeof(struct tpm_status));

	/* execute TPM Status Query */
	args[0] = DACI_FLASH_ARG_TPM;
	if (!fu_provider_dell_execute_simple_smi(provider_dell,
						 DACI_FLASH_INTERFACE_CLASS,
						 DACI_FLASH_INTERFACE_SELECT,
						 args,
						 (guint32 *) out))
		return FALSE;

	if (out->ret != 0) {
		g_debug ("Dell: Failed to query system for TPM information: "
			 "(%d)", out->ret);
		return FALSE;
	}
	/* HW version is output in second /input/ arg
	 * it may be relevant as next gen TPM is enabled
	 */
	g_debug ("Dell: TPM HW version: 0x%x", args[1]);
	g_debug ("Dell: TPM Status: 0x%x", out->status);

	/* test TPM enabled (Bit 0) */
	if (!(out->status & TPM_EN_MASK)) {
		g_debug ("Dell: TPM not enabled (%x)", out->status);
		return FALSE;
	}

	/* test TPM mode to determine current mode */
	if (((out->status & TPM_TYPE_MASK) >> 8) == TPM_1_2_MODE) {
		tpm_mode = "1.2";
		tpm_mode_alt = "2.0";
	} else if (((out->status & TPM_TYPE_MASK) >> 8) == TPM_2_0_MODE) {
		tpm_mode = "2.0";
		tpm_mode_alt = "1.2";
	} else {
		g_debug ("Dell: Unable to determine TPM mode");
		return FALSE;
	}

	if (!priv->fake_smbios)
		system_id = sysinfo_get_dell_system_id ();

	tpm_guid_raw = g_strdup_printf ("%04x-%s", system_id, tpm_mode);
	tpm_guid = as_utils_guid_from_string (tpm_guid_raw);
	tpm_id = g_strdup_printf ("DELL-%s" G_GUINT64_FORMAT, tpm_guid);

	tpm_guid_raw_alt = g_strdup_printf ("%04x-%s", system_id, tpm_mode_alt);
	tpm_guid_alt = as_utils_guid_from_string (tpm_guid_raw_alt);
	tpm_id_alt = g_strdup_printf ("DELL-%s" G_GUINT64_FORMAT, tpm_guid_alt);

	g_debug ("Dell: Creating primary TPM GUID %s and secondary TPM GUID %s",
		 tpm_guid_raw, tpm_guid_raw_alt);
	version_str = as_utils_version_from_uint32 (out->fw_version,
						    AS_VERSION_PARSE_FLAG_NONE);

	/* make it clear that the TPM is a discrete device of the product */
	if (!g_file_get_contents ("/sys/class/dmi/id/product_name",
				  &product_name,NULL, NULL)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "Unable to read product information");
		return FALSE;
	}
	g_strchomp (product_name);
	pretty_tpm_name = g_strdup_printf ("%s TPM %s", product_name, tpm_mode);
	pretty_tpm_name_alt = g_strdup_printf ("%s TPM %s", product_name, tpm_mode_alt);

	/* build Standard device nodes */
	dev = fu_device_new ();
	fu_device_set_id (dev, tpm_id);
	fu_device_add_guid (dev, tpm_guid);
	fu_device_set_name (dev, pretty_tpm_name);
	fu_device_set_version (dev, version_str);
	fu_device_add_flag (dev, FU_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag (dev, FU_DEVICE_FLAG_REQUIRE_AC);
	if (out->flashes_left > 0) {
		fu_device_add_flag (dev, FU_DEVICE_FLAG_ALLOW_OFFLINE);
		fu_device_set_flashes_left (dev, out->flashes_left);
	}

	/* build alternate device node */
	dev_alt = fu_device_new();
	fu_device_set_id (dev_alt, tpm_id_alt);
	fu_device_add_guid (dev_alt, tpm_guid_alt);
	fu_device_set_name (dev_alt, pretty_tpm_name_alt);
	fu_device_add_flag (dev_alt, FU_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag (dev_alt, FU_DEVICE_FLAG_REQUIRE_AC);
	fu_device_add_flag (dev_alt, FU_DEVICE_FLAG_LOCKED);
	fu_device_set_alternate (dev_alt, dev);

	/* If TPM is not owned and at least 1 flash left allow mode switching
	 *
	 * Mode switching is turned on by setting flashes left on alternate
	 * device.
	 */
	if (!((out->status) & TPM_OWN_MASK) &&
	    out->flashes_left > 0) {
		fu_device_set_flashes_left (dev_alt, out->flashes_left);
	} else {
		g_debug ("Dell: %s mode switch disabled due to TPM ownership",
			 pretty_tpm_name);
	}

	fu_provider_device_add (provider, dev);
	fu_provider_device_add (provider, dev_alt);
	return TRUE;
}

/**
 * fu_provider_dell_coldplug:
 **/
static gboolean
fu_provider_dell_coldplug (FuProvider *provider, GError **error)
{
	FuProviderDell *provider_dell = FU_PROVIDER_DELL (provider);
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);
	guint8 dell_supported = 0;
	gint uefi_supported = 0;
	struct smbios_struct *de_table;

	if (priv->fake_smbios) {
		g_debug("Dell: called with fake SMBIOS implementation. "
			"We're ignoring test for SBMIOS table and ESRT. "
			"Individual calls will need to be properly staged.");
		return TRUE;
	}

	/* look at offset 0x00 for identifier meaning DELL is supported */
	de_table = smbios_get_next_struct_by_type (0, 0xDE);
	smbios_struct_get_data (de_table, &(dell_supported), 0x00, sizeof(guint8));
	if (dell_supported != 0xDE) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Dell: firmware updating not supported (%x)",
			     dell_supported);
		return FALSE;
	}
	/* Check and make sure that ESRT is supported as well.
	 *
	 * This will indicate capsule support on the system.
	 *
	 * If ESRT is not turned on, fwupd will have already created an
	 * unlock device (if compiled with support).
	 *
	 * Once unlocked, that will enable this provider too.
	 *
	 * that means we should only look for supported = 1
	 */
	uefi_supported = fwup_supported ();
	if (uefi_supported != 1) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Dell: UEFI capsule firmware updating not supported (%x)",
			     uefi_supported);
		return FALSE;
	}


	/* enumerate looking for a connected dock */
	g_usb_context_enumerate (priv->usb_ctx);

	/* look for switchable TPM */
	if (!fu_provider_dell_detect_tpm (provider, error))
		g_debug ("Dell: No switchable TPM detected");

	return TRUE;
}


/**
 * _fwup_resource_iter_free:
 **/
static void
_fwup_resource_iter_free (fwup_resource_iter *iter)
{
	fwup_resource_iter_destroy (&iter);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(fwup_resource_iter, _fwup_resource_iter_free);

/**
 * fu_provider_dell_unlock:
 **/
static gboolean
fu_provider_dell_unlock(FuProvider *provider,
			FuDevice *device,
			GError **error)
{
	FuDevice *device_alt = NULL;
	FwupdDeviceFlags device_flags_alt = 0;
	guint flashes_left = 0;
	guint flashes_left_alt = 0;

	/* for unlocking TPM1.2 <-> TPM2.0 switching */
	g_debug ("Dell: Unlocking upgrades for: %s (%s)", fu_device_get_name (device),
		 fu_device_get_id (device));
	device_alt = fu_device_get_alternate (device);

	if (!device_alt)
		return FALSE;
	g_debug ("Dell: Preventing upgrades for: %s (%s)", fu_device_get_name (device_alt),
		 fu_device_get_id (device_alt));

	flashes_left = fu_device_get_flashes_left (device);
	flashes_left_alt = fu_device_get_flashes_left (device_alt);
	if (flashes_left == 0) {
		/* flashes left == 0 on both means no flashes left */
		if (flashes_left_alt == 0) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "ERROR: %s has no flashes left.",
				     fu_device_get_name(device));
		/* flashes left == 0 on just unlocking device is ownership */
		} else {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "ERROR: %s is currently OWNED. "
				     "Ownership must be removed to switch modes.",
				     fu_device_get_name(device_alt));
		}
		return FALSE;
	}


	/* clone the info from real device but prevent it from being flashed */
	device_flags_alt = fu_device_get_flags (device_alt);
	fu_device_set_flags (device, device_flags_alt);
	fu_device_set_flags (device_alt, device_flags_alt & ~FU_DEVICE_FLAG_ALLOW_OFFLINE);

	/* make sure that this unlocked device can be updated */
	fu_device_set_version (device, "0.0.0.0");

	return TRUE;
}

/**
 * fu_provider_dell_update:
 **/
static gboolean
fu_provider_dell_update (FuProvider *provider,
			FuDevice *device,
			GBytes *blob_fw,
			FwupdInstallFlags flags,
			GError **error)
{
	FuProviderDell *provider_dell = FU_PROVIDER_DELL (provider);
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);
	g_autoptr(fwup_resource_iter) iter = NULL;
	fwup_resource *re = NULL;
	const gchar *name = NULL;
	gint rc;
	guint flashes_left;
#ifdef HAVE_UEFI_GUID
	const gchar *guidstr = NULL;
	efi_guid_t guid;
#endif

	/* FIXME when any one dock component has been marked for update, unmark
		the rest as updatable
	 */

	/* test the flash counter
	 * - devices with 0 left at coldplug aren't allowed offline updates
	 * - devices greater than 0 should show a warning when near 0
	 */
	flashes_left = fu_device_get_flashes_left (device);
	if (flashes_left > 0) {
		name = fu_device_get_name (device);
		g_debug ("Dell: %s has %d flashes left", name, flashes_left);
		if ((flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
			   flashes_left <= 2) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "WARNING: %s only has %d flashes left. "
				     "To update anyway please run the update with --force.",
				     name, flashes_left);
			return FALSE;
		}
	}

	if (priv->fake_smbios)
		return TRUE;

	/* perform the update */
	g_debug ("Dell: Performing capsule update");

	/* Stuff the payload into a different GUID
	 * - with fwup 0.5 this uses the ESRT GUID
	 * - with fwup 0.6 this uses the payload's GUID
	 * it's preferable to use payload GUID to avoid
	 * a corner case scenario of UEFI BIOS and non-ESRT
	 * update happening at same time
	 */
	fwup_resource_iter_create (&iter);
	fwup_resource_iter_next (iter, &re);
#ifdef HAVE_UEFI_GUID
	guidstr = fu_device_get_guid_default (device);
	rc = efi_str_to_guid (guidstr, &guid);
	if (rc < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Failed to convert guid to string");
		return FALSE;
	}
	rc = fwup_set_guid (iter, &re, &guid);
	if (rc < 0 || re == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Failed to update GUID %s",
			     strerror (rc));
		return FALSE;
	}
#endif
	/* NOTE: if there are problems with this working, adjust the
	 * GUID in the capsule header to match something in ESRT.
	 * This won't actually cause any bad behavior because the real
	 * payload GUID is extracted later on.
	 */
	fu_provider_set_status (provider, FWUPD_STATUS_SCHEDULING);
	rc = fwup_set_up_update_with_buf (re, 0,
					  g_bytes_get_data (blob_fw, NULL),
					  g_bytes_get_size (blob_fw));
	if (rc < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "DELL capsule update failed: %s",
			     strerror (rc));
		return FALSE;
	}
	return TRUE;
}

/**
 * fu_provider_dell_class_init:
 **/
static void
fu_provider_dell_class_init (FuProviderDellClass *klass)
{
	FuProviderClass *provider_class = FU_PROVIDER_CLASS (klass);
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	provider_class->get_name = fu_provider_dell_get_name;
	provider_class->coldplug = fu_provider_dell_coldplug;
	provider_class->unlock = fu_provider_dell_unlock;
	provider_class->update_offline = fu_provider_dell_update;
	provider_class->get_results = fu_provider_dell_get_results;
	object_class->finalize = fu_provider_dell_finalize;
}

/**
 * fu_provider_dell_init:
 **/
static void
fu_provider_dell_init (FuProviderDell *provider_dell)
{
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);
	const gchar *tmp;

	priv->devices = g_hash_table_new_full (g_str_hash, g_str_equal,
					       g_free, (GDestroyNotify) fu_provider_dell_device_free);
	priv->usb_ctx = g_usb_context_new (NULL);
	g_signal_connect (priv->usb_ctx, "device-added",
			  G_CALLBACK (fu_provider_dell_device_added_cb),
			  provider_dell);
	g_signal_connect (priv->usb_ctx, "device-removed",
			  G_CALLBACK (fu_provider_dell_device_removed_cb),
			  provider_dell);
	priv->fake_smbios = FALSE;
	tmp = g_getenv ("FWUPD_DELL_FAKE_SMBIOS");
	if (tmp != NULL)
		priv->fake_smbios = TRUE;
}

/**
 * fu_provider_dell_finalize:
 **/
static void
fu_provider_dell_finalize (GObject *object)
{
	FuProviderDell *provider_dell = FU_PROVIDER_DELL (object);
	FuProviderDellPrivate *priv = GET_PRIVATE (provider_dell);

	g_hash_table_unref (priv->devices);
	g_object_unref (priv->usb_ctx);

	G_OBJECT_CLASS (fu_provider_dell_parent_class)->finalize (object);
}

/**
 * fu_provider_dell_new:
 **/
FuProvider *
fu_provider_dell_new (void)
{
	FuProviderDell *provider;
	provider = g_object_new (FU_TYPE_PROVIDER_DELL , NULL);
	return FU_PROVIDER (provider);
}
