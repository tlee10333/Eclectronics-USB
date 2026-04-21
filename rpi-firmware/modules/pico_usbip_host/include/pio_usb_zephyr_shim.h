/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Include before Pico-PIO-USB sources to map SDK helpers to Zephyr.
 */
#pragma once

#include <zephyr/sys/printk.h>

#ifndef printf
#define printf(...) printk(__VA_ARGS__)
#endif
