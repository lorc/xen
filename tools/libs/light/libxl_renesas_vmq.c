/*
 * Copyright (C) 2022 EPAM Systems Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_internal.h"

#include <xen/io/renesas_vmq.h>

static int libxl__device_renesas_vmq_setdefault(libxl__gc *gc, uint32_t domid,
                                           libxl_device_renesas_vmq *renesas_vmq,
                                           bool hotplug)
{
    return libxl__resolve_domid(gc, renesas_vmq->backend_domname,
                                &renesas_vmq->backend_domid);
}

static int libxl__renesas_vmq_from_xenstore(libxl__gc *gc, const char *libxl_path,
                                       libxl_devid devid,
                                       libxl_device_renesas_vmq *renesas_vmq)
{
    const char *be_path;
    int rc;

    rc = libxl__xs_read_mandatory(gc, XBT_NULL,
                                  GCSPRINTF("%s/backend", libxl_path),
                                  &be_path);
    if (rc) return rc;

    renesas_vmq->devid = devid;

    return libxl__backendpath_parse_domid(gc, be_path, &renesas_vmq->backend_domid);
}

static void libxl__update_config_renesas_vmq(libxl__gc *gc,
                                        libxl_device_renesas_vmq *dst,
                                        libxl_device_renesas_vmq *src)
{
    dst->devid = src->devid;
    dst->type = src->type;
    dst->if_num = src->if_num;
    dst->osid = src->osid;
}

static int libxl_device_renesas_vmq_compare(const libxl_device_renesas_vmq *d1,
                                       const libxl_device_renesas_vmq *d2)
{
    return COMPARE_DEVID(d1, d2);
}

static void libxl__device_renesas_vmq_add(libxl__egc *egc, uint32_t domid,
                                     libxl_device_renesas_vmq *renesas_vmq,
                                     libxl__ao_device *aodev)
{
    libxl__device_add_async(egc, domid, &libxl__renesas_vmq_devtype, renesas_vmq, aodev);
}

static int libxl__set_xenstore_renesas_vmq(libxl__gc *gc, uint32_t domid,
                                      libxl_device_renesas_vmq *renesas_vmq,
                                      flexarray_t *back, flexarray_t *front,
                                      flexarray_t *ro_front)
{
    flexarray_append_pair(ro_front, XEN_RENESAS_VMQ_FIELD_TYPE,
                          GCSPRINTF("%s",
			  libxl_renesas_vmq_type_to_string(renesas_vmq->type)));

    flexarray_append_pair(ro_front, XEN_RENESAS_VMQ_FIELD_IF_NUM,
                          GCSPRINTF("%d", renesas_vmq->if_num));

    flexarray_append_pair(ro_front, XEN_RENESAS_VMQ_FIELD_OSID,
                          GCSPRINTF("%d", renesas_vmq->osid));


    return 0;
}

int libxl_device_renesas_vmq_getinfo(libxl_ctx *ctx, uint32_t domid,
                                const libxl_device_renesas_vmq *renesas_vmq,
                                libxl_renesas_vmq_info *info)
{
    GC_INIT(ctx);
    char *libxl_path, *devpath;
    char *val;
    int rc;

    libxl_renesas_vmq_info_init(info);
    info->devid = renesas_vmq->devid;

    devpath = libxl__domain_device_frontend_path(gc, domid, info->devid,
                                                 LIBXL__DEVICE_KIND_RENESAS_VMQ);
    libxl_path = libxl__domain_device_libxl_path(gc, domid, info->devid,
                                                 LIBXL__DEVICE_KIND_RENESAS_VMQ);

    info->backend = xs_read(ctx->xsh, XBT_NULL,
                            GCSPRINTF("%s/backend", libxl_path),
                            NULL);
    if (!info->backend) { rc = ERROR_FAIL; goto out; }

    rc = libxl__backendpath_parse_domid(gc, info->backend, &info->backend_id);
    if (rc) goto out;

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/state", devpath));
    info->state = val ? strtoul(val, NULL, 10) : -1;

    info->frontend = xs_read(ctx->xsh, XBT_NULL,
                             GCSPRINTF("%s/frontend", libxl_path),
                             NULL);
    info->frontend_id = domid;

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/"XEN_RENESAS_VMQ_FIELD_IF_NUM, devpath));
    info->if_num = val ? strtoul(val, NULL, 10) : 0;

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/"XEN_RENESAS_VMQ_FIELD_OSID, devpath));
    info->osid = val ? strtoul(val, NULL, 10) : 0;

    val = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/"XEN_RENESAS_VMQ_FIELD_TYPE, devpath));
    if (val)
	rc = libxl_renesas_vmq_type_from_string(val, &info->type);
    else {
	info->type = LIBXL_RENESAS_VMQ_TYPE_VMQ;
	rc = 0;
    }

out:
     GC_FREE;
     return rc;
}

static LIBXL_DEFINE_DEVICE_FROM_TYPE(renesas_vmq)
static LIBXL_DEFINE_UPDATE_DEVID(renesas_vmq)
static LIBXL_DEFINE_DEVICES_ADD(renesas_vmq)

LIBXL_DEFINE_DEVID_TO_DEVICE(renesas_vmq)
LIBXL_DEFINE_DEVICE_ADD(renesas_vmq)
LIBXL_DEFINE_DEVICE_REMOVE(renesas_vmq)
LIBXL_DEFINE_DEVICE_LIST(renesas_vmq)

DEFINE_DEVICE_TYPE_STRUCT(renesas_vmq, RENESAS_VMQ, renesas_vmqs,
    .update_config = (device_update_config_fn_t)libxl__update_config_renesas_vmq,
    .from_xenstore = (device_from_xenstore_fn_t)libxl__renesas_vmq_from_xenstore,
    .set_xenstore_config = (device_set_xenstore_config_fn_t)
                           libxl__set_xenstore_renesas_vmq
);

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
