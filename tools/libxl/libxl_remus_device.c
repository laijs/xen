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

/*----- helper functions -----*/

static int init_device_subkind(libxl__remus_device_state *rds)
{
    int rc;
    const libxl__remus_device_subkind_ops **ops;

    for (ops = rds->ops; *ops; ops++) {
        rc = (*ops)->init(rds);
        if (rc) {
            goto out;
        }
    }

    rc = 0;
out:
    return rc;

}

static void destroy_device_subkind(libxl__remus_device_state *rds)
{
    const libxl__remus_device_subkind_ops **ops;

    for (ops = rds->ops; *ops; ops++)
        (*ops)->destroy(rds);
}

static bool all_devices_handled(libxl__remus_device_state *rds)
{
    return rds->num_devices == (rds->num_nics + rds->num_disks);
}

/*----- setup() and teardown() -----*/

/* callbacks */

static void device_match_cb(libxl__egc *egc,
                            libxl__remus_device *dev,
                            int rc);
static void device_setup_cb(libxl__egc *egc,
                            libxl__remus_device *dev,
                            int rc);
static void device_teardown_cb(libxl__egc *egc,
                               libxl__remus_device *dev,
                               int rc);

/* remus device setup and teardown */

static void libxl__remus_device_init(libxl__egc *egc,
                                     libxl__remus_device_state *rds,
                                     libxl__remus_device_kind kind,
                                     void *libxl_dev);
void libxl__remus_devices_setup(libxl__egc *egc, libxl__remus_device_state *rds)
{
    int i;
    STATE_AO_GC(rds->ao);

    if (!rds->ops[0])
        goto out;

    rds->saved_rc = init_device_subkind(rds);
    if (rds->saved_rc)
        goto out;

    rds->num_devices = 0;
    rds->num_nics = 0;
    rds->num_disks = 0;

    /* TBD: Remus setup - i.e. enable disk buffering, etc */
    if (rds->netbufscript)
        rds->nics = libxl_device_nic_list(CTX, rds->domid, &rds->num_nics);

    if (rds->num_nics == 0 && rds->num_disks == 0)
        goto out;

    GCNEW_ARRAY(rds->dev, rds->num_nics + rds->num_disks);

    /* TBD: CALL libxl__remus_device_init to init remus devices */
    for (i = 0; i < rds->num_nics; i++) {
        libxl__remus_device_init(egc, rds,
                                 LIBXL__REMUS_DEVICE_NIC, &rds->nics[i]);
    }

    return;

out:
    rds->callback(egc, rds, rds->saved_rc);
    return;
}

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
    dev->ops_index = 0;
    dev->ops = rds->ops[dev->ops_index];
    for (; dev->ops; dev->ops = rds->ops[++dev->ops_index]) {
        if (dev->ops->kind == dev->kind) {
            if (dev->ops->match) {
                dev->callback = device_match_cb;
                dev->ops->match(dev);
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

    if (!dev->ops) {
        rds->num_devices++;
        rds->saved_rc = ERROR_REMUS_DEVICE_NOT_SUPPORTED;
        if (all_devices_handled(rds))
            rds->callback(egc, rds, rds->saved_rc);
    }
}

static void device_match_cb(libxl__egc *egc,
                            libxl__remus_device *dev,
                            int rc)
{
    libxl__remus_device_state *const rds = dev->rds;

    STATE_AO_GC(rds->ao);

    if (rds->saved_rc) {
        /* there's already an error happened, we do not need to continue */
        rds->num_devices++;
        if (all_devices_handled(rds))
            rds->callback(egc, rds, rds->saved_rc);
        return;
    }

    if (rc) {
        /* the ops does not match, try next ops */
        dev->ops = rds->ops[++dev->ops_index];
        if (!dev->ops || rc != ERROR_REMUS_DEVOPS_NOT_MATCH) {
            /* the device can not be matched */
            rds->num_devices++;
            rds->saved_rc = ERROR_REMUS_DEVICE_NOT_SUPPORTED;
            if (all_devices_handled(rds))
                rds->callback(egc, rds, rds->saved_rc);
            return;
        }
        for ( ; dev->ops; dev->ops = rds->ops[++dev->ops_index]) {
            if (dev->ops->kind == dev->kind) {
                /*
                 * we have entered match process, that means this *kind* of
                 * device's ops must have a match() implementation.
                 */
                assert(dev->ops->match);
                dev->ops->match(dev);
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

    STATE_AO_GC(rds->ao);

    rds->num_devices++;
    /*
     * the netbuf script was designed as below:
     * 1. when setup failed, the script won't teardown the device itself.
     * 2. the teardown op is ok to be executed many times.
     *
     * we add devices that have been set up to the array no matter
     * the setup process succeed or failed because we need to ensure
     * the device been teardown while setup failed. If any of the
     * device setup failed, we will quit remus, but before we exit,
     * we will teardown the devices that have been added to **dev
     */
    rds->dev[rds->num_set_up++] = dev;
    /* we preserve the first error that happened */
    if (rc && !rds->saved_rc)
        rds->saved_rc = rc;

    if (all_devices_handled(rds))
        rds->callback(egc, rds, rds->saved_rc);
}

void libxl__remus_devices_teardown(libxl__egc *egc, libxl__remus_device_state *rds)
{
    int i, num_set_up;
    libxl__remus_device *dev;

    STATE_AO_GC(rds->ao);

    rds->saved_rc = 0;

    if (rds->num_set_up == 0) {
        destroy_device_subkind(rds);
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
    rds->callback(egc, rds, rds->saved_rc);
    return;
}

static void device_teardown_cb(libxl__egc *egc,
                               libxl__remus_device *dev,
                               int rc)
{
    int i;
    libxl__remus_device_state *const rds = dev->rds;

    STATE_AO_GC(rds->ao);

    /* we preserve the first error that happened */
    if (rc && !rds->saved_rc)
        rds->saved_rc = rc;

    /* ignore teardown errors to teardown as many devs as possible*/
    rds->num_set_up--;

    if (rds->num_set_up == 0) {
        /* clean nic */
        for (i = 0; i < rds->num_nics; i++)
            libxl_device_nic_dispose(&rds->nics[i]);
        free(rds->nics);
        rds->nics = NULL;
        rds->num_nics = 0;

        destroy_device_subkind(rds);
        rds->callback(egc, rds, rds->saved_rc);
    }
}

/*----- checkpointing APIs -----*/

/* callbacks */

static void device_checkpoint_cb(libxl__egc *egc,
                                 libxl__remus_device *dev,
                                 int rc);

/* API implementations */

#define define_remus_device_checkpoint_api(api)                             \
void libxl__remus_devices_##api(libxl__egc *egc,                            \
                                libxl__remus_device_state *rds)             \
{                                                                           \
    int i;                                                                  \
    libxl__remus_device *dev;                                               \
                                                                            \
    STATE_AO_GC(rds->ao);                                                   \
                                                                            \
    rds->num_devices = 0;                                                   \
    rds->saved_rc = 0;                                                      \
                                                                            \
    if (rds->num_set_up == 0)                                               \
        goto out;                                                           \
                                                                            \
    for (i = 0; i < rds->num_set_up; i++) {                                 \
        dev = rds->dev[i];                                                  \
        dev->callback = device_checkpoint_cb;                               \
        if (dev->ops->api) {                                                \
            dev->ops->api(dev);                                             \
        } else {                                                            \
            rds->num_devices++;                                             \
            if (rds->num_devices == rds->num_set_up)                        \
                rds->callback(egc, rds, rds->saved_rc);                     \
        }                                                                   \
    }                                                                       \
                                                                            \
    return;                                                                 \
                                                                            \
out:                                                                        \
    rds->callback(egc, rds, rds->saved_rc);                                 \
}

define_remus_device_checkpoint_api(postsuspend);

define_remus_device_checkpoint_api(preresume);

define_remus_device_checkpoint_api(commit);

static void device_checkpoint_cb(libxl__egc *egc,
                                 libxl__remus_device *dev,
                                 int rc)
{
    /* Convenience aliases */
    libxl__remus_device_state *const rds = dev->rds;

    STATE_AO_GC(rds->ao);

    rds->num_devices++;

    if (rc)
        rds->saved_rc = ERROR_FAIL;

    if (rds->num_devices == rds->num_set_up)
        rds->callback(egc, rds, rds->saved_rc);
}
