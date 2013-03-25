/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:cd-icc
 * @short_description: A XML parser that exposes a ICC tree
 */

#include "config.h"

#include <glib.h>
#include <lcms2.h>
#include <string.h>
#include <stdlib.h>

#include "cd-icc.h"

static void	cd_icc_class_init	(CdIccClass	*klass);
static void	cd_icc_init		(CdIcc		*icc);
static void	cd_icc_load_named_colors (CdIcc		*icc);
static void	cd_icc_finalize		(GObject	*object);

#define CD_ICC_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CD_TYPE_ICC, CdIccPrivate))

typedef enum {
	CD_MLUC_DESCRIPTION,
	CD_MLUC_COPYRIGHT,
	CD_MLUC_MANUFACTURER,
	CD_MLUC_MODEL,
	CD_MLUC_LAST
} CdIccMluc;

/**
 * CdIccPrivate:
 *
 * Private #CdIcc data
 **/
struct _CdIccPrivate
{
	CdColorspace		 colorspace;
	CdProfileKind		 kind;
	cmsHPROFILE		 lcms_profile;
	gboolean		 can_delete;
	gchar			*checksum;
	gchar			*filename;
	gdouble			 version;
	GHashTable		*mluc_data[CD_MLUC_LAST]; /* key is 'en_GB' or '' for default */
	GHashTable		*metadata;
	guint32			 size;
	GPtrArray		*named_colors;
};

G_DEFINE_TYPE (CdIcc, cd_icc, G_TYPE_OBJECT)

enum {
	PROP_0,
	PROP_SIZE,
	PROP_FILENAME,
	PROP_VERSION,
	PROP_KIND,
	PROP_COLORSPACE,
	PROP_CAN_DELETE,
	PROP_CHECKSUM,
	PROP_LAST
};

/**
 * cd_icc_error_quark:
 *
 * Return value: An error quark.
 *
 * Since: 0.1.32
 **/
GQuark
cd_icc_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("cd_icc_error");
	return quark;
}

/**
 * cd_icc_fix_utf8_string:
 *
 * NC entries are supposed to be 7-bit ASCII, although some profile vendors
 * try to be clever which breaks handling them as UTF-8.
 **/
static gboolean
cd_icc_fix_utf8_string (GString *string)
{
	guint i;
	guchar tmp;

	/* replace clever characters */
	for (i = 0; i < string->len; i++) {
		tmp = (guchar) string->str[i];

		/* (R) */
		if (tmp == 0xae) {
			string->str[i] = 0xc2;
			g_string_insert_c (string, i + 1, tmp);
			i += 1;
		}

		/* unknown */
		if (tmp == 0x86)
			g_string_erase (string, i, 1);
	}

	/* check if we repaired it okay */
	return g_utf8_validate (string->str, string->len, NULL);
}

/**
 * cd_icc_uint32_to_str:
 **/
static void
cd_icc_uint32_to_str (guint32 id, gchar *str)
{
	/* this is a hack */
	memcpy (str, &id, 4);
	str[4] = '\0';
}

/**
 * cd_icc_to_string:
 * @icc: a #CdIcc instance.
 *
 * Returns a string representation of the ICC profile.
 *
 * Return value: an allocated string
 *
 * Since: 0.1.32
 **/
gchar *
cd_icc_to_string (CdIcc *icc)
{
	CdIccPrivate *priv = icc->priv;
	cmsInt32Number tag_size;
	cmsTagSignature sig;
	cmsTagSignature sig_link;
	cmsTagTypeSignature tag_type;
	gboolean ret;
	gchar tag_str[5] = "    ";
	GDateTime *created;
	GString *str;
	guint32 i;
	guint32 number_tags;
	guint32 tmp;
	guint8 profile_id[4];

	g_return_val_if_fail (CD_IS_ICC (icc), NULL);

	/* print header */
	str = g_string_new ("icc:\nHeader:\n");

	/* print size */
	tmp = cd_icc_get_size (icc);
	if (tmp > 0)
		g_string_append_printf (str, "  Size\t\t= %i bytes\n", tmp);

	/* version */
	g_string_append_printf (str, "  Version\t= %.1f\n",
				cd_icc_get_version (icc));

	/* device class */
	g_string_append_printf (str, "  Profile Kind\t= %s\n",
				cd_profile_kind_to_string (cd_icc_get_kind (icc)));

	/* colorspace */
	g_string_append_printf (str, "  Colorspace\t= %s\n",
				cd_colorspace_to_string (cd_icc_get_colorspace (icc)));


	/* PCS */
	g_string_append (str, "  Conn. Space\t= ");
	switch (cmsGetPCS (priv->lcms_profile)) {
	case cmsSigXYZData:
		g_string_append (str, "xyz\n");
		break;
	case cmsSigLabData:
		g_string_append (str, "lab\n");
		break;
	default:
		g_string_append (str, "unknown\n");
		break;
	}

	/* date and time */
	created = cd_icc_get_created (icc);
	if (created != NULL) {
		gchar *created_str;
		created_str = g_date_time_format (created, "%F, %T");
		g_string_append_printf (str, "  Date, Time\t= %s\n", created_str);
		g_free (created_str);
		g_date_time_unref (created);
	}

	/* profile use flags */
	g_string_append (str, "  Flags\t\t= ");
	tmp = cmsGetHeaderFlags (priv->lcms_profile);
	g_string_append (str, (tmp & cmsEmbeddedProfileTrue) > 0 ?
				"Embedded profile" : "Not embedded profile");
	g_string_append (str, ", ");
	g_string_append (str, (tmp & cmsUseWithEmbeddedDataOnly) > 0 ?
				"Use with embedded data only" : "Use anywhere");
	g_string_append (str, "\n");

	/* rendering intent */
	g_string_append (str, "  Rndrng Intnt\t= ");
	switch (cmsGetHeaderRenderingIntent (priv->lcms_profile)) {
	case INTENT_PERCEPTUAL:
		g_string_append (str, "perceptual\n");
		break;
	case INTENT_RELATIVE_COLORIMETRIC:
		g_string_append (str, "relative-colorimetric\n");
		break;
	case INTENT_SATURATION:
		g_string_append (str, "saturation\n");
		break;
	case INTENT_ABSOLUTE_COLORIMETRIC:
		g_string_append (str, "absolute-colorimetric\n");
		break;
	default:
		g_string_append (str, "unknown\n");
		break;
	}

	/* profile ID */
	cmsGetHeaderProfileID (priv->lcms_profile, profile_id);
	g_string_append_printf (str, "  Profile ID\t= 0x%02x%02x%02x%02x\n",
				profile_id[0],
				profile_id[1],
				profile_id[2],
				profile_id[3]);

	/* print tags */
	g_string_append (str, "\n");
	number_tags = cmsGetTagCount (priv->lcms_profile);
	for (i = 0; i < number_tags; i++) {
		sig = cmsGetTagSignature (priv->lcms_profile, i);

		/* convert to text */
		cd_icc_uint32_to_str (GUINT32_FROM_BE (sig), tag_str);

		/* print header */
		g_string_append_printf (str, "tag %02i:\n", i);
		g_string_append_printf (str, "  sig\t'%s' [0x%x]\n", tag_str, sig);

		/* is this linked to another data area? */
		sig_link = cmsTagLinkedTo (priv->lcms_profile, sig);
		if (sig_link != 0) {
			cd_icc_uint32_to_str (GUINT32_FROM_BE (sig_link), tag_str);
			g_string_append_printf (str, "  link\t'%s' [0x%x]\n", tag_str, sig_link);
			continue;
		}

		tag_size = cmsReadRawTag (priv->lcms_profile, sig, &tmp, 4);
		cd_icc_uint32_to_str (tmp, tag_str);
		tag_type = GUINT32_FROM_BE (tmp);
		g_string_append_printf (str, "  type\t'%s' [0x%x]\n", tag_str, tag_type);
		g_string_append_printf (str, "  size\t%i\n", tag_size);

		/* print tag details */
		switch (tag_type) {
		case cmsSigTextType:
		case cmsSigTextDescriptionType:
		case cmsSigMultiLocalizedUnicodeType:
		{
			cmsMLU *mlu;
			gchar text_buffer[128];
			guint32 text_size;

			g_string_append_printf (str, "Text:\n");
			mlu = cmsReadTag (priv->lcms_profile, sig);
			if (mlu == NULL) {
				g_string_append_printf (str, "  Info:\t\tMLU invalid!\n");
				break;
			}
			text_size = cmsMLUgetASCII (mlu,
						    cmsNoLanguage,
						    cmsNoCountry,
						    text_buffer,
						    sizeof (text_buffer));
			if (text_size > 0) {
				g_string_append_printf (str, "  en_US:\t%s [%i bytes]\n",
							text_buffer, text_size);
			}
			break;
		}
		case cmsSigXYZType:
		{
			cmsCIEXYZ *xyz;
			xyz = cmsReadTag (priv->lcms_profile, sig);
			g_string_append_printf (str, "XYZ:\n");
			g_string_append_printf (str, "  X:%f Y:%f Z:%f\n",
						xyz->X, xyz->Y, xyz->Z);
			break;
		}
		case cmsSigCurveType:
		{
			cmsToneCurve *curve;
			gdouble estimated_gamma;
			g_string_append_printf (str, "Curve:\n");
			curve = cmsReadTag (priv->lcms_profile, sig);
			estimated_gamma = cmsEstimateGamma (curve, 0.01);
			if (estimated_gamma > 0) {
				g_string_append_printf (str,
							"  Curve is gamma of %f\n",
							estimated_gamma);
			}
			break;
		}
		case cmsSigDictType:
		{
			cmsHANDLE dict;
			const cmsDICTentry *entry;
			gchar ascii_name[1024];
			gchar ascii_value[1024];

			g_string_append_printf (str, "Dictionary:\n");
			dict = cmsReadTag (priv->lcms_profile, sig);
			for (entry = cmsDictGetEntryList (dict);
			     entry != NULL;
			     entry = cmsDictNextEntry (entry)) {

				/* convert from wchar_t to UTF-8 */
				wcstombs (ascii_name, entry->Name, sizeof (ascii_name));
				wcstombs (ascii_value, entry->Value, sizeof (ascii_value));
				g_string_append_printf (str, "  %s\t->\t%s\n",
							ascii_name, ascii_value);
			}
			break;
		}
		case cmsSigNamedColor2Type:
		{
			CdColorLab lab;
			cmsNAMEDCOLORLIST *nc2;
			cmsUInt16Number pcs[3];
			gchar name[cmsMAX_PATH];
			gchar prefix[33];
			gchar suffix[33];
			GString *string;
			guint j;

			g_string_append_printf (str, "Named colors:\n");
			nc2 = cmsReadTag (priv->lcms_profile, sig);
			if (nc2 == NULL) {
				g_string_append_printf (str, "  Info:\t\tNC invalid!\n");
				continue;
			}

			/* get the number of NCs */
			tmp = cmsNamedColorCount (nc2);
			if (tmp == 0) {
				g_string_append_printf (str, "  Info:\t\tNo NC's!\n");
				continue;
			}
			for (j = 0; j < tmp; j++) {

				/* parse title */
				string = g_string_new ("");
				ret = cmsNamedColorInfo (nc2, j,
							 name,
							 prefix,
							 suffix,
							 (cmsUInt16Number *)&pcs,
							 NULL);
				if (!ret) {
					g_string_append_printf (str, "  Info:\t\tFailed to get NC #%i", j);
					continue;
				}
				if (prefix[0] != '\0')
					g_string_append_printf (string, "%s ", prefix);
				g_string_append (string, name);
				if (suffix[0] != '\0')
					g_string_append_printf (string, " %s", suffix);

				/* check is valid */
				ret = g_utf8_validate (string->str, string->len, NULL);
				if (!ret) {
					g_string_append (str, "  Info:\t\tInvalid 7 bit ASCII / UTF8\n");
					ret = cd_icc_fix_utf8_string (string);
					if (!ret) {
						g_string_append (str, "  Info:\t\tIFailed to fix: skipping entry\n");
						continue;
					}
				}

				/* get color */
				cmsLabEncoded2Float ((cmsCIELab *) &lab, pcs);
				g_string_append_printf (str, "  %03i:\t %s\tL:%.2f a:%.3f b:%.3f\n",
							j,
							string->str,
							lab.L, lab.a, lab.b);
				g_string_free (string, TRUE);
			}
			break;
		}
		default:
			break;
		}

		/* done! */
		g_string_append_printf (str, "\n");
	}

	/* remove trailing newline */
	if (str->len > 0)
		g_string_truncate (str, str->len - 1);

	return g_string_free (str, FALSE);
}

/* map lcms profile class to colord type */
const struct {
	cmsProfileClassSignature	lcms;
	CdProfileKind			colord;
} map_profile_kind[] = {
	{ cmsSigInputClass,		CD_PROFILE_KIND_INPUT_DEVICE },
	{ cmsSigDisplayClass,		CD_PROFILE_KIND_DISPLAY_DEVICE },
	{ cmsSigOutputClass,		CD_PROFILE_KIND_OUTPUT_DEVICE },
	{ cmsSigLinkClass,		CD_PROFILE_KIND_DEVICELINK },
	{ cmsSigColorSpaceClass,	CD_PROFILE_KIND_COLORSPACE_CONVERSION },
	{ cmsSigAbstractClass,		CD_PROFILE_KIND_ABSTRACT },
	{ cmsSigNamedColorClass,	CD_PROFILE_KIND_NAMED_COLOR },
	{ 0,				CD_PROFILE_KIND_LAST }
};

/* map lcms colorspace to colord type */
const struct {
	cmsColorSpaceSignature		lcms;
	CdColorspace			colord;
} map_colorspace[] = {
	{ cmsSigXYZData,		CD_COLORSPACE_XYZ },
	{ cmsSigLabData,		CD_COLORSPACE_LAB },
	{ cmsSigLuvData,		CD_COLORSPACE_LUV },
	{ cmsSigYCbCrData,		CD_COLORSPACE_YCBCR },
	{ cmsSigYxyData,		CD_COLORSPACE_YXY },
	{ cmsSigRgbData,		CD_COLORSPACE_RGB },
	{ cmsSigGrayData,		CD_COLORSPACE_GRAY },
	{ cmsSigHsvData,		CD_COLORSPACE_HSV },
	{ cmsSigCmykData,		CD_COLORSPACE_CMYK },
	{ cmsSigCmyData,		CD_COLORSPACE_CMY },
	{ 0,				CD_COLORSPACE_LAST }
};

/**
 * cd_icc_get_precooked_md5:
 **/
static gchar *
cd_icc_get_precooked_md5 (cmsHPROFILE lcms_profile)
{
	cmsUInt8Number icc_id[16];
	gboolean md5_precooked = FALSE;
	gchar *md5 = NULL;
	guint i;

	/* check to see if we have a pre-cooked MD5 */
	cmsGetHeaderProfileID (lcms_profile, icc_id);
	for (i = 0; i < 16; i++) {
		if (icc_id[i] != 0) {
			md5_precooked = TRUE;
			break;
		}
	}
	if (md5_precooked == FALSE)
		goto out;

	/* convert to a hex string */
	md5 = g_new0 (gchar, 32 + 1);
	for (i = 0; i < 16; i++)
		g_snprintf (md5 + i * 2, 3, "%02x", icc_id[i]);
out:
	return md5;
}

/**
 * cd_icc_load:
 **/
static void
cd_icc_load (CdIcc *icc, CdIccLoadFlags flags)
{
	CdIccPrivate *priv = icc->priv;
	cmsColorSpaceSignature colorspace;
	cmsHANDLE dict;
	cmsProfileClassSignature profile_class;
	guint i;

	/* get version */
	priv->version = cmsGetProfileVersion (priv->lcms_profile);

	/* convert profile kind */
	profile_class = cmsGetDeviceClass (priv->lcms_profile);
	for (i = 0; map_profile_kind[i].colord != CD_PROFILE_KIND_LAST; i++) {
		if (map_profile_kind[i].lcms == profile_class) {
			priv->kind = map_profile_kind[i].colord;
			break;
		}
	}

	/* convert colorspace */
	colorspace = cmsGetColorSpace (priv->lcms_profile);
	for (i = 0; map_colorspace[i].colord != CD_COLORSPACE_LAST; i++) {
		if (map_colorspace[i].lcms == colorspace) {
			priv->colorspace = map_colorspace[i].colord;
			break;
		}
	}

	/* read optional metadata? */
	if ((flags & CD_ICC_LOAD_FLAGS_METADATA) > 0) {
		dict = cmsReadTag (priv->lcms_profile, cmsSigMetaTag);
		if (dict != NULL) {
			const cmsDICTentry *entry;
			gchar ascii_name[1024];
			gchar ascii_value[1024];
			for (entry = cmsDictGetEntryList (dict);
			     entry != NULL;
			     entry = cmsDictNextEntry (entry)) {
				wcstombs (ascii_name,
					  entry->Name,
					  sizeof (ascii_name));
				wcstombs (ascii_value,
					  entry->Value,
					  sizeof (ascii_value));
				g_hash_table_insert (priv->metadata,
						     g_strdup (ascii_name),
						     g_strdup (ascii_value));
			}
		}
	}

	/* get precooked profile ID if one exists */
	priv->checksum = cd_icc_get_precooked_md5 (priv->lcms_profile);

	/* read default translations */
	cd_icc_get_description (icc, NULL, NULL);
	cd_icc_get_copyright (icc, NULL, NULL);
	cd_icc_get_manufacturer (icc, NULL, NULL);
	cd_icc_get_model (icc, NULL, NULL);
	if ((flags & CD_ICC_LOAD_FLAGS_TRANSLATIONS) > 0) {
		/* FIXME: get the locale list from LCMS */
	}

	/* read named colors if the client cares */
	if ((flags & CD_ICC_LOAD_FLAGS_NAMED_COLORS) > 0)
		cd_icc_load_named_colors (icc);
}

/**
 * cd_icc_load_data:
 * @icc: a #CdIcc instance.
 * @data: binary data
 * @data_len: Length of @data
 * @flags: a set of #CdIccLoadFlags
 * @error: A #GError or %NULL
 *
 * Loads an ICC profile from raw byte data.
 *
 * Since: 0.1.32
 **/
gboolean
cd_icc_load_data (CdIcc *icc,
		  const guint8 *data,
		  gsize data_len,
		  CdIccLoadFlags flags,
		  GError **error)
{
	CdIccPrivate *priv = icc->priv;
	gboolean ret = TRUE;

	g_return_val_if_fail (CD_IS_ICC (icc), FALSE);
	g_return_val_if_fail (data != NULL, FALSE);
	g_return_val_if_fail (data_len != 0, FALSE);
	g_return_val_if_fail (priv->lcms_profile == NULL, FALSE);

	/* ensure we have the header */
	if (data_len < 0x84) {
		ret = FALSE;
		g_set_error_literal (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_FAILED_TO_PARSE,
				     "icc was not valid (file size too small)");
		goto out;
	}

	/* load icc into lcms */
	priv->lcms_profile = cmsOpenProfileFromMem (data, data_len);
	if (priv->lcms_profile == NULL) {
		ret = FALSE;
		g_set_error_literal (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_FAILED_TO_PARSE,
				     "failed to load: not an ICC icc");
		goto out;
	}

	/* save length to avoid trusting the profile */
	priv->size = data_len;

	/* load cached data */
	cd_icc_load (icc, flags);

	/* calculate the data MD5 if there was no embedded profile */
	if (priv->checksum == NULL &&
	    (flags & CD_ICC_LOAD_FLAGS_FALLBACK_MD5) > 0) {
		priv->checksum = g_compute_checksum_for_data (G_CHECKSUM_MD5,
							      (const guchar *) data,
							      data_len);
	}
out:
	return ret;
}

/**
 * utf8_to_wchar_t:
 **/
static wchar_t *
utf8_to_wchar_t (const char *src)
{
	gssize len;
	gssize converted;
	wchar_t *buf = NULL;

	len = mbstowcs (NULL, src, 0);
	if (len < 0) {
		g_warning ("Invalid UTF-8 in string %s", src);
		goto out;
	}
	len += 1;
	buf = g_malloc (sizeof (wchar_t) * len);
	converted = mbstowcs (buf, src, len - 1);
	g_assert (converted != -1);
	buf[converted] = '\0';
out:
	return buf;
}

/**
 * _cmsDictAddEntryAscii:
 **/
static cmsBool
_cmsDictAddEntryAscii (cmsHANDLE dict,
		       const gchar *key,
		       const gchar *value)
{
	cmsBool ret = FALSE;
	wchar_t *mb_key = NULL;
	wchar_t *mb_value = NULL;

	mb_key = utf8_to_wchar_t (key);
	if (mb_key == NULL)
		goto out;
	mb_value = utf8_to_wchar_t (value);
	if (mb_value == NULL)
		goto out;
	ret = cmsDictAddEntry (dict, mb_key, mb_value, NULL, NULL);
out:
	g_free (mb_key);
	g_free (mb_value);
	return ret;
}

typedef struct {
	gchar		*language_code;	/* will always be xx\0 */
	gchar		*country_code;	/* will always be xx\0 */
	wchar_t		*wtext;
} CdMluObject;

/**
 * cd_util_mlu_object_free:
 **/
static void
cd_util_mlu_object_free (gpointer data)
{
	CdMluObject *obj = (CdMluObject *) data;
	g_free (obj->language_code);
	g_free (obj->country_code);
	g_free (obj->wtext);
	g_free (obj);
}

/**
 * cd_util_mlu_object_parse:
 **/
static CdMluObject *
cd_util_mlu_object_parse (const gchar *locale, const gchar *utf8_text)
{
	CdMluObject *obj = NULL;
	gchar *key = NULL;
	gchar **split = NULL;
	guint type;
	wchar_t *wtext;

	/* untranslated version */
	if (locale == NULL || locale[0] == '\0') {
		obj = g_new0 (CdMluObject, 1);
		obj->wtext = utf8_to_wchar_t (utf8_text);
		goto out;
	}

	/* ignore ##@latin */
	if (g_strstr_len (locale, -1, "@") != NULL)
		goto out;

	key = g_strdup (locale);
	g_strdelimit (key, ".", '\0');
	split = g_strsplit (key, "_", -1);
	if (strlen (split[0]) != 2)
		goto out;
	type = g_strv_length (split);
	if (type > 2)
		goto out;

	/* convert to wchars */
	wtext = utf8_to_wchar_t (utf8_text);
	if (wtext == NULL)
		goto out;

	/* lv */
	if (type == 1) {
		obj = g_new0 (CdMluObject, 1);
		obj->language_code = g_strdup (split[0]);
		obj->wtext = wtext;
		goto out;
	}

	/* en_GB */
	if (strlen (split[1]) != 2)
		goto out;
	obj = g_new0 (CdMluObject, 1);
	obj->language_code = g_strdup (split[0]);
	obj->country_code = g_strdup (split[1]);
	obj->wtext = wtext;
out:
	g_free (key);
	g_strfreev (split);
	return obj;
}

/**
 * cd_util_write_tag_localized:
 **/
static gboolean
cd_util_write_tag_localized (CdIcc *icc,
			     cmsTagSignature sig,
			     GHashTable *hash,
			     GError **error)
{
	CdIccPrivate *priv = icc->priv;
	CdMluObject *obj;
	cmsMLU *mlu = NULL;
	const gchar *locale;
	gboolean ret = TRUE;
	GList *keys;
	GList *l;
	GPtrArray *array;
	guint i;

	/* convert all the hash entries into CdMluObject's */
	keys = g_hash_table_get_keys (hash);
	array = g_ptr_array_new_with_free_func (cd_util_mlu_object_free);
	for (l = keys; l != NULL; l = l->next) {
		locale = l->data;
		obj = cd_util_mlu_object_parse (locale,
						g_hash_table_lookup (hash, locale));
		if (obj == NULL)
			continue;
		g_ptr_array_add (array, obj);
	}

	/* delete tag if there is no data */
	if (array->len == 0) {
		cmsWriteTag (priv->lcms_profile, sig, NULL);
		goto out;
	}

	/* promote V2 profiles so we can write a 'mluc' type */
	if (array->len > 1 && priv->version < 4.0)
		cmsSetProfileVersion (priv->lcms_profile, 4.0);

	/* create MLU object to hold all the translations */
	mlu = cmsMLUalloc (NULL, array->len);
	for (i = 0; i < array->len; i++) {
		obj = g_ptr_array_index (array, i);
		ret = cmsMLUsetWide (mlu,
				     obj->language_code != NULL ? obj->language_code : cmsNoLanguage,
				     obj->country_code != NULL ? obj->country_code : cmsNoCountry,
				     obj->wtext);
		if (!ret) {
			g_set_error_literal (error,
					     CD_ICC_ERROR,
					     CD_ICC_ERROR_FAILED_TO_SAVE,
					     "cannot write MLU text");
			goto out;
		}
	}

	/* write tag */
	ret = cmsWriteTag (priv->lcms_profile, sig, mlu);
	if (!ret) {
		g_set_error (error,
			     CD_ICC_ERROR,
			     CD_ICC_ERROR_FAILED_TO_SAVE,
			     "cannot write tag: 0x%x",
			     sig);
		goto out;
	}

	/* remove apple-specific cmsSigProfileDescriptionTagML */
	if (sig == cmsSigProfileDescriptionTag) {
		cmsWriteTag (priv->lcms_profile,
			     0x6473636d,
			     NULL);
	}
out:
	g_list_free (keys);
	if (mlu != NULL)
		cmsMLUfree (mlu);
	return ret;
}

/**
 * cd_icc_save_file:
 * @icc: a #CdIcc instance.
 * @file: a #GFile
 * @flags: a set of #CdIccSaveFlags
 * @cancellable: A #GCancellable or %NULL
 * @error: A #GError or %NULL
 *
 * Saves an ICC profile to a local or remote file.
 *
 * Return vale: %TRUE for success.
 *
 * Since: 0.1.32
 **/
gboolean
cd_icc_save_file (CdIcc *icc,
		  GFile *file,
		  CdIccSaveFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	CdIccPrivate *priv = icc->priv;
	cmsHANDLE dict = NULL;
	const gchar *key;
	const gchar *value;
	gboolean ret = FALSE;
	gchar *data = NULL;
	GError *error_local = NULL;
	GList *l;
	GList *md_keys = NULL;
	gsize length;
	guint i;

	g_return_val_if_fail (CD_IS_ICC (icc), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	/* convert profile kind */
	for (i = 0; map_profile_kind[i].colord != CD_PROFILE_KIND_LAST; i++) {
		if (map_profile_kind[i].colord == priv->kind) {
			cmsSetDeviceClass (priv->lcms_profile,
					   map_profile_kind[i].lcms);
			break;
		}
	}

	/* convert colorspace */
	for (i = 0; map_colorspace[i].colord != CD_COLORSPACE_LAST; i++) {
		if (map_colorspace[i].colord == priv->colorspace) {
			cmsSetColorSpace (priv->lcms_profile,
					  map_colorspace[i].lcms);
			break;
		}
	}

	/* set version */
	if (priv->version > 0.0)
		cmsSetProfileVersion (priv->lcms_profile, priv->version);

	/* save metadata */
	if (g_hash_table_size (priv->metadata) != 0) {
		dict = cmsDictAlloc (NULL);
		md_keys = g_hash_table_get_keys (priv->metadata);
		if (md_keys != NULL) {
			for (l = md_keys; l != NULL; l = l->next) {
				key = l->data;
				value = g_hash_table_lookup (priv->metadata, key);
				_cmsDictAddEntryAscii (dict, key, value);
			}
		}
		ret = cmsWriteTag (priv->lcms_profile, cmsSigMetaTag, dict);
		if (!ret) {
			g_set_error_literal (error,
					     CD_ICC_ERROR,
					     CD_ICC_ERROR_FAILED_TO_SAVE,
					     "cannot write metadata");
			goto out;
		}
	} else {
		cmsWriteTag (priv->lcms_profile, cmsSigMetaTag, NULL);
	}

	/* save translations */
	ret = cd_util_write_tag_localized (icc,
					   cmsSigProfileDescriptionTag,
					   priv->mluc_data[CD_MLUC_DESCRIPTION],
					   error);
	if (!ret)
		goto out;
	ret = cd_util_write_tag_localized (icc,
					   cmsSigCopyrightTag,
					   priv->mluc_data[CD_MLUC_COPYRIGHT],
					   error);
	if (!ret)
		goto out;
	ret = cd_util_write_tag_localized (icc,
					   cmsSigDeviceMfgDescTag,
					   priv->mluc_data[CD_MLUC_MANUFACTURER],
					   error);
	if (!ret)
		goto out;
	ret = cd_util_write_tag_localized (icc,
					   cmsSigDeviceModelDescTag,
					   priv->mluc_data[CD_MLUC_MODEL],
					   error);
	if (!ret)
		goto out;

	/* write profile id */
	ret = cmsMD5computeID (priv->lcms_profile);
	if (!ret) {
		g_set_error_literal (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_FAILED_TO_SAVE,
				     "failed to compute profile id");
		goto out;
	}

	/* get size of profile */
	ret = cmsSaveProfileToMem (priv->lcms_profile,
				   NULL,
				   (guint32 *) &length);
	if (!ret) {
		g_set_error_literal (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_FAILED_TO_SAVE,
				     "failed to dump ICC file");
		goto out;
	}

	/* allocate and get profile data */
	data = g_new0 (gchar, length);
	ret = cmsSaveProfileToMem (priv->lcms_profile,
				   data,
				   (guint32 *) &length);
	if (!ret) {
		g_set_error_literal (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_FAILED_TO_SAVE,
				     "failed to dump ICC file to memory");
		goto out;
	}

	/* actually write file */
	ret = g_file_replace_contents (file,
				       data,
				       length,
				       NULL,
				       FALSE,
				       G_FILE_CREATE_NONE,
				       NULL,
				       cancellable,
				       &error_local);
	if (!ret) {
		g_set_error (error,
			     CD_ICC_ERROR,
			     CD_ICC_ERROR_FAILED_TO_SAVE,
			     "failed to dump ICC file: %s",
			     error_local->message);
		g_error_free (error_local);
		goto out;
	}
out:
	g_list_free (md_keys);
	if (dict != NULL)
		cmsDictFree (dict);
	g_free (data);
	return ret;
}

/**
 * cd_icc_load_file:
 * @icc: a #CdIcc instance.
 * @file: a #GFile
 * @flags: a set of #CdIccLoadFlags
 * @cancellable: A #GCancellable or %NULL
 * @error: A #GError or %NULL
 *
 * Loads an ICC profile from a local or remote file.
 *
 * Since: 0.1.32
 **/
gboolean
cd_icc_load_file (CdIcc *icc,
		  GFile *file,
		  CdIccLoadFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	CdIccPrivate *priv = icc->priv;
	gboolean ret = FALSE;
	gchar *data = NULL;
	GError *error_local = NULL;
	GFileInfo *info = NULL;
	gsize length;

	g_return_val_if_fail (CD_IS_ICC (icc), FALSE);
	g_return_val_if_fail (G_IS_FILE (file), FALSE);

	/* load files */
	ret = g_file_load_contents (file, cancellable, &data, &length,
				    NULL, &error_local);
	if (!ret) {
		g_set_error (error,
			     CD_ICC_ERROR,
			     CD_ICC_ERROR_FAILED_TO_OPEN,
			     "failed to load file: %s",
			     error_local->message);
		g_error_free (error_local);
		goto out;
	}

	/* parse the data */
	ret = cd_icc_load_data (icc,
				(const guint8 *) data,
				length,
				flags,
				error);
	if (!ret)
		goto out;

	/* find out if the user could delete this profile */
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE,
				  G_FILE_QUERY_INFO_NONE,
				  cancellable,
				  &error_local);
	if (info == NULL) {
		g_set_error (error,
			     CD_ICC_ERROR,
			     CD_ICC_ERROR_FAILED_TO_OPEN,
			     "failed to query file: %s",
			     error_local->message);
		g_error_free (error_local);
		goto out;
	}
	priv->can_delete = g_file_info_get_attribute_boolean (info,
							      G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE);

	/* save filename for later */
	priv->filename = g_file_get_path (file);
out:
	if (info != NULL)
		g_object_unref (info);
	g_free (data);
	return ret;
}

/**
 * cd_icc_load_fd:
 * @icc: a #CdIcc instance.
 * @fd: a file descriptor
 * @flags: a set of #CdIccLoadFlags
 * @error: A #GError or %NULL
 *
 * Loads an ICC profile from an open file descriptor.
 *
 * Since: 0.1.32
 **/
gboolean
cd_icc_load_fd (CdIcc *icc,
		gint fd,
		CdIccLoadFlags flags,
		GError **error)
{
	CdIccPrivate *priv = icc->priv;
	FILE *stream = NULL;
	gboolean ret = TRUE;

	g_return_val_if_fail (CD_IS_ICC (icc), FALSE);
	g_return_val_if_fail (fd > 0, FALSE);

	/* convert the file descriptor to a stream */
	stream = fdopen (fd, "r");
	if (stream == NULL) {
		ret = FALSE;
		g_set_error (error,
			     CD_ICC_ERROR,
			     CD_ICC_ERROR_FAILED_TO_OPEN,
			     "failed to open stream from fd %i",
			     fd);
		goto out;
	}

	/* parse the ICC file */
	priv->lcms_profile = cmsOpenProfileFromStream (stream, "r");
	if (priv->lcms_profile == NULL) {
		ret = FALSE;
		g_set_error_literal (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_FAILED_TO_OPEN,
				     "failed to open stream");
		goto out;
	}

	/* load cached data */
	cd_icc_load (icc, flags);
out:
	return ret;
}

/**
 * cd_icc_get_handle:
 * @icc: a #CdIcc instance.
 *
 * Return the cmsHPROFILE instance used locally. This may be required if you
 * are using the profile in a transform.
 *
 * Return value: (transfer none): Do not call cmsCloseProfile() on this value!
 **/
gpointer
cd_icc_get_handle (CdIcc *icc)
{
	g_return_val_if_fail (CD_IS_ICC (icc), NULL);
	return icc->priv->lcms_profile;
}

/**
 * cd_icc_set_handle:
 * @icc: a #CdIcc instance.
 * @handle: a cmsHPROFILE instance
 *
 * Set the internal cmsHPROFILE instance. This may be required if you create
 * the profile using cmsCreateRGBProfile() and then want to use the
 * functionality in #CdIcc.
 *
 * Do not call cmsCloseProfile() on @handle in the caller, this will be done
 * when the @icc object is finalized. Treat the profile like it's been adopted
 * by this module.
 *
 * Additionally, this function cannot be called more than once, and also can't
 * be called if cd_icc_load_file() has previously been used on the @icc object.
 **/
void
cd_icc_set_handle (CdIcc *icc, gpointer handle)
{
	g_return_if_fail (CD_IS_ICC (icc));
	g_return_if_fail (handle != NULL);
	g_return_if_fail (icc->priv->lcms_profile == NULL);
	icc->priv->lcms_profile = handle;
}

/**
 * cd_icc_get_size:
 *
 * Gets the ICC profile file size
 *
 * Return value: The size in bytes, or 0 for unknown.
 *
 * Since: 0.1.32
 **/
guint32
cd_icc_get_size (CdIcc *icc)
{
	g_return_val_if_fail (CD_IS_ICC (icc), 0);
	return icc->priv->size;
}

/**
 * cd_icc_get_filename:
 * @icc: A valid #CdIcc
 *
 * Gets the filename of the ICC data, if one exists.
 *
 * Return value: A filename, or %NULL
 *
 * Since: 0.1.32
 **/
const gchar *
cd_icc_get_filename (CdIcc *icc)
{
	CdIccPrivate *priv = icc->priv;
	g_return_val_if_fail (CD_IS_ICC (icc), NULL);
	return priv->filename;
}

/**
 * cd_icc_get_version:
 * @icc: a #CdIcc instance.
 *
 * Gets the ICC profile version, typically 2.1 or 4.2
 *
 * Return value: A floating point version number, or 0.0 for unknown
 *
 * Since: 0.1.32
 **/
gdouble
cd_icc_get_version (CdIcc *icc)
{
	g_return_val_if_fail (CD_IS_ICC (icc), 0.0f);
	return icc->priv->version;
}

/**
 * cd_icc_set_version:
 * @icc: a #CdIcc instance.
 * @version: the profile version, e.g. 2.1 or 4.0
 *
 * Sets the profile version.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_version (CdIcc *icc, gdouble version)
{
	g_return_if_fail (CD_IS_ICC (icc));
	icc->priv->version = version;
	g_object_notify (G_OBJECT (icc), "version");
}

/**
 * cd_icc_get_kind:
 * @icc: a #CdIcc instance.
 *
 * Gets the profile kind.
 *
 * Return value: The kind, e.g. %CD_PROFILE_KIND_INPUT
 *
 * Since: 0.1.32
 **/
CdProfileKind
cd_icc_get_kind (CdIcc *icc)
{
	g_return_val_if_fail (CD_IS_ICC (icc), CD_PROFILE_KIND_UNKNOWN);
	return icc->priv->kind;
}

/**
 * cd_icc_set_kind:
 * @icc: a #CdIcc instance.
 * @kind: the profile kind, e.g. %CD_PROFILE_KIND_DISPLAY_DEVICE
 *
 * Sets the profile kind.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_kind (CdIcc *icc, CdProfileKind kind)
{
	g_return_if_fail (CD_IS_ICC (icc));
	icc->priv->kind = kind;
	g_object_notify (G_OBJECT (icc), "kind");
}

/**
 * cd_icc_get_colorspace:
 * @icc: a #CdIcc instance.
 *
 * Gets the profile colorspace
 *
 * Return value: The profile colorspace, e.g. %CD_COLORSPACE_RGB
 *
 * Since: 0.1.32
 **/
CdColorspace
cd_icc_get_colorspace (CdIcc *icc)
{
	g_return_val_if_fail (CD_IS_ICC (icc), CD_COLORSPACE_UNKNOWN);
	return icc->priv->colorspace;
}

/**
 * cd_icc_set_colorspace:
 * @icc: a #CdIcc instance.
 * @colorspace: the profile colorspace, e.g. %CD_COLORSPACE_RGB
 *
 * Sets the colorspace kind.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_colorspace (CdIcc *icc, CdColorspace colorspace)
{
	g_return_if_fail (CD_IS_ICC (icc));
	icc->priv->colorspace = colorspace;
	g_object_notify (G_OBJECT (icc), "colorspace");
}

/**
 * cd_icc_get_metadata:
 * @icc: A valid #CdIcc
 *
 * Gets all the metadata from the ICC profile.
 *
 * Return value: (transfer container): The profile metadata
 *
 * Since: 0.1.32
 **/
GHashTable *
cd_icc_get_metadata (CdIcc *icc)
{
	g_return_val_if_fail (CD_IS_ICC (icc), NULL);
	return g_hash_table_ref (icc->priv->metadata);
}

/**
 * cd_icc_get_metadata_item:
 * @icc: A valid #CdIcc
 * @key: the dictionary key
 *
 * Gets an item of data from the ICC metadata store.
 *
 * Return value: The dictionary data, or %NULL if the key does not exist.
 *
 * Since: 0.1.32
 **/
const gchar *
cd_icc_get_metadata_item (CdIcc *icc, const gchar *key)
{
	g_return_val_if_fail (CD_IS_ICC (icc), NULL);
	g_return_val_if_fail (key != NULL, NULL);
	return (const gchar *) g_hash_table_lookup (icc->priv->metadata, key);
}

/**
 * cd_icc_add_metadata:
 * @icc: A valid #CdIcc
 * @key: the metadata key
 * @value: the metadata value
 *
 * Sets an item of data to the profile metadata, overwriting it if
 * it already exists.
 *
 * Since: 0.1.32
 **/
void
cd_icc_add_metadata (CdIcc *icc, const gchar *key, const gchar *value)
{
	g_return_if_fail (CD_IS_ICC (icc));
	g_return_if_fail (key != NULL);
	g_return_if_fail (value != NULL);
	g_hash_table_insert (icc->priv->metadata,
			     g_strdup (key),
			     g_strdup (value));
}

/**
 * cd_icc_remove_metadata:
 * @icc: A valid #CdIcc
 * @key: the metadata key
 *
 * Removes an item of metadata.
 *
 * Since: 0.1.32
 **/
void
cd_icc_remove_metadata (CdIcc *icc, const gchar *key)
{
	g_return_if_fail (CD_IS_ICC (icc));
	g_return_if_fail (key != NULL);
	g_hash_table_remove (icc->priv->metadata, key);
}

/**
 * cd_icc_load_named_colors:
 **/
static void
cd_icc_load_named_colors (CdIcc *icc)
{
	CdColorLab lab;
	CdColorSwatch *swatch;
	CdIccPrivate *priv = icc->priv;
	cmsNAMEDCOLORLIST *nc2;
	cmsUInt16Number pcs[3];
	gboolean ret;
	gchar name[cmsMAX_PATH];
	gchar prefix[33];
	gchar suffix[33];
	GString *string;
	guint j;
	guint size;

	/* do any named colors exist? */
	nc2 = cmsReadTag (priv->lcms_profile, cmsSigNamedColor2Type);
	if (nc2 == NULL)
		goto out;

	/* get each NC */
	size = cmsNamedColorCount (nc2);
	for (j = 0; j < size; j++) {

		/* parse title */
		ret = cmsNamedColorInfo (nc2, j,
					 name,
					 prefix,
					 suffix,
					 (cmsUInt16Number *) &pcs,
					 NULL);
		if (!ret)
			continue;
		string = g_string_new ("");
		if (prefix[0] != '\0')
			g_string_append_printf (string, "%s ", prefix);
		g_string_append (string, name);
		if (suffix[0] != '\0')
			g_string_append_printf (string, " %s", suffix);

		/* check is valid */
		ret = g_utf8_validate (string->str, string->len, NULL);
		if (!ret)
			ret = cd_icc_fix_utf8_string (string);

		/* save color if valid */
		if (ret) {
			cmsLabEncoded2Float ((cmsCIELab *) &lab, pcs);
			swatch = cd_color_swatch_new ();
			cd_color_swatch_set_name (swatch, string->str);
			cd_color_swatch_set_value (swatch, (const CdColorLab *) &lab);
			g_ptr_array_add (icc->priv->named_colors, swatch);
		}
		g_string_free (string, TRUE);
	}
out:
	return;
}

/**
 * cd_icc_get_named_colors:
 * @icc: a #CdIcc instance.
 *
 * Gets any named colors in the profile.
 * This function will only return results if the profile was loaded with the
 * %CD_ICC_LOAD_FLAGS_NAMED_COLORS flag.
 *
 * Return value: (transfer container): An array of #CdColorSwatch
 *
 * Since: 0.1.32
 **/
GPtrArray *
cd_icc_get_named_colors (CdIcc *icc)
{
	g_return_val_if_fail (CD_IS_ICC (icc), NULL);
	return g_ptr_array_ref (icc->priv->named_colors);
}

/**
 * cd_icc_get_can_delete:
 * @icc: a #CdIcc instance.
 *
 * Finds out if the profile could be deleted.
 * This is only applicable for profiles loaded with cd_icc_load_file() as
 * obviously data and fd's cannot be sanely unlinked.
 *
 * Return value: %TRUE if g_file_delete() would likely work
 *
 * Since: 0.1.32
 **/
gboolean
cd_icc_get_can_delete (CdIcc *icc)
{
	g_return_val_if_fail (CD_IS_ICC (icc), FALSE);
	return icc->priv->can_delete;
}

/**
 * cd_icc_get_created:
 * @icc: A valid #CdIcc
 *
 * Gets the ICC creation date and time.
 *
 * Return value: A #GDateTime object, or %NULL for not set
 *
 * Since: 0.1.32
 **/
GDateTime *
cd_icc_get_created (CdIcc *icc)
{
	CdIccPrivate *priv = icc->priv;
	gboolean ret;
	GDateTime *created = NULL;
	struct tm created_tm;
	time_t created_t;

	g_return_val_if_fail (CD_IS_ICC (icc), NULL);

	/* get the profile creation time and date */
	ret = cmsGetHeaderCreationDateTime (priv->lcms_profile, &created_tm);
	if (!ret)
		goto out;

	/* convert to UNIX time */
	created_t = mktime (&created_tm);
	if (created_t == (time_t) -1)
		goto out;

	/* instantiate object */
	created = g_date_time_new_from_unix_utc (created_t);
out:
	return created;
}

/**
 * cd_icc_get_checksum:
 * @icc: A valid #CdIcc
 *
 * Gets the profile checksum if one exists.
 * This will either be the embedded profile ID, or the file checksum if
 * the #CdIcc object was loaded using cd_icc_load_data() or cd_icc_load_file()
 * and the %CD_ICC_LOAD_FLAGS_FALLBACK_MD5 flag is used.
 *
 * Return value: An embedded MD5 checksum, or %NULL for not set
 *
 * Since: 0.1.32
 **/
const gchar *
cd_icc_get_checksum (CdIcc *icc)
{
	g_return_val_if_fail (CD_IS_ICC (icc), NULL);
	return icc->priv->checksum;
}

/**
 * cd_icc_get_locale_key:
 **/
static gchar *
cd_icc_get_locale_key (const gchar *locale)
{
	gchar *locale_key;

	/* en_US is the default locale in an ICC profile */
	if (locale == NULL || g_str_has_prefix (locale, "en_US")) {
		locale_key = g_strdup ("");
		goto out;
	}
	locale_key = g_strdup (locale);
	g_strdelimit (locale_key, ".(", '\0');
out:
	return locale_key;
}

/**
 * cd_icc_get_mluc_data:
 **/
static const gchar *
cd_icc_get_mluc_data (CdIcc *icc,
		      const gchar *locale,
		      CdIccMluc mluc,
		      cmsTagSignature *sigs,
		      GError **error)
{
	CdIccPrivate *priv = icc->priv;
	cmsMLU *mlu = NULL;
	const gchar *country_code = "\0\0\0";
	const gchar *language_code = "\0\0\0";
	const gchar *value;
	gchar *locale_key = NULL;
	gchar text_buffer[128];
	gchar *tmp;
	gsize rc;
	guint32 text_size;
	guint i;
	wchar_t wtext[128];

	g_return_val_if_fail (CD_IS_ICC (icc), NULL);

	/* does cache entry exist already? */
	locale_key = cd_icc_get_locale_key (locale);
	value = g_hash_table_lookup (priv->mluc_data[mluc], locale_key);
	if (value != NULL)
		goto out;

	/* convert the locale into something we can use as a key, in this case
	 * 'en_GB.UTF-8' -> 'en_GB'
	 * 'fr'          -> 'fr' */
	if (locale_key[0] != '\0') {

		/* decompose it into language and country codes */
		tmp = g_strstr_len (locale_key, -1, "_");
		language_code = locale_key;
		if (tmp != NULL) {
			country_code = tmp + 1;
			*tmp = '\0';
		}

		/* check the format is correct */
		if (strlen (language_code) != 2) {
			g_set_error (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_INVALID_LOCALE,
				     "invalid locale: %s", locale);
			goto out;
		}
		if (country_code != NULL &&
		    country_code[0] != '\0' &&
		    strlen (country_code) != 2) {
			g_set_error (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_INVALID_LOCALE,
				     "invalid locale: %s", locale);
			goto out;
		}
	}

	/* read each MLU entry in order of preference */
	for (i = 0; sigs[i] != 0; i++) {
		mlu = cmsReadTag (priv->lcms_profile, sigs[i]);
		if (mlu != NULL)
			break;
	}
	if (mlu == NULL) {
		g_set_error_literal (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_NO_DATA,
				     "cmsSigProfile*Tag mising");
		goto out;
	}
	text_size = cmsMLUgetWide (mlu,
				   language_code,
				   country_code,
				   wtext,
				   sizeof (wtext));
	if (text_size == 0)
		goto out;
	rc = wcstombs (text_buffer,
		       wtext,
		       sizeof (text_buffer));
	if (rc == (gsize) -1) {
		g_set_error_literal (error,
				     CD_ICC_ERROR,
				     CD_ICC_ERROR_NO_DATA,
				     "invalid UTF-8");
		goto out;
	}

	/* insert into locale cache */
	tmp = g_strdup (text_buffer);
	g_hash_table_insert (priv->mluc_data[mluc],
			     g_strdup (locale_key),
			     tmp);
	value = tmp;
out:
	g_free (locale_key);
	return value;
}

/**
 * cd_icc_get_description:
 * @icc: A valid #CdIcc
 * @locale: A locale, e.g. "en_GB.UTF-8" or %NULL for the profile default
 * @error: A #GError or %NULL
 *
 * Gets the profile description.
 * If the translated text is not available in the selected locale then the
 * default untranslated (en_US) text is returned.
 *
 * Return value: The text as a UTF-8 string, or %NULL of the locale is invalid
 *               or the tag does not exist.
 *
 * Since: 0.1.32
 **/
const gchar *
cd_icc_get_description (CdIcc *icc, const gchar *locale, GError **error)
{
	cmsTagSignature sigs[] = { 0x6473636d, /* 'dscm' */
				   cmsSigProfileDescriptionTag,
				   0 };
	return cd_icc_get_mluc_data (icc,
				     locale,
				     CD_MLUC_DESCRIPTION,
				     sigs,
				     error);
}

/**
 * cd_icc_get_copyright:
 * @icc: A valid #CdIcc
 * @locale: A locale, e.g. "en_GB.UTF-8" or %NULL for the profile default
 * @error: A #GError or %NULL
 *
 * Gets the profile copyright.
 * If the translated text is not available in the selected locale then the
 * default untranslated (en_US) text is returned.
 *
 * Return value: The text as a UTF-8 string, or %NULL of the locale is invalid
 *               or the tag does not exist.
 *
 * Since: 0.1.32
 **/
const gchar *
cd_icc_get_copyright (CdIcc *icc, const gchar *locale, GError **error)
{
	cmsTagSignature sigs[] = { cmsSigCopyrightTag, 0 };
	return cd_icc_get_mluc_data (icc,
				     locale,
				     CD_MLUC_COPYRIGHT,
				     sigs,
				     error);
}

/**
 * cd_icc_get_manufacturer:
 * @icc: A valid #CdIcc
 * @locale: A locale, e.g. "en_GB.UTF-8" or %NULL for the profile default
 * @error: A #GError or %NULL
 *
 * Gets the profile manufacturer.
 * If the translated text is not available in the selected locale then the
 * default untranslated (en_US) text is returned.
 *
 * Return value: The text as a UTF-8 string, or %NULL of the locale is invalid
 *               or the tag does not exist.
 *
 * Since: 0.1.32
 **/
const gchar *
cd_icc_get_manufacturer (CdIcc *icc, const gchar *locale, GError **error)
{
	cmsTagSignature sigs[] = { cmsSigDeviceMfgDescTag, 0 };
	return cd_icc_get_mluc_data (icc,
				     locale,
				     CD_MLUC_MANUFACTURER,
				     sigs,
				     error);
}

/**
 * cd_icc_get_model:
 * @icc: A valid #CdIcc
 * @locale: A locale, e.g. "en_GB.UTF-8" or %NULL for the profile default
 * @error: A #GError or %NULL
 *
 * Gets the profile model.
 * If the translated text is not available in the selected locale then the
 * default untranslated (en_US) text is returned.
 *
 * Return value: The text as a UTF-8 string, or %NULL of the locale is invalid
 *               or the tag does not exist.
 *
 * Since: 0.1.32
 **/
const gchar *
cd_icc_get_model (CdIcc *icc, const gchar *locale, GError **error)
{
	cmsTagSignature sigs[] = { cmsSigDeviceModelDescTag, 0 };
	return cd_icc_get_mluc_data (icc,
				     locale,
				     CD_MLUC_MODEL,
				     sigs,
				     error);
}

/**
 * cd_icc_set_description:
 * @icc: A valid #CdIcc
 * @locale: A locale, e.g. "en_GB.UTF-8" or %NULL for the profile default
 * @value: New string value
 *
 * Sets the profile description for a specific locale.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_description (CdIcc *icc, const gchar *locale, const gchar *value)
{
	CdIccPrivate *priv = icc->priv;
	g_hash_table_insert (priv->mluc_data[CD_MLUC_DESCRIPTION],
			     cd_icc_get_locale_key (locale),
			     g_strdup (value));
}

/**
 * cd_icc_set_description_items:
 * @icc: A valid #CdIcc
 * @values: New translated values, with the key being the locale.
 *
 * Sets the profile descriptions for specific locales.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_description_items (CdIcc *icc, GHashTable *values)
{
	const gchar *key;
	const gchar *value;
	GList *keys;
	GList *l;

	g_return_if_fail (CD_IS_ICC (icc));

	/* add each translation */
	keys = g_hash_table_get_keys (values);
	for (l = keys; l != NULL; l = l->next) {
		key = l->data;
		value = g_hash_table_lookup (values, key);
		cd_icc_set_description (icc, key, value);
	}
	g_list_free (keys);
}

/**
 * cd_icc_set_copyright:
 * @icc: A valid #CdIcc
 * @locale: A locale, e.g. "en_GB.UTF-8" or %NULL for the profile default
 * @value: New string value
 *
 * Sets the profile _copyright for a specific locale.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_copyright (CdIcc *icc, const gchar *locale, const gchar *value)
{
	CdIccPrivate *priv = icc->priv;
	g_hash_table_insert (priv->mluc_data[CD_MLUC_COPYRIGHT],
			     cd_icc_get_locale_key (locale),
			     g_strdup (value));
}

/**
 * cd_icc_set_copyright_items:
 * @icc: A valid #CdIcc
 * @values: New translated values, with the key being the locale.
 *
 * Sets the profile copyrights for specific locales.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_copyright_items (CdIcc *icc, GHashTable *values)
{
	const gchar *key;
	const gchar *value;
	GList *keys;
	GList *l;

	g_return_if_fail (CD_IS_ICC (icc));

	/* add each translation */
	keys = g_hash_table_get_keys (values);
	for (l = keys; l != NULL; l = l->next) {
		key = l->data;
		value = g_hash_table_lookup (values, key);
		cd_icc_set_copyright (icc, key, value);
	}
	g_list_free (keys);
}

/**
 * cd_icc_set_manufacturer:
 * @icc: A valid #CdIcc
 * @locale: A locale, e.g. "en_GB.UTF-8" or %NULL for the profile default
 * @value: New string value
 *
 * Sets the profile manufacturer for a specific locale.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_manufacturer (CdIcc *icc, const gchar *locale, const gchar *value)
{
	CdIccPrivate *priv = icc->priv;
	g_hash_table_insert (priv->mluc_data[CD_MLUC_MANUFACTURER],
			     cd_icc_get_locale_key (locale),
			     g_strdup (value));
}

/**
 * cd_icc_set_manufacturer_items:
 * @icc: A valid #CdIcc
 * @values: New translated values, with the key being the locale.
 *
 * Sets the profile manufacturers for specific locales.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_manufacturer_items (CdIcc *icc, GHashTable *values)
{
	const gchar *key;
	const gchar *value;
	GList *keys;
	GList *l;

	g_return_if_fail (CD_IS_ICC (icc));

	/* add each translation */
	keys = g_hash_table_get_keys (values);
	for (l = keys; l != NULL; l = l->next) {
		key = l->data;
		value = g_hash_table_lookup (values, key);
		cd_icc_set_manufacturer (icc, key, value);
	}
	g_list_free (keys);
}

/**
 * cd_icc_set_model:
 * @icc: A valid #CdIcc
 * @locale: A locale, e.g. "en_GB.UTF-8" or %NULL for the profile default
 * @value: New string value
 *
 * Sets the profile model for a specific locale.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_model (CdIcc *icc, const gchar *locale, const gchar *value)
{
	CdIccPrivate *priv = icc->priv;
	g_hash_table_insert (priv->mluc_data[CD_MLUC_MODEL],
			     cd_icc_get_locale_key (locale),
			     g_strdup (value));
}

/**
 * cd_icc_set_model_items:
 * @icc: A valid #CdIcc
 * @values: New translated values, with the key being the locale.
 *
 * Sets the profile models for specific locales.
 *
 * Since: 0.1.32
 **/
void
cd_icc_set_model_items (CdIcc *icc, GHashTable *values)
{
	const gchar *key;
	const gchar *value;
	GList *keys;
	GList *l;

	g_return_if_fail (CD_IS_ICC (icc));

	/* add each translation */
	keys = g_hash_table_get_keys (values);
	for (l = keys; l != NULL; l = l->next) {
		key = l->data;
		value = g_hash_table_lookup (values, key);
		cd_icc_set_model (icc, key, value);
	}
	g_list_free (keys);
}

/**
 * cd_icc_get_property:
 **/
static void
cd_icc_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	CdIcc *icc = CD_ICC (object);
	CdIccPrivate *priv = icc->priv;

	switch (prop_id) {
	case PROP_SIZE:
		g_value_set_uint (value, priv->size);
		break;
	case PROP_FILENAME:
		g_value_set_string (value, priv->filename);
		break;
	case PROP_VERSION:
		g_value_set_double (value, priv->version);
		break;
	case PROP_KIND:
		g_value_set_uint (value, priv->kind);
		break;
	case PROP_COLORSPACE:
		g_value_set_uint (value, priv->colorspace);
		break;
	case PROP_CAN_DELETE:
		g_value_set_boolean (value, priv->can_delete);
		break;
	case PROP_CHECKSUM:
		g_value_set_string (value, priv->checksum);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * cd_icc_set_property:
 **/
static void
cd_icc_set_property (GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	CdIcc *icc = CD_ICC (object);
	switch (prop_id) {
	case PROP_KIND:
		cd_icc_set_kind (icc, g_value_get_uint (value));
		break;
	case PROP_COLORSPACE:
		cd_icc_set_colorspace (icc, g_value_get_uint (value));
		break;
	case PROP_VERSION:
		cd_icc_set_version (icc, g_value_get_double (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * cd_icc_class_init:
 */
static void
cd_icc_class_init (CdIccClass *klass)
{
	GParamSpec *pspec;
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = cd_icc_finalize;
	object_class->get_property = cd_icc_get_property;
	object_class->set_property = cd_icc_set_property;

	/**
	 * CdIcc:size:
	 */
	pspec = g_param_spec_uint ("size", NULL, NULL,
				   0, G_MAXUINT, 0,
				   G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_SIZE, pspec);

	/**
	 * CdIcc:filename:
	 */
	pspec = g_param_spec_string ("filename", NULL, NULL,
				     NULL,
				     G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_FILENAME, pspec);

	/**
	 * CdIcc:version:
	 */
	pspec = g_param_spec_double ("version", NULL, NULL,
				     0, G_MAXFLOAT, 0,
				     G_PARAM_READWRITE);
	g_object_class_install_property (object_class, PROP_VERSION, pspec);

	/**
	 * CdIcc:kind:
	 */
	pspec = g_param_spec_uint ("kind", NULL, NULL,
				   0, G_MAXUINT, 0,
				   G_PARAM_READWRITE);
	g_object_class_install_property (object_class, PROP_KIND, pspec);

	/**
	 * CdIcc:colorspace:
	 */
	pspec = g_param_spec_uint ("colorspace", NULL, NULL,
				   0, G_MAXUINT, 0,
				   G_PARAM_READWRITE);
	g_object_class_install_property (object_class, PROP_COLORSPACE, pspec);

	/**
	 * CdIcc:can-delete:
	 */
	pspec = g_param_spec_boolean ("can-delete", NULL, NULL,
				      FALSE,
				      G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_CAN_DELETE, pspec);

	/**
	 * CdIcc:checksum:
	 */
	pspec = g_param_spec_string ("checksum", NULL, NULL,
				     NULL,
				     G_PARAM_READABLE);
	g_object_class_install_property (object_class, PROP_CHECKSUM, pspec);

	g_type_class_add_private (klass, sizeof (CdIccPrivate));
}

/**
 * cd_icc_init:
 */
static void
cd_icc_init (CdIcc *icc)
{
	guint i;

	icc->priv = CD_ICC_GET_PRIVATE (icc);
	icc->priv->kind = CD_PROFILE_KIND_UNKNOWN;
	icc->priv->colorspace = CD_COLORSPACE_UNKNOWN;
	icc->priv->named_colors = g_ptr_array_new_with_free_func ((GDestroyNotify) cd_color_swatch_free);
	icc->priv->metadata = g_hash_table_new_full (g_str_hash,
						     g_str_equal,
						     g_free,
						     g_free);
	for (i = 0; i < CD_MLUC_LAST; i++) {
		icc->priv->mluc_data[i] = g_hash_table_new_full (g_str_hash,
								 g_str_equal,
								 g_free,
								 g_free);
	}
}

/**
 * cd_icc_finalize:
 */
static void
cd_icc_finalize (GObject *object)
{
	CdIcc *icc = CD_ICC (object);
	CdIccPrivate *priv = icc->priv;
	guint i;

	g_free (priv->filename);
	g_free (priv->checksum);
	g_ptr_array_unref (priv->named_colors);
	g_hash_table_destroy (priv->metadata);
	for (i = 0; i < CD_MLUC_LAST; i++)
		g_hash_table_destroy (priv->mluc_data[i]);
	if (priv->lcms_profile != NULL)
		cmsCloseProfile (priv->lcms_profile);

	G_OBJECT_CLASS (cd_icc_parent_class)->finalize (object);
}

/**
 * cd_icc_new:
 *
 * Creates a new #CdIcc object.
 *
 * Return value: a new CdIcc object.
 *
 * Since: 0.1.32
 **/
CdIcc *
cd_icc_new (void)
{
	CdIcc *icc;
	icc = g_object_new (CD_TYPE_ICC, NULL);
	return CD_ICC (icc);
}