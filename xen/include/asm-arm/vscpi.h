/*
 * xen/include/asm-arm/vscpi.h
 *
 * Volodymyr Babchuk <volodymyr_babchuk@epam.com>
 * Copyright (c) 2019 EPAM Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef __ASM_VSCPI_H__
#define __ASM_VSCPI_H__

#define ARM_SMCCC_SCPI_MBOX_TRIGGER 0x82000001

enum vscpi_opp {
  VSCPI_OPP_MIN = 0,
  VSCPI_OPP_LOW,
  VSCPI_OPP_NOM,
  VSCPI_OPP_HIGH,
  VSCPI_OPP_TURBO
};

bool vscpi_handle_call(struct cpu_user_regs *regs);

#endif

