/**
 * RP2040 USB CDC SPI Bridge
 *
 * - USB CDC is treated as a byte stream
 * - SPI runs as a slave, continuously clocked by master
 * - IRQ does FIFO only
 */

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/irq.h"
#include "tusb.h"

//  Configuration 

#define SPI_INST       spi0
#define SPI_RX_PIN     16  /* MISO */
#define SPI_CLK_PIN    18
#define SPI_TX_PIN     19  /* MOSI */
#define SPI_CS_PIN     17

#define LED_PIN        25

#define USB_RX_BUF     1024
#define SPI_RX_BUF     1024
#define SPI_TX_BUF     1024

#define FRAME_HDR      2
#define MAX_PAYLOAD    512
#define SPI_IDLE_BYTE  0x00

//DEBUG DELETE LATER
#define DEBUG_ECHO_SPI 1

//  State 

// USB RX stream buffer
static uint8_t  usb_rx_buf[USB_RX_BUF];
static uint32_t usb_rx_len = 0;

// SPI RX stream buffer
static uint8_t  spi_rx_buf[SPI_RX_BUF];
static uint32_t spi_rx_len = 0;
static bool     spi_frame_ready = false;

// SPI TX (single-slot)
static uint8_t  spi_tx_buf[SPI_TX_BUF];
static volatile uint32_t spi_tx_len = 0;
static volatile uint32_t spi_tx_pos = 0;

// Heartbeat LED
static uint32_t last_led_toggle = 0;
static bool led_on = false;


static inline uint16_t rd_u16(const uint8_t *b) {
    return ((uint16_t)b[0] << 8) | b[1];
}


static void led_update(void) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    if (now - last_led_toggle >= 500) {
        led_on = !led_on;
        gpio_put(LED_PIN, led_on);
        last_led_toggle = now;
    }
}


//DEBUG OONLY DELETE LATER
#if DEBUG_ECHO_SPI
static void debug_echo_spi_tx_to_rx(uint16_t frame_len) {
    // Only echo if RX buffer is empty to avoid overwriting real SPI data
    if (spi_rx_len == 0 && frame_len <= SPI_RX_BUF) {
        memcpy(spi_rx_buf, spi_tx_buf, frame_len);
        spi_rx_len = frame_len;
        spi_frame_ready = true;
    }
}
#endif


//  SPI IRQ (FIFO ONLY)
static void __isr spi_irq(void) {
    spi_hw_t *hw = spi_get_hw(SPI_INST);

    // RX
    while (spi_is_readable(SPI_INST)) {
        uint8_t b = hw->dr & 0xFF;
        if (spi_rx_len < SPI_RX_BUF) {
            spi_rx_buf[spi_rx_len++] = b;
        }
    }

    // TX
    while (spi_is_writable(SPI_INST)) {
        uint8_t out = SPI_IDLE_BYTE;
        if (spi_tx_pos < spi_tx_len) {
            out = spi_tx_buf[spi_tx_pos++];
        }
        hw->dr = out;
    }

    hw->icr = 0x3;
}

//  Init 

static void led_init(void) {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, false);
}

static void spi_slave_init(void) {
    // Baudrate ignored in slave mode; clock driven by SPI master
    spi_init(SPI_INST, 1 * 1000 * 1000);
    spi_set_slave(SPI_INST, true);

    gpio_set_function(SPI_RX_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_TX_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_CLK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI_CS_PIN, GPIO_FUNC_SPI);

    spi_get_hw(SPI_INST)->imsc = SPI_SSPIMSC_RXIM_BITS;
    irq_set_exclusive_handler(SPI0_IRQ, spi_irq);
    irq_set_enabled(SPI0_IRQ, true);
}

//  USB CDC 

void tud_cdc_rx_cb(uint8_t itf) {
    (void)itf;
    uint32_t n = tud_cdc_read(
        usb_rx_buf + usb_rx_len,
        USB_RX_BUF - usb_rx_len
    );
    usb_rx_len += n;
}

//  Main 

int main(void) {
    stdio_init_all();
    led_init();
    tusb_init();
    spi_slave_init();

    // Prime TX FIFO
    for (int i = 0; i < 8; i++) {
        spi_get_hw(SPI_INST)->dr = SPI_IDLE_BYTE;
    }

    while (1) {
        tud_task();
        led_update();

        // -------- USB -> SPI --------
        while (usb_rx_len >= FRAME_HDR) {
            uint16_t len = rd_u16(usb_rx_buf);

            if (len > MAX_PAYLOAD) {
                usb_rx_len = 0;
                break;
            }

            if (usb_rx_len < FRAME_HDR + len)
                break;

            // Only consume USB frame if TX slot is free
            if (spi_tx_len == 0) {
                uint16_t frame_len = FRAME_HDR + len;

                irq_set_enabled(SPI0_IRQ, false);
                memcpy(spi_tx_buf, usb_rx_buf, frame_len);
                spi_tx_len = frame_len;
                spi_tx_pos = 0;
                irq_set_enabled(SPI0_IRQ, true);


            //DEBUG ONLY DELETE LATER
            #if DEBUG_ECHO_SPI
                debug_echo_spi_tx_to_rx(frame_len);

                //faking SPI_IRQ to trigger echo immediately for debugging only
                irq_set_enabled(SPI0_IRQ, false);
                spi_tx_pos = spi_tx_len;
                spi_tx_len = 0;
                irq_set_enabled(SPI0_IRQ, true);

            #endif

                memmove(
                    usb_rx_buf,
                    usb_rx_buf + frame_len,
                    usb_rx_len - frame_len
                );
                usb_rx_len -= frame_len;
            }
            
            
            else {
                // SPI busy: retry later
                break;
            }
        }

        // -------- SPI -> USB --------
        if (!spi_frame_ready && spi_rx_len >= FRAME_HDR) {
            uint16_t len = rd_u16(spi_rx_buf);
            if (len > MAX_PAYLOAD) {
                spi_rx_len = 0;
            } else if (spi_rx_len >= FRAME_HDR + len) {
                spi_frame_ready = true;
            }
        }

        if (spi_frame_ready) {
            uint16_t len = rd_u16(spi_rx_buf);

            irq_set_enabled(SPI0_IRQ, false);
            tud_cdc_write(spi_rx_buf, FRAME_HDR + len);
            tud_cdc_write_flush();

            memmove(
                spi_rx_buf,
                spi_rx_buf + FRAME_HDR + len,
                spi_rx_len - (FRAME_HDR + len)
            );
            spi_rx_len -= FRAME_HDR + len;
            spi_frame_ready = false;
            irq_set_enabled(SPI0_IRQ, true);
        }

        // TX complete
        if (spi_tx_len != 0 && spi_tx_pos >= spi_tx_len) {
            irq_set_enabled(SPI0_IRQ, false);
            spi_tx_len = 0;
            spi_tx_pos = 0;
            irq_set_enabled(SPI0_IRQ, true);
        }

        sleep_ms(1);
    }
}