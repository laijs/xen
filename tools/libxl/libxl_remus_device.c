/*
 * Copyright (C) 2014
 * Author: Yang Hongyang <yanghy@cn.fujitsu.com>
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

extern const libxl__remus_device_ops remus_device_nic;
extern const libxl__remus_device_ops remus_device_drbd_disk;
static const libxl__remus_device_ops *dev_ops[] = {
    &remus_device_nic,
    &remus_device_drbd_disk,
};

/*----- checkpointing APIs -----*/

/* callbacks */

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

    if (rds->num_devices == rds->num_set_up)
        rs->callback(egc, rs, rs->saved_rc);
}

/* API implementations */

void libxl__remus_device_postsuspend(libxl__egc *egc, libxl__remus_state *rs)
{
    int i;
    libxl__remus_device *dev;
    STATE_AO_GC(rs->ao);

    /* Convenience aliases */
    libxl__remus_device_state *const rds = &rs->dev_state;

    rds->num_devices = 0;
    rs->saved_rc = 0;

    if(rds->num_set_up == 0)
        goto out;

    for (i = 0; i < rds->num_set_up; i++) {
        dev = rds->dev[i];
        dev->callback = device_common_cb;
        if (dev->ops->postsuspend) {
            dev->ops->postsuspend(dev);
        } else {
            rds->num_devices++;
            if (rds->num_devices == rds->num_set_up)
                rs->callback(egc, rs, rs->saved_rc);
        }
    }

    return;

out:
    rs->callback(egc, rs, rs->saved_rc);
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

    if(rds->num_set_up == 0)
        goto out;

    for (i = 0; i < rds->num_set_up; i++) {
        dev = rds->dev[i];
        dev->callback = device_common_cb;
        if (dev->ops->preresume) {
            dev->ops->preresume(dev);
        } else {
            rds->num_devices++;
            if (rds->num_devices == rds->num_set_up)
                rs->callback(egc, rs, rs->saved_rc);
        }
    }

    return;

out:
    rs->callback(egc, rs, rs->saved_rc);
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

    if(rds->num_set_up == 0)
        goto out;

    for (i = 0; i < rds->num_set_up; i++) {
        dev = rds->dev[i];
        dev->callback = device_common_cb;
        if (dev->ops->commit) {
            dev->ops->commit(dev);
        } else {
            rds->num_devices++;
            if (rds->num_devices == rds->num_set_up)
                rs->callback(egc, rs, rs->saved_rc);
        }
    }

    return;

out:
    rs->callback(egc, rs, rs->saved_rc);
}

/*----- main flow of control -----*/

/* callbacks */

static void device_setup_cb(libxl__egc *egc,
                            libxl__remus_device *dev,
                            int rc);
static void device_match_cb(libxl__egc *egc,
                            libxl__remus_device *dev,
                            int rc)
{
    libxl__remus_device_state *const rds = dev->rds;
    libxl__remus_state *rs = CONTAINER_OF(rds, *rs, dev_state);

    STATE_AO_GC(rs->ao);

    if (rc) {
        if (++dev->ops_index >= ARRAY_SIZE(dev_ops) ||
            rc != ERROR_REMUS_DEVOPS_NOT_MATCH) {
            /* the device can not be matched */
            rds->num_devices++;
            rs->saved_rc = ERROR_FAIL;
            if (rds->num_devices == (rds->num_nics + rds->num_disks))
                rs->callback(egc, rs, rs->saved_rc);
            return;
        }
        /* the ops does not match, try next ops */
        for ( ; dev->ops_index < ARRAY_SIZE(dev_ops); dev->ops_index++) {
            dev->ops = dev_ops[dev->ops_index];
            if (dev->ops->kind == dev->kind) {
                /*
                 * we have entered match process, that means this *kind* of
                 * device's ops must have a match() implementation.
                 */
                assert(dev->ops->match);
                dev->ops->match(dev->ops, dev);
                break;
            }
        }
    } else {
        /* the ops matched, setup the device */
        dev->callback = device_setup_cb;
        dev->ops->setup(dev);
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
    rds->dev[rds->num_set_up++] = dev;
    if (rc) {
        /* setup failed */
        rs->saved_rc = ERROR_FAIL;
    }

    if (rds->num_devices == (rds->num_nics + rds->num_disks))
        rs->callback(egc, rs, rs->saved_rc);
}

static void destroy_device_ops(libxl__remus_state *rs);
static void device_teardown_cb(libxl__egc *egc,
                               libxl__remus_device *dev,
                               int rc)
{
    int i;
    libxl__remus_device_state *const rds = dev->rds;
    libxl__remus_state *rs = CONTAINER_OF(rds, *rs, dev_state);

    STATE_AO_GC(rs->ao);

    /* ignore teardown errors to teardown as many devs as possible*/
    rds->num_set_up--;

    if (rds->num_set_up == 0) {
        /* clean nic */
        for (i = 0; i < rds->num_nics; i++)
            libxl_device_nic_dispose(&rds->nics[i]);
        free(rds->nics);
        rds->nics = NULL;
        rds->num_nics = 0;

        /* clean disk */
        for (i = 0; i < rds->num_disks; i++)
            libxl_device_disk_dispose(&rds->disks[i]);
        free(rds->disks);
        rds->disks = NULL;
        rds->num_disks = 0;

        destroy_device_ops(rs);
        rs->callback(egc, rs, rs->saved_rc);
    }
}

/* remus device setup and teardown */

static void libxl__remus_device_init(libxl__egc *egc,
                                     libxl__remus_device_state *rds,
                                     libxl__remus_device_kind kind,
                                     void *libxl_dev)
{
    libxl__remus_device *dev = NULL;

    STATE_AO_GC(rds->ao);
    GCNEW(dev);
    dev->backend_dev = libxl_dev;
    dev->kind = kind;
    dev->rds = rds;

    libxl__async_exec_init(&dev->aes);
    libxl__ev_child_init(&dev->child);

    /* match the ops begin */
    for (dev->ops_index = 0;
         dev->ops_index < ARRAY_SIZE(dev_ops);
         dev->ops_index++) {
        dev->ops = dev_ops[dev->ops_index];
        if (dev->ops->kind == dev->kind) {
            if (dev->ops->match) {
                dev->callback = device_match_cb;
                dev->ops->match(dev->ops, dev);
            } else {
                /*
                 * This devops do not have match() implementation.
                 * That means this *kind* of device's ops is always
                 * matched with the *kind* of device.
                 */
                dev->callback = device_setup_cb;
                dev->ops->setup(dev);
            }
            break;
        }
    }
}

static int init_device_ops(libxl__remus_state *rs)
{
    int i, rc;
    const libxl__remus_device_ops *ops;

    for (i = 0; i < ARRAY_SIZE(dev_ops); i++) {
        ops = dev_ops[i];
        if (ops->init(ops, rs)) {
            rc = ERROR_FAIL;
            goto out;
        }
    }

    rc = 0;
out:
    return rc;

}

static void destroy_device_ops(libxl__remus_state *rs)
{
    int i;
    const libxl__remus_device_ops *ops;

    for (i = 0; i < ARRAY_SIZE(dev_ops); i++) {
        ops = dev_ops[i];
        ops->destroy(ops, rs);
    }
}

void libxl__remus_device_setup(libxl__egc *egc, libxl__remus_state *rs)
{
    int i;
    STATE_AO_GC(rs->ao);

    /* Convenience aliases */
    libxl__remus_device_state *const rds = &rs->dev_state;

    if (ARRAY_SIZE(dev_ops) == 0)
        goto out;

    rs->saved_rc = init_device_ops(rs);
    if (rs->saved_rc)
        goto out;

    rds->ao = rs->ao;
    rds->egc = egc;
    rds->num_devices = 0;
    rds->num_nics = 0;
    rds->num_disks = 0;

    if (rs->netbufscript)
        rds->nics = libxl_device_nic_list(CTX, rs->domid, &rds->num_nics);

    if (rs->diskbuf)
        rds->disks = libxl_device_disk_list(CTX, rs->domid, &rds->num_disks);

    if (rds->num_nics == 0 && rds->num_disks == 0)
        goto out;

    GCNEW_ARRAY(rds->dev, rds->num_nics + rds->num_disks);

    for (i = 0; i < rds->num_nics; i++) {
        libxl__remus_device_init(egc, rds,
                                 LIBXL__REMUS_DEVICE_NIC, &rds->nics[i]);
    }

    for (i = 0; i < rds->num_disks; i++) {
        libxl__remus_device_init(egc, rds,
                                 LIBXL__REMUS_DEVICE_DISK, &rds->disks[i]);
    }

    return;

out:
    rs->callback(egc, rs, rs->saved_rc);
    return;
}

void libxl__remus_device_teardown(libxl__egc *egc, libxl__remus_state *rs)
{
    int i, num_set_up;
    libxl__remus_device *dev;

    STATE_AO_GC(rs->ao);

    /* Convenience aliases */
    libxl__remus_device_state *const rds = &rs->dev_state;

    if (rds->num_set_up == 0) {
        destroy_device_ops(rs);
        goto out;
    }

    /* we will decrease rds->num_set_up in the teardown callback */
    num_set_up = rds->num_set_up;
    for (i = 0; i < num_set_up; i++) {
        dev = rds->dev[i];
        dev->callback = device_teardown_cb;
        dev->ops->teardown(dev);
    }

    return;

out:
    rs->callback(egc, rs, rs->saved_rc);
    return;
}
