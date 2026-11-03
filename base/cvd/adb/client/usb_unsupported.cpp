/*
 * Copyright (C) 2025 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// "Native" USB backend for hosts that only have the libusb one (e.g. the
// BSDs). Only used when libusb is disabled with ADB_LIBUSB=0.

#define TRACE_TAG USB

#include "sysdeps.h"

#include "client/usb.h"

#include <errno.h>

#include "adb.h"
#include "transport.h"

struct usb_handle {};

void usb_init() {
    LOG(WARNING) << "no native USB backend on this platform, set ADB_LIBUSB=1";
    adb_notify_device_scan_complete();
}

void usb_cleanup() {
    if (is_libusb_enabled()) {
        close_usb_devices();
    }
}

int usb_write(usb_handle*, const void*, int) {
    errno = ENOSYS;
    return -1;
}

int usb_read(usb_handle*, void*, int) {
    errno = ENOSYS;
    return -1;
}

int usb_close(usb_handle*) {
    return 0;
}

void usb_reset(usb_handle*) {}

void usb_kick(usb_handle*) {}

size_t usb_get_max_packet_size(usb_handle*) {
    return 0;
}
