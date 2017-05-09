// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/binding.h>
#include <ddk/protocol/bcm-bus.h>
#include <ddk/protocol/display.h>

#include <magenta/syscalls.h>
#include <magenta/assert.h>

#include <bcm/bcm28xx.h>
#include <bcm/ioctl.h>

typedef struct {
    mx_device_t* mxdev;
    mx_device_t* busdev;
    bcm_bus_protocol_t* bus_proto;
} bcm_display_t;

typedef struct {
    uint32_t phys_width;    //request
    uint32_t phys_height;   //request
    uint32_t virt_width;    //request
    uint32_t virt_height;   //request
    uint32_t pitch;         //response
    uint32_t depth;         //request
    uint32_t virt_x_offs;   //request
    uint32_t virt_y_offs;   //request
    uint32_t fb_p;          //response
    uint32_t fb_size;       //response
} bcm_fb_desc_t;

static mx_display_info_t disp_info;

static bcm_fb_desc_t bcm_vc_framebuffer;
static uint8_t* vc_framebuffer = (uint8_t*)NULL;

static mx_status_t vc_set_mode(mx_device_t* dev, mx_display_info_t* info) {
    return NO_ERROR;
}

static mx_status_t vc_get_mode(mx_device_t* dev, mx_display_info_t* info) {
    assert(info);
    memcpy(info, &disp_info, sizeof(mx_display_info_t));
    return NO_ERROR;
}

static mx_status_t vc_get_framebuffer(mx_device_t* dev, void** framebuffer) {
    assert(framebuffer);
    (*framebuffer) = vc_framebuffer;
    return NO_ERROR;
}

void vc_flush_framebuffer(mx_device_t* dev) {
    mx_cache_flush(vc_framebuffer, bcm_vc_framebuffer.fb_size,
                   MX_CACHE_FLUSH_DATA);
}

static mx_display_protocol_t vc_display_proto = {
    .set_mode = vc_set_mode,
    .get_mode = vc_get_mode,
    .get_framebuffer = vc_get_framebuffer,
    .flush = vc_flush_framebuffer
};

static mx_protocol_device_t empty_device_proto = {
    .version = DEVICE_OPS_VERSION,
};

static mx_status_t bcm_vc_get_framebuffer(bcm_display_t* display, bcm_fb_desc_t* fb_desc) {
    mx_status_t ret = NO_ERROR;
    iotxn_t* txn;

    if (!vc_framebuffer) {
        // buffer needs to be aligned on 16 byte boundary, pad the alloc to make sure we have room to adjust
        const size_t txnsize = sizeof(bcm_fb_desc_t) + 16;
        ret = iotxn_alloc(&txn, IOTXN_ALLOC_CONTIGUOUS | IOTXN_ALLOC_POOL, txnsize);
        if (ret < 0)
            return ret;

        iotxn_physmap(txn);
        MX_DEBUG_ASSERT(txn->phys_count == 1);
        mx_paddr_t phys = iotxn_phys(txn);

        // calculate offset in buffer that will provide 16 byte alignment (physical)
        uint32_t offset = (16 - (phys % 16)) % 16;

        iotxn_copyto(txn, fb_desc, sizeof(bcm_fb_desc_t), offset);
        iotxn_cacheop(txn, IOTXN_CACHE_CLEAN, 0, txnsize);

        ret = display->bus_proto->set_framebuffer(display->busdev, phys + offset);
        if (ret != NO_ERROR)
            return ret;

        iotxn_cacheop(txn, IOTXN_CACHE_INVALIDATE, 0, txnsize);
        iotxn_copyfrom(txn, &bcm_vc_framebuffer, sizeof(bcm_fb_desc_t), offset);

        uintptr_t page_base;

        // map framebuffer into userspace
        mx_mmap_device_memory(
            get_root_resource(),
            bcm_vc_framebuffer.fb_p & 0x3fffffff, bcm_vc_framebuffer.fb_size,
            MX_CACHE_POLICY_CACHED, &page_base);
        vc_framebuffer = (uint8_t*)page_base;
        memset(vc_framebuffer, 0x00, bcm_vc_framebuffer.fb_size);

        iotxn_release(txn);
    }
    memcpy(fb_desc, &bcm_vc_framebuffer, sizeof(bcm_fb_desc_t));
    return sizeof(bcm_fb_desc_t);
}

mx_status_t bcm_display_bind(mx_driver_t* driver, mx_device_t* parent, void** cookie) {
    bcm_display_t* display = calloc(1, sizeof(bcm_display_t));
    if (!display) {
        return ERR_NO_MEMORY;
    }

    display->busdev = parent;
    if (device_op_get_protocol(parent, MX_PROTOCOL_BCM_BUS, (void**)&display->bus_proto)) {
        free(display);
        return ERR_NOT_SUPPORTED;
    }

    bcm_fb_desc_t framebuff_descriptor;

    // For now these are set to work with the rpi 5" lcd didsplay
    // TODO: add a mechanisms to specify and change settings outside the driver

    framebuff_descriptor.phys_width = 800;
    framebuff_descriptor.phys_height = 480;
    framebuff_descriptor.virt_width = 800;
    framebuff_descriptor.virt_height = 480;
    framebuff_descriptor.pitch = 0;
    framebuff_descriptor.depth = 32;
    framebuff_descriptor.virt_x_offs = 0;
    framebuff_descriptor.virt_y_offs = 0;
    framebuff_descriptor.fb_p = 0;
    framebuff_descriptor.fb_size = 0;

    bcm_vc_get_framebuffer(display, &framebuff_descriptor);

    disp_info.format = MX_PIXEL_FORMAT_ARGB_8888;
    disp_info.width = 800;
    disp_info.height = 480;
    disp_info.stride = 800;

    mx_set_framebuffer(get_root_resource(), vc_framebuffer,
                       bcm_vc_framebuffer.fb_size, disp_info.format,
                       disp_info.width, disp_info.height, disp_info.stride);

    device_add_args_t vc_fbuff_args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "bcm-vc-fbuff",
        .ctx = display,
        .driver = driver,
        .ops = &empty_device_proto,
        .proto_id = MX_PROTOCOL_DISPLAY,
        .proto_ops = &vc_display_proto,
    };

    mx_status_t status = device_add(parent, &vc_fbuff_args, &display->mxdev);
    if (status != NO_ERROR) {
        free(display);
    }
    return status;
}

static mx_driver_ops_t bcm_display_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = bcm_display_bind,
};

MAGENTA_DRIVER_BEGIN(bcm_display, bcm_display_driver_ops, "magenta", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, MX_PROTOCOL_SOC),
    BI_ABORT_IF(NE, BIND_SOC_VID, SOC_VID_BROADCOMM),
    BI_MATCH_IF(EQ, BIND_SOC_DID, SOC_DID_BROADCOMM_DISPLAY),
MAGENTA_DRIVER_END(bcm_display)