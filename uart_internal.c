/*
    uart_internal.c

    Derived from uart_udp.c from simavr:

    Copyright 2008, 2009 Michel Pollet <buserror@gmail.com>

    This file is part of simavr.

    simavr is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    simavr is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with simavr.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include "uart_internal.h"
#include "avr_uart.h"
#include "sim_hex.h"

// #define TRACE(_w) _w
#ifndef TRACE
#define TRACE(_w)
#endif

DEFINE_FIFO(uint8_t, uart_internal_fifo);

/*
 * called when a byte is send via the uart on the AVR
 */
static void uart_internal_in_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
    uart_internal_t *p = (uart_internal_t *)param;
    TRACE(printf("uart_internal_in_hook %02x\n", value));
    uart_internal_fifo_write(&p->in, value);
}

/*
 * Called when the uart has room in it's input buffer. This is called repeateadly
 * if necessary, while the xoff is called only when the uart fifo is FULL
 */
static void uart_internal_xon_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
    uart_internal_t *p = (uart_internal_t *)param;
    TRACE(if (!p->xon) printf("uart_internal_xon_hook\n");)

    p->xon = 1;
    // try to empty our fifo, the uart_internal_xoff_hook() will be called when
    // other side is full
    while (p->xon && !uart_internal_fifo_isempty(&p->out))
    {
        uint8_t byte = uart_internal_fifo_read(&p->out);
        TRACE(printf("uart_internal_xon_hook send %02x\n", byte);)
        avr_raise_irq(p->irq + IRQ_UART_INTERNAL_BYTE_OUT, byte);
    }
}

/*
 * Called when the uart ran out of room in it's input buffer
 */
static void uart_internal_xoff_hook(struct avr_irq_t *irq, uint32_t value, void *param)
{
    uart_internal_t *p = (uart_internal_t *)param;
    TRACE(if (p->xon) printf("uart_internal_xoff_hook\n");)
    p->xon = 0;
}

static const char *irq_names[IRQ_UART_INTERNAL_COUNT] = {
    [IRQ_UART_INTERNAL_BYTE_IN] = "8<uart_internal.in",
    [IRQ_UART_INTERNAL_BYTE_OUT] = "8>uart_internal.out",
};

void uart_internal_init(struct avr_t *avr, uart_internal_t *p)
{
    p->avr = avr;
    p->irq = avr_alloc_irq(&avr->irq_pool, 0, IRQ_UART_INTERNAL_COUNT, irq_names);
    avr_irq_register_notify(p->irq + IRQ_UART_INTERNAL_BYTE_IN, uart_internal_in_hook, p);
}

void uart_internal_connect(uart_internal_t *p, char uart)
{
    // disable the stdio dump, as we are sending binary there
    uint32_t f = 0;
    avr_ioctl(p->avr, AVR_IOCTL_UART_GET_FLAGS(uart), &f);
    f &= ~AVR_UART_FLAG_STDIO;
    avr_ioctl(p->avr, AVR_IOCTL_UART_SET_FLAGS(uart), &f);

    avr_irq_t *src = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUTPUT);
    avr_irq_t *dst = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_INPUT);
    avr_irq_t *xon = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XON);
    avr_irq_t *xoff = avr_io_getirq(p->avr, AVR_IOCTL_UART_GETIRQ(uart), UART_IRQ_OUT_XOFF);
    if (src && dst)
    {
        avr_connect_irq(src, p->irq + IRQ_UART_INTERNAL_BYTE_IN);
        avr_connect_irq(p->irq + IRQ_UART_INTERNAL_BYTE_OUT, dst);
    }
    if (xon)
        avr_irq_register_notify(xon, uart_internal_xon_hook, p);
    if (xoff)
        avr_irq_register_notify(xoff, uart_internal_xoff_hook, p);
}

int uart_internal_write(uart_internal_t *p, char b)
{
    return uart_internal_fifo_write(&p->out, b);
}

int uart_internal_read(uart_internal_t *p)
{
    if (uart_internal_fifo_isempty(&p->in)) {
        return -1;
    }
    return uart_internal_fifo_read(&p->in);
}
