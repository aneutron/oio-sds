/*
OpenIO SDS core library
Copyright (C) 2015-2016 OpenIO, as part of OpenIO Software Defined Storage

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 3.0 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library.
*/

#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "internals.h"
#include "oio_core.h"
#include "oio_sds.h"

#define MAYBERETURN(E,T) do { \
	struct oio_error_s *_e = (struct oio_error_s*)(E); \
	if (_e) { \
		g_printerr ("%s: (%d) %s\n", (T), oio_error_code(_e), oio_error_message(_e)); \
		return err; \
	} \
} while (0)

static const char random_chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz"
	"0123456789";

static const char hex_chars[] = "0123456789ABCDEF";

static int
_on_item (void *ctx, const struct oio_sds_list_item_s *item)
{
	(void) ctx;
	GRID_DEBUG ("Listed item %s, size %"G_GSIZE_FORMAT" version %"G_GSIZE_FORMAT"\n",
			item->name, item->size, item->version);
	return 0;
}

struct file_info_s
{
	guint8 h[32];
	gsize hs;
	gsize fs;
};

static GError *
_checksum_file (const char *path, struct file_info_s *fi)
{
	GError *err = NULL;
	gchar *file_content = NULL;
	g_file_get_contents (path, &file_content, &fi->fs, &err);
	if (err) return err;

	fi->hs = g_checksum_type_get_length (G_CHECKSUM_SHA256);

	GChecksum *checksum = g_checksum_new (G_CHECKSUM_SHA256);
	g_checksum_update (checksum, (guint8*)file_content, fi->fs);
	g_checksum_get_digest (checksum, fi->h, &fi->hs);
	g_checksum_free (checksum);
	g_free (file_content);

	return NULL;
}

static struct oio_error_s *
_roundtrip_common (struct oio_sds_s *client, struct oio_url_s *url,
		const char *path)
{
	gchar tmppath[256], content_id[17] = "", prop1 [7]="", prop2[37]="";
	gchar *properties [5] = {prop1, prop1, prop2, prop2, NULL};

	struct file_info_s fi, fi0;
	struct oio_error_s *err = NULL;
	int has = 0;

	err = (struct oio_error_s*) _checksum_file (path, &fi0);
	MAYBERETURN(err, "Checksum error (original): ");

	oio_str_randomize(prop1, sizeof(prop1), random_chars);
	oio_str_randomize(prop2, sizeof(prop2), random_chars);
	oio_str_randomize (content_id, sizeof(content_id), hex_chars);
	g_snprintf (tmppath, sizeof(tmppath),
			"/tmp/test-roundtrip-%d-%"G_GINT64_FORMAT"-",
			getpid(), oio_ext_real_time());
	oio_str_randomize (tmppath+strlen(tmppath), 17, random_chars);

	GRID_INFO ("Roundtrip on local(%s) distant(%s) content_id(%s)", tmppath,
			oio_url_get (url, OIOURL_WHOLE), content_id);

	/* Check the content is not present yet */
	err = oio_sds_has (client, url, &has);
	if (!err && has) err = (struct oio_error_s*) NEWERROR(0,"content already present");
	MAYBERETURN(err, "Check error");
	GRID_INFO("Content absent as expected");

	/* Upload the content */
	struct oio_sds_ul_dst_s ul_dst = OIO_SDS_UPLOAD_DST_INIT;
	ul_dst.url = url;
	ul_dst.autocreate = 1;
	ul_dst.append = 0;
	ul_dst.out_size = 0;
	ul_dst.content_id = content_id;
	ul_dst.properties = (char**) properties;

	err = oio_sds_upload_from_file (client, &ul_dst, path, 0, 0);
	MAYBERETURN(err, "Upload error");
	GRID_INFO("Content uploaded");

	/* Check it is now present */
	has = 0;
	err = oio_sds_has (client, url, &has);
	if (!err && !has) err = (struct oio_error_s*) NEWERROR(0, "content not found");
	MAYBERETURN(err, "Check error");
	GRID_INFO("Content present as expected");

	/* Get it to validate the content is accessible */
	err = oio_sds_download_to_file (client, url, tmppath);
	MAYBERETURN(err, "Download error");
	GRID_INFO("Content downloaded to a file");

	/* Validate the original and the copy match */
	err = (struct oio_error_s*) _checksum_file (tmppath, &fi);
	MAYBERETURN(err, "Checksum error (copy): ");
	if (fi.fs != fi0.fs)
		MAYBERETURN(NEWERROR(0, "Copy sizes mismatch (expected %zu, got %zu)",
					fi0.fs, fi.fs), "Validation error");
	if (0 != memcmp(fi.h, fi0.h, fi.hs))
		MAYBERETURN(NEWERROR(0, "Copy hash mismatch"), "Validation error");
	GRID_INFO("The original file and its copy match");

	/* Get it an other way in a buffer. */
	do {
		guint8 buf[1024];
		struct oio_sds_dl_dst_s dl_dst = {
			.type = OIO_DL_DST_BUFFER,
			.data = {.buffer = {.ptr = buf, .length=1024}}
		};
		struct oio_sds_dl_src_s dl_src = {
			.url = url,
			.ranges = NULL,
		};
		err = oio_sds_download (client, &dl_src, &dl_dst);
	} while (0);
	MAYBERETURN(err, "Download error");
	GRID_INFO("Content downloaded to a buffer");

	/* link the container */
	struct oio_url_s *url1 = oio_url_dup (url);
	oio_url_set (url1, OIOURL_PATH, tmppath);
	err = oio_sds_link (client, url1, content_id);
	MAYBERETURN(err, "Link error: ");

	/* List the container, the content must appear */
	struct oio_sds_list_param_s list_in = {
		.url = url,
		.prefix = NULL, .marker = NULL, .end = NULL, .delimiter = 0,
		.flag_allversions = 0, .flag_nodeleted = 0,
	};
	struct oio_sds_list_listener_s list_out = {
		.ctx = NULL,
		.on_item = _on_item, .on_prefix = NULL, .on_bound = NULL,
	};
	err = oio_sds_list (client, &list_in, &list_out);
	MAYBERETURN(err, "List error");

	/* list the properties on the content */
	GPtrArray *val = g_ptr_array_new();
	void save_elements(void *ptrarray, const char *k, const char*v) {
		GPtrArray *array = (GPtrArray *) ptrarray;
		g_ptr_array_add(array,g_strdup(k));
		g_ptr_array_add(array,g_strdup(v));
	}
	err = oio_sds_get_content_properties(client, url, save_elements, val);
	MAYBERETURN(err, "GetProperties error");
	if (val->len != 4)
		return (struct oio_error_s*) NEWERROR(0, "error of properties!");

	g_ptr_array_add(val, NULL);
	gchar **elts = (gchar **) g_ptr_array_free(val, FALSE);
	const gboolean t1 = g_strcmp0(elts [0], prop1) && g_strcmp0(elts [1], prop1);
	const gboolean t2 = g_strcmp0(elts [2], prop2) && g_strcmp0(elts [3], prop2);
	if (!t1 || !t2)
		return (struct oio_error_s*) NEWERROR(0, "error of properties!");
	g_strfreev(elts);

	/* get details on the content */
	gsize max_offset = 0;
	gsize max_size = 0;
	void _on_metachunk (void *i UNUSED, guint seq, gsize offt, gsize len) {
		GRID_INFO("metachunk: %u, %"G_GSIZE_FORMAT" %"G_GSIZE_FORMAT,
				seq, offt, len);
		max_offset = MAX(max_offset, offt);
		max_size = MAX(max_size, offt+len);
	}
	void _on_property (void *i UNUSED, const char *k, const char *v) {
		GRID_INFO("property: '%s' -> '%s'", k, v);
	}
	err = oio_sds_show_content (client, url, NULL, _on_metachunk, _on_property);
	MAYBERETURN(err, "Show error");

	/* if there is more than one metachunk, voluntarily ask a range crossing
	 * the borders of a metachunk */
	if (max_offset > 0 && max_size > 0 && max_offset != max_size) {
		GRID_DEBUG("max_offset=%"G_GSIZE_FORMAT" max_size=%"G_GSIZE_FORMAT,
				max_offset, max_size);
		guint8 buf[4];
		struct oio_sds_dl_range_s range0 = {
			.offset = max_offset - 1,
			.size = 2,
		};
		struct oio_sds_dl_range_s range1 = {
			.offset = max_offset + 1,
			.size = 2,
		};
		struct oio_sds_dl_range_s *rangev[3] = {NULL, NULL, NULL};
		struct oio_sds_dl_src_s dl_src = { .url = url, .ranges = rangev, };

		/* first attempt with a buffer voluntarily too small */
		rangev[0] = &range0;
		rangev[1] = NULL;
		struct oio_sds_dl_dst_s dl_dst = {
			.type = OIO_DL_DST_BUFFER,
			.data = {.buffer = {.ptr = buf, .length=1}}
		};
		err = oio_sds_download (client, &dl_src, &dl_dst);
		if (!err)
			return (struct oio_error_s*)SYSERR("Unexpected success: DL of a range in a buffer too small");
		g_clear_error ((GError**)&err);

		/* second attempt with a buffer exactly the expected size */
		rangev[0] = &range0;
		rangev[1] = NULL;
		dl_dst.data.buffer.length = 2;
		err = oio_sds_download (client, &dl_src, &dl_dst);
		MAYBERETURN(err, "Download error");
		GRID_INFO("Content downloaded to a buffer");

		/* now with 2 contiguous ranges */
		rangev[0] = &range0;
		rangev[1] = &range1;
		dl_dst.data.buffer.length = 4;
		err = oio_sds_download (client, &dl_src, &dl_dst);
		MAYBERETURN(err, "Download error");
		GRID_INFO("Content downloaded to a buffer");

		/* now with 2 non-contiguous ranges */
		rangev[0] = &range1;
		rangev[1] = &range0;
		dl_dst.data.buffer.length = 4;
		err = oio_sds_download (client, &dl_src, &dl_dst);
		MAYBERETURN(err, "Download error");
		GRID_INFO("Content downloaded to a buffer");

		/* try to download the end of a metachunk */
		rangev[0] = &range0;
		rangev[1] = NULL;
		dl_dst.data.buffer.length = 2;
		range0.offset = max_offset - 1;
		range0.size = 1;
		err = oio_sds_download (client, &dl_src, &dl_dst);
		MAYBERETURN(err, "Download error");
		GRID_INFO("Content downloaded to a buffer");

		/* try to download the start of a metachunk */
		dl_dst.data.buffer.length = 2;
		range0.offset = max_offset;
		range0.size = 1;
		err = oio_sds_download (client, &dl_src, &dl_dst);
		MAYBERETURN(err, "Download error");
		GRID_INFO("Content downloaded to a buffer");
	}

	/* Remove the content from the content */
	err = oio_sds_delete (client, url);
	MAYBERETURN(err, "Delete error");
	GRID_INFO("Content removed");

	/* Check the content is not present anymore */
	has = 0;
	err = oio_sds_has (client, url, &has);
	if (!err && has) err = (struct oio_error_s*) NEWERROR(0, "content still present");
	MAYBERETURN(err, "Check error");
	GRID_INFO("Content absent as expected");

	/* **** */

	GRID_INFO("--- Partial upload ---");
	oio_str_randomize (content_id, sizeof(content_id), hex_chars);
	err = oio_sds_upload_from_file (client, &ul_dst, path, 0, 0);
	MAYBERETURN(err, "Upload error");
	GRID_INFO("Content part 0 uploaded");

	ul_dst.offset = fi0.fs;
	ul_dst.meta_pos = 1;
	ul_dst.partial = TRUE;
	err = oio_sds_upload_from_file (client, &ul_dst, path, 0, 0);
	MAYBERETURN(err, "Upload error");
	GRID_INFO("Content part 1 uploaded");

	ul_dst.offset = 0;
	ul_dst.meta_pos = 0;
	ul_dst.partial = TRUE;
	err = oio_sds_upload_from_file (client, &ul_dst, path, 0, 0);
	MAYBERETURN(err, "Upload error");
	GRID_INFO("Content part 0 uploaded again");

	/* Check it is now present */
	has = 0;
	err = oio_sds_has (client, url, &has);
	if (!err && !has) err = (struct oio_error_s*) NEWERROR(0, "content not found");
	MAYBERETURN(err, "Check error");
	GRID_INFO("Content present as expected");

	/* Remove the content from the content */
	err = oio_sds_delete (client, url);
	MAYBERETURN(err, "Delete error");
	GRID_INFO("Content removed");

	/* Check the content is not preset anymore */
	has = 0;
	err = oio_sds_has (client, url, &has);
	if (!err && has) err = (struct oio_error_s*) NEWERROR(0, "content still present");
	MAYBERETURN(err, "Check error");
	GRID_INFO("Content absent as expected");

	/* TODO deleting twice SHOULD fail. */
	/* TODO setting properties on the content MUST fail */
	/* TODO getting properties from the content MUST fail */

	oio_url_pclean(&url1);
	g_remove (tmppath);
	oio_error_pfree (&err);
	return NULL;
}

static struct oio_error_s *
_roundtrip_header_case (struct oio_sds_s *client, struct oio_url_s *url,
		const char *path)
{
	struct oio_error_s *err;
	oio_header_case = OIO_HDRCASE_NONE;
	if (NULL != (err = _roundtrip_common (client, url, path)))
		return err;
	oio_header_case = OIO_HDRCASE_LOW;
	if (NULL != (err = _roundtrip_common (client, url, path)))
		return err;
	oio_header_case = OIO_HDRCASE_1CAP;
	if (NULL != (err = _roundtrip_common (client, url, path)))
		return err;
	oio_header_case = OIO_HDRCASE_RANDOM;
	if (NULL != (err = _roundtrip_common (client, url, path)))
		return err;
	return NULL;
}

static struct oio_error_s *
_roundtrip_autocontainer (struct oio_sds_s *client, struct oio_url_s *url,
		const char *path)
{
	struct file_info_s fi;
	GError *err = _checksum_file (path, &fi);

	/* compute the autocontainer with the SHA1, consider only the first 17 bits */
	struct oio_str_autocontainer_config_s cfg = {
		.src_offset = 0, .src_size = 0,
		.dst_bits = 17,
	};
	char tmp[65];
	const char *auto_container = oio_str_autocontainer_hash (fi.h, fi.hs, tmp, &cfg);

	/* build a new URL with the computed container name */
	struct oio_url_s *url_auto = oio_url_dup (url);
	oio_url_set (url_auto, OIOURL_USER, auto_container);
	err = (GError*) _roundtrip_header_case (client, url_auto, path);
	oio_url_pclean (&url_auto);
	return (struct oio_error_s*) err;
}

int
main(int argc, char **argv)
{
	oio_log_to_stderr();
	oio_sds_default_autocreate = 1;
	oio_log_init_level_from_env("G_DEBUG_LEVEL");

	if (argc != 2) {
		g_printerr ("Usage: %s PATH\n", argv[0]);
		return 1;
	}

	const char *path = argv[1];

	struct oio_url_s *url = oio_url_empty ();
	oio_url_set (url, OIOURL_NS, g_getenv("OIO_NS"));
	oio_url_set (url, OIOURL_ACCOUNT, g_getenv("OIO_ACCOUNT"));
	oio_url_set (url, OIOURL_USER, g_getenv("OIO_USER"));
	oio_url_set (url, OIOURL_TYPE, g_getenv("OIO_TYPE"));
	oio_url_set (url, OIOURL_PATH, g_getenv("OIO_PATH"));

	if (!oio_url_has_fq_path(url)) {
		g_printerr ("Partial URL [%s]: requires a NS (%s), an ACCOUNT (%s),"
				" an USER (%s) and a PATH (%s) (+ optional TYPE: %s)\n",
				oio_url_get (url, OIOURL_WHOLE),
				oio_url_has (url, OIOURL_NS)?"ok":"missing",
				oio_url_has (url, OIOURL_ACCOUNT)?"ok":"missing",
				oio_url_has (url, OIOURL_USER)?"ok":"missing",
				oio_url_has (url, OIOURL_PATH)?"ok":"missing",
				oio_url_has (url, OIOURL_TYPE)?"ok":"missing");
		return 3;
	}
	GRID_INFO("URL valid [%s]", oio_url_get (url, OIOURL_WHOLE));

	struct oio_sds_s *client = NULL;
	struct oio_error_s *err = NULL;

	/* Initiate a client */
	err = oio_sds_init (&client, oio_url_get(url, OIOURL_NS));
	if (err) {
		g_printerr ("Client init error: (%d) %s\n", oio_error_code(err),
				oio_error_message(err));
		return 4;
	}
	GRID_INFO("Client ready to [%s]", oio_url_get (url, OIOURL_NS));

	err = _roundtrip_header_case (client, url, path);
	if (!err)
		err = _roundtrip_autocontainer (client, url, path);
	int rc = err != NULL;

	oio_error_pfree (&err);
	oio_sds_pfree (&client);
	oio_url_pclean (&url);
	return rc;
}

