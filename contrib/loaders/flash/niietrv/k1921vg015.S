/***************************************************************************
 *   Copyright (C) 2023 by  Alexander Dykhno                               *
 *   dykhno@niiet.ru                                                       *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.                                        *
 ***************************************************************************/

/* To assemble:
 * riscv-none-embed-gcc -c k1921vg015.S
 *
 * To disassemble:
 * riscv-none-embed-objdump -DS k1921vg015.o > k1921vg015.dasm
 *
 * To generate binary file:
 * riscv-none-embed-objcopy -O binary k1921vg015.o k1921vg015.bin
 *
 * To generate include file:
 * xxd -i k1921vg015.bin > k1921vg015.h
 */

/* K1921VG015 has 128-bitwidth flash, so it`s able to load 4x32-bit words at the time.
 * And only after all words loaded we can start write
 */

/* Registers addresses */
#define FLASH_ADDR   0x00           /* Address reg */
#define FLASH_DATA0  0x04           /* Data0 reg */
#define FLASH_DATA1  0x08           /* Data1 reg */
#define FLASH_DATA2  0x0C           /* Data2 reg */
#define FLASH_DATA3  0x10           /* Data3 reg */
#define FLASH_CMD    0x44           /* Command reg */
#define FLASH_STAT   0x48           /* Operation status reg */

.text
    /* Params:
     * a0 - write cmd (in), status (out)
     * a1 - count (halfword-16bit)
     * a2 - workarea start
     * a3 - workarea end
     * a4 - target address
     * a5 - tmp
     * Clobbered:
     * s5 - rp
     * s6 - wp, tmp
     * s7 - tmp
     * s8 - flash base
*/

.global _start
_start:
    lui s8,0x3000D          /* Flash reg base*/
    j   wait_fifo
_exit:
    ebreak
wait_fifo:
    lw  s6, 0(a2)           /* read wp */
    beqz    s6, exit        /* abort if wp == 0 */
    lw  s5, 0(a3)           /* read rp */
    sub a5, s5, s6
    beqz a5, wait_fifo      /* wait until rp != wp */

load_data:    
    lw s6, 0(s5)            /* copy data from RAM to flash (read data0) */
    sw s6, 0x04(s8)         /* write to FLASH_DATA0 */
    addi    s5, s5, 4       /* target_address++ = rp++ */

    lw s6, 0(s5)            /* copy data from RAM to flash (read data0) */
    sw s6, 0x08(s8)         /* write to FLASH_DATA1 */
    addi    s5, s5, 4       /* target_address++ = rp++ */

    lw s6, 0(s5)            /* copy data from RAM to flash (read data0) */
    sw s6, 0x0C(s8)         /* write to FLASH_DATA2 */
    addi    s5, s5, 4       /* target_address++ = rp++ */

    lw s6, 0(s5)            /* copy data from RAM to flash (read data0) */
    sw s6, 0x10(s8)         /* write to FLASH_DATA3 */
    addi    s5, s5, 4       /* target_address++ = rp++ */        

start_write:
    sw a4, 0(s8)            /* set addr */
    addi    a4, a4, 16
    sw a4, 0x44(s8)         /* write cmd */

busy:
    lw  s7, 0x48(s8)        /* wait until BSY flag is reset */
    andi    a5, s7, 1
    bnez    a5, busy
#    andi    a5, s7, 0x14    /* check the error bits */
#    bnez    a5, error
    bgtu    a3, s5, no_wrap /* wrap rp at end of buffer */
    mv  s5, a2
    add s5, s5, 8
no_wrap:
    sw  s5, 4(a2)           /* store rp */
    addi    a1, a1, -1      /* decrement 16-byte block count */
    beqz  a1, exit          /* loop if not done */
    j   wait_fifo
error:
    mv  a0, x0
    sw  a0, 4(a2)           /* set rp = 0 on error */
exit:
    mv  a0, s7              /* return status in a0 */
    j   _exit