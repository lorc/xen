/*
 * xen/include/asm-arm/qcom-uart.h
 *
 * Common constant definition between early printk and the UART driver
 * for the Qualcomm debug UART
 *
 * Volodymyr Babchuk <volodymyr_babchuk@epam.com>
 * Copyright (C) 2024, EPAM Systems.
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

#ifndef __ASM_ARM_QCOM_UART_H
#define __ASM_ARM_QCOM_UART_H

#define SE_UART_TX_TRANS_LEN            0x270
#define SE_GENI_M_CMD0                  0x600
#define   UART_START_TX                 0x1
#define   M_OPCODE_SHFT                 27

#define SE_GENI_M_CMD_CTRL_REG          0x604
#define   M_GENI_CMD_ABORT              BIT(1, U)
#define SE_GENI_M_IRQ_STATUS            0x610
#define   M_CMD_DONE_EN                 BIT(0, U)
#define   M_CMD_ABORT_EN                BIT(5, U)
#define   M_TX_FIFO_WATERMARK_EN        BIT(30, U)
#define SE_GENI_M_IRQ_CLEAR             0x618
#define SE_GENI_TX_FIFOn                0x700
#define SE_GENI_TX_WATERMARK_REG        0x80c

#endif /* __ASM_ARM_QCOM_UART_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
