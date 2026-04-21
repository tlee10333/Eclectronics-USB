/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pico-PIO-USB declares this in the header; TinyUSB provides a task loop.
 * Zephyr drives the host via pio_usb_host_frame() on a timer instead.
 */
void pio_usb_host_task(void)
{
}
