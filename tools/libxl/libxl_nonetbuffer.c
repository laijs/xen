/*
 * Copyright (C) 2014
 * Author Shriram Rajagopalan <rshriram@cs.ubc.ca>
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

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"

int libxl__netbuffer_enabled(libxl__gc *gc)
{
    return 0;
}

static void nic_match(libxl__remus_device *dev)
{
    STATE_AO_GC(dev->rds->ao);

    dev->callback(dev->rds->egc, dev, ERROR_FAIL);
}

static int nic_init(libxl__remus_device_state *rds)
{
    return 0;
}

static void nic_destroy(libxl__remus_device_state *rds)
{
    return;
}

const libxl__remus_device_subkind_ops remus_device_nic = {
    .kind = LIBXL__REMUS_DEVICE_NIC,
    .init = nic_init,
    .destroy = nic_destroy,
    .match = nic_match,
};

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
