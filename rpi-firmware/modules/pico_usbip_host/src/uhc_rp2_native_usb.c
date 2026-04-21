/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr USB host controller driver for RP2 native USB host
 * via TinyUSB (Pico SDK).
 */

#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>

#include "uhc_common.h"
#include "tusb.h"

#define DT_DRV_COMPAT raspberrypi_rp2_usb_uhc

LOG_MODULE_REGISTER(uhc_rp2_native, CONFIG_LOG_DEFAULT_LEVEL);

#define RP2_NATIVE_RHPORT 0U
#define RP2_MAX_DEV_ADDR 127U
#define RP2_EP_SLOT_COUNT 32U

struct rp2_native_uhc_data {
	const struct device *dev;
	struct uhc_transfer *active_xfer;
	struct k_work_delayable service_work;
	bool tinyusb_ready;
	bool reset_pending;
	bool transfer_done;
	int transfer_status;
	uint32_t transfer_actual;
	/*
	 * After TinyUSB finishes internal enumeration it clears _dev0.enumerating; Zephyr
	 * may then bus-reset and enumerate at address 0. usbh.c only allows daddr==0
	 * control xfers while _dev0.enumerating is set unless this override is true.
	 */
	bool allow_dev0_xfer;
	bool ep_opened[RP2_MAX_DEV_ADDR + 1U][RP2_EP_SLOT_COUNT];
};

static struct rp2_native_uhc_data rp2_native_priv;

bool tuh_dev0_xfer_override(void)
{
	return rp2_native_priv.allow_dev0_xfer;
}

void tuh_xfer_complete_cb(tuh_xfer_t *xfer);

static int rp2_native_validate_xfer(struct uhc_transfer *xfer)
{
	if (xfer == NULL) {
		LOG_ERR("Rejecting null transfer handle");
		return -EINVAL;
	}

	if (xfer->udev == NULL) {
		LOG_ERR("Rejecting transfer without device context (ep=0x%02x)", xfer->ep);
		return -EINVAL;
	}

	if (xfer->udev->addr > RP2_MAX_DEV_ADDR) {
		LOG_ERR("Rejecting transfer with invalid USB address %u", xfer->udev->addr);
		return -EINVAL;
	}

	/*
	 * Address 0 is the pre-enumeration default; only EP0 (control) may be used
	 * until SET_ADDRESS completes.
	 */
	if (xfer->udev->addr == 0U && USB_EP_GET_IDX(xfer->ep) != 0U) {
		LOG_ERR("Rejecting non-control transfer at address 0 (ep=0x%02x)", xfer->ep);
		return -EINVAL;
	}

	if ((USB_EP_GET_IDX(xfer->ep) != 0U) && (xfer->buf == NULL)) {
		LOG_ERR("Rejecting non-control transfer without data buffer (ep=0x%02x)", xfer->ep);
		return -EINVAL;
	}

	return 0;
}

static uint8_t ep_slot(uint8_t ep_addr)
{
	return (USB_EP_GET_IDX(ep_addr) << 1) | (USB_EP_DIR_IS_IN(ep_addr) ? 1U : 0U);
}

static int tinyusb_result_to_errno(xfer_result_t result)
{
	switch (result) {
	case XFER_RESULT_SUCCESS:
		return 0;
	case XFER_RESULT_STALLED:
		return -EPIPE;
	case XFER_RESULT_TIMEOUT:
		return -ETIMEDOUT;
	default:
		return -EIO;
	}
}

static bool rp2_native_open_data_ep(struct rp2_native_uhc_data *priv, struct uhc_transfer *xfer)
{
	struct usb_device *udev = xfer->udev;
	const uint8_t ep = xfer->ep;
	const uint8_t ep_idx = USB_EP_GET_IDX(ep) & 0x0fU;
	struct usb_ep_descriptor *zephyr_ep = NULL;
	uint8_t slot;

	if (ep_idx == 0U) {
		return true;
	}

	if (USB_EP_DIR_IS_IN(ep)) {
		zephyr_ep = udev->ep_in[ep_idx].desc;
	} else {
		zephyr_ep = udev->ep_out[ep_idx].desc;
	}

	if (zephyr_ep == NULL) {
		LOG_ERR("No endpoint descriptor for dev=%u ep=0x%02x", udev->addr, ep);
		return false;
	}

	slot = ep_slot(ep);
	if (priv->ep_opened[udev->addr][slot]) {
		return true;
	}

	if (!tuh_edpt_open(udev->addr, (const tusb_desc_endpoint_t *)zephyr_ep)) {
		LOG_ERR("tuh_edpt_open failed dev=%u ep=0x%02x", udev->addr, ep);
		return false;
	}

	priv->ep_opened[udev->addr][slot] = true;
	return true;
}

static int rp2_native_submit_xfer(struct rp2_native_uhc_data *priv, struct uhc_transfer *xfer)
{
	struct usb_device *udev = xfer->udev;
	const bool is_ctrl = (USB_EP_GET_IDX(xfer->ep) == 0U);
	tuh_xfer_t tx = {
		.daddr = udev->addr,
		.ep_addr = xfer->ep,
		.complete_cb = tuh_xfer_complete_cb,
		.user_data = 0,
		.buffer = NULL,
		.buflen = 0U,
		.setup = NULL,
	};
	bool ok;

	int vret = rp2_native_validate_xfer(xfer);

	if (vret != 0) {
		return vret;
	}

	priv->transfer_done = false;
	priv->transfer_status = -EIO;
	priv->transfer_actual = 0U;

	if (is_ctrl) {
		/*
		 * Zephyr queues one UHC transfer per control request and expects the
		 * controller to run SETUP → DATA → STATUS without completing the UHC
		 * transfer until the status stage finishes. TinyUSB's tuh_control_xfer
		 * performs that entire sequence in one call, so we submit from SETUP
		 * only. Returning 0 from submit would incorrectly complete the UHC
		 * transfer before any USB traffic (previous bug: empty buffer,
		 * bNumConfigurations appeared as 0).
		 */
		if (xfer->stage != UHC_CONTROL_STAGE_SETUP) {
			LOG_ERR("Unexpected control stage %u (TinyUSB path uses SETUP only)",
				(unsigned int)xfer->stage);
			return -EINVAL;
		}

		/* TinyUSB requires ep_addr == 0; Zephyr uses 0x80 for control-IN. */
		tx.ep_addr = 0U;

		tx.setup = (const tusb_control_request_t *)xfer->setup_pkt;

		if (xfer->buf != NULL && sys_get_le16(&xfer->setup_pkt[6]) != 0U) {
			if (USB_EP_DIR_IS_IN(xfer->ep)) {
				tx.buffer = net_buf_tail(xfer->buf);
				tx.buflen = net_buf_tailroom(xfer->buf);
			} else {
				tx.buffer = xfer->buf->data;
				tx.buflen = xfer->buf->len;
			}
		}

		ok = tuh_control_xfer(&tx);
	} else {
		if (!rp2_native_open_data_ep(priv, xfer)) {
			return -ENOENT;
		}

		if (USB_EP_DIR_IS_IN(xfer->ep)) {
			tx.buffer = net_buf_tail(xfer->buf);
			tx.buflen = net_buf_tailroom(xfer->buf);
		} else {
			tx.buffer = xfer->buf->data;
			tx.buflen = xfer->buf->len;
		}

		ok = tuh_edpt_xfer(&tx);
	}

	if (!ok) {
		LOG_ERR("TinyUSB rejected transfer submission dev=%u ep=0x%02x stage=%u", udev->addr,
			xfer->ep, (unsigned int)xfer->stage);
		return -EIO;
	}

	return 1;
}

static void rp2_native_complete_active(struct rp2_native_uhc_data *priv)
{
	struct uhc_transfer *xfer = priv->active_xfer;

	if (xfer == NULL) {
		return;
	}

	if (priv->transfer_done) {
		if ((xfer->buf != NULL) && USB_EP_DIR_IS_IN(xfer->ep) && (priv->transfer_actual > 0U)) {
			uint32_t room = net_buf_tailroom(xfer->buf);

			if (priv->transfer_actual > room) {
				LOG_ERR("IN transfer overflow: actual=%u tailroom=%u ep=0x%02x",
					(unsigned int)priv->transfer_actual, (unsigned int)room, xfer->ep);
				uhc_xfer_return(priv->dev, xfer, -EOVERFLOW);
				priv->active_xfer = NULL;
				return;
			}
			net_buf_add(xfer->buf, priv->transfer_actual);
		}
		uhc_xfer_return(priv->dev, xfer, priv->transfer_status);
		priv->active_xfer = NULL;
		return;
	}
}

static int rp2_native_lock(const struct device *dev)
{
	return uhc_lock_internal(dev, K_FOREVER);
}

static int rp2_native_unlock(const struct device *dev)
{
	return uhc_unlock_internal(dev);
}

static int rp2_native_probe(const struct device *dev)
{
	struct uhc_data *data = dev->data;

	k_mutex_init(&data->mutex);
	return 0;
}

static void rp2_native_service_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = CONTAINER_OF(work, struct k_work_delayable, work);
	struct rp2_native_uhc_data *priv =
		CONTAINER_OF(dwork, struct rp2_native_uhc_data, service_work);
	struct uhc_transfer *next = NULL;

	if (priv->tinyusb_ready) {
		tuh_task_ext(0U, false);
	}

	if (priv->reset_pending) {
		uhc_submit_event(priv->dev, UHC_EVT_RESETED, 0);
		priv->reset_pending = false;
	}

	rp2_native_complete_active(priv);

	if (priv->active_xfer == NULL) {
		next = uhc_xfer_get_next(priv->dev);
		if (next != NULL) {
			int ret = rp2_native_submit_xfer(priv, next);

			if (ret < 0) {
				uhc_xfer_return(priv->dev, next, ret);
			} else if (ret == 0) {
				uhc_xfer_return(priv->dev, next, 0);
			} else {
				priv->active_xfer = next;
			}
		}
	}

	k_work_schedule(&priv->service_work, K_MSEC(1));
}

static int rp2_native_init(const struct device *dev)
{
	struct rp2_native_uhc_data *priv = uhc_get_private(dev);

	memset(priv, 0, sizeof(*priv));
	priv->dev = dev;
	LOG_INF("RP2 native USB host backend initialized (rhport=%u)", RP2_NATIVE_RHPORT);
	return 0;
}

static int rp2_native_enable(const struct device *dev)
{
	struct rp2_native_uhc_data *priv = uhc_get_private(dev);

	if (priv->dev != dev) {
		LOG_ERR("UHC private state mismatch during enable");
		return -EIO;
	}

	if (!priv->tinyusb_ready) {
		if (!tuh_init(RP2_NATIVE_RHPORT)) {
			LOG_ERR("tuh_init failed for rhport %u", RP2_NATIVE_RHPORT);
			return -EIO;
		}
		priv->tinyusb_ready = true;
		LOG_INF("TinyUSB host initialized on rhport %u", RP2_NATIVE_RHPORT);
	}

	k_work_init_delayable(&priv->service_work, rp2_native_service_work_handler);
	k_work_schedule(&priv->service_work, K_NO_WAIT);
	LOG_INF("Native USB host service work scheduled");
	return 0;
}

static int rp2_native_disable(const struct device *dev)
{
	struct rp2_native_uhc_data *priv = uhc_get_private(dev);

	k_work_cancel_delayable(&priv->service_work);
	priv->allow_dev0_xfer = false;
	if (priv->active_xfer != NULL) {
		LOG_WRN("Cancelling active transfer during disable (ep=0x%02x)", priv->active_xfer->ep);
		uhc_xfer_return(dev, priv->active_xfer, -ESHUTDOWN);
		priv->active_xfer = NULL;
	}
	if (priv->tinyusb_ready) {
		if (!tuh_deinit(RP2_NATIVE_RHPORT)) {
			LOG_WRN("tuh_deinit failed for rhport %u", RP2_NATIVE_RHPORT);
		}
		priv->tinyusb_ready = false;
		LOG_INF("TinyUSB host deinitialized on rhport %u", RP2_NATIVE_RHPORT);
	}
	return 0;
}

static int rp2_native_shutdown(const struct device *dev)
{
	return rp2_native_disable(dev);
}

static int rp2_native_bus_reset(const struct device *dev)
{
	struct rp2_native_uhc_data *priv = uhc_get_private(dev);

	if (!priv->tinyusb_ready) {
		LOG_ERR("Bus reset requested before TinyUSB host init");
		return -EIO;
	}

	LOG_INF("Native USB bus reset start");
	if (!tuh_rhport_reset_bus(RP2_NATIVE_RHPORT, true)) {
		LOG_ERR("Failed to assert bus reset on rhport %u", RP2_NATIVE_RHPORT);
		return -EIO;
	}
	k_sleep(K_MSEC(20));
	if (!tuh_rhport_reset_bus(RP2_NATIVE_RHPORT, false)) {
		LOG_ERR("Failed to release bus reset on rhport %u", RP2_NATIVE_RHPORT);
		return -EIO;
	}

	priv->reset_pending = true;
	/* Zephyr will enumerate at address 0; TinyUSB cleared _dev0.enumerating already. */
	priv->allow_dev0_xfer = true;
	LOG_INF("Native USB bus reset complete");
	return 0;
}

static int rp2_native_sof_enable(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int rp2_native_bus_suspend(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOTSUP;
}

static int rp2_native_bus_resume(const struct device *dev)
{
	ARG_UNUSED(dev);
	return -ENOTSUP;
}

static int rp2_native_enqueue(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct rp2_native_uhc_data *priv = uhc_get_private(dev);
	int vret;

	if (!priv->tinyusb_ready) {
		LOG_WRN("Queueing transfer before TinyUSB init (ep=0x%02x)", xfer ? xfer->ep : 0U);
	}

	vret = rp2_native_validate_xfer(xfer);
	if (vret != 0) {
		return vret;
	}

	return uhc_xfer_append(dev, xfer);
}

static int rp2_native_dequeue(const struct device *dev, struct uhc_transfer *const xfer)
{
	struct rp2_native_uhc_data *priv = uhc_get_private(dev);
	bool aborted = true;

	if (priv->active_xfer == xfer) {
		if (USB_EP_GET_IDX(xfer->ep) == 0U) {
			/*
			 * TinyUSB host control state is shared for EP0 regardless of direction.
			 * Aborting once is sufficient; a second abort call can fail after the first
			 * one transitions control stage to IDLE.
			 */
			if (!tuh_edpt_abort_xfer(xfer->udev->addr, xfer->ep)) {
				LOG_WRN("Abort failed for control dev=%u ep=0x%02x", xfer->udev->addr,
					xfer->ep);
				aborted = false;
			}
		} else {
			if (!tuh_edpt_abort_xfer(xfer->udev->addr, xfer->ep)) {
				LOG_WRN("Abort failed dev=%u ep=0x%02x", xfer->udev->addr, xfer->ep);
				aborted = false;
			}
		}

		/*
		 * TinyUSB abort does not emit a completion callback for this transfer path.
		 * Complete it here so UHC state cannot stall with a dangling active_xfer.
		 */
		if (!aborted) {
			LOG_WRN("Forcing dequeue completion after abort failure dev=%u ep=0x%02x",
				xfer->udev->addr, xfer->ep);
		}
		priv->transfer_done = true;
		priv->transfer_status = -ECONNRESET;
		priv->transfer_actual = 0U;
		uhc_xfer_return(dev, xfer, -ECONNRESET);
		priv->active_xfer = NULL;
		return 0;
	}
	xfer->err = -ECONNRESET;
	return 0;
}

static const struct uhc_api rp2_native_api = {
	.lock = rp2_native_lock,
	.unlock = rp2_native_unlock,
	.init = rp2_native_init,
	.enable = rp2_native_enable,
	.disable = rp2_native_disable,
	.shutdown = rp2_native_shutdown,
	.bus_reset = rp2_native_bus_reset,
	.sof_enable = rp2_native_sof_enable,
	.bus_suspend = rp2_native_bus_suspend,
	.bus_resume = rp2_native_bus_resume,
	.ep_enqueue = rp2_native_enqueue,
	.ep_dequeue = rp2_native_dequeue,
};

void tuh_mount_cb(uint8_t daddr)
{
	tusb_speed_t speed = tuh_speed_get(daddr);
	enum uhc_event_type event = UHC_EVT_DEV_CONNECTED_FS;

	if (rp2_native_priv.dev == NULL) {
		LOG_WRN("Ignoring mount callback without active UHC device (addr=%u)", daddr);
		return;
	}

	if (speed == TUSB_SPEED_LOW) {
		event = UHC_EVT_DEV_CONNECTED_LS;
	}

	LOG_INF("Native root port mount: addr=%u speed=%s", daddr,
		(speed == TUSB_SPEED_LOW) ? "low" : "full");
	uhc_submit_event(rp2_native_priv.dev, event, 0);
}

void tuh_umount_cb(uint8_t daddr)
{
	if (rp2_native_priv.dev == NULL) {
		LOG_WRN("Ignoring unmount callback without active UHC device (addr=%u)", daddr);
		return;
	}

	if (rp2_native_priv.active_xfer != NULL) {
		LOG_WRN("Cancelling active transfer on unmount (dev=%u ep=0x%02x)",
			rp2_native_priv.active_xfer->udev->addr, rp2_native_priv.active_xfer->ep);
		uhc_xfer_return(rp2_native_priv.dev, rp2_native_priv.active_xfer, -ENODEV);
		rp2_native_priv.active_xfer = NULL;
		rp2_native_priv.transfer_done = false;
		rp2_native_priv.transfer_status = -ENODEV;
		rp2_native_priv.transfer_actual = 0U;
	}

	rp2_native_priv.allow_dev0_xfer = false;
	LOG_INF("Native root port unmount: addr=%u", daddr);
	uhc_submit_event(rp2_native_priv.dev, UHC_EVT_DEV_REMOVED, 0);
}

void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr)
{
	ARG_UNUSED(rhport);
	ARG_UNUSED(eventid);
	ARG_UNUSED(in_isr);
}

void tuh_xfer_complete_cb(tuh_xfer_t *xfer)
{
	if (xfer == NULL) {
		LOG_ERR("Received null TinyUSB completion callback");
		rp2_native_priv.transfer_status = -EIO;
		rp2_native_priv.transfer_done = true;
		return;
	}

	rp2_native_priv.transfer_status = tinyusb_result_to_errno(xfer->result);
	rp2_native_priv.transfer_actual = xfer->actual_len;
	rp2_native_priv.transfer_done = true;
	LOG_DBG("TinyUSB xfer complete: daddr=%u ep=0x%02x result=%d actual=%u",
		xfer->daddr, xfer->ep_addr, xfer->result, (unsigned int)xfer->actual_len);
}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
static struct uhc_data rp2_native_data = {
	.priv = &rp2_native_priv,
};

DEVICE_DT_INST_DEFINE(0, rp2_native_probe, NULL, &rp2_native_data, NULL, POST_KERNEL,
		      CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &rp2_native_api);
#endif
