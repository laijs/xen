/*
 * Copyright (C) 2014 FUJITSU LIMITED
 * Author Lai Jiangshan <laijs@cn.fujitsu.com>
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

/*** drbd implementation ***/
const int DRBD_SEND_CHECKPOINT = 20;
const int DRBD_WAIT_CHECKPOINT_ACK = 30;

struct libxl__remus_drbd_state {
    libxl__ao *ao;
    char *drbd_probe_script;
};

typedef struct libxl__remus_drbd_disk {
    int ctl_fd;
    int ackwait;
    const char *path;

    libxl__async_exec_state aes;
    libxl__ev_child child;
} libxl__remus_drbd_disk;

/*----- helper functions, for async calls -----*/
static void drbd_async_call(libxl__remus_device *dev,
                            void func(libxl__remus_device *),
                            libxl__ev_child_callback callback)
{
    int pid = -1;
    STATE_AO_GC(dev->rds->ao);

    /* Fork and call */
    pid = libxl__ev_child_fork(gc, &dev->child, callback);
    if (pid == -1) {
        LOG(ERROR, "unable to fork");
        goto out;
    }

    if (!pid) {
        /* child */
        func(dev);
        /* notreached */
        abort();
    }

    return;

out:
    dev->callback(dev->rds->egc, dev, ERROR_FAIL);
}

/*----- init() and cleanup() -----*/
static int drbd_init(libxl__remus_device_state *rds)
{
    libxl__remus_drbd_state *drbd_state;

    STATE_AO_GC(rds->ao);

    GCNEW(drbd_state);
    CTX->drbd_state = drbd_state;
    drbd_state->ao = ao;
    drbd_state->drbd_probe_script = GCSPRINTF("%s/block-drbd-probe",
                                              libxl__xen_script_dir_path());

    return 0;
}

static void drbd_cleanup(libxl__remus_device_state *rds)
{
    return;
}

/*----- match(), setup() and teardown() -----*/

/* callbacks */
static void match_async_exec_cb(libxl__egc *egc,
                                libxl__async_exec_state *aes,
                                int status);

/* implementations */

static void match_async_exec(libxl__egc *egc, libxl__remus_device *dev)
{
    int arraysize, nr = 0;
    const libxl_device_disk *disk = dev->backend_dev;
    libxl__async_exec_state *aes = &dev->aes;
    STATE_AO_GC(dev->rds->ao);

    libxl__remus_drbd_state *drbd_state = CTX->drbd_state;
    /* setup env & args */
    arraysize = 1;
    GCNEW_ARRAY(aes->env, arraysize);
    aes->env[nr++] = NULL;
    assert(nr <= arraysize);

    arraysize = 3;
    nr = 0;
    GCNEW_ARRAY(aes->args, arraysize);
    aes->args[nr++] = drbd_state->drbd_probe_script;
    aes->args[nr++] = disk->pdev_path;
    aes->args[nr++] = NULL;
    assert(nr <= arraysize);

    aes->ao = drbd_state->ao;
    aes->what = GCSPRINTF("%s %s", aes->args[0], aes->args[1]);
    aes->timeout_ms = LIBXL_HOTPLUG_TIMEOUT * 1000;
    aes->callback = match_async_exec_cb;
    aes->stdfds[0] = -1;
    aes->stdfds[1] = -1;
    aes->stdfds[2] = -1;

    if (libxl__async_exec_start(gc, aes))
        goto out;

    return;

out:
    dev->callback(egc, dev, ERROR_FAIL);
}

static void drbd_match(libxl__remus_device *dev)
{
    match_async_exec(dev->rds->egc, dev);
}

static void match_async_exec_cb(libxl__egc *egc,
                                libxl__async_exec_state *aes,
                                int status)
{
    libxl__remus_device *dev = CONTAINER_OF(aes, *dev, aes);

    if (status) {
        dev->callback(egc, dev, ERROR_REMUS_DEVOPS_NOT_MATCH);
    } else {
        dev->callback(egc, dev, 0);
    }
}

static void drbd_setup(libxl__remus_device *dev)
{
    libxl__remus_drbd_disk *drbd_disk;
    const libxl_device_disk *disk = dev->backend_dev;
    STATE_AO_GC(dev->rds->ao);

    GCNEW(drbd_disk);
    dev->concrete_data = drbd_disk;
    drbd_disk->path = disk->pdev_path;
    drbd_disk->ackwait = 0;
    drbd_disk->ctl_fd = open(drbd_disk->path, O_RDONLY);
    if (drbd_disk->ctl_fd < 0)
        dev->callback(dev->rds->egc, dev, ERROR_FAIL);
    else
        dev->callback(dev->rds->egc, dev, 0);
}

static void drbd_teardown(libxl__remus_device *dev)
{
    libxl__remus_drbd_disk *drbd_disk = dev->concrete_data;

    close(drbd_disk->ctl_fd);
    dev->callback(dev->rds->egc, dev, 0);
}

/*----- checkpointing APIs -----*/

/* callbacks */
static void chekpoint_async_call_done(libxl__egc *egc,
                                      libxl__ev_child *child,
                                      pid_t pid, int status);

/* API implementations */

/* this op will not wait and block, so implement as sync op */
static void drbd_postsuspend(libxl__remus_device *dev)
{
    libxl__remus_drbd_disk *rdd = dev->concrete_data;

    if (!rdd->ackwait) {
        if (ioctl(rdd->ctl_fd, DRBD_SEND_CHECKPOINT, 0) <= 0)
            rdd->ackwait = 1;
    }

    dev->callback(dev->rds->egc, dev, 0);
}

static void drbd_preresume_async(libxl__remus_device *dev)
{
    libxl__remus_drbd_disk *rdd = dev->concrete_data;
    int ackwait = rdd->ackwait;

    if (ackwait) {
        ioctl(rdd->ctl_fd, DRBD_WAIT_CHECKPOINT_ACK, 0);
        ackwait = 0;
    }

    _exit(ackwait);
}

static void drbd_preresume(libxl__remus_device *dev)
{
    drbd_async_call(dev, drbd_preresume_async, chekpoint_async_call_done);
}

static void chekpoint_async_call_done(libxl__egc *egc,
                                      libxl__ev_child *child,
                                      pid_t pid, int status)
{
    libxl__remus_device *dev = CONTAINER_OF(child, *dev, child);
    libxl__remus_drbd_disk *rdd = dev->concrete_data;
    STATE_AO_GC(dev->rds->ao);

    if (WIFEXITED(status)) {
        rdd->ackwait = WEXITSTATUS(status);
        dev->callback(egc, dev, 0);
    } else {
        dev->callback(egc, dev, ERROR_FAIL);
    }
}

const libxl__remus_device_subkind_ops remus_device_drbd_disk = {
    .kind = LIBXL__REMUS_DEVICE_DISK,
    .init = drbd_init,
    .cleanup = drbd_cleanup,
    .match = drbd_match,
    .setup = drbd_setup,
    .teardown = drbd_teardown,
    .postsuspend = drbd_postsuspend,
    .preresume = drbd_preresume,
};
