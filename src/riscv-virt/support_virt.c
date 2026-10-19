/*
    Copyright © 2026 Ramon Martinez

    This Source Code Form is subject to the terms of the
    Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
    with this file, You can obtain one at http://mozilla.org/MPL/2.0/.
*/

/*
    Console for qemu-system-riscv64 -M virt: the NS16550 named by
    /chosen/stdout-path (or the first ns16550a node), reached through the
    linear map. Nothing is printed before setup_serial() has found it.
*/

#include <stdint.h>
#include <stdarg.h>
#include "support.h"
#include "devicetree.h"
#include "riscv64/mmu_rv.h"

#define UART_THR 0
#define UART_RBR 0
#define UART_IER 1
#define UART_LSR 5
#define LSR_DR   0x01
#define LSR_THRE 0x20

static volatile uint8_t *uart;
static int uart_shift;
uintptr_t virt_uart_phys;
of_node_t *virt_uart_node;

/*
    Console input, with the serial_irq boot option: the 16550's receive
    interrupt, through the PLIC, moves every byte into this queue, which the
    emulated PL011 hands to the m68k side (UARTDR, UARTFR.RXFE). A full
    queue drops the newest bytes.
*/
#define RX_SIZE 256
static volatile uint8_t rx_buf[RX_SIZE];
static volatile uint32_t rx_head, rx_tail;
volatile uint64_t virt_console_rx_bytes, virt_console_rx_dropped;

void virt_console_rx_drain(void)
{
    while (uart[UART_LSR << uart_shift] & LSR_DR) {
        uint8_t c = uart[UART_RBR << uart_shift];
        virt_console_rx_bytes++;
        if (rx_head - rx_tail < RX_SIZE)
            rx_buf[rx_head++ % RX_SIZE] = c;
        else
            virt_console_rx_dropped++;
    }
}

int virt_console_getc(void)
{
    if (rx_head == rx_tail)
        return -1;
    return rx_buf[rx_tail++ % RX_SIZE];
}

int virt_console_rx_ready(void)
{
    return rx_head != rx_tail;
}

void virt_console_rx_enable(void)
{
    if (uart)
        uart[UART_IER << uart_shift] |= 0x01;      /* received data available */
}

static void putByte(void *io_base, char chr)
{
    (void)io_base;
    if (uart == NULL)
        return;
    if (chr == '\n')
        putByte(io_base, '\r');
    while ((uart[UART_LSR << uart_shift] & LSR_THRE) == 0)
        ;
    uart[UART_THR << uart_shift] = (uint8_t)chr;
}

/* Raw console output, used by the emulated PL011. */
void virt_console_putc(char c)
{
    putByte(NULL, c);
}

void kprintf(const char * restrict format, ...)
{
    va_list v;
    va_start(v, format);
    vkprintf_pc(putByte, NULL, format, v);
    va_end(v);
}

void vkprintf(const char * restrict format, va_list args)
{
    vkprintf_pc(putByte, NULL, format, args);
}

/* First address in a reg property, honouring the parent's #address-cells. */
static uint64_t reg_address(of_node_t *node)
{
    of_property_t *p = dt_find_property(node, "reg");
    if (p == NULL)
        return 0;
    int cells = dt_get_property_value_u32(node->on_parent, "#address-cells", 2, TRUE);
    const uint32_t *v = p->op_value;
    return cells == 2 ? ((uint64_t)BE32(v[0]) << 32) | BE32(v[1]) : BE32(v[0]);
}

void setup_serial()
{
    of_node_t *uart_node = NULL;
    of_property_t *p = dt_find_property(dt_find_node("/chosen"), "stdout-path");

    if (p) {
        char path[128];
        const char *s = p->op_value;
        int i;
        for (i = 0; s[i] && s[i] != ':' && i < 127; i++)
            path[i] = s[i];
        path[i] = 0;
        uart_node = dt_find_node(path);
    }
    if (uart_node == NULL)
        uart_node = dt_find_node("/soc/serial");
    if (uart_node == NULL)
        return;

    virt_uart_node = uart_node;
    virt_uart_phys = reg_address(uart_node);
    uart_shift = dt_get_property_value_u32(uart_node, "reg-shift", 0, FALSE);
    uart = (volatile uint8_t *)RV_PHYS_TO_VIRT(virt_uart_phys);
}
