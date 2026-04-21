/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr USB host controller driver for RP2350 using Pico-PIO-USB.
 */

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_ch9.h>

#include "uhc_common.h"
#include "pio_usb.h"
#include "pio_usb_ll.h"
#include "usb_definitions.h"

#define DT_DRV_COMPAT raspberrypi_rp2350_pio_usb_uhc

LOG_MODULE_REGISTER(uhc_rp2350_pio, CONFIG_UHC_RP2350_PIO_USB_LOG_LEVEL);

struct pio_uhc_config {
	struct gpio_dt_spec dp;
	uint8_t pio_tx_num;
	uint8_t pio_rx_num;
	uint8_t dma_tx_ch;
	uint8_t sm_tx;
	uint8_t sm_rx;
	uint8_t sm_eop;
	bool pinout_dpdm;
};

struct pio_uhc_data {
	const struct device *dev;
	struct uhc_transfer *last_xfer;
	bool xfer_started;
	struct k_work_delayable frame_work;
};

static struct pio_uhc_data uhc_pio_priv;

static inline endpoint_t *pio_find_ep(uint8_t root_idx, uint8_t device_address, uint8_t ep_address)
{
	for (int ep_pool_idx = 0; ep_pool_idx < PIO_USB_EP_POOL_CNT; ep_pool_idx++) {
		endpoint_t *ep = PIO_USB_ENDPOINT(ep_pool_idx);

		if ((ep->root_idx == root_idx) && (ep->dev_addr == device_address) && ep->size &&
		    ((ep->ep_num == ep_address) ||
		     (((ep_address & 0x7f) == 0) && ((ep->ep_num & 0x7f) == 0)))) {
			return ep;
		}
	}
	return NULL;
}

static void pack_ep_desc(endpoint_descriptor_t *out, const struct usb_ep_descriptor *in)
{
	out->length = in->bLength;
	out->type = in->bDescriptorType;
	out->epaddr = in->bEndpointAddress;
	out->attr = in->bmAttributes;
	out->max_size[0] = in->wMaxPacketSize & 0xff;
	out->max_size[1] = (in->wMaxPacketSize >> 8) & 0xff;
	out->interval = in->bInterval;
}

static int pio_open_ep_for_xfer(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct usb_device *const udev = xfer->udev;
	const uint8_t ep = xfer->ep;
	const uint8_t ep_idx = USB_EP_GET_IDX(ep) & 0x0fU;
	struct usb_ep_descriptor *zephyr_ep;

	if (ep_idx == 0) {
		static endpoint_descriptor_t ep0;

		ep0.length = 7U;
		ep0.type = USB_DESC_ENDPOINT;
		ep0.epaddr = USB_CONTROL_EP_OUT;
		ep0.attr = USB_EP_TYPE_CONTROL;
		ep0.max_size[0] = udev->dev_desc.bMaxPacketSize0;
		ep0.max_size[1] = 0;
		ep0.interval = 0;
		return pio_usb_host_endpoint_open(0, udev->addr, (const uint8_t *)&ep0, false) ? 0 : -ENOMEM;
	}

	if (USB_EP_DIR_IS_IN(ep)) {
		zephyr_ep = udev->ep_in[ep_idx].desc;
	} else {
		zephyr_ep = udev->ep_out[ep_idx].desc;
	}
	if (zephyr_ep == NULL) {
		LOG_ERR("No descriptor for ep 0x%02x", ep);
		return -ENOENT;
	}

	endpoint_descriptor_t ped;

	pack_ep_desc(&ped, zephyr_ep);
	return pio_usb_host_endpoint_open(0, udev->addr, (const uint8_t *)&ped, false) ? 0 : -ENOMEM;
}

static int pio_start_xfer(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct pio_uhc_data *priv = uhc_get_private(dev);
	struct usb_device *const udev = xfer->udev;
	int ret;

	ret = pio_open_ep_for_xfer(dev, xfer);
	if (ret) {
		return ret;
	}

	if (USB_EP_GET_IDX(xfer->ep) == 0) {
		if (xfer->stage == UHC_CONTROL_STAGE_SETUP) {
			if (!pio_usb_host_send_setup(0, udev->addr, xfer->setup_pkt)) {
				return -EIO;
			}
		} else if (xfer->stage == UHC_CONTROL_STAGE_DATA) {
			if (xfer->buf == NULL) {
				return -EINVAL;
			}
			if (USB_EP_DIR_IS_IN(xfer->ep)) {
				const uint16_t len = net_buf_tailroom(xfer->buf);

				if (!pio_usb_host_endpoint_transfer(0, udev->addr, USB_CONTROL_EP_IN,
								    net_buf_tail(xfer->buf), len)) {
					return -EIO;
				}
			} else {
				if (!pio_usb_host_endpoint_transfer(0, udev->addr, USB_CONTROL_EP_OUT,
								    xfer->buf->data, xfer->buf->len)) {
					return -EIO;
				}
			}
		} else if (xfer->stage == UHC_CONTROL_STAGE_STATUS) {
			const uint8_t ep = USB_EP_DIR_IS_IN(xfer->ep) ? USB_CONTROL_EP_OUT :
									USB_CONTROL_EP_IN;

			if (!pio_usb_host_endpoint_transfer(0, udev->addr, ep, NULL, 0)) {
				return -EIO;
			}
		} else {
			return -EINVAL;
		}
	} else {
		struct net_buf *buf = xfer->buf;

		if (buf == NULL) {
			return -EINVAL;
		}
		if (USB_EP_DIR_IS_IN(xfer->ep)) {
			if (!pio_usb_host_endpoint_transfer(0, udev->addr, xfer->ep,
							    net_buf_tail(buf), net_buf_tailroom(buf))) {
				return -EIO;
			}
		} else {
			if (!pio_usb_host_endpoint_transfer(0, udev->addr, xfer->ep,
							    buf->data, buf->len)) {
				return -EIO;
			}
		}
	}

	priv->last_xfer = xfer;
	priv->xfer_started = true;
	return 0;
}

static void pio_finish_xfer(const struct device *dev, struct uhc_transfer *const xfer, int err)
{
	struct pio_uhc_data *priv = uhc_get_private(dev);

	priv->last_xfer = NULL;
	priv->xfer_started = false;
	uhc_xfer_return(dev, xfer, err);
}

static void pio_try_complete_active(const struct device *dev)
{
	struct pio_uhc_data *priv = uhc_get_private(dev);
	struct uhc_transfer *const xfer = priv->last_xfer;

	if (xfer == NULL || !priv->xfer_started) {
		return;
	}

	endpoint_t *ep = pio_find_ep(0, xfer->udev->addr, xfer->ep);

	if (ep == NULL) {
		return;
	}

	if (ep->has_transfer) {
		return;
	}

	if (USB_EP_GET_IDX(xfer->ep) == 0) {
		if (xfer->stage == UHC_CONTROL_STAGE_DATA && xfer->buf &&
		    USB_EP_DIR_IS_IN(xfer->ep)) {
			net_buf_add(xfer->buf, ep->actual_len);
		}
	} else if (xfer->buf != NULL && USB_EP_DIR_IS_IN(xfer->ep)) {
		net_buf_add(xfer->buf, ep->actual_len);
	}

	pio_finish_xfer(dev, xfer, 0);
}

static void pio_handle_root_events(const struct device *dev)
{
	root_port_t *root = PIO_USB_ROOT_PORT(0);

	if (root->event == EVENT_CONNECT) {
		enum uhc_event_type t = root->is_fullspeed ? UHC_EVT_DEV_CONNECTED_FS :
							     UHC_EVT_DEV_CONNECTED_LS;
		LOG_INF("Root port connect detected (%s-speed)",
			root->is_fullspeed ? "full" : "low");

		uhc_submit_event(dev, t, 0);
		root->event = EVENT_NONE;
	} else if (root->event == EVENT_DISCONNECT) {
		LOG_INF("Root port disconnect detected");
		uhc_submit_event(dev, UHC_EVT_DEV_REMOVED, 0);
		root->event = EVENT_NONE;
	}
}

static void frame_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
	struct pio_uhc_data *priv = CONTAINER_OF(dwork, struct pio_uhc_data, frame_work);
	const struct device *dev = priv->dev;

	pio_usb_host_frame();
	pio_handle_root_events(dev);
	pio_try_complete_active(dev);

	if (!priv->last_xfer) {
		struct uhc_transfer *next = uhc_xfer_get_next(dev);

		if (next) {
			if (pio_start_xfer(dev, next)) {
				uhc_xfer_return(dev, next, -EIO);
			}
		}
	}

	k_work_schedule(&priv->frame_work, K_MSEC(1));
}

static int pio_uhc_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int pio_uhc_unlock(const struct device *dev)
{
	return uhc_unlock_internal(dev);
}

static int pio_uhc_probe(const struct device *dev)
{
	struct uhc_data *data = dev->data;

	k_mutex_init(&data->mutex);
	return 0;
}

static int pio_uhc_init(const struct device *dev)
{
	const struct pio_uhc_config *cfg = dev->config;
	pio_usb_configuration_t pcfg = {
		.pin_dp = cfg->dp.pin,
		.pio_tx_num = cfg->pio_tx_num,
		.sm_tx = cfg->sm_tx,
		.tx_ch = cfg->dma_tx_ch,
		.pio_rx_num = cfg->pio_rx_num,
		.sm_rx = cfg->sm_rx,
		.sm_eop = cfg->sm_eop,
		.alarm_pool = NULL,
		.debug_pin_rx = -1,
		.debug_pin_eop = -1,
		.skip_alarm_pool = true,
		.pinout = cfg->pinout_dpdm ? PIO_USB_PINOUT_DPDM : PIO_USB_PINOUT_DMDP,
	};

	if (!gpio_is_ready_dt(&cfg->dp)) {
		LOG_ERR("D+ GPIO not ready");
		return -ENODEV;
	}

	LOG_INF("PIO USB host init: dp_pin=%u pio_tx=%u pio_rx=%u dma_tx=%u sm_tx=%u sm_rx=%u sm_eop=%u pinout=%s",
		cfg->dp.pin, cfg->pio_tx_num, cfg->pio_rx_num, cfg->dma_tx_ch,
		cfg->sm_tx, cfg->sm_rx, cfg->sm_eop, cfg->pinout_dpdm ? "DPDM" : "DMDP");

	pio_usb_host_init(&pcfg);
	if (pio_usb_host_add_port(cfg->dp.pin, pcfg.pinout) != 0) {
		LOG_ERR("pio_usb_host_add_port failed");
		return -EIO;
	}

	LOG_INF("PIO USB host port added on D+ pin %u", cfg->dp.pin);

	return 0;
}

static int pio_uhc_enable(const struct device *dev)
{
	struct pio_uhc_data *priv = uhc_get_private(dev);

	priv->dev = dev;
	k_work_init_delayable(&priv->frame_work, frame_work_handler);
	k_work_schedule(&priv->frame_work, K_NO_WAIT);
	LOG_INF("PIO USB host frame scheduler started");
	return 0;
}

static int pio_uhc_disable(const struct device *dev)
{
	struct pio_uhc_data *priv = uhc_get_private(dev);

	k_work_cancel_delayable(&priv->frame_work);
	return 0;
}

static int pio_uhc_shutdown(const struct device *dev)
{
	return pio_uhc_disable(dev);
}

static int pio_uhc_bus_reset(const struct device *dev)
{
	LOG_INF("PIO USB bus reset start");
	pio_usb_host_port_reset_start(0);
	k_sleep(K_MSEC(20));
	pio_usb_host_port_reset_end(0);
	LOG_INF("PIO USB bus reset complete");
	uhc_submit_event(dev, UHC_EVT_RESETED, 0);
	return 0;
}

static int pio_uhc_sof_enable(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int pio_uhc_bus_suspend(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOTSUP;
}

static int pio_uhc_bus_resume(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOTSUP;
}

static int pio_uhc_enqueue(const struct device *dev, struct uhc_transfer *const xfer)
{
	return uhc_xfer_append(dev, xfer);
}

static int pio_uhc_dequeue(const struct device *dev, struct uhc_transfer *const xfer)
{
	ARG_UNUSED(dev);
	xfer->err = -ECONNRESET;
	return 0;
}

static const struct uhc_api pio_uhc_api = {
	.lock = pio_uhc_lock,
	.unlock = pio_uhc_unlock,
	.init = pio_uhc_init,
	.enable = pio_uhc_enable,
	.disable = pio_uhc_disable,
	.shutdown = pio_uhc_shutdown,
	.bus_reset = pio_uhc_bus_reset,
	.sof_enable = pio_uhc_sof_enable,
	.bus_suspend = pio_uhc_bus_suspend,
	.bus_resume = pio_uhc_bus_resume,
	.ep_enqueue = pio_uhc_enqueue,
	.ep_dequeue = pio_uhc_dequeue,
};

static struct uhc_data pio_uhc_data = {
	.priv = &uhc_pio_priv,
};

static const struct pio_uhc_config pio_uhc_cfg = {
	.dp = GPIO_DT_SPEC_INST_GET(0, dp_gpios),
	.pio_tx_num = DT_INST_PROP(0, pio_tx_instance),
	.pio_rx_num = DT_INST_PROP(0, pio_rx_instance),
	.dma_tx_ch = DT_INST_PROP(0, dma_tx_channel),
	.sm_tx = DT_INST_PROP(0, sm_tx),
	.sm_rx = DT_INST_PROP(0, sm_rx),
	.sm_eop = DT_INST_PROP(0, sm_eop),
	.pinout_dpdm = DT_INST_PROP(0, pinout_dpdm),
};

DEVICE_DT_INST_DEFINE(0, pio_uhc_probe, NULL, &pio_uhc_data, &pio_uhc_cfg, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &pio_uhc_api);
