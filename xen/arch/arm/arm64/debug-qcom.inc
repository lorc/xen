/*
 * xen/arch/arm/arm64/debug-qcom.inc
 *
 * Qualcomm debug UART specific debug code
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

#include <asm/qcom-uart.h>

.macro early_uart_init xb c
        mov   w\c, #M_GENI_CMD_ABORT
        str   w\c, [\xb, #SE_GENI_M_CMD_CTRL_REG]
1:
        ldr   w\c, [\xb, #SE_GENI_M_IRQ_STATUS]   /* Load IRQ status */
        tst   w\c, #M_CMD_ABORT_EN         /* Check TX_FIFI_WATERMARK_EN bit */
        beq   1b                          /* Wait for the UART to be ready */
        mov   w\c, #M_CMD_ABORT_EN
        orr   w\c, w\c, #M_CMD_DONE_EN
        str   w\c, [\xb, #SE_GENI_M_IRQ_CLEAR]

        mov   w\c, #1
        str   w\c, [\xb, #SE_UART_TX_TRANS_LEN]         /* write len */

        mov   w\c, #(UART_START_TX << M_OPCODE_SHFT)    /* Prepare cmd  */
        str   w\c, [\xb, #SE_GENI_M_CMD0]               /* write cmd */
.endm
/*
 * wait for UART to be ready to transmit
 * xb: register which contains the UART base address
 * c: scratch register
 */
.macro early_uart_ready xb c
1:
        ldr   w\c, [\xb, #SE_GENI_M_IRQ_STATUS] /* Load IRQ status */
        tst   w\c, #M_TX_FIFO_WATERMARK_EN  /* Check TX_FIFI_WATERMARK_EN bit */
        beq    1b                           /* Wait for the UART to be ready */
.endm

/*
 * UART transmit character
 * xb: register which contains the UART base address
 * wt: register which contains the character to transmit
 */
.macro early_uart_transmit xb wt
        str   \wt, [\xb, #SE_GENI_TX_FIFOn]             /* Put char to FIFO */
        mov   \wt, #M_TX_FIFO_WATERMARK_EN              /* Prepare to FIFO */
        str   \wt, [\xb, #SE_GENI_M_IRQ_CLEAR]          /* Kick FIFO */
95:
        ldr   \wt, [\xb, #SE_GENI_M_IRQ_STATUS]         /* Load IRQ status */
        tst   \wt, #M_CMD_DONE_EN           /* Check TX_FIFO_WATERMARK_EN bit */
        beq   95b                           /* Wait for the UART to be ready */
        mov   \wt, #M_CMD_DONE_EN
        str   \wt, [\xb, #SE_GENI_M_IRQ_CLEAR]

        mov   \wt, #(UART_START_TX << M_OPCODE_SHFT)    /* Prepare next cmd */
        str   \wt, [\xb, #SE_GENI_M_CMD0]               /* write cmd */
.endm

/*
 * Local variables:
 * mode: ASM
 * indent-tabs-mode: nil
 * End:
 */
