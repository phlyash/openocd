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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

#define FLASH_DRIVER_VER    0x00010001

/*==============================================================================
 *                    K1921VK028 CONTROL REGS
 *==============================================================================
 */

#define MAIN_REGION         0
#define NVR_REGION          1

/*-- SIU ---------------------------------------------------------------------*/
#define SIU_CHIPID_K1921VK028   0x3abf2fd1
#define SIU_BASE                0x40080000
#define SIU_SERVCTL             (SIU_BASE + 0x014)
#define SIU_CHIPID              (SIU_BASE + 0xFFC)
/*---- SIU->SERVCTL: Service mode control register */
#define SIU_SERVCTL_CHIPCLR     (1<<0)          /* Start full clear of all embedded flash memories */
#define SIU_SERVCTL_DONE        (1<<1)          /* Full clear done flag */
#define SIU_SERVCTL_SERVEN      (1<<31)         /* Service mode enable flag */

/*-- MFLASH ------------------------------------------------------------------*/
#define MFLASH                  0
#define MFLASH_WORD_WIDTH       16
#define MFLASH_PAGE_SIZE        16384
#define MFLASH_MAIN_PAGE_TOTAL  128
#define MFLASH_NVR_PAGE_TOTAL   4
#define MFLASH_REGS_BASE        0x40060000

/*-- BFLASH ------------------------------------------------------------------*/
#define BFLASH                  1
#define BFLASH_WORD_WIDTH       4
#define BFLASH_PAGE_SIZE        4096
#define BFLASH_MAIN_PAGE_TOTAL  128
#define BFLASH_NVR_PAGE_TOTAL   4
#define BFLASH_REGS_BASE        0x40061000

/*-- xFLASH registers ------------------------------------------------------- */
#define FLASH_ADDR             0x00
#define FLASH_DATA0            0x04
#define FLASH_DATA1            0x08
#define FLASH_DATA2            0x0C
#define FLASH_DATA3            0x10
#define FLASH_DATA4            0x14
#define FLASH_DATA5            0x18
#define FLASH_DATA6            0x1C
#define FLASH_DATA7            0x20
#define FLASH_DATA8            0x24
#define FLASH_DATA9            0x28
#define FLASH_DATA10           0x2C
#define FLASH_DATA11           0x30
#define FLASH_DATA12           0x34
#define FLASH_DATA13           0x38
#define FLASH_DATA14           0x3C
#define FLASH_DATA15           0x40
#define FLASH_CMD              0x44
#define FLASH_STAT             0x48
/*---- xFLASH->CMD: Command register */
#define FLASH_CMD_RD           (1<<0)           /* Read data in region */
#define FLASH_CMD_WR           (1<<1)           /* Write data in region */
#define FLASH_CMD_ERSEC        (1<<2)           /* Sector erase in region */
#define FLASH_CMD_ERALL        (3<<2)           /* Erase all sectors in region */
#define FLASH_CMD_NVRON        (1<<8)           /* Select NVR region for command operation */
#define FLASH_CMD_KEY          (0xC0DE<<16)     /* Command enable key */
/*---- xFLASH->STAT: Status register */
#define FLASH_STAT_BUSY        (1<<0)           /* Flag operation busy */

/*-- CFGWORD (in BFLASH NVR) ------------------------------------------------ */
#define CFGWORD_PAGE                3
#define CFGWORD0_ADDR_OFFSET        0x000003F0
#define CFGWORD0_ADDR               (BFLASH_PAGE_SIZE*CFGWORD_PAGE+CFGWORD0_ADDR_OFFSET)
#define CFGWORD0_RDC_Pos            0           /*!< External memory read cycle length (default 0xF - 16 cycles) */
#define CFGWORD0_WRC_Pos            4           /*!< External memory write cycle length (default 0xF - 16 cycles) */
#define CFGWORD0_MASK_Pos           8           /*!< External memory address mask (default 0xFFFFF - all bits masked) */
#define CFGWORD0_RDC_Msk            0x0000000F
#define CFGWORD0_WRC_Msk            0x000000F0
#define CFGWORD0_MASK_Msk           0xFFFFFF00
#define CFGWORD1_ADDR               (BFLASH_PAGE_SIZE*CFGWORD_PAGE+CFGWORD1_ADDR_OFFSET)
#define CFGWORD1_ADDR_OFFSET        0x000003F4
#define CFGWORD1_TAC_Pos            0           /*!< External memory turnaround cycle length (default 0xF - 16 cycles) */
#define CFGWORD1_MODE_Pos           4           /*!< External memory bit mode (default 1 - 16 bit; 0 - 8 bit) */
#define CFGWORD1_AF_Pos             5           /*!< External memory GPIO alternative function (default 1 - AF1; 0 - AF0) */
#define CFGWORD1_MFLASHWE_Pos       6           /*!< Main flash region write enable (default 1 - enabled) */
#define CFGWORD1_MNVRWE_Pos         7           /*!< NVR flash region write enable (default 1 - enabled) */
#define CFGWORD1_BFLASHWE_Pos       8           /*!< Main flash region write enable (default 1 - enabled) */
#define CFGWORD1_BNVRWE_Pos         9           /*!< NVR flash region write enable (default 1 - enabled) */
#define CFGWORD1_JTAGEN_Pos         10          /*!< Enable JTAG pins (default 1 - enabled) */
#define CFGWORD1_DEBUGEN_Pos        11          /*!< Enable core debug (default 1 - enabled) */
#define CFGWORD1_TAC_Msk            0x0000000F
#define CFGWORD1_MODE_Msk           0x00000010
#define CFGWORD1_AF_Msk             0x00000020
#define CFGWORD1_MFLASHWE_Msk       0x00000040
#define CFGWORD1_MNVRWE_Msk         0x00000080
#define CFGWORD1_BFLASHWE_Msk       0x00000100
#define CFGWORD1_BNVRWE_Msk         0x00000200
#define CFGWORD1_JTAGEN_Msk         0x00000400
#define CFGWORD1_DEBUGEN_Msk        0x00000800

typedef enum {
    REMAP_MFLASH,
    REMAP_BFLASH,
    REMAP_RAM0,
    REMAP_EXTMEM
} Remap_TypeDef;

/**
 * Private data for flash driver.
 */
struct k1921vk028_flash_bank {
    /* target params */
    bool probed;
    char *chip_name;
    char chip_brief[4096];
    int emrdc;
    int emwrc;
    int emmask;
    int emtac;
    int emmode;
    int emaf;
    bool mflashwe;
    bool mnvrwe;
    bool bflashwe;
    bool bnvrwe;
    Remap_TypeDef remap;
};


/*==============================================================================
 *                     FLASH HARDWARE CONTROL FUNCTIONS
 *==============================================================================
 */

/**
 * Wait while operation with flash being performed
 */
static int k1921vk028_flash_waitdone(struct target *target, uint32_t type)
{
    uint32_t regs_base = (type == BFLASH)? BFLASH_REGS_BASE : MFLASH_REGS_BASE;

    int retval;
    int timeout = 5000;

    uint32_t flash_status;
    retval = target_read_u32(target, regs_base + FLASH_STAT, &flash_status);
    if (retval != ERROR_OK)
        return retval;

    while ((flash_status & FLASH_STAT_BUSY) == FLASH_STAT_BUSY) {
        retval = target_read_u32(target, regs_base + FLASH_STAT, &flash_status);
        if (retval != ERROR_OK)
            return retval;
        if (timeout-- <= 0) {
            LOG_ERROR("Flash operation timeout");
            return ERROR_FLASH_OPERATION_FAILED;
            }
        busy_sleep(1);  /* can use busy sleep for short times. */
    }

    return retval;
}

/**
 * Erase flash sector
 */
static int k1921vk028_flash_erase(struct target *target, int page_num, uint32_t type, uint32_t region)
{
    uint32_t regs_base = (type == BFLASH)? BFLASH_REGS_BASE : MFLASH_REGS_BASE;
    uint32_t page_size = (type == BFLASH)? BFLASH_PAGE_SIZE : MFLASH_PAGE_SIZE;

    int retval = ERROR_OK;

    uint32_t flash_cmd = FLASH_CMD_KEY | FLASH_CMD_ERSEC;
    if (region == NVR_REGION)
        flash_cmd |= FLASH_CMD_NVRON;

    retval = target_write_u32(target, regs_base + FLASH_ADDR, page_num*page_size);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, regs_base + FLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vk028_flash_waitdone(target, type);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/**
 * Mass flash erase
 */
static int k1921vk028_flash_mass_erase(struct target *target, uint32_t type, uint32_t region)
{
    uint32_t regs_base = (type == BFLASH)? BFLASH_REGS_BASE : MFLASH_REGS_BASE;

    int retval = ERROR_OK;

    uint32_t flash_cmd = FLASH_CMD_KEY | FLASH_CMD_ERALL;
    if (region == NVR_REGION)
        flash_cmd |= FLASH_CMD_NVRON;

    retval = target_write_u32(target, regs_base + FLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vk028_flash_waitdone(target, type);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/**
 * Read flash address
 */
static int k1921vk028_flash_read(struct target *target, uint32_t addr, uint32_t *data, uint32_t type, uint32_t region)
{
    uint32_t regs_base = (type == BFLASH)? BFLASH_REGS_BASE : MFLASH_REGS_BASE;
    int data_total = (type == BFLASH)? BFLASH_WORD_WIDTH : MFLASH_WORD_WIDTH;

    int retval = ERROR_OK;

    uint32_t flash_cmd = FLASH_CMD_KEY | FLASH_CMD_RD;
    if (region == NVR_REGION)
        flash_cmd |= FLASH_CMD_NVRON;

    retval = target_write_u32(target, regs_base + FLASH_ADDR, addr);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, regs_base + FLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vk028_flash_waitdone(target, type);
    if (retval != ERROR_OK)
        return retval;
    for (int i = 0; i < data_total; i++) {
        retval = target_read_u32(target, regs_base + FLASH_DATA0 + i*4, &data[i]);
        if (retval != ERROR_OK)
            return retval;
    }


    return retval;
}

/**
 * Write flash address
 */
static int k1921vk028_flash_write(struct target *target, uint32_t addr, uint32_t *data, uint32_t type, uint32_t region)
{
    uint32_t regs_base = (type == BFLASH)? BFLASH_REGS_BASE : MFLASH_REGS_BASE;
    int data_total = (type == BFLASH)? BFLASH_WORD_WIDTH : MFLASH_WORD_WIDTH;

    int retval = ERROR_OK;

    uint32_t flash_cmd = FLASH_CMD_KEY | FLASH_CMD_WR;
    if (region == NVR_REGION)
        flash_cmd |= FLASH_CMD_NVRON;

    retval = target_write_u32(target, regs_base + FLASH_ADDR, addr);
    if (retval != ERROR_OK)
        return retval;
    for (int i = 0; i < data_total; i++) {
        retval = target_write_u32(target, regs_base + FLASH_DATA0 + i*4, data[i]);
        if (retval != ERROR_OK)
            return retval;
    }
    retval = target_write_u32(target, regs_base + FLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vk028_flash_waitdone(target, type);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/**
 * Dump flash sector.
 */
static int k1921vk028_flash_sector_dump(struct target *target, uint32_t *dump, int page_num, uint32_t type, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t data[MFLASH_WORD_WIDTH];
    int page_size = (type == BFLASH)? BFLASH_PAGE_SIZE : MFLASH_PAGE_SIZE;
    int data_total = (type == BFLASH)? BFLASH_WORD_WIDTH : MFLASH_WORD_WIDTH;
    int first = page_num * page_size;
    int last = first + page_size;

    for (int i = first; i < last; i+=4*data_total) {
        retval = k1921vk028_flash_read(target, i, data, type, region);
        if (retval != ERROR_OK)
            return retval;
        for (int j = 0; j < data_total; j++) {
            dump[(i%MFLASH_PAGE_SIZE)/4+j] = data[j];
        }
    }

    return retval;
}

/**
 * Load flash sector dump back to memory
 */
static int k1921vk028_flash_sector_load(struct target *target, uint32_t *dump, int page_num, uint32_t type, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t data[MFLASH_WORD_WIDTH];
    int page_size = (type == BFLASH)? BFLASH_PAGE_SIZE : MFLASH_PAGE_SIZE;
    int data_total = (type == BFLASH)? BFLASH_WORD_WIDTH : MFLASH_WORD_WIDTH;
    int first = page_num * page_size;
    int last = first + page_size;

    retval = k1921vk028_flash_erase(target, page_num, type, region);
    if (retval != ERROR_OK)
        return retval;

    for (int i = first; i < last; i+=4*data_total) {
        for (int j = 0; j < data_total; j++) {
            data[j] = dump[(i%MFLASH_PAGE_SIZE)/4+j];
        }
        retval = k1921vk028_flash_write(target, i, data, type, region);
        if (retval != ERROR_OK)
            return retval;
    }

    return retval;
}

/**
 * Read CFGWORD
 */
static int k1921vk028_flash_read_cfgword(struct target *target, uint32_t *cfgword)
{
    int retval = ERROR_OK;

    uint32_t data[4];

    retval = k1921vk028_flash_read(target, CFGWORD0_ADDR, data, BFLASH, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    cfgword[0] = data[0];
    cfgword[1] = data[1];

    return retval;
}

/**
 * Modify CFGWORD
 */
static int k1921vk028_flash_modify_cfgword(struct target *target, uint32_t enable, uint32_t *param_mask)
{
    int retval = ERROR_OK;

    /* dump */
    uint32_t flash_dump[BFLASH_PAGE_SIZE/4];
    retval = k1921vk028_flash_sector_dump(target, flash_dump, CFGWORD_PAGE, BFLASH, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    /* modify dump */
    if (enable) { /* we need to clear bit to enable */
        flash_dump[CFGWORD0_ADDR_OFFSET] &= ~param_mask[0];
        flash_dump[CFGWORD1_ADDR_OFFSET] &= ~param_mask[1];
    }
    else {
        flash_dump[CFGWORD0_ADDR_OFFSET] |= param_mask[0];
        flash_dump[CFGWORD1_ADDR_OFFSET] |= param_mask[1];
    }

    /* write dump to flash */
    retval = k1921vk028_flash_sector_load(target, flash_dump, CFGWORD_PAGE, BFLASH, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/*==============================================================================
 *                          FLASH DRIVER COMMANDS
 *==============================================================================
 */
COMMAND_HANDLER(k1921vk028_handle_read_command)
{
    if (CMD_ARGC < 3)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t type;
    if (strcmp("mflash", CMD_ARGV[0]) == 0)
        type = MFLASH;
    else if (strcmp("bflash", CMD_ARGV[0]) == 0)
        type = BFLASH;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    uint32_t region;
    if (strcmp("main", CMD_ARGV[1]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[1]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    uint32_t flash_addr;
    uint32_t flash_data[MFLASH_WORD_WIDTH];

    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[2], flash_addr);

    retval = k1921vk028_flash_read(target, flash_addr, flash_data, type, region);
    if (retval != ERROR_OK)
        return retval;

    if (type == MFLASH)
        command_print(cmd,  "Read MFLASH %s region:\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x", region ? "NVR" : "main",
                                                                    flash_addr+0x00, flash_data[0],
                                                                    flash_addr+0x04, flash_data[1],
                                                                    flash_addr+0x08, flash_data[2],
                                                                    flash_addr+0x0C, flash_data[3],
                                                                    flash_addr+0x10, flash_data[4],
                                                                    flash_addr+0x14, flash_data[5],
                                                                    flash_addr+0x18, flash_data[6],
                                                                    flash_addr+0x1C, flash_data[7],
                                                                    flash_addr+0x20, flash_data[8],
                                                                    flash_addr+0x24, flash_data[9],
                                                                    flash_addr+0x28, flash_data[10],
                                                                    flash_addr+0x2C, flash_data[11],
                                                                    flash_addr+0x30, flash_data[12],
                                                                    flash_addr+0x34, flash_data[13],
                                                                    flash_addr+0x38, flash_data[14],
                                                                    flash_addr+0x3C, flash_data[15]);
    else /* (type == BFLASH) */
        command_print(cmd,  "Read BFLASH %s region:\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x", region ? "NVR" : "main",
                                                                    flash_addr+0x0, flash_data[0],
                                                                    flash_addr+0x4, flash_data[1],
                                                                    flash_addr+0x8, flash_data[2],
                                                                    flash_addr+0xC, flash_data[3]);
    return retval;
}

COMMAND_HANDLER(k1921vk028_handle_write_command)
{
    uint32_t type;
    if (strcmp("mflash", CMD_ARGV[0]) == 0)
        type = MFLASH;
    else if (strcmp("bflash", CMD_ARGV[0]) == 0)
        type = BFLASH;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    if ((CMD_ARGC < 5) || ((type == BFLASH) && CMD_ARGC > 8))
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t save_sector;
    uint32_t erase_sector;
    uint32_t flash_addr;
    uint32_t flash_data[MFLASH_WORD_WIDTH];
    for (int i=0;i<MFLASH_WORD_WIDTH;i++) {
        flash_data[i] = 0xFFFFFFFF;
    }

    uint32_t region;
    if (strcmp("main", CMD_ARGV[1]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[1]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    if (strcmp("erase", CMD_ARGV[2]) == 0) {
        save_sector = 0;
        erase_sector = 1;
    }
    else if (strcmp("save", CMD_ARGV[2]) == 0) {
        save_sector = 1;
        erase_sector = 0;
    }
    else if (strcmp("none", CMD_ARGV[2]) == 0) {
        save_sector = 0;
        erase_sector = 0;
    }
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[3], flash_addr);
    for (int i=4; i<(int)CMD_ARGC; i++) {
        COMMAND_PARSE_NUMBER(uint, CMD_ARGV[i], flash_data[i-4]);
    }

    int page_size = (type == BFLASH)? BFLASH_PAGE_SIZE : MFLASH_PAGE_SIZE;
    int page_num = flash_addr/page_size;
    int data_total = (type == BFLASH)? BFLASH_WORD_WIDTH : MFLASH_WORD_WIDTH;

    if (type == MFLASH)
        command_print(cmd,  "Write MFLASH %s region%s:\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x", region ? "NVR" : "main",
                                                                    save_sector ? " (save sector data)" :
                                                                                  erase_sector ? " (erase sector data)" : "",
                                                                    flash_addr+0x00, flash_data[0],
                                                                    flash_addr+0x04, flash_data[1],
                                                                    flash_addr+0x08, flash_data[2],
                                                                    flash_addr+0x0C, flash_data[3],
                                                                    flash_addr+0x10, flash_data[4],
                                                                    flash_addr+0x14, flash_data[5],
                                                                    flash_addr+0x18, flash_data[6],
                                                                    flash_addr+0x1C, flash_data[7],
                                                                    flash_addr+0x20, flash_data[8],
                                                                    flash_addr+0x24, flash_data[9],
                                                                    flash_addr+0x28, flash_data[10],
                                                                    flash_addr+0x2C, flash_data[11],
                                                                    flash_addr+0x30, flash_data[12],
                                                                    flash_addr+0x34, flash_data[13],
                                                                    flash_addr+0x38, flash_data[14],
                                                                    flash_addr+0x3C, flash_data[15]);
    else /* (type == BFLASH) */
        command_print(cmd,  "Write BFLASH %s region%s:\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x\n"
                                "    addr = 0x%04x, data = 0x%04x", region ? "NVR" : "main",
                                                                    save_sector ? " (save sector data)" :
                                                                                  erase_sector ? " (erase sector data)" : "",
                                                                    flash_addr+0x0, flash_data[0],
                                                                    flash_addr+0x4, flash_data[1],
                                                                    flash_addr+0x8, flash_data[2],
                                                                    flash_addr+0xC, flash_data[3]);

    if (save_sector) {
        /* dump */
        uint32_t flash_dump[MFLASH_PAGE_SIZE];
        retval = k1921vk028_flash_sector_dump(target, flash_dump, page_num, type, region);
        if (retval != ERROR_OK)
            return retval;

        /* modify dump */
        for (int i = 0; i < data_total; i++) {
            flash_dump[(flash_addr%page_size)/4+i] = flash_data[i];
        }

        /* write dump to userflash */
        retval = k1921vk028_flash_sector_load(target, flash_dump, page_num, type, region);
        if (retval != ERROR_OK)
            return retval;
    } else {
        if (erase_sector) {
            retval = k1921vk028_flash_erase(target, page_num, type, region);
            if (retval != ERROR_OK)
                return retval;
        }
        retval = k1921vk028_flash_write(target, flash_addr, flash_data, type, region);
        if (retval != ERROR_OK)
            return retval;
    }

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vk028_handle_mass_erase_command)
{
    if (CMD_ARGC < 2)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t type;
    if (strcmp("mflash", CMD_ARGV[0]) == 0)
        type = MFLASH;
    else if (strcmp("bflash", CMD_ARGV[0]) == 0)
        type = BFLASH;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    uint32_t region;
    if (strcmp("main", CMD_ARGV[1]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[1]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    command_print(cmd, "Mass erase %s %s region\n"
                           "Please wait ... ", type ? "BFLASH" : "MFLASH",
                                               region ? "NVR" : "main");

    retval = k1921vk028_flash_mass_erase(target, type, region);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vk028_handle_erase_command)
{
    if (CMD_ARGC < 4)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t type;
    if (strcmp("mflash", CMD_ARGV[0]) == 0)
        type = MFLASH;
    else if (strcmp("bflash", CMD_ARGV[0]) == 0)
        type = BFLASH;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    uint32_t region;
    if (strcmp("main", CMD_ARGV[1]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[1]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    unsigned int first, last;
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[2], first);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[3], last);

    command_print(cmd, "Erase %s %s region sectors %d through %d\n"
                           "Please wait ... ", type ? "BFLASH" : "MFLASH",
                                               region ? "NVR" : "main", first, last);

    for (unsigned int i = first; i <= last; i++) {
        retval = k1921vk028_flash_erase(target, i, type, region);
        if (retval != ERROR_OK)
            return retval;
    }

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vk028_handle_protect_command)
{
    if (CMD_ARGC < 3)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t type;
    if (strcmp("mflash", CMD_ARGV[0]) == 0)
        type = MFLASH;
    else if (strcmp("bflash", CMD_ARGV[0]) == 0)
        type = BFLASH;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    uint32_t region;
    if (strcmp("main", CMD_ARGV[1]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[1]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    int protect_enable;
    if (strcmp("enable", CMD_ARGV[2]) == 0)
        protect_enable = 1;
    else if (strcmp("disable", CMD_ARGV[2]) == 0)
        protect_enable = 0;

    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    command_print(cmd, "Try to %s %s %s region write protection\n"
                           "Please wait ... ", protect_enable ? "enable" : "disable",
                                               type ? "BFLASH" : "MFLASH",
                                               region ? "NVR" : "main");

    uint32_t param_mask[2];
    param_mask[0] = 0;
    if (type == MFLASH)
        param_mask[1] = region ? CFGWORD1_MNVRWE_Msk : CFGWORD1_MFLASHWE_Msk;
    else /* (type == MFLASH) */
        param_mask[1] = region ? CFGWORD1_BNVRWE_Msk : CFGWORD1_BFLASHWE_Msk;
    retval = k1921vk028_flash_modify_cfgword(target, protect_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vk028_handle_debug_command)
{
    if (CMD_ARGC < 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    int debug_enable;
    if (strcmp("enable", CMD_ARGV[0]) == 0)
        debug_enable = 1;
    else if (strcmp("disable", CMD_ARGV[0]) == 0)
        debug_enable = 0;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    command_print(cmd, "Try to %s core debug\n"
                           "Please wait ... ", debug_enable ? "enable" : "disable");

    uint32_t param_mask[2];
    param_mask[0] = 0;
    param_mask[1] = CFGWORD1_DEBUGEN_Msk;
    retval = k1921vk028_flash_modify_cfgword(target, debug_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vk028_handle_jtag_command)
{
    if (CMD_ARGC < 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    int jtag_enable;
    if (strcmp("enable", CMD_ARGV[0]) == 0)
        jtag_enable = 1;
    else if (strcmp("disable", CMD_ARGV[0]) == 0)
        jtag_enable = 0;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    command_print(cmd, "Try to %s JTAG interface\n"
                           "Please wait ... ", jtag_enable ? "enable" : "disable");

    uint32_t param_mask[2];
    param_mask[0] = 0;
    param_mask[1] = CFGWORD1_JTAGEN_Msk;
    retval = k1921vk028_flash_modify_cfgword(target, jtag_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vk028_handle_extmem_cfg_command)
{
    if (CMD_ARGC < 6)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }
    uint32_t rd_cycle, wr_cycle, ta_cycle, addr_mask, bit_mode, af_sel;
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[0], rd_cycle);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], wr_cycle);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[2], ta_cycle);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[3], addr_mask);

    if (strcmp("8bit", CMD_ARGV[4]) == 0)
        bit_mode = 0;
    else if (strcmp("16bit", CMD_ARGV[4]) == 0)
        bit_mode = 1;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    if (strcmp("af2", CMD_ARGV[5]) == 0)
        af_sel = 0;
    else if (strcmp("af1", CMD_ARGV[5]) == 0)
        af_sel = 1;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    command_print(cmd,  "Try to configure external memory boot interface:\n"
                            "rd cycles = %s\n"
                            "wr cycles  = %s\n"
                            "ta cycles  = %s\n"
                            "addr mask  = %s\n"
                            "data bit width  = %s\n"
                            "alt func = %s\n"
                            "Please wait ...", CMD_ARGV[0], CMD_ARGV[1], CMD_ARGV[2], CMD_ARGV[3], CMD_ARGV[4], CMD_ARGV[5]);

    /* dump */
    uint32_t flash_dump[BFLASH_PAGE_SIZE/4];
    retval = k1921vk028_flash_sector_dump(target, flash_dump, CFGWORD_PAGE, BFLASH, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    /* modify dump */
    flash_dump[CFGWORD0_ADDR_OFFSET] &= ~(CFGWORD0_MASK_Msk | CFGWORD0_RDC_Msk | CFGWORD0_WRC_Msk);
    flash_dump[CFGWORD1_ADDR_OFFSET] &= ~(CFGWORD1_TAC_Msk | CFGWORD1_MODE_Msk | CFGWORD1_AF_Msk);
    flash_dump[CFGWORD0_ADDR_OFFSET] |= (addr_mask<<CFGWORD0_MASK_Pos) | (rd_cycle<<CFGWORD0_RDC_Pos) | (wr_cycle<<CFGWORD0_WRC_Pos);
    flash_dump[CFGWORD1_ADDR_OFFSET] |= (ta_cycle<<CFGWORD1_TAC_Pos) | (bit_mode<<CFGWORD1_MODE_Pos) | (af_sel<<CFGWORD1_AF_Pos);

    /* write dump to flash */
    retval = k1921vk028_flash_sector_load(target, flash_dump, CFGWORD_PAGE, BFLASH, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vk028_handle_srv_erase_command)
{
    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    command_print(cmd, "Try to perform service mode erase - all flash memories will be erased\n"
                           "Please wait ... ");

    retval = target_write_u32(target, SIU_SERVCTL, SIU_SERVCTL_CHIPCLR);
    if (retval != ERROR_OK)
        return retval;

    int timeout = 500;
    uint32_t status;

    retval = target_read_u32(target, SIU_SERVCTL, &status);
    if (retval != ERROR_OK)
        return retval;

    while ((status & SIU_SERVCTL_DONE) != SIU_SERVCTL_DONE) {
        retval = target_read_u32(target, SIU_SERVCTL, &status);
        if (retval != ERROR_OK)
            return retval;
        if (timeout-- <= 0) {
            LOG_ERROR("Service mode erase timeout");
            return ERROR_FLASH_OPERATION_FAILED;
            }
        busy_sleep(1);	/* can use busy sleep for short times. */
    }
    command_print(cmd, "done! Power on reset cycle and SRV low are required for the return to normal operation mode.");

    return retval;
}

COMMAND_HANDLER(k1921vk028_handle_driver_info_command)
{
    int retval = ERROR_OK;

    command_print(cmd, "k1921vk028 flash driver\n"
                           "version: %d.%d\n"
                           "author: Bogdan Kolbov\n"
                           "mail: kolbov@niiet.ru",
                           FLASH_DRIVER_VER>>16,
                           FLASH_DRIVER_VER&0xFFFF);

    return retval;
}

static const struct command_registration k1921vk028_exec_command_handlers[] = {
    {
        .name = "read",
        .handler = k1921vk028_handle_read_command,
        .mode = COMMAND_EXEC,
        .usage = "(mflash|bflash) (main|nvr) address",
        .help = "Read 16|4 32-bit words from MFLASH|BFLASH address in main|nvr region. Address should be 64|16 bytes aligned.",
    },
    {
        .name = "write",
        .handler = k1921vk028_handle_write_command,
        .mode = COMMAND_EXEC,
        .usage = "(mflash|bflash) (main|nvr) (erase|save|none) address (data0 ... data3|data0 ... data15)",
        .help = "Write up to 16|4 32-bit words to MFLASH|BFLASH address in main|nvr region. Unspecified values padded with 1's. Address should be 64|16 bytes aligned. There is option that selects between to erase modified sector, to save all data and to do nothing - only write.",
    },
    {
        .name = "mass_erase",
        .handler = k1921vk028_handle_mass_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "(mflash|bflash) (main|nvr)",
        .help = "Erase entire MFLASH|BFLASH main|nvr region",
    },
    {
        .name = "erase",
        .handler = k1921vk028_handle_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "(mflash|bflash) (main|nvr) first_sector_num last_sector_num",
        .help = "Erase sectors of MFLASH|BFLASH main|nvr region, starting at sector first up to and including last",
    },
    {
        .name = "protect",
        .handler = k1921vk028_handle_protect_command,
        .mode = COMMAND_EXEC,
        .usage = "(mflash|bflash) (main|nvr) (enable|disable)",
        .help = "MFLASH|BFLASH main|nvr region write protect control. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "debug",
        .handler = k1921vk028_handle_debug_command,
        .mode = COMMAND_EXEC,
        .usage = "(enable|disable)",
        .help = "Control core debug function. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "jtag",
        .handler = k1921vk028_handle_jtag_command,
        .mode = COMMAND_EXEC,
        .usage = "(enable|disable)",
        .help = "Control JTAG interface. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "extmem_cfg",
        .handler = k1921vk028_handle_extmem_cfg_command,
        .mode = COMMAND_EXEC,
        .usage = "rd_cycle wr_cycle ta_cycle mask (8bit|16bit) (af1|af2)",
        .help = "Configure external memory interface for boot - number of cycles (0x0 to 0xF), address mask (24 bits), data bus width and used IO alternative function",
    },
    {
        .name = "srv_erase",
        .handler = k1921vk028_handle_srv_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "",
        .help = "Perform mass erase of all chip flash memories. Power on reset cycle and SERVEN pin tied low are required for the return to normal operation mode.",
    },
    {
        .name = "driver_info",
        .handler = k1921vk028_handle_driver_info_command,
        .mode = COMMAND_EXEC,
        .usage = "",
        .help = "Show information about flash driver",
    },
    COMMAND_REGISTRATION_DONE
};

static const struct command_registration k1921vk028_command_handlers[] = {
    {
        .name = "k1921vk028",
        .mode = COMMAND_ANY,
        .help = "k1921vk028 flash command group",
        .usage = "",
        .chain = k1921vk028_exec_command_handlers,
    },
    COMMAND_REGISTRATION_DONE
};

/*==============================================================================
 *                          FLASH INTERFACE
 *==============================================================================
 */

FLASH_BANK_COMMAND_HANDLER(k1921vk028_flash_bank_command)
{
    struct k1921vk028_flash_bank *k1921vk028_info;

    if (CMD_ARGC < 6)
        return ERROR_COMMAND_SYNTAX_ERROR;

    k1921vk028_info = malloc(sizeof(struct k1921vk028_flash_bank));

    bank->driver_priv = k1921vk028_info;

    /* information will be updated by probing */
    k1921vk028_info->probed = false;
    k1921vk028_info->chip_name = "K1921VK028";
    k1921vk028_info->emrdc = 0xF;
    k1921vk028_info->emwrc = 0xF;
    k1921vk028_info->emmask = 0xFFFFFF;
    k1921vk028_info->emtac = 0xF;
    k1921vk028_info->emmode = 1;
    k1921vk028_info->emaf = 1;
    k1921vk028_info->mflashwe = true;
    k1921vk028_info->mnvrwe = true;
    k1921vk028_info->bflashwe = true;
    k1921vk028_info->bnvrwe = true;

    return ERROR_OK;
}

static int k1921vk028_protect_check(struct flash_bank *bank)
{
    struct k1921vk028_flash_bank *k1921vk028_info = bank->driver_priv;

    uint32_t protect_enable;
    bool bflash_used = k1921vk028_info->remap == REMAP_BFLASH;
    bool mflash_used = k1921vk028_info->remap == REMAP_MFLASH;

    if ((bflash_used && !k1921vk028_info->bflashwe) ||
        (mflash_used && !k1921vk028_info->mflashwe))
        protect_enable = 1;
    else
        protect_enable = 0;

    for (unsigned int i = 0; i < bank->num_sectors; i++)
        bank->sectors[i].is_protected = protect_enable;

    return ERROR_OK;
}

static int k1921vk028_mass_erase(struct flash_bank *bank)
{
    struct k1921vk028_flash_bank *k1921vk028_info = bank->driver_priv;

    struct target *target = bank->target;
    int retval = ERROR_FLASH_OPERATION_FAILED;
    bool bflash_used = k1921vk028_info->remap == REMAP_BFLASH;
    bool mflash_used = k1921vk028_info->remap == REMAP_MFLASH;

    uint32_t type;
    if (bflash_used)
        type = BFLASH;
    else if (mflash_used)
        type = MFLASH;
    else {
        LOG_ERROR("Flash not remaped to 0x00000000!");
        return retval;
    }

    retval = k1921vk028_flash_mass_erase(target, type, MAIN_REGION);
    if (retval != ERROR_OK)
        return retval;

    for (unsigned int i = 0; i <= bank->num_sectors; i++) {
        bank->sectors[i].is_erased = 1;
    }

    return retval;
}

static int k1921vk028_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
    struct k1921vk028_flash_bank *k1921vk028_info = bank->driver_priv;
    struct target *target = bank->target;

    int retval = ERROR_FLASH_OPERATION_FAILED;
    bool bflash_used = k1921vk028_info->remap == REMAP_BFLASH;
    bool mflash_used = k1921vk028_info->remap == REMAP_MFLASH;

    uint32_t type;
    if (bflash_used)
        type = BFLASH;
    else if (mflash_used)
        type = MFLASH;
    else {
        LOG_ERROR("Flash not remaped to 0x00000000!");
        return retval;
    }

    if (bank->target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    if ((first == 0) && (last == (bank->num_sectors - 1))) {
        retval = k1921vk028_mass_erase(bank);
        if (retval != ERROR_OK)
            return retval;
    } else {
        /* erasing pages */
        for (unsigned int i = first; i <= last; i++) {
            retval = k1921vk028_flash_erase(target, i, type, MAIN_REGION);
            if (retval != ERROR_OK)
                return retval;
            bank->sectors[i].is_erased = 1;
        }
    }

    return retval;
}

static int k1921vk028_protect(struct flash_bank *bank, int set, unsigned int first, unsigned int last)
{
    (void)first;
    (void)last;
    struct target *target = bank->target;
    struct k1921vk028_flash_bank *k1921vk028_info = bank->driver_priv;

    int retval = ERROR_FLASH_OPERATION_FAILED;
    bool bflash_used = k1921vk028_info->remap == REMAP_BFLASH;
    bool mflash_used = k1921vk028_info->remap == REMAP_MFLASH;

    if (!bflash_used && !mflash_used){
        LOG_ERROR("Flash not remaped to 0x00000000!");
        return retval;
    }

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    LOG_INFO("Plese wait ...");

    uint32_t param_mask[2] = {0, bflash_used ? CFGWORD1_BFLASHWE_Msk : CFGWORD1_MFLASHWE_Msk};
    retval = k1921vk028_flash_modify_cfgword(target, set, param_mask);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

static int k1921vk028_write_block(struct flash_bank *bank, const uint8_t *buffer,
        uint32_t offset, uint32_t count)
{
    struct target *target = bank->target;
    struct k1921vk028_flash_bank *k1921vk028_info = bank->driver_priv;
    uint32_t buffer_size = 16384 + 8; /* 8 bytes for rp and wp */
    struct working_area *write_algorithm;
    struct working_area *source;
    uint32_t address = bank->base + offset;
    struct reg_param reg_params[5];
    struct armv7m_algorithm armv7m_info;
    int retval = ERROR_OK;
    bool bflash_used = k1921vk028_info->remap == REMAP_BFLASH;

    static const uint8_t k1921vk028_flash_write_code[] = {
#include "../../../contrib/loaders/flash/niiet/k1921vk028.inc"
    };

    /* flash write code */
    if (target_alloc_working_area(target, sizeof(k1921vk028_flash_write_code),
            &write_algorithm) != ERROR_OK) {
        LOG_WARNING("no working area available, can't do block memory writes");
        return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
    }

    retval = target_write_buffer(target, write_algorithm->address,
            sizeof(k1921vk028_flash_write_code), k1921vk028_flash_write_code);
    if (retval != ERROR_OK)
        return retval;

    /* memory buffer */
    uint32_t bytes_total = (bflash_used)? BFLASH_WORD_WIDTH*4 : MFLASH_WORD_WIDTH*4;
    while (target_alloc_working_area_try(target, buffer_size, &source) != ERROR_OK) {
        buffer_size /= 2;
        buffer_size &= ~(bytes_total-1); /* Make sure it's byte aligned */
        buffer_size += 8; /* And 8 bytes for WP and RP */
        if (buffer_size <= 256) {
            /* we already allocated the writing code, but failed to get a
             * buffer, free the algorithm */
            target_free_working_area(target, write_algorithm);

            LOG_WARNING("no large enough working area available, can't do block memory writes");
            return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
        }
    }

    init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT); /* flash regs base (in), status (out) */
    init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);    /* count */
    init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);    /* buffer start */
    init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);    /* buffer end */
    init_reg_param(&reg_params[4], "r4", 32, PARAM_IN_OUT); /* target address */

    buf_set_u32(reg_params[0].value, 0, 32, bflash_used? BFLASH_REGS_BASE : MFLASH_REGS_BASE);
    buf_set_u32(reg_params[1].value, 0, 32, count);
    buf_set_u32(reg_params[2].value, 0, 32, source->address);
    buf_set_u32(reg_params[3].value, 0, 32, source->address + source->size);
    buf_set_u32(reg_params[4].value, 0, 32, address);

    armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
    armv7m_info.core_mode = ARM_MODE_THREAD;

    retval = target_run_flash_async_algorithm(target, buffer, count, bytes_total,
            0, NULL,
            5, reg_params,
            source->address, source->size,
            write_algorithm->address, 0,
            &armv7m_info);

    if (retval == ERROR_FLASH_OPERATION_FAILED)
        LOG_ERROR("flash write failed at address 0x%"PRIx32,
                buf_get_u32(reg_params[4].value, 0, 32));

    target_free_working_area(target, source);
    target_free_working_area(target, write_algorithm);

    destroy_reg_param(&reg_params[0]);
    destroy_reg_param(&reg_params[1]);
    destroy_reg_param(&reg_params[2]);
    destroy_reg_param(&reg_params[3]);
    destroy_reg_param(&reg_params[4]);

    return retval;
}

static int k1921vk028_write(struct flash_bank *bank, const uint8_t *buffer,
        uint32_t offset, uint32_t count)
{
    struct target *target = bank->target;
    struct k1921vk028_flash_bank *k1921vk028_info = bank->driver_priv;
    uint8_t *new_buffer = NULL;
    bool bflash_used = k1921vk028_info->remap == REMAP_BFLASH;
    bool mflash_used = k1921vk028_info->remap == REMAP_MFLASH;

    if (!bflash_used && !mflash_used){
        LOG_ERROR("Flash not remaped to 0x00000000!");
        return ERROR_FLASH_OPERATION_FAILED;
    }

    if (bank->target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    //int page_size = (type == BFLASH)? BFLASH_PAGE_SIZE : MFLASH_PAGE_SIZE;
    //int page_num = flash_addr/page_size;
    unsigned int bytes_total = (bflash_used)? BFLASH_WORD_WIDTH*4 : MFLASH_WORD_WIDTH*4;

    if (offset & (bytes_total-1)) {
        LOG_ERROR("offset 0x%" PRIx32 " breaks required word alignment", offset);
        return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
    }

    /* If there's an odd number of words, the data has to be padded. Duplicate
     * the buffer and use the normal code path with a single block write since
     * it's probably cheaper than to special case the last odd write using
     * discrete accesses. */

    unsigned int rem = count % bytes_total;
    if (rem) {
        new_buffer = malloc(count + bytes_total - rem);
        if (new_buffer == NULL) {
            LOG_ERROR("Odd number of words to write and no memory for padding buffer");
            return ERROR_FAIL;
        }
        LOG_INFO("Odd number of words to write, padding with 0xFFFFFFFF");
        buffer = memcpy(new_buffer, buffer, count);
        while (rem < bytes_total) {
            new_buffer[count++] = 0xff;
            rem++;
        }
    }

    int retval;

    /* try using block write */
    retval = k1921vk028_write_block(bank, buffer, offset, count/bytes_total);
    uint32_t flash_addr, flash_cmd, flash_data;
    uint32_t regs_base = (bflash_used)? BFLASH_REGS_BASE : MFLASH_REGS_BASE;

    if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE) {
        /* if block write failed (no sufficient working area),
         * we use normal (slow) single halfword accesses */
        LOG_WARNING("Can't use block writes, falling back to single memory accesses");
        LOG_INFO("Plese wait ..."); /* it`s quite a long process */

        flash_cmd = FLASH_CMD_KEY | FLASH_CMD_WR;

        for (unsigned int i = 0; i < count; i += bytes_total) {
            /* current addr */
            LOG_INFO("%d byte of %d", i, count);
            flash_addr = offset + i;
            retval = target_write_u32(target, regs_base + FLASH_ADDR, flash_addr);
            if (retval != ERROR_OK)
                goto free_buffer;

            /* Prepare data */
            uint32_t value[MFLASH_WORD_WIDTH]; //because mflash is wider
            memcpy(&value, buffer + i, (bytes_total/4)*sizeof(uint32_t));

            /* place in data regs  */
            for (unsigned int j = 0; j < bytes_total/4; j++) {
                flash_data = value[j];
                retval = target_write_u32(target, regs_base + FLASH_DATA0 + 4*j, flash_data);
                if (retval != ERROR_OK)
                    goto free_buffer;
            }

            /* write start */
            retval = target_write_u32(target, regs_base + FLASH_CMD, flash_cmd);
            if (retval != ERROR_OK)
                goto free_buffer;

            /* status check */
            retval = k1921vk028_flash_waitdone(target, bflash_used? BFLASH : MFLASH);
            if (retval != ERROR_OK)
                goto free_buffer;
        }

    }

free_buffer:
    if (new_buffer)
        free(new_buffer);

    return retval;
}

static int k1921vk028_probe(struct flash_bank *bank)
{
    struct k1921vk028_flash_bank *k1921vk028_info = bank->driver_priv;
    struct target *target = bank->target;

    if (bank->sectors) {
        free(bank->sectors);
        bank->sectors = NULL;
    }
    uint32_t retval;
    uint32_t chipid;

    retval = target_read_u32(target, SIU_CHIPID, &chipid);
    if ((retval != ERROR_OK) || (chipid != SIU_CHIPID_K1921VK028)) {
        LOG_INFO("CHIPID error");
        return ERROR_FAIL;
    }

    LOG_INFO("K1921VK028 detected");

    /* REMAP check */
    uint32_t temp;
    retval = target_read_u32(target, 0x00010000, &temp);
    if (retval == ERROR_OK) {
        retval = target_read_u32(target, 0x00080000, &temp);
        if (retval == ERROR_OK) {
            retval = target_read_u32(target, 0x00200000, &temp);
            if (retval == ERROR_OK) {
                k1921vk028_info->remap = REMAP_EXTMEM;
            }
            else {
                k1921vk028_info->remap = REMAP_MFLASH;
            }
        }
        else {
            k1921vk028_info->remap = REMAP_BFLASH;
        }
    }
    else {
        k1921vk028_info->remap = REMAP_RAM0;
    }

    /* check if we in service mode */
    uint32_t service_mode;
    retval = target_read_u32(target, SIU_SERVCTL, &service_mode);
    if (retval != ERROR_OK)
        return retval;
    if (service_mode & SIU_SERVCTL_SERVEN)
        service_mode = 1;
    else
        service_mode = 0;

    if (!service_mode) {
        uint32_t cfgword[2] = {0xFFFFFFFF, 0xFFFFFFFF};
        /* read CFGWORD */
        retval = k1921vk028_flash_read_cfgword(target, cfgword);
        if (retval != ERROR_OK)
            return retval;

        k1921vk028_info->emrdc = (cfgword[0] & CFGWORD0_RDC_Msk) >> CFGWORD0_RDC_Pos;
        k1921vk028_info->emwrc = (cfgword[0] & CFGWORD0_WRC_Msk) >> CFGWORD0_WRC_Pos;
        k1921vk028_info->emmask = (cfgword[0] & CFGWORD0_MASK_Msk) >> CFGWORD0_MASK_Pos;
        k1921vk028_info->emtac = (cfgword[1] & CFGWORD1_TAC_Msk) >> CFGWORD1_TAC_Pos;
        k1921vk028_info->emmode = (cfgword[1] & CFGWORD1_MODE_Msk) >> CFGWORD1_MODE_Pos;
        k1921vk028_info->emaf = (cfgword[1] & CFGWORD1_AF_Msk) >> CFGWORD1_AF_Pos;
        if (!(cfgword[1] & CFGWORD1_MFLASHWE_Msk))
            k1921vk028_info->mflashwe = false;
        if (!(cfgword[1] & CFGWORD1_MNVRWE_Msk))
            k1921vk028_info->mnvrwe = false;
        if (!(cfgword[1] & CFGWORD1_BFLASHWE_Msk))
            k1921vk028_info->bflashwe = false;
        if (!(cfgword[1] & CFGWORD1_BNVRWE_Msk))
            k1921vk028_info->bnvrwe = false;

        bank->base = 0x00000000;
        if (k1921vk028_info->remap == REMAP_BFLASH) {
            bank->size = BFLASH_PAGE_SIZE*BFLASH_MAIN_PAGE_TOTAL;
            bank->num_sectors = BFLASH_MAIN_PAGE_TOTAL;
        } else if (k1921vk028_info->remap == REMAP_MFLASH) {
            bank->size = MFLASH_PAGE_SIZE*MFLASH_MAIN_PAGE_TOTAL;
            bank->num_sectors = MFLASH_MAIN_PAGE_TOTAL;
        } else {
            LOG_INFO("Internal flash not remaped to 0 address! Writing and debugging are not supported in this mode yet!");
            bank->size = MFLASH_PAGE_SIZE*MFLASH_MAIN_PAGE_TOTAL;
            bank->num_sectors = MFLASH_MAIN_PAGE_TOTAL;
        }

        snprintf(k1921vk028_info->chip_brief,
                sizeof(k1921vk028_info->chip_brief),
                "\n"
                "[MEMORY CONFIGURATION]\n"
                "Remap:\n"
                "    %s\n"
                "\n"
                "[CFGWORD]\n"
                "External memory read cycles :\n"
                "    %0d\n"
                "External memory write cycles :\n"
                "     %0d\n"
                "External memory turn around cycles :\n"
                "     %0d\n"
                "External memory address mask :\n"
                "    0x%6x\n"
                "External memory data bit width :\n"
                "    %s\n"
                "External memory IO alternative function:\n"
                "    %s\n"
                "MFLASH main region write protection :\n"
                "    %s\n"
                "MFLASH NVR region write protection :\n"
                "    %s\n"
                "BFLASH main region write protection :\n"
                "    %s\n"
                "BFLASH NVR region write protection :\n"
                "    %s\n",
                k1921vk028_info->remap == REMAP_BFLASH ? "BFLASH" :
                (k1921vk028_info->remap == REMAP_MFLASH) ? "MFLASH" :
                (k1921vk028_info->remap == REMAP_RAM0) ? "RAM0" : "EXTMEM",
                k1921vk028_info->emrdc,
                k1921vk028_info->emwrc,
                k1921vk028_info->emtac,
                k1921vk028_info->emmask,
                k1921vk028_info->emmode ? "16 bit" : "8 bit",
                k1921vk028_info->emaf ? "af1" : "af2",
                k1921vk028_info->mflashwe ? "disable" : "enable",
                k1921vk028_info->mnvrwe ? "disable" : "enable",
                k1921vk028_info->bflashwe ? "disable" : "enable",
                k1921vk028_info->bnvrwe ? "disable" : "enable");
    } else {
        bank->size = MFLASH_PAGE_SIZE*MFLASH_MAIN_PAGE_TOTAL;
        bank->num_sectors = MFLASH_MAIN_PAGE_TOTAL;

        sprintf(k1921vk028_info->chip_brief,
                "\n"
                "SERVEN was HIGH during startup. Device entered service mode.\n"
                "All flash memories were locked and can not be readen.\n"
                "If you want to perform emergency erase (erase all entire memory),\n"
                "please use \"srv_erase\" command and reset device.\n"
                "Do not forget, SRV should be pulled down during reset for returning to normal operation mode.\n"
                );
    }

    int page_total = bank->num_sectors;
    int page_size = bank->size / page_total;

    bank->sectors = malloc(sizeof(struct flash_sector) * page_total);

    for (int i = 0; i < page_total; i++) {
        bank->sectors[i].offset = i * page_size;
        bank->sectors[i].size = page_size;
        bank->sectors[i].is_erased = -1;
        bank->sectors[i].is_protected = -1;
    }

    k1921vk028_info->probed = true;

    return ERROR_OK;
}

static int k1921vk028_auto_probe(struct flash_bank *bank)
{
    struct k1921vk028_flash_bank *k1921vk028_info = bank->driver_priv;
    if (k1921vk028_info->probed)
        return ERROR_OK;
    return k1921vk028_probe(bank);
}

static int get_k1921vk028_info(struct flash_bank *bank, struct command_invocation *cmd)
{
    struct k1921vk028_flash_bank *k1921vk028_info = bank->driver_priv;
    LOG_INFO("\nNIIET %s\n%s", k1921vk028_info->chip_name, k1921vk028_info->chip_brief);
    //snprintf(buf, buf_size, " ");
    command_print_sameline(cmd, " ");

    return ERROR_OK;
}

const struct flash_driver k1921vk028_flash = {
    .name = "k1921vk028",
    .usage = "flash bank <name> k1921vk028 <base> <size> 0 0 <target#>",
    .commands = k1921vk028_command_handlers,
    .flash_bank_command = k1921vk028_flash_bank_command,
    .erase = k1921vk028_erase,
    .protect = k1921vk028_protect,
    .write = k1921vk028_write,
    .read = default_flash_read,
    .probe = k1921vk028_probe,
    .auto_probe = k1921vk028_auto_probe,
    .erase_check = default_flash_blank_check,
    .protect_check = k1921vk028_protect_check,
    .info = get_k1921vk028_info,
	.free_driver_priv = default_flash_free_driver_priv,    
};
