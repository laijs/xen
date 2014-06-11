/*
 * Copyright (C) 2014
 * Author: Lai Jiangshan <laijs@cn.fujitsu.com>
 *         Yang Hongyang <yanghy@cn.fujitsu.com>
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

static libxl__remus_device_ops *dev_ops[] = {
};

static void device_common_cb(libxl__egc *egc,
                             libxl__remus_device *dev,
                             int rc)
{
    /* Convenience aliases */
    libxl__remus_device_state *const rds = dev->rds;
    libxl__remus_state *const rs = CONTAINER_OF(rds, *rs, dev_state);

    STATE_AO_GC(rs->ao);

    rds->num_devices++;

    if (rc)
        rs->saved_rc = ERROR_FAIL;

    if (rds->num_devices == rds->num_setuped)
        rs->callback(egc, rs, rs->saved_rc);
}

void libxl__remus_device_postsuspend(libxl__egc *egc, libxl__remus_state *rs)
{
    int i;
    libxl__remus_device *dev;
    STATE_AO_GC(rs->ao);

    /* Convenience aliases */
    libxl__remus_device_state *const rds = &rs->dev_state;

    rds->num_devices = 0;
    rs->saved_rc = 0;
    for (i = 0; i < rds->num_setuped; i++) {
        dev = rds->dev[i];
        dev->callback = device_common_cb;
        if (dev->ops->postsuspend) {
            dev->ops->postsuspend(dev);
        } else {
            rds->num_devices++;
            if (rds->num_devices == rds->num_setuped)
                rs->callback(egc, rs, rs->saved_rc);
        }
    }
}

void libxl__remus_device_preresume(libxl__egc *egc, libxl__remus_state *rs)
{
    int i;
    libxl__remus_device *dev;
    STATE_AO_GC(rs->ao);

    /* Convenience aliases */
    libxl__remus_device_state *const rds = &rs->dev_state;

    rds->num_devices = 0;
    rs->saved_rc = 0;
    for (i = 0; i < rds->num_setuped; i++) {
        dev = rds->dev[i];
        dev->callback = device_common_cb;
        if (dev->ops->preresume) {
            dev->ops->preresume(dev);
        } else {
            rds->num_devices++;
            if (rds->num_devices == rds->num_setuped)
                rs->callback(egc, rs, rs->saved_rc);
        }
    }
}

void libxl__remus_device_commit(libxl__egc *egc, libxl__remus_state *rs)
{
    int i;
    libxl__remus_device *dev;
    STATE_AO_GC(rs->ao);

    /*
     * REMUS TODO: Wait for disk and explicit memory ack (through restore
     * callback from remote) before releasing network buffer.
     */
    /* Convenience aliases */
    libxl__remus_device_state *const rds = &rs->dev_state;

    rds->num_devices = 0;
    rs->saved_rc = 0;
    for (i = 0; i < rds->num_setuped; i++) {
        dev = rds->dev[i];
        dev->callback = device_common_cb;
        if (dev->ops->commit) {
            dev->ops->commit(dev);
        } else {
            rds->num_devices++;
            if (rds->num_devices == rds->num_setuped)
                rs->callback(egc, rs, rs->saved_rc);
        }
    }
}

static void device_setup_cb(libxl__egc *egc,
                            libxl__remus_device *dev,
                            int rc)
{
    /* Convenience aliases */
    libxl__remus_device_state *const rds = dev->rds;
    libxl__remus_state *const rs = CONTAINER_OF(rds, *rs, dev_state);

    STATE_AO_GC(rs->ao);

    rds->num_devices++;
    /*
     * we add devices that have been setuped to the array no matter
     * the setup process succeed or failed because we need to ensure
     * the device been teardown while setup failed. If any of the
     * device setup failed, we will quit remus, but before we exit,
     * we will teardown the devices that have been added to **dev
     */
    rds->dev[rds->num_setuped++] = dev;
    if (rc) {
        /* setup failed */
        rs->saved_rc = ERROR_FAIL;
    }

    if (rds->num_devices == (rds->num_nics + rds->num_disks))
        rs->callback(egc, rs, rs->saved_rc);
}

static void device_match_cb(libxl__egc *egc,
                            libxl__remus_device *dev,
                            int rc)
{
    libxl__remus_device_state *const rds = dev->rds;
    libxl__remus_state *rs = CONTAINER_OF(rds, *rs, dev_state);

    STATE_AO_GC(rs->ao);

    if (rc) {
        if (++dev->ops_index >= ARRAY_SIZE(dev_ops) ||
            rc != ERROR_NOT_MATCH) {
            /* the device can not be matched */
            rds->num_devices++;
            rs->saved_rc = ERROR_FAIL;
            if (rds->num_devices == (rds->num_nics + rds->num_disks))
                rs->callback(egc, rs, rs->saved_rc);
            return;
        }
        /* the ops does not match, try next ops */
        dev->ops = dev_ops[dev->ops_index];
        dev->ops->match(dev->ops, dev);
    } else {
        /* the ops matched, setup the device */
        dev->callback = device_setup_cb;
        dev->ops->setup(dev);
    }
}

static void device_teardown_cb(libxl__egc *egc,
                               libxl__remus_device *dev,
                               int rc)
{
    int i;
    libxl__remus_device_ops *ops;
    libxl__remus_device_state *const rds = dev->rds;
    libxl__remus_state *rs = CONTAINER_OF(rds, *rs, dev_state);

    STATE_AO_GC(rs->ao);

    /* ignore teardown errors to teardown as many devs as possible*/
    rds->num_setuped--;

    if (rds->num_setuped == 0) {
        /* clean device ops */
        for (i = 0; i < ARRAY_SIZE(dev_ops); i++) {
            ops = dev_ops[i];
            ops->destroy(ops);
        }
        rs->callback(egc, rs, rs->saved_rc);
    }
}

static __attribute__((unused)) void libxl__remus_device_init(libxl__egc *egc,
                                     libxl__remus_device_state *rds,
                                     libxl__remus_device_kind kind,
                                     void *libxl_dev)
{
    libxl__remus_device *dev = NULL;
    libxl_device_nic *nic = NULL;
    libxl_device_disk *disk = NULL;

    STATE_AO_GC(rds->ao);
    GCNEW(dev);
    dev->ops_index = 0; /* we will match the ops later */
    dev->backend_dev = libxl_dev;
    dev->kind = kind;
    dev->rds = rds;

    switch (kind) {
        case LIBXL__REMUS_DEVICE_NIC:
            nic = libxl_dev;
            dev->devid = nic->devid;
            break;
        case LIBXL__REMUS_DEVICE_DISK:
            disk = libxl_dev;
            /* there are no dev id for disk devices */
            dev->devid = -1;
            break;
        default:
            return;
    }

    libxl__async_exec_init(&dev->aes);
    libxl__ev_child_init(&dev->child);

    /* match the ops begin */
    dev->callback = device_match_cb;
    dev->ops = dev_ops[dev->ops_index];
    dev->ops->match(dev->ops, dev);
}

void libxl__remus_device_setup(libxl__egc *egc, libxl__remus_state *rs)
{
    int i;
    libxl__remus_device_ops *ops;

    /* Convenience aliases */
    libxl__remus_device_state *const rds = &rs->dev_state;

    STATE_AO_GC(rs->ao);

    if (ARRAY_SIZE(dev_ops) == 0)
        goto out;

    for (i = 0; i < ARRAY_SIZE(dev_ops); i++) {
        ops = dev_ops[i];
        if (ops->init(ops, rs)) {
            rs->saved_rc = ERROR_FAIL;
            goto out;
        }
    }

    rds->ao = rs->ao;
    rds->egc = egc;
    rds->num_devices = 0;
    rds->num_nics = 0;
    rds->num_disks = 0;

    /* TBD: Remus setup - i.e. attach qdisc, enable disk buffering, etc */

    GCNEW_ARRAY(rds->dev, rds->num_nics + rds->num_disks);

    /* TBD: CALL libxl__remus_device_init to init remus devices */

    if (rds->num_nics == 0 && rds->num_disks == 0)
        goto out;

    return;

out:
    rs->callback(egc, rs, rs->saved_rc);
    return;
}

void libxl__remus_device_teardown(libxl__egc *egc, libxl__remus_state *rs)
{
    int i;
    libxl__remus_device *dev;
    libxl__remus_device_ops *ops;

    STATE_AO_GC(rs->ao);

    /* Convenience aliases */
    libxl__remus_device_state *const rds = &rs->dev_state;

    if (rds->num_setuped == 0) {
        /* clean device ops */
        for (i = 0; i < ARRAY_SIZE(dev_ops); i++) {
            ops = dev_ops[i];
            ops->destroy(ops);
        }
        goto out;
    }

    for (i = 0; i < rds->num_setuped; i++) {
        dev = rds->dev[i];
        dev->callback = device_teardown_cb;
        dev->ops->teardown(dev);
    }

    return;

out:
    rs->callback(egc, rs, rs->saved_rc);
    return;
}
