// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>
#include <ddk/protocol/test.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <magenta/listnode.h>

typedef struct test_device {
    mx_device_t device;
    mx_handle_t output;
    mx_handle_t control;
    test_func_t test_func;
    void* cookie;
} test_device_t;

#define get_test_device(dev) containerof(dev, test_device_t, device)

static void test_device_set_test_func(mx_device_t* dev, test_func_t func, void* cookie) {
    test_device_t* device = get_test_device(dev);
    device->test_func = func;
    device->cookie = cookie;
}

static mx_status_t test_device_run_tests(mx_device_t* dev, test_report_t* report, const void* arg, size_t arglen) {
    test_device_t* device = get_test_device(dev);
    if (device->test_func != NULL) {
        return device->test_func(device->cookie, report, arg, arglen);
    } else {
        return ERR_NOT_SUPPORTED;
    }
}

static test_protocol_t test_test_proto = {
    .set_test_func = test_device_set_test_func,
    .run_tests = test_device_run_tests,
};

static ssize_t test_device_ioctl(mx_device_t* dev, uint32_t op, const void* in, size_t inlen, void* out, size_t outlen) {
    switch (op) {
        case IOCTL_TEST_RUN_TESTS:
            printf("test_device_ioctl: run_tests\n");
            if (outlen != sizeof(test_report_t)) {
                return ERR_BUFFER_TOO_SMALL;
            }
            test_device_run_tests(dev, (test_report_t*)out, in, inlen);
            return sizeof(test_report_t);

        case IOCTL_TEST_DESTROY_DEVICE:
            printf("test_device_ioctl: destroy\n");
            device_remove(dev);
            return 0;

        default:
            return ERR_NOT_SUPPORTED;
    }
}

static mx_status_t test_device_release(mx_device_t* dev) {
    printf("test_device_release: dev %p(%s)\n", dev, dev->name);
    test_device_t* device = get_test_device(dev);
    free(device);
    return NO_ERROR;
}

static mx_protocol_device_t test_device_proto = {
    .ioctl = test_device_ioctl,
    .release = test_device_release,
};


#define DEV_TEST "/dev/misc/test"

static ssize_t test_ioctl(mx_device_t* dev, uint32_t op, const void* in, size_t inlen, void* out, size_t outlen) {
    if (op != IOCTL_TEST_CREATE_DEVICE) {
        return ERR_NOT_SUPPORTED;
    }

    char devname[MX_DEVICE_NAME_MAX + 1];
    if (inlen > 0) {
        strncpy(devname, in, sizeof(devname));
    } else {
        strncpy(devname, "testdev", sizeof(devname));
    }

    if (outlen < strlen(devname) + sizeof(DEV_TEST) + 1) {
        return ERR_BUFFER_TOO_SMALL;
    }

    test_device_t* device = calloc(1, sizeof(test_device_t));
    if (device == NULL) {
        return ERR_NO_MEMORY;
    }

    device_init(&device->device, dev->driver, devname, &test_device_proto);
    device->device.protocol_id = MX_PROTOCOL_TEST;
    device->device.protocol_ops = &test_test_proto;

    mx_status_t status;
    if ((status = device_add(&device->device, dev)) != NO_ERROR) {
        printf("added device %s\n", devname);
        free(device);
        return status;
    }

    return snprintf(out, outlen,"%s/%s", DEV_TEST, devname) + 1;
}

static mx_protocol_device_t test_root_proto = {
    .ioctl = test_ioctl,
};

static mx_status_t test_bind(mx_driver_t* drv, mx_device_t* dev, void** cookie) {
    mx_device_t* device;
    if (device_create(&device, drv, "test", &test_root_proto) == NO_ERROR) {
        if (device_add(device, dev) < 0) {
            printf("test: device_add() failed\n");
            free(device);
        }
    }
    return NO_ERROR;
}

mx_driver_t _driver_test = {
    .ops = {
        .bind = test_bind,
    },
};

MAGENTA_DRIVER_BEGIN(_driver_test, "test", "magenta", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, MX_PROTOCOL_MISC_PARENT),
MAGENTA_DRIVER_END(_driver_test)
