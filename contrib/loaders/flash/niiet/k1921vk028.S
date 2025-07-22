/***************************************************************************
 *   Copyright (C) 2018 by Bogdan Kolbov                                   *
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

/* To assemble:
 * arm-none-eabi-gcc -c k1921vk028.S
 *
 * To disassemble:
 * arm-none-eabi-objdump -DS k1921vk028.o > k1921vk028.dasm
 *
 * To generate binary file:
 * arm-none-eabi-objcopy -O binary k1921vk028.o k1921vk028.bin
 *
 * To generate include file:
 * xxd -i k1921vk028.bin > k1921vk028.inc
 */

/* K1921VK028 has 2 flashes: 512-bitwidth mflash and 128-bitwidth bflash, so it`s able to load multiple words at the time.
 * And only after all words loaded we can start write.
 */

/* Registers addresses */
#define FLASH_ADDR          0x00           /* Address reg */
#define FLASH_DATA0         0x04           /* Data0 reg */
#define FLASH_DATA1         0x08           /* Data1 reg */
#define FLASH_DATA2         0x0C           /* Data2 reg */
#define FLASH_DATA3         0x10           /* Data3 reg */
#define FLASH_DATA4         0x14           /* Data4 reg */
#define FLASH_DATA5         0x18           /* Data5 reg */
#define FLASH_DATA6         0x1C           /* Data6 reg */
#define FLASH_DATA7         0x20           /* Data7 reg */
#define FLASH_DATA8         0x24           /* Data8 reg */
#define FLASH_DATA9         0x28           /* Data9 reg */
#define FLASH_DATA10        0x2C           /* Data10 reg */
#define FLASH_DATA11        0x30           /* Data11 reg */
#define FLASH_DATA12        0x34           /* Data12 reg */
#define FLASH_DATA13        0x38           /* Data13 reg */
#define FLASH_DATA14        0x3C           /* Data14 reg */
#define FLASH_DATA15        0x40           /* Data15 reg */
#define FLASH_CMD           0x44           /* Command reg */
#define FLASH_STAT          0x48           /* Operation status reg */
#define FLASH_STAT_BUSY     0x00000001     /* Busy status */
#define FLASH_CMD_WR        0xC0DE0002     /* Write command */
#define FLASH_CMD_CODE      0xC0DE0000     /* Write command */

#define MFLASH_REGS_BASE    0x40060000
#define BFLASH_REGS_BASE    0x40061000

/* Params:
 * r0 - flash regs base (in), status (out)
 * r1 - count
 * r2 - workarea start
 * r3 - workarea end
 * r4 - target address
 * Clobbered:
 * r5 - rp
 * r6 - wp, tmp
 * r7 - write cmd
 */

.thumb_func
.global _start
_start:
ldr     r7, =#FLASH_CMD_WR          /* Store write command */

wait_fifo:
    ldr     r6, [r2, #0]            /* read wp */
    cmp     r6, #0                  /* abort if wp == 0 */
    beq     exit
    ldr     r5, [r2, #4]            /* read rp */
    cmp     r5, r6                  /* wait until rp != wp */
    beq     wait_fifo

load_data:
    ldr     r6, [r5]                /* read and load data0 */
    str     r6, [r0, #FLASH_DATA0]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data1 */
    str     r6, [r0, #FLASH_DATA1]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data2 */
    str     r6, [r0, #FLASH_DATA2]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data3 */
    str     r6, [r0, #FLASH_DATA3]
    adds    r5, #4

    ldr     r6, =#BFLASH_REGS_BASE
    cmp     r0, r6
    beq     start_write             /* jump to write if we programming bflash ... */

    ldr     r6, [r5]                /* read and load data4 */
    str     r6, [r0, #FLASH_DATA4]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data5 */
    str     r6, [r0, #FLASH_DATA5]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data6 */
    str     r6, [r0, #FLASH_DATA6]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data7 */
    str     r6, [r0, #FLASH_DATA7]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data8 */
    str     r6, [r0, #FLASH_DATA8]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data9 */
    str     r6, [r0, #FLASH_DATA9]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data10 */
    str     r6, [r0, #FLASH_DATA10]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data11 */
    str     r6, [r0, #FLASH_DATA11]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data12 */
    str     r6, [r0, #FLASH_DATA12]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data13 */
    str     r6, [r0, #FLASH_DATA13]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data14 */
    str     r6, [r0, #FLASH_DATA14]
    adds    r5, #4

    ldr     r6, [r5]                /* read and load data15 */
    str     r6, [r0, #FLASH_DATA15]
    adds    r5, #4

start_write:
    str     r4, [r0, #FLASH_ADDR]   /* set addr */
    str     r7, [r0, #FLASH_CMD]    /* write cmd */
    adds    r4, #16
    ldr     r6, =#BFLASH_REGS_BASE
    cmp     r0, r6
    beq     busy                    /* jump to busy if we programming bflash ... */
    adds    r4, #48

busy:
    ldr     r6, [r0, #FLASH_STAT]   /* wait until busy flag clear */
    tst     r6, #FLASH_STAT_BUSY
    bne     busy

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

    .pool
