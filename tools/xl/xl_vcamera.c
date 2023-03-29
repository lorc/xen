/*
 * Copyright (C) 2019 EPAM Systems Inc.
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

#include <stdlib.h>

#include <libxl.h>
#include <libxl_utils.h>
#include <libxlutil.h>

#include "xl.h"
#include "xl_utils.h"
#include "xl_parse.h"

int main_vcameraattach(int argc, char **argv)
{
    int opt;
    int rc;
    uint32_t domid;
    libxl_device_vcamera vcamera;

    SWITCH_FOREACH_OPT(opt, "", NULL, "vcamera-attach", 1) {
        /* No options */
    }

    libxl_device_vcamera_init(&vcamera);
    domid = find_domain(argv[optind++]);

    for (argv += optind, argc -= optind; argc > 0; ++argv, --argc) {
        rc = parse_vcamera_item(&vcamera, *argv);
        if (rc) goto out;
    }

    if (dryrun_only) {
        char *json = libxl_device_vcamera_to_json(ctx, &vcamera);
        printf("vcamera: %s\n", json);
        free(json);
        rc = 0;
        goto out;
    }

    if (libxl_device_vcamera_add(ctx, domid, &vcamera, 0)) {
        fprintf(stderr, "libxl_device_vcamera_add failed.\n");
        rc = ERROR_FAIL; goto out;
    }

    rc = 0;

out:
    libxl_device_vcamera_dispose(&vcamera);
    return rc;
}

int main_vcameralist(int argc, char **argv)
{
   return 0;
}

int main_vcameradetach(int argc, char **argv)
{
    uint32_t domid, devid;
    int opt, rc;
    libxl_device_vcamera vcamera;

    SWITCH_FOREACH_OPT(opt, "", NULL, "vcamera-detach", 2) {
        /* No options */
    }

    domid = find_domain(argv[optind++]);
    devid = atoi(argv[optind++]);

    libxl_device_vcamera_init(&vcamera);

    if (libxl_devid_to_device_vcamera(ctx, domid, devid, &vcamera)) {
        fprintf(stderr, "Error: Device %d not connected.\n", devid);
        rc = ERROR_FAIL;
        goto out;
    }

    rc = libxl_device_vcamera_remove(ctx, domid, &vcamera, 0);
    if (rc) {
        fprintf(stderr, "libxl_device_vcamera_remove failed.\n");
        rc = ERROR_FAIL;
        goto out;
    }

    rc = 0;

out:
    libxl_device_vcamera_dispose(&vcamera);
    return rc;
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
