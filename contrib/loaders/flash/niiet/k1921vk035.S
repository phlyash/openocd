/***************************************************************************
 *   Copyright (C) 2017 by Bogdan Kolbov                                   *
 *   kolbov@niiet.ru                                                       *
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

    .text
    .syntax unified
    .cpu cortex-m4
    .thumb
    .thumb_func

/* To assemble:
 * arm-none-eabi-gcc -c k1921vk035t.S
 *
 * To disassemble:
 * arm-none-eabi-objdump -DS k1921vk035t.o > k1921vk035t.dasm
 *
 * To generate binary file:
 * arm-none-eabi-objcopy -O binary k1921vk035t.o k1921vk035t.bin
 *
 * To generate include file:
 * xxd -i k1921vk035t.bin > k1921vk035t.h
 */

/* K1921VK035T has 64-bitwidth flash, so it`s able to load 2x32-bit words at the time.
 * And only after all words loaded we can start write
 */

/* Registers addresses */
#define FLASH_ADDR   0x00           /* Address reg */
#define FLASH_DATA0  0x04           /* Data0 reg */
#define FLASH_DATA1  0x08           /* Data1 reg */
#define FLASH_CMD    0x24           /* Command reg */
#define FLASH_STAT   0x28           /* Operation status reg */

/* Params:
 * r0 - write cmd (in), status (out)
 * r1 - count
 * r2 - workarea start
 * r3 - workarea end
 * r4 - target address
 * Clobbered:
 * r5 - rp
 * r6 - wp, tmp
 * r7 - flash base
 */

ldr     r7, =#0x40030000            /* Flash reg base*/

wait_fifo:
    ldr     r6, [r2, #0]            /* read wp */
    cmp     r6, #0                  /* abort if wp == 0 */
    beq     exit
    ldr     r5, [r2, #4]            /* read rp */
    cmp     r5, r6                  /* wait until rp != wp */
    beq     wait_fifo

load_data:
    ldr     r6, [r5]                /* read data0 */
    str     r6, [r7, #FLASH_DATA0]
    adds    r5, #4

    ldr     r6, [r5]                /* read data1 */
    str     r6, [r7, #FLASH_DATA1]
    adds    r5, #4

start_write:
    str     r4, [r7, #FLASH_ADDR]   /* set addr */
    adds    r4, #8
    str     r0, [r7, #FLASH_CMD]    /* write cmd */

busy:
    ldr     r6, [r7, #FLASH_STAT]   /* wait until busy flag clear */
    cmp     r6, #1
    beq     busy

    cmp     r5, r3                  /* wrap rp at end of buffer */
    bcc     no_wrap
    mov     r5, r2
    adds    r5, #8

no_wrap:
    str     r5, [r2, #4]            /* store rp */
    subs    r1, r1, #1              /* decrement 8-byte block count */
    cmp     r1, #0
    beq     exit                    /* loop if not done */
    b       wait_fifo

exit:
    mov     r0, r6                  /* return status in r0 */
    bkpt    #0
