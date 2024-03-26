/*
 * xen/drivers/char/qcom-uart.c
 *
 * Driver for Qualcomm GENI-based UART interface
 *
 * Volodymyr Babchuk <volodymyr_babchuk@epam.com>
 * Copyright (C) EPAM Systems 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/console.h>
#include <xen/const.h>
#include <xen/errno.h>
#include <xen/serial.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/mm.h>
#include <xen/delay.h>
#include <asm/device.h>
#include <asm/qcom-uart.h>
#include <asm/io.h>

#define GENI_FORCE_DEFAULT_REG          0x20
#define   FORCE_DEFAULT                 BIT(0, U)
#define   DEF_TX_WM                     2
#define SE_GENI_TX_PACKING_CFG0         0x260
#define SE_GENI_TX_PACKING_CFG1         0x264
#define SE_GENI_RX_PACKING_CFG0         0x284
#define SE_GENI_RX_PACKING_CFG1         0x288
#define SE_GENI_M_IRQ_EN                0x614
#define   M_SEC_IRQ_EN                  BIT(31, U)
#define   M_RX_FIFO_WATERMARK_EN        BIT(26, U)
#define   M_RX_FIFO_LAST_EN             BIT(27, U)
#define SE_GENI_S_CMD0                  0x630
#define   UART_START_READ               0x1
#define   S_OPCODE_SHFT                 27
#define SE_GENI_S_CMD_CTRL_REG          0x634
#define   S_GENI_CMD_ABORT              BIT(1, U)
#define SE_GENI_S_IRQ_STATUS            0x640
#define SE_GENI_S_IRQ_EN                0x644
#define   S_RX_FIFO_LAST_EN             BIT(27, U)
#define   S_RX_FIFO_WATERMARK_EN        BIT(26, U)
#define   S_CMD_ABORT_EN                BIT(5, U)
#define   S_CMD_DONE_EN                 BIT(0, U)
#define SE_GENI_S_IRQ_CLEAR             0x648
#define SE_GENI_RX_FIFOn                0x780
#define SE_GENI_TX_FIFO_STATUS          0x800
#define   TX_FIFO_WC                    GENMASK(27, 0)
#define SE_GENI_RX_FIFO_STATUS          0x804
#define   RX_LAST                       BIT(31, U)
#define   RX_LAST_BYTE_VALID_MSK        GENMASK(30, 28)
#define   RX_LAST_BYTE_VALID_SHFT       28
#define   RX_FIFO_WC_MSK                GENMASK(24, 0)
#define SE_GENI_TX_WATERMARK_REG        0x80c

static struct qcom_uart {
    unsigned int irq;
    char __iomem *regs;
    struct irqaction irqaction;
} qcom_com = {0};

static bool qcom_uart_poll_bit(void *addr, uint32_t mask, bool set)
{
    unsigned long timeout_us = 20000;
    uint32_t reg;

    while ( timeout_us ) {
        reg = readl(addr);
        if ( (bool)(reg & mask) == set )
            return true;
        udelay(10);
        timeout_us -= 10;
    }

    return false;
}

static void __init qcom_uart_init_preirq(struct serial_port *port)
{
    struct qcom_uart *uart = port->uart;

    /* Stop anything in TX that earlyprintk configured and clear all errors */
    writel(M_GENI_CMD_ABORT, uart->regs + SE_GENI_M_CMD_CTRL_REG);
    qcom_uart_poll_bit(uart->regs + SE_GENI_M_IRQ_STATUS, M_CMD_ABORT_EN,
                       true);
    writel(M_CMD_ABORT_EN, uart->regs + SE_GENI_M_IRQ_CLEAR);

    /*
     * Configure FIFO length: 1 byte per FIFO entry. This is terribly
     * ineffective, as it is possible to cram 4 bytes per FIFO word,
     * like Linux does. But using one byte per FIFO entry makes this
     * driver much simpler.
     */
    writel(0xf, uart->regs + SE_GENI_TX_PACKING_CFG0);
    writel(0x0, uart->regs + SE_GENI_TX_PACKING_CFG1);
    writel(0xf, uart->regs + SE_GENI_RX_PACKING_CFG0);
    writel(0x0, uart->regs + SE_GENI_RX_PACKING_CFG1);

    /* Reset RX state machine */
    writel(S_GENI_CMD_ABORT, uart->regs + SE_GENI_S_CMD_CTRL_REG);
    qcom_uart_poll_bit(uart->regs + SE_GENI_S_CMD_CTRL_REG,
                       S_GENI_CMD_ABORT, false);
    writel(S_CMD_DONE_EN | S_CMD_ABORT_EN, uart->regs + SE_GENI_S_IRQ_CLEAR);
    writel(FORCE_DEFAULT, uart->regs + GENI_FORCE_DEFAULT_REG);
}

static void qcom_uart_interrupt(int irq, void *data, struct cpu_user_regs *regs)
{
    struct serial_port *port = data;
    struct qcom_uart *uart = port->uart;
    uint32_t m_irq_status, s_irq_status;

    m_irq_status = readl(uart->regs + SE_GENI_M_IRQ_STATUS);
    s_irq_status = readl(uart->regs + SE_GENI_S_IRQ_STATUS);
    writel(m_irq_status, uart->regs + SE_GENI_M_IRQ_CLEAR);
    writel(s_irq_status, uart->regs + SE_GENI_S_IRQ_CLEAR);

    if ( s_irq_status & (S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN) )
        serial_rx_interrupt(port, regs);
}

static void __init qcom_uart_init_postirq(struct serial_port *port)
{
    struct qcom_uart *uart = port->uart;
    int rc;
    uint32_t val;

    uart->irqaction.handler = qcom_uart_interrupt;
    uart->irqaction.name    = "qcom_uart";
    uart->irqaction.dev_id  = port;

    if ( (rc = setup_irq(uart->irq, 0, &uart->irqaction)) != 0 )
        dprintk(XENLOG_ERR, "Failed to allocated qcom_uart IRQ %d\n",
                uart->irq);

    /* Enable TX/RX and Error Interrupts  */
    writel(S_GENI_CMD_ABORT, uart->regs + SE_GENI_S_CMD_CTRL_REG);
    qcom_uart_poll_bit(uart->regs + SE_GENI_S_CMD_CTRL_REG,
                       S_GENI_CMD_ABORT, false);
    writel(S_CMD_DONE_EN | S_CMD_ABORT_EN, uart->regs + SE_GENI_S_IRQ_CLEAR);
    writel(FORCE_DEFAULT, uart->regs + GENI_FORCE_DEFAULT_REG);

    val = readl(uart->regs + SE_GENI_S_IRQ_EN);
    val = S_RX_FIFO_WATERMARK_EN | S_RX_FIFO_LAST_EN;
    writel(val, uart->regs + SE_GENI_S_IRQ_EN);

    val = readl(uart->regs + SE_GENI_M_IRQ_EN);
    val = M_RX_FIFO_WATERMARK_EN | M_RX_FIFO_LAST_EN;
    writel(val, uart->regs + SE_GENI_M_IRQ_EN);

    /* Send RX command */
    writel(UART_START_READ << S_OPCODE_SHFT, uart->regs + SE_GENI_S_CMD0);
    qcom_uart_poll_bit(uart->regs + SE_GENI_M_IRQ_STATUS, M_SEC_IRQ_EN,
                       true);
}

static void qcom_uart_putc(struct serial_port *port, char c)
{
    struct qcom_uart *uart = port->uart;
    uint32_t irq_clear = M_CMD_DONE_EN;
    uint32_t m_cmd;
    bool done;

    /* Setup TX */
    writel(1, uart->regs + SE_UART_TX_TRANS_LEN);

    writel(DEF_TX_WM, uart->regs + SE_GENI_TX_WATERMARK_REG);

    m_cmd = UART_START_TX << M_OPCODE_SHFT;
    writel(m_cmd, uart->regs + SE_GENI_M_CMD0);

    qcom_uart_poll_bit(uart->regs + SE_GENI_M_IRQ_STATUS,
                       M_TX_FIFO_WATERMARK_EN, true);

    writel(c, uart->regs + SE_GENI_TX_FIFOn);
    writel(M_TX_FIFO_WATERMARK_EN, uart->regs + SE_GENI_M_IRQ_CLEAR);

    /* Check for TX done */
    done = qcom_uart_poll_bit(uart->regs + SE_GENI_M_IRQ_STATUS, M_CMD_DONE_EN,
                              true);
    if ( !done )
    {
        writel(M_GENI_CMD_ABORT, uart->regs + SE_GENI_M_CMD_CTRL_REG);
        irq_clear |= M_CMD_ABORT_EN;
        qcom_uart_poll_bit(uart->regs + SE_GENI_M_IRQ_STATUS, M_CMD_ABORT_EN,
                           true);

    }
    writel(irq_clear, uart->regs + SE_GENI_M_IRQ_CLEAR);
}

static int qcom_uart_getc(struct serial_port *port, char *pc)
{
    struct qcom_uart *uart = port->uart;

    if ( !readl(uart->regs + SE_GENI_RX_FIFO_STATUS) )
        return 0;

    *pc = readl(uart->regs + SE_GENI_RX_FIFOn) & 0xFF;

    writel(UART_START_READ << S_OPCODE_SHFT, uart->regs + SE_GENI_S_CMD0);
    qcom_uart_poll_bit(uart->regs + SE_GENI_M_IRQ_STATUS, M_SEC_IRQ_EN,
                       true);

    return 1;

}

static struct uart_driver __read_mostly qcom_uart_driver = {
    .init_preirq  = qcom_uart_init_preirq,
    .init_postirq = qcom_uart_init_postirq,
    .putc         = qcom_uart_putc,
    .getc         = qcom_uart_getc,
};

static const struct dt_device_match qcom_uart_dt_match[] __initconst =
{
    { .compatible = "qcom,geni-debug-uart"},
    { /* sentinel */ },
};

static int __init qcom_uart_init(struct dt_device_node *dev,
                                 const void *data)
{
    const char *config = data;
    struct qcom_uart *uart;
    int res;
    paddr_t addr, size;

    if ( strcmp(config, "") )
        printk("WARNING: UART configuration is not supported\n");

    uart = &qcom_com;

    res = dt_device_get_paddr(dev, 0, &addr, &size);
    if ( res )
    {
        printk("qcom-uart: Unable to retrieve the base"
               " address of the UART\n");
        return res;
    }

    res = platform_get_irq(dev, 0);
    if ( res < 0 )
    {
        printk("qcom-uart: Unable to retrieve the IRQ\n");
        return res;
    }
    uart->irq = res;

    uart->regs = ioremap_nocache(addr, size);
    if ( !uart->regs )
    {
        printk("qcom-uart: Unable to map the UART memory\n");
        return -ENOMEM;
    }

    /* Register with generic serial driver */
    serial_register_uart(SERHND_DTUART, &qcom_uart_driver, uart);

    dt_device_set_used_by(dev, DOMID_XEN);

    return 0;
}

DT_DEVICE_START(qcom_uart, "QCOM UART", DEVICE_SERIAL)
    .dt_match = qcom_uart_dt_match,
    .init = qcom_uart_init,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
