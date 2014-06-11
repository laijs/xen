/*
 * Copyright (C) 2013
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

static void async_call_done(libxl__egc *egc,
                            libxl__ev_child *child,
                            pid_t pid, int status)
{
    libxl__remus_device *dev = CONTAINER_OF(child, *dev, child);
    libxl__remus_device_state *rds = dev->rds;
    STATE_AO_GC(rds->ao);

    if (WIFEXITED(status)) {
        dev->callback(egc, dev, -WEXITSTATUS(status));
    } else {
        dev->callback(egc, dev, ERROR_FAIL);
    }
}

static void nic_match_async(const libxl__remus_device_ops *self,
                            libxl__remus_device *dev)
{
    if (dev->kind == LIBXL__REMUS_DEVICE_NIC)
        _exit(-ERROR_FAIL);

    _exit(-ERROR_NOT_MATCH);
}

static void nic_match(libxl__remus_device_ops *self,
                      libxl__remus_device *dev)
{
    int pid = -1;
    STATE_AO_GC(dev->rds->ao);

    /* Fork and call */
    pid = libxl__ev_child_fork(gc, &dev->child, async_call_done);
    if (pid == -1) {
        LOG(ERROR, "unable to fork");
        goto out;
    }

    if (!pid) {
        /* child */
        nic_match_async(self, dev);
        /* notreached */
        abort();
    }

    return;

out:
    dev->callback(dev->rds->egc, dev, ERROR_FAIL);
}

static int nic_init(libxl__remus_device_ops *self,
                    libxl__remus_state *rs)
{
    return 0;
}

static void nic_destroy(libxl__remus_device_ops *self)
{
    return;
}

libxl__remus_device_ops remus_device_nic = {
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
