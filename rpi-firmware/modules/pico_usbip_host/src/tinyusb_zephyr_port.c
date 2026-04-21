/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal TinyUSB/PicoSDK compatibility shims for Zephyr.
 *
 * Pico SDK TinyUSB calls irq_add_shared_handler() to install the USB ISR, then
 * irq_set_enabled(USBCTRL_IRQ, true). The previous no-op shim left NVIC IRQ
 * enabled with no Zephyr ISR registered → unhandled IRQ (RP2350: IRQ 14).
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "hardware/irq.h"

LOG_MODULE_REGISTER(tinyusb_zephyr_port, CONFIG_LOG_DEFAULT_LEVEL);

/*
 * NVIC line for the on-chip USB controller; must match Pico SDK USBCTRL_IRQ
 * (RP2040: 5, RP2350: 14). Taken from devicetree so it matches the active SoC.
 */
#define USBCTRL_NVIC_IRQ DT_IRQN(DT_NODELABEL(usbd))

static irq_handler_t usbctrl_irq_handler;

static void usbctrl_zephyr_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (usbctrl_irq_handler != NULL) {
		usbctrl_irq_handler();
	}
}

/*
 * IRQ_CONNECT expands to a brace block; it must run inside a function, not at
 * file scope (same pattern as Zephyr's usb_dc_rpi_pico.c).
 */
static int tinyusb_zephyr_usb_irq_connect(void)
{
	IRQ_CONNECT(USBCTRL_NVIC_IRQ, 2, usbctrl_zephyr_isr, NULL, 0);
	return 0;
}

SYS_INIT(tinyusb_zephyr_usb_irq_connect, PRE_KERNEL_2, 49);

uint32_t tusb_time_millis_api(void)
{
	return k_uptime_get_32();
}

void irq_add_shared_handler(uint num, irq_handler_t handler, uint8_t order_priority)
{
	ARG_UNUSED(order_priority);

	if ((unsigned int)num != (unsigned int)USBCTRL_NVIC_IRQ) {
		LOG_WRN("irq_add_shared_handler: unsupported irq %u (expected %u)", (unsigned int)num,
			(unsigned int)USBCTRL_NVIC_IRQ);
		return;
	}

	usbctrl_irq_handler = handler;
	LOG_DBG("USBCTRL ISR registered (%p)", handler);
}

void irq_remove_handler(uint num, irq_handler_t handler)
{
	if ((unsigned int)num != (unsigned int)USBCTRL_NVIC_IRQ) {
		return;
	}

	if (usbctrl_irq_handler == handler) {
		usbctrl_irq_handler = NULL;
		LOG_DBG("USBCTRL ISR removed");
	}
}

void irq_set_enabled(uint num, bool enabled)
{
	if (enabled) {
		irq_enable((unsigned int)num);
	} else {
		irq_disable((unsigned int)num);
	}
}
