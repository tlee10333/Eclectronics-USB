#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <errno.h>

#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi_mgmt.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_INF);


static struct net_if *wifi_sap_iface(void)
{
	return net_if_get_wifi_sap();
}

static int configure_dhcpv4_server(struct net_if *iface, const char *addr_str)
{
	struct net_in_addr pool_base;
	struct net_in_addr addr;
	int ret;

	ret = net_addr_pton(NET_AF_INET, addr_str, &addr);
	if (ret) {
		LOG_ERR("Invalid address %s", addr_str);
		return ret;
	}

	pool_base = addr;
	pool_base.s4_addr[3] += 1U; /* First lease after AP address (e.g. .1 -> pool from .2) */

	return net_dhcpv4_server_start(iface, &pool_base);
}

static int configure_dhcpv4_server_with_retry(struct net_if *iface, const char *addr_str)
{
	int ret;

	for (int attempt = 0; attempt < 10; attempt++) {
		ret = configure_dhcpv4_server(iface, addr_str);
		if (ret != -ENOENT) {
			return ret;
		}

		LOG_WRN("DHCPv4 start deferred (iface not ready), attempt %d/10", attempt + 1);
		k_sleep(K_MSEC(200));
	}

	return ret;
}

static bool wait_for_iface_up(struct net_if *iface, k_timeout_t timeout)
{
	int64_t end_ms = k_uptime_get() + k_ticks_to_ms_floor64(timeout.ticks);

	while (k_uptime_get() < end_ms) {
		if (net_if_is_admin_up(iface) && net_if_is_up(iface)) {
			return true;
		}

		k_sleep(K_MSEC(100));
	}

	return false;
}

static void log_iface_identity(const char *tag, struct net_if *iface)
{
	char if_name[16] = { 0 };
	char src_buf[NET_IPV4_ADDR_LEN];
	char mask_buf[NET_IPV4_ADDR_LEN];
	const struct net_in_addr *src = NULL;
	struct net_in_addr probe_addr;
	struct net_in_addr mask;
	int ret;

	if (iface == NULL) {
		LOG_ERR("%s: iface is NULL", tag);
		return;
	}

	ret = net_if_get_name(iface, if_name, sizeof(if_name));
	if (ret < 0) {
		LOG_WRN("%s: iface=%p idx=%d admin=%d up=%d name_err=%d",
			tag, iface, net_if_get_by_iface(iface),
			net_if_is_admin_up(iface), net_if_is_up(iface), ret);
		return;
	}

	(void)net_addr_pton(NET_AF_INET, "192.168.4.2", &probe_addr);
	src = net_if_ipv4_select_src_addr(iface, &probe_addr);
	mask = src ? net_if_ipv4_get_netmask_by_addr(iface, src) : (struct net_in_addr){ 0 };
	if (src) {
		(void)net_addr_ntop(NET_AF_INET, src, src_buf, sizeof(src_buf));
		(void)net_addr_ntop(NET_AF_INET, &mask, mask_buf, sizeof(mask_buf));
	}

	LOG_INF("%s: iface=%p idx=%d name=%s admin=%d up=%d src=%s mask=%s",
		tag, iface, net_if_get_by_iface(iface), if_name,
		net_if_is_admin_up(iface), net_if_is_up(iface),
		src ? src_buf : "none",
		src ? mask_buf : "none");
}

static int wifi_start_softap(void)
{
	struct net_if *iface = wifi_sap_iface();
	struct wifi_connect_req_params ap = {
		.ssid = (const uint8_t *)CONFIG_PICO_USBIP_AP_SSID,
		.ssid_length = sizeof(CONFIG_PICO_USBIP_AP_SSID) - 1U,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
	};

	if (iface == NULL) {
		LOG_ERR("Wi-Fi SAP interface not available");
		return -ENODEV;
	}

	if (sizeof(CONFIG_PICO_USBIP_AP_PSK) == 1) {
		ap.security = WIFI_SECURITY_TYPE_NONE;
		ap.psk = NULL;
		ap.psk_length = 0U;
	} else {
		ap.security = WIFI_SECURITY_TYPE_PSK;
		ap.psk = (const uint8_t *)CONFIG_PICO_USBIP_AP_PSK;
		ap.psk_length = sizeof(CONFIG_PICO_USBIP_AP_PSK) - 1U;
	}

	LOG_INF("Starting SoftAP '%s' (channel any)", CONFIG_PICO_USBIP_AP_SSID);

	int ret = net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &ap, sizeof(ap));
	if (ret == 0) {
		return 0;
	}

	ap.channel = 1U;
	LOG_WRN("SoftAP(any) failed: %d, retrying on channel 1", ret);
	return net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &ap, sizeof(ap));
}

static void vbus_enable_at_boot(void)
{
#if DT_NODE_HAS_PROP(DT_PATH(zephyr_user), vbus_gpios)
	const struct gpio_dt_spec vbus = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), vbus_gpios);

	if (!gpio_is_ready_dt(&vbus)) {
		LOG_ERR("VBUS GPIO controller not ready");
		return;
	}

	if (gpio_pin_configure_dt(&vbus, GPIO_OUTPUT_ACTIVE) != 0) {
		LOG_ERR("VBUS GPIO configure failed");
		return;
	}

	LOG_INF("VBUS enable asserted on %s pin %u", vbus.port->name, vbus.pin);
#else
	LOG_WRN("No vbus-gpios in devicetree; enable VBUS in hardware or add zephyr_user,vbus-gpios");
#endif
}

static void wait_for_console_connection(void)
{
	const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0U;
	int ret;

	if (!device_is_ready(console)) {
		LOG_ERR("Console device is not ready; continuing without DTR wait");
		return;
	}

	LOG_INF("Waiting for USB serial terminal (DTR) before Wi-Fi init");

	while (true) {
		ret = uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
		if ((ret == 0) && (dtr != 0U)) {
			break;
		}

		if ((ret == -ENOTSUP) || (ret == -ENOSYS)) {
			LOG_WRN("Console does not support line control; skipping DTR wait");
			return;
		}

		k_sleep(K_MSEC(200));
	}

	LOG_INF("USB serial terminal connected");
}

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
static const struct gpio_dt_spec heartbeat_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool heartbeat_led_enabled;
static bool heartbeat_led_state;

static void heartbeat_led_init(void)
{
	if (!gpio_is_ready_dt(&heartbeat_led)) {
		LOG_WRN("LED device not ready; heartbeat disabled");
		return;
	}

	if (gpio_pin_configure_dt(&heartbeat_led, GPIO_OUTPUT_INACTIVE) != 0) {
		LOG_WRN("LED pin configure failed; heartbeat disabled");
		return;
	}

	heartbeat_led_enabled = true;
	heartbeat_led_state = false;
}

static void heartbeat_led_toggle(void)
{
	if (!heartbeat_led_enabled) {
		return;
	}

	(void)gpio_pin_set_dt(&heartbeat_led, heartbeat_led_state);
	heartbeat_led_state = !heartbeat_led_state;
}

#endif

int main(void)
{
	k_sleep(K_SECONDS(1));
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
	heartbeat_led_init();
#endif
	struct net_if *ap;
	int ret;
	wait_for_console_connection();
	vbus_enable_at_boot();

	ap = wifi_sap_iface();
	if (ap == NULL) {
		LOG_ERR("No SAP iface");
		while (true) {
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
			heartbeat_led_toggle();
#endif
			k_sleep(K_MSEC(120));
		}
	}
	log_iface_identity("SAP(before AP enable)", ap);

	struct net_in_addr ap_addr;
	struct net_in_addr netmask;

	ret = wifi_start_softap();
	if (ret) {
		LOG_ERR("SoftAP failed: %d", ret);
	} else {
		LOG_INF("SoftAP enable accepted");
	}
	ap = wifi_sap_iface();
	if (ap == NULL) {
		LOG_ERR("SAP iface unavailable after SoftAP enable");
	}
	log_iface_identity("SAP(after AP enable)", ap);

	net_addr_pton(NET_AF_INET, "192.168.4.1", &ap_addr);
	net_addr_pton(NET_AF_INET, "255.255.255.0", &netmask);

	(void)net_if_up(ap);
	if (!wait_for_iface_up(ap, K_SECONDS(3))) {
		LOG_WRN("AP iface not fully up yet (admin:%d link:%d)",
			net_if_is_admin_up(ap), net_if_is_up(ap));
	}
	log_iface_identity("SAP(before DHCP start)", ap);

	net_if_ipv4_set_gw(ap, &ap_addr);
	if (net_if_ipv4_addr_add(ap, &ap_addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("Failed to set AP IPv4 address");
	}
	if (!net_if_ipv4_set_netmask_by_addr(ap, &ap_addr, &netmask)) {
		LOG_ERR("Failed to set AP netmask");
	}
	LOG_INF("AP IPv4 configured to 192.168.4.1/24");

	ret = configure_dhcpv4_server_with_retry(ap, "192.168.4.1");
	if (ret) {
		LOG_ERR("DHCPv4 server failed: %d", ret);
	}

	LOG_INF("USB/IP runs on port 3240; connect to Wi-Fi '%s' then use usbip on Linux",
		CONFIG_PICO_USBIP_AP_SSID);
	while (true) {
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
		heartbeat_led_toggle();
#endif
		k_sleep(K_MSEC(300));
	}
}
