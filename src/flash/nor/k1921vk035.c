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
 *                    K1921VK035 CONTROL REGS
 *==============================================================================
 */
/*-- SIU ---------------------------------------------------------------------*/
#define SIU_CHIPID_K1921VK035   0x5a298fe1
#define SIU_BASE                0x40040000
#define SIU_SERVCTL             (SIU_BASE + 0x014)
#define SIU_CHIPID              (SIU_BASE + 0xFFC)
/*---- SIU->SERVCTL: Service mode control register */
#define SIU_SERVCTL_CHIPCLR     (1<<0)              /* Start full clear of all embedded flash memories */
#define SIU_SERVCTL_DONE        (1<<1)              /* Full clear done flag */
#define SIU_SERVCTL_SERVEN      (1<<31)             /* Service mode enable flag */

/*-- MFLASH ------------------------------------------------------------------*/
#define MAIN_REGION             0
#define NVR_REGION              1
#define MFLASH_PAGE_SIZE        1024
#define MFLASH_MAIN_PAGE_TOTAL  64
#define MFLASH_NVR_PAGE_TOTAL   4
#define MFLASH_WORD_WIDTH       2
#define MFLASH_BASE             0x40030000
#define MFLASH_ADDR             (MFLASH_BASE + 0x00)
#define MFLASH_DATA0            (MFLASH_BASE + 0x04)
#define MFLASH_DATA1            (MFLASH_BASE + 0x08)
#define MFLASH_CMD              (MFLASH_BASE + 0x24)
#define MFLASH_STAT             (MFLASH_BASE + 0x28)
/*---- MFLASH->CMD: Command register */
#define MFLASH_CMD_RD           (1<<0)              /* Read data in region */
#define MFLASH_CMD_WR           (1<<1)              /* Write data in region */
#define MFLASH_CMD_ERSEC        (1<<2)              /* Sector erase in region */
#define MFLASH_CMD_ERALL        (1<<3)              /* Erase all sectors in region */
#define MFLASH_CMD_NVRON        (1<<8)              /* Select NVR region for command operation */
#define MFLASH_CMD_KEY          (0xC0DE<<16)        /* Command enable key */
/*---- MFLASH->STAT: Status register */
#define MFLASH_STAT_BUSY        (1<<0)              /* Flag operation busy */

/*---- CFGWORD (in MFLASH NVR)----------------------------------------------- */
#define CFGWORD_PAGE                3
#define CFGWORD_ADDR_OFFSET         0x00
#define CFGWORD_ADDR                (MFLASH_PAGE_SIZE*CFGWORD_PAGE+CFGWORD_ADDR_OFFSET)
#define CFGWORD_JTAGEN              (1<<0)          /* Enable JTAG interface */
#define CFGWORD_DEBUGEN             (1<<1)          /* Enable core debug */
#define CFGWORD_NVRWE               (1<<2)          /* MFLASH NVR region write enable */
#define CFGWORD_FLASHWE             (1<<3)          /* MFLASH main region write enable */
#define CFGWORD_BMODEDIS            (1<<4)          /* Disable boot from NVR region */

/**
 * Private data for flash driver.
 */
struct k1921vk035_flash_bank {
    /* target params */
    bool probed;
    char *chip_name;
    char chip_brief[4096];
    bool bmodedis;
    bool flashwe;
    bool nvrwe;
};


/*==============================================================================
 *                     FLASH HARDWARE CONTROL FUNCTIONS
 *==============================================================================
 */

/**
 * Wait while operation with flash being performed
 */
static int k1921vk035_flash_waitdone(struct target *target)
{
    int retval;
    int timeout = 5000;

    uint32_t flash_status;
    retval = target_read_u32(target, MFLASH_STAT, &flash_status);
    if (retval != ERROR_OK)
        return retval;

    while ((flash_status & MFLASH_STAT_BUSY) == MFLASH_STAT_BUSY) {
        retval = target_read_u32(target, MFLASH_STAT, &flash_status);
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
static int k1921vk035_flash_erase(struct target *target, int page_num, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t flash_cmd = MFLASH_CMD_KEY | MFLASH_CMD_ERSEC;
    if (region == NVR_REGION)
        flash_cmd |= MFLASH_CMD_NVRON;

    retval = target_write_u32(target, MFLASH_ADDR, page_num*MFLASH_PAGE_SIZE);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, MFLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vk035_flash_waitdone(target);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/**
 * Mass flash erase
 */
static int k1921vk035_flash_mass_erase(struct target *target, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t flash_cmd = MFLASH_CMD_KEY | MFLASH_CMD_ERALL;
    if (region == NVR_REGION)
        flash_cmd |= MFLASH_CMD_NVRON;

    retval = target_write_u32(target, MFLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vk035_flash_waitdone(target);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/**
 * Read flash address
 */
static int k1921vk035_flash_read(struct target *target, uint32_t addr, uint32_t *data, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t flash_cmd = MFLASH_CMD_KEY | MFLASH_CMD_RD;
    if (region == NVR_REGION)
        flash_cmd |= MFLASH_CMD_NVRON;

    retval = target_write_u32(target, MFLASH_ADDR, addr);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, MFLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vk035_flash_waitdone(target);
    if (retval != ERROR_OK)
        return retval;
    for (int i = 0; i < MFLASH_WORD_WIDTH; i++) {
        retval = target_read_u32(target, MFLASH_DATA0 + i*4, &data[i]);
        if (retval != ERROR_OK)
            return retval;
    }

    return retval;
}

/**
 * Write flash address
 */
static int k1921vk035_flash_write(struct target *target, uint32_t addr, uint32_t *data, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t flash_cmd = MFLASH_CMD_KEY | MFLASH_CMD_WR;
    if (region == NVR_REGION)
        flash_cmd |= MFLASH_CMD_NVRON;

    retval = target_write_u32(target, MFLASH_ADDR, addr);
    if (retval != ERROR_OK)
        return retval;
    for (int i = 0; i < MFLASH_WORD_WIDTH; i++) {
        retval = target_write_u32(target, MFLASH_DATA0 + i*4, data[i]);
        if (retval != ERROR_OK)
            return retval;
    }
    retval = target_write_u32(target, MFLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vk035_flash_waitdone(target);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/**
 * Dump flash sector.
 */
static int k1921vk035_flash_sector_dump(struct target *target, uint32_t *dump, int page_num, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t data[MFLASH_WORD_WIDTH];
    int first = page_num * MFLASH_PAGE_SIZE;
    int last = first + MFLASH_PAGE_SIZE;

    for (int i = first; i < last; i+=MFLASH_WORD_WIDTH*4) {
        retval = k1921vk035_flash_read(target, i, data, region);
        if (retval != ERROR_OK)
            return retval;
        for (int j = 0; j < MFLASH_WORD_WIDTH; j++) {
            dump[(i%MFLASH_PAGE_SIZE)/4+j] = data[j];
        }
    }

    return retval;
}

/**
 * Load flash sector dump back to memory
 */
static int k1921vk035_flash_sector_load(struct target *target, uint32_t *dump, int page_num, uint32_t region)
{
    int i;
    int retval = ERROR_OK;

    uint32_t data[MFLASH_WORD_WIDTH];
    int first = page_num*MFLASH_PAGE_SIZE;
    int last = first + MFLASH_PAGE_SIZE;

    retval = k1921vk035_flash_erase(target, page_num, region);
    if (retval != ERROR_OK)
        return retval;

    for (i = first; i < last; i+=MFLASH_WORD_WIDTH*4) {
        for (int j = 0; j < MFLASH_WORD_WIDTH; j++) {
            data[j] = dump[(i%MFLASH_PAGE_SIZE)/4+j];
        }
        retval = k1921vk035_flash_write(target, i, data, region);
        if (retval != ERROR_OK)
            return retval;
    }

    return retval;
}

/**
 * Read CFGWORD
 */
static int k1921vk035_flash_read_cfgword(struct target *target, uint32_t *cfgword)
{
    int retval = ERROR_OK;

    uint32_t data[MFLASH_WORD_WIDTH];

    retval = k1921vk035_flash_read(target, CFGWORD_ADDR, data, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    *cfgword = data[0];

    return retval;
}

/**
 * Modify CFGWORD
 */
static int k1921vk035_flash_modify_cfgword(struct target *target, uint32_t enable, uint32_t param_mask)
{
    int retval = ERROR_OK;

    /* dump */
    uint32_t flash_dump[MFLASH_PAGE_SIZE/4];
    retval = k1921vk035_flash_sector_dump(target, flash_dump, CFGWORD_PAGE, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    /* modify dump */
    if (enable) /* we need to clear bit to enable */
        flash_dump[CFGWORD_ADDR_OFFSET] &= ~param_mask;
    else
        flash_dump[CFGWORD_ADDR_OFFSET] |= param_mask;

    /* write dump to flash */
    retval = k1921vk035_flash_sector_load(target, flash_dump, CFGWORD_PAGE, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/*==============================================================================
 *                          FLASH DRIVER COMMANDS
 *==============================================================================
 */
COMMAND_HANDLER(k1921vk035_handle_read_command)
{
    if (CMD_ARGC < 2)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t region;
    if (strcmp("main", CMD_ARGV[0]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[0]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    uint32_t flash_addr;
    uint32_t flash_data[MFLASH_WORD_WIDTH];
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], flash_addr);

    retval = k1921vk035_flash_read(target, flash_addr, flash_data, region);
    if (retval != ERROR_OK)
        return retval;
    command_print(cmd,  "Read MFLASH %s region:\n"
                            "    addr = 0x%04x, data = 0x%04x\n"
                            "    addr = 0x%04x, data = 0x%04x", (region == NVR_REGION) ? "NVR" : "main",
                                                                flash_addr,   flash_data[0],
                                                                flash_addr+4, flash_data[1]);
    return retval;
}

COMMAND_HANDLER(k1921vk035_handle_write_command)
{
    if (CMD_ARGC < 5)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t region;
    if (strcmp("main", CMD_ARGV[0]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[0]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    uint32_t save_sector;
    uint32_t erase_sector;
    if (strcmp("erase", CMD_ARGV[1]) == 0) {
        save_sector = 0;
        erase_sector = 1;
    }
    else if (strcmp("save", CMD_ARGV[1]) == 0) {
        save_sector = 1;
        erase_sector = 0;
    }
    else if (strcmp("none", CMD_ARGV[1]) == 0) {
        save_sector = 0;
        erase_sector = 0;
    }
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    uint32_t flash_addr;
    uint32_t flash_data[MFLASH_WORD_WIDTH];
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[2], flash_addr);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[3], flash_data[0]);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[4], flash_data[1]);

    int page_num = flash_addr/MFLASH_PAGE_SIZE;

    command_print(cmd, "Write MFLASH %s region%s:\n"
                           "    addr = 0x%04x, data = 0x%04x,\n"
                           "    addr = 0x%04x, data = 0x%04x,\n"
                           "    Please wait ... ", (region == NVR_REGION) ? "NVR" : "main",
                                                   save_sector ? " (save sector data)" :
                                                                 erase_sector ? " (erase sector data)" : "",
                                                   flash_addr,   flash_data[0],
                                                   flash_addr+4, flash_data[1]);
    if (save_sector) {
        /* dump */
        uint32_t flash_dump[MFLASH_PAGE_SIZE];
        retval = k1921vk035_flash_sector_dump(target, flash_dump, page_num, region);
        if (retval != ERROR_OK)
            return retval;

        /* modify dump */
        flash_dump[(flash_addr%MFLASH_PAGE_SIZE)/4]   = flash_data[0];
        flash_dump[(flash_addr%MFLASH_PAGE_SIZE)/4+1] = flash_data[1];

        /* write dump to userflash */
        retval = k1921vk035_flash_sector_load(target, flash_dump, page_num, region);
        if (retval != ERROR_OK)
            return retval;
    } else {
        if (erase_sector) {
            retval = k1921vk035_flash_erase(target, page_num, region);
            if (retval != ERROR_OK)
                return retval;
        }
        retval = k1921vk035_flash_write(target, flash_addr, flash_data, region);
        if (retval != ERROR_OK)
            return retval;
    }

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vk035_handle_mass_erase_command)
{
    if (CMD_ARGC < 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t region;
    if (strcmp("main", CMD_ARGV[0]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[0]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    command_print(cmd, "Mass erase MFLASH %s region\n"
                           "Please wait ... ", (region == NVR_REGION) ? "NVR" : "main");

    retval = k1921vk035_flash_mass_erase(target, region);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vk035_handle_erase_command)
{
    if (CMD_ARGC < 3)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t region;
    if (strcmp("main", CMD_ARGV[0]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[0]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    unsigned int first, last;
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], first);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[2], last);

    command_print(cmd, "Erase MFLASH %s region sectors %d through %d\n"
                           "Please wait ... ", (region == NVR_REGION) ? "NVR" : "main", first, last);

    for (unsigned int i = first; i <= last; i++) {
        retval = k1921vk035_flash_erase(target, i, region);
        if (retval != ERROR_OK)
            return retval;
    }

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vk035_handle_protect_command)
{
    if (CMD_ARGC < 2)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    uint32_t region;
    if (strcmp("main", CMD_ARGV[0]) == 0)
        region = MAIN_REGION;
    else if (strcmp("nvr", CMD_ARGV[0]) == 0)
        region = NVR_REGION;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    int protect_enable;
    if (strcmp("enable", CMD_ARGV[1]) == 0)
        protect_enable = 1;
    else if (strcmp("disable", CMD_ARGV[1]) == 0)
        protect_enable = 0;

    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    command_print(cmd, "Try to %s MFLASH %s region write protection\n"
                           "Please wait ... ", protect_enable ? "enable" : "disable",
                                               (region == NVR_REGION) ? "NVR" : "main");

    uint32_t param_mask = (region == NVR_REGION) ? CFGWORD_NVRWE : CFGWORD_FLASHWE;
    retval = k1921vk035_flash_modify_cfgword(target, protect_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vk035_handle_nvr_boot_command)
{
    if (CMD_ARGC < 1)
        return ERROR_COMMAND_SYNTAX_ERROR;

    int retval;
    struct target *target = get_current_target(CMD_CTX);

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    int nvr_boot_enable;
    if (strcmp("enable", CMD_ARGV[0]) == 0)
        nvr_boot_enable = 1;
    else if (strcmp("disable", CMD_ARGV[0]) == 0)
        nvr_boot_enable = 0;
    else
        return ERROR_COMMAND_SYNTAX_ERROR;

    command_print(cmd, "Try to %s remapping MFLASH NVR region to 0x00000000\n"
                           "Please wait ... ", nvr_boot_enable ? "enable" : "disable");

    uint32_t param_mask = CFGWORD_BMODEDIS;
    retval = k1921vk035_flash_modify_cfgword(target, nvr_boot_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vk035_handle_debug_command)
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

    uint32_t param_mask = CFGWORD_DEBUGEN;
    retval = k1921vk035_flash_modify_cfgword(target, debug_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vk035_handle_jtag_command)
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

    uint32_t param_mask = CFGWORD_JTAGEN;
    retval = k1921vk035_flash_modify_cfgword(target, jtag_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vk035_handle_srv_erase_command)
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
    command_print(cmd, "done! Power on reset cycle and SERVEN low are required for the return to normal operation mode.");

    return retval;
}

COMMAND_HANDLER(k1921vk035_handle_driver_info_command)
{
    int retval = ERROR_OK;

    command_print(cmd, "K1921VK035 flash driver\n"
                           "version: %d.%d\n"
                           "author: Bogdan Kolbov\n"
                           "mail: kolbov@niiet.ru",
                           FLASH_DRIVER_VER>>16,
                           FLASH_DRIVER_VER&0xFFFF);

    return retval;
}

static const struct command_registration k1921vk035_exec_command_handlers[] = {
    {
        .name = "read",
        .handler = k1921vk035_handle_read_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr) address",
        .help = "Read two 32-bit words from MFLASH main or NVR region address. Address should be 8 bytes aligned.",
    },
    {
        .name = "write",
        .handler = k1921vk035_handle_write_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr) (erase|save|none) address data0 data1",
        .help = "Write two 32-bit words to MFLASH main or NVR region address. Address should be 8 bytes aligned. There is option that selects between to erase modified sector, to save all data and to do nothing - only write.",
    },
    {
        .name = "mass_erase",
        .handler = k1921vk035_handle_mass_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr)",
        .help = "Erase entire MFLASH main or NVR region",
    },
    {
        .name = "erase",
        .handler = k1921vk035_handle_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr) first_sector_num last_sector_num",
        .help = "Erase sectors of MFLASH main or NVR region, starting at sector first up to and including last",
    },
    {
        .name = "protect",
        .handler = k1921vk035_handle_protect_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr) (enable|disable)",
        .help = "MFLASH main or NVR region write protect control. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "nvr_boot",
        .handler = k1921vk035_handle_nvr_boot_command,
        .mode = COMMAND_EXEC,
        .usage = "(enable|disable)",
        .help = "Enable remapping MFLASH NVR region to 0x00000000. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "debug",
        .handler = k1921vk035_handle_debug_command,
        .mode = COMMAND_EXEC,
        .usage = "(enable|disable)",
        .help = "Control core debug function. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "jtag",
        .handler = k1921vk035_handle_jtag_command,
        .mode = COMMAND_EXEC,
        .usage = "(enable|disable)",
        .help = "Control JTAG interface. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "srv_erase",
        .handler = k1921vk035_handle_srv_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "",
        .help = "Perform mass erase of all chip flash memories. Power on reset cycle and SERVEN pin tied low are required for the return to normal operation mode.",
    },
    {
        .name = "driver_info",
        .handler = k1921vk035_handle_driver_info_command,
        .mode = COMMAND_EXEC,
        .usage = "",
        .help = "Show information about flash driver",
    },
    COMMAND_REGISTRATION_DONE
};

static const struct command_registration k1921vk035_command_handlers[] = {
    {
        .name = "k1921vk035",
        .mode = COMMAND_ANY,
        .help = "k1921vk035 flash command group",
        .usage = "",
        .chain = k1921vk035_exec_command_handlers,
    },
    COMMAND_REGISTRATION_DONE
};

/*==============================================================================
 *                          FLASH INTERFACE
 *==============================================================================
 */

FLASH_BANK_COMMAND_HANDLER(k1921vk035_flash_bank_command)
{
    struct k1921vk035_flash_bank *k1921vk035_info;

    if (CMD_ARGC < 6)
        return ERROR_COMMAND_SYNTAX_ERROR;

    k1921vk035_info = malloc(sizeof(struct k1921vk035_flash_bank));

    bank->driver_priv = k1921vk035_info;

    /* information will be updated by probing */
    k1921vk035_info->probed = false;
    k1921vk035_info->chip_name = "K1921VK035";
    k1921vk035_info->bmodedis = true;
    k1921vk035_info->flashwe = true;
    k1921vk035_info->nvrwe = true;

    return ERROR_OK;
}

static int k1921vk035_protect_check(struct flash_bank *bank)
{
    struct k1921vk035_flash_bank *k1921vk035_info = bank->driver_priv;

    uint32_t protect_enable;

    if ((k1921vk035_info->bmodedis && !k1921vk035_info->flashwe) ||
        (!k1921vk035_info->bmodedis && !k1921vk035_info->nvrwe))
        protect_enable = 1;
    else
        protect_enable = 0;

    for (unsigned int i = 0; i < bank->num_sectors; i++)
        bank->sectors[i].is_protected = protect_enable;

    return ERROR_OK;
}

static int k1921vk035_mass_erase(struct flash_bank *bank)
{
    struct k1921vk035_flash_bank *k1921vk035_info = bank->driver_priv;

    struct target *target = bank->target;
    int retval;
    uint32_t region;

    if (k1921vk035_info->bmodedis)
        region = MAIN_REGION;
    else
        region = NVR_REGION;

    retval = k1921vk035_flash_mass_erase(target, region);
    if (retval != ERROR_OK)
        return retval;

    for (unsigned int i = 0; i <= bank->num_sectors; i++) {
        bank->sectors[i].is_erased = 1;
    }

    return retval;
}

static int k1921vk035_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
    struct k1921vk035_flash_bank *k1921vk035_info = bank->driver_priv;
    struct target *target = bank->target;

    int retval = ERROR_FLASH_OPERATION_FAILED;
    uint32_t region;
    if (k1921vk035_info->bmodedis)
        region = MAIN_REGION;
    else
        region = NVR_REGION;

    if (bank->target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    if ((first == 0) && (last == (bank->num_sectors - 1))) {
        retval = k1921vk035_mass_erase(bank);
        if (retval != ERROR_OK)
            return retval;
    } else {
        /* erasing pages */
        for (unsigned int i = first; i <= last; i++) {
            retval = k1921vk035_flash_erase(target, i, region);
            if (retval != ERROR_OK)
                return retval;
            bank->sectors[i].is_erased = 1;
        }
    }

    return retval;
}

static int k1921vk035_protect(struct flash_bank *bank, int set, unsigned int first, unsigned int last)
{
    struct target *target = bank->target;
    struct k1921vk035_flash_bank *k1921vk035_info = bank->driver_priv;

    int retval;
    uint32_t region;
    if (k1921vk035_info->bmodedis)
        region = MAIN_REGION;
    else
        region = NVR_REGION;

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    LOG_INFO("Plese wait ...");

    uint32_t param_mask = (region == NVR_REGION) ? CFGWORD_NVRWE : CFGWORD_FLASHWE;
    retval = k1921vk035_flash_modify_cfgword(target, set, param_mask);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

static int k1921vk035_write_block(struct flash_bank *bank, const uint8_t *buffer,
        uint32_t offset, uint32_t count)
{
    struct target *target = bank->target;
    struct k1921vk035_flash_bank *k1921vk035_info = bank->driver_priv;
    uint32_t buffer_size = 4096 + 8; /* 8 bytes for rp and wp */
    struct working_area *write_algorithm;
    struct working_area *source;
    uint32_t address = bank->base + offset;
    struct reg_param reg_params[5];
    struct armv7m_algorithm armv7m_info;
    int retval = ERROR_OK;

    static const uint8_t k1921vk035_flash_write_code[] = {
#include "../../../contrib/loaders/flash/niiet/k1921vk035.inc"
    };

    /* flash write code */
    if (target_alloc_working_area(target, sizeof(k1921vk035_flash_write_code),
            &write_algorithm) != ERROR_OK) {
        LOG_WARNING("no working area available, can't do block memory writes");
        return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
    }

    retval = target_write_buffer(target, write_algorithm->address,
            sizeof(k1921vk035_flash_write_code), k1921vk035_flash_write_code);
    if (retval != ERROR_OK)
        return retval;

    /* memory buffer */
    while (target_alloc_working_area_try(target, buffer_size, &source) != ERROR_OK) {
        buffer_size /= 2;
        buffer_size &= ~(MFLASH_WORD_WIDTH*4-1); /* Make sure it's aligned */
        buffer_size += 8; /* And 8 bytes for WP and RP */
        if (buffer_size <= 256) {
            /* we already allocated the writing code, but failed to get a
             * buffer, free the algorithm */
            target_free_working_area(target, write_algorithm);

            LOG_WARNING("no large enough working area available, can't do block memory writes");
            return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
        }
    }

    init_reg_param(&reg_params[0], "r0", 32, PARAM_IN_OUT); /* write_cmd base (in), status (out) */
    init_reg_param(&reg_params[1], "r1", 32, PARAM_OUT);    /* count (64bit) */
    init_reg_param(&reg_params[2], "r2", 32, PARAM_OUT);    /* buffer start */
    init_reg_param(&reg_params[3], "r3", 32, PARAM_OUT);    /* buffer end */
    init_reg_param(&reg_params[4], "r4", 32, PARAM_IN_OUT); /* target address */

    uint32_t flash_cmd;
    flash_cmd = MFLASH_CMD_KEY | MFLASH_CMD_WR;
    if (!k1921vk035_info->bmodedis)
        flash_cmd |= MFLASH_CMD_NVRON;

    buf_set_u32(reg_params[0].value, 0, 32, flash_cmd);
    buf_set_u32(reg_params[1].value, 0, 32, count);
    buf_set_u32(reg_params[2].value, 0, 32, source->address);
    buf_set_u32(reg_params[3].value, 0, 32, source->address + source->size);
    buf_set_u32(reg_params[4].value, 0, 32, address);

    armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
    armv7m_info.core_mode = ARM_MODE_THREAD;

    retval = target_run_flash_async_algorithm(target, buffer, count, MFLASH_WORD_WIDTH*4,
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

static int k1921vk035_write(struct flash_bank *bank, const uint8_t *buffer,
        uint32_t offset, uint32_t count)
{
    struct target *target = bank->target;
    struct k1921vk035_flash_bank *k1921vk035_info = bank->driver_priv;
    uint8_t *new_buffer = NULL;

    if (bank->target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    if (offset & 0x7) {
        LOG_ERROR("offset 0x%" PRIx32 " breaks required 2-word alignment", offset);
        return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
    }

    /* If there's an odd number of words, the data has to be padded. Duplicate
     * the buffer and use the normal code path with a single block write since
     * it's probably cheaper than to special case the last odd write using
     * discrete accesses. */

    int rem = count % MFLASH_WORD_WIDTH*4;
    if (rem) {
        new_buffer = malloc(count + MFLASH_WORD_WIDTH*4 - rem);
        if (new_buffer == NULL) {
            LOG_ERROR("Odd number of words to write and no memory for padding buffer");
            return ERROR_FAIL;
        }
        LOG_INFO("Odd number of words to write, padding with 0xFFFFFFFF");
        buffer = memcpy(new_buffer, buffer, count);
        while (rem < MFLASH_WORD_WIDTH*4) {
            new_buffer[count++] = 0xff;
            rem++;
        }
    }

    int retval;

    /* try using block write */
    retval = k1921vk035_write_block(bank, buffer, offset, count/(MFLASH_WORD_WIDTH*4));
    uint32_t flash_addr, flash_cmd, flash_data;

    if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE) {
        /* if block write failed (no sufficient working area),
         * we use normal (slow) single halfword accesses */
        LOG_WARNING("Can't use block writes, falling back to single memory accesses");
        LOG_INFO("Plese wait ..."); /* it`s quite a long process */

        flash_cmd = MFLASH_CMD_KEY | MFLASH_CMD_WR;
        /* chose between main and nvr region */

        if (!k1921vk035_info->bmodedis)
            flash_cmd |= MFLASH_CMD_NVRON;

        /* write multiple bytes per try */
        for (unsigned int i = 0; i < count; i += MFLASH_WORD_WIDTH*4) {
            /* current addr */
            LOG_INFO("%d byte of %d", i, count);
            flash_addr = offset + i;
            retval = target_write_u32(target, MFLASH_ADDR, flash_addr);
            if (retval != ERROR_OK)
                goto free_buffer;

            /* Prepare data */
            uint32_t value[MFLASH_WORD_WIDTH];
            memcpy(&value, buffer + i, MFLASH_WORD_WIDTH*sizeof(uint32_t));

            /* place in reg data */
            for (int j = 0; j < MFLASH_WORD_WIDTH; j++) {
                flash_data = value[j];
                retval = target_write_u32(target, MFLASH_DATA0 + j*4, flash_data);
                if (retval != ERROR_OK)
                    goto free_buffer;
            }

            /* write start */
            retval = target_write_u32(target, MFLASH_CMD, flash_cmd);
            if (retval != ERROR_OK)
                goto free_buffer;

            /* status check */
            retval = k1921vk035_flash_waitdone(target);
            if (retval != ERROR_OK)
                goto free_buffer;
        }

    }

free_buffer:
    if (new_buffer)
        free(new_buffer);

    return retval;
}

static int k1921vk035_probe(struct flash_bank *bank)
{
    struct k1921vk035_flash_bank *k1921vk035_info = bank->driver_priv;
    struct target *target = bank->target;

    if (bank->sectors) {
        free(bank->sectors);
        bank->sectors = NULL;
    }
    uint32_t retval;
    uint32_t chipid;

    retval = target_read_u32(target, SIU_CHIPID, &chipid);
    if ((retval != ERROR_OK) || (chipid != SIU_CHIPID_K1921VK035)) {
        LOG_INFO("CHIPID error");
        return ERROR_FAIL;
    }

    LOG_INFO("K1921VK035 detected");

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
        uint32_t cfgword = 0xFFFFFFFF;
        /* read CFGWORD */
        retval = k1921vk035_flash_read_cfgword(target, &cfgword);
        if (retval != ERROR_OK)
            return retval;

        if (!(cfgword & CFGWORD_BMODEDIS))
            k1921vk035_info->bmodedis = false;
        if (!(cfgword & CFGWORD_FLASHWE))
            k1921vk035_info->flashwe = false;
        if (!(cfgword & CFGWORD_NVRWE))
            k1921vk035_info->nvrwe = false;

        bank->base = 0x00000000;
        if (k1921vk035_info->bmodedis) {
            bank->size = MFLASH_PAGE_SIZE*MFLASH_MAIN_PAGE_TOTAL;
            bank->num_sectors = MFLASH_MAIN_PAGE_TOTAL;
        } else {
            bank->size = MFLASH_PAGE_SIZE*MFLASH_NVR_PAGE_TOTAL;
            bank->num_sectors = MFLASH_NVR_PAGE_TOTAL;
        }

        snprintf(k1921vk035_info->chip_brief,
                sizeof(k1921vk035_info->chip_brief),
                "\n"
                "[MEMORY CONFIGURATION]\n"
                "Memory mapped to 0x00000000 (will be used for writing and debugging):\n"
                "    %s\n"
                "\n"
                "[CFGWORD]\n"
                "Boot from MFLASH NVR :\n"
                "    %s\n"
                "MFLASH main region write protection :\n"
                "    %s\n"
                "MFLASH NVR region write protection :\n"
                "    %s\n",
                k1921vk035_info->bmodedis ? "MFLASH" : "MFLASH NVR",
                k1921vk035_info->bmodedis ? "disable" : "enable",
                k1921vk035_info->flashwe ? "disable" : "enable",
                k1921vk035_info->nvrwe ? "disable" : "enable");
    } else {
        bank->size = MFLASH_PAGE_SIZE*MFLASH_MAIN_PAGE_TOTAL;
        bank->num_sectors = MFLASH_MAIN_PAGE_TOTAL;

        sprintf(k1921vk035_info->chip_brief,
                "\n"
                "SERVEN was HIGH during startup. Device entered service mode.\n"
                "All flash memories were locked and can not be readen.\n"
                "If you want to perform emergency erase (erase all entire memory),\n"
                "please use \"srv_erase\" command and reset device.\n"
                "Do not forget, SERVEN should be pulled down during reset for returning to normal operation mode.\n"
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

    k1921vk035_info->probed = true;

    return ERROR_OK;
}

static int k1921vk035_auto_probe(struct flash_bank *bank)
{
    struct k1921vk035_flash_bank *k1921vk035_info = bank->driver_priv;
    if (k1921vk035_info->probed)
        return ERROR_OK;
    return k1921vk035_probe(bank);
}

static int get_k1921vk035_info(struct flash_bank *bank, struct command_invocation *cmd)
{
    struct k1921vk035_flash_bank *k1921vk035_info = bank->driver_priv;
    LOG_INFO("\nNIIET %s\n%s", k1921vk035_info->chip_name, k1921vk035_info->chip_brief);
    //snprintf(buf, buf_size, " ");
    command_print_sameline(cmd, " ");

    return ERROR_OK;
}


const struct flash_driver k1921vk035_flash = {
    .name = "k1921vk035",
    .usage = "flash bank <name> k1921vk035 <base> <size> 0 0 <target#>",
    .commands = k1921vk035_command_handlers,
    .flash_bank_command = k1921vk035_flash_bank_command,
    .erase = k1921vk035_erase,
    .protect = k1921vk035_protect,
    .write = k1921vk035_write,
    .read = default_flash_read,
    .probe = k1921vk035_probe,
    .auto_probe = k1921vk035_auto_probe,
    .erase_check = default_flash_blank_check,
    .protect_check = k1921vk035_protect_check,
    .info = get_k1921vk035_info,
	.free_driver_priv = default_flash_free_driver_priv,    
};
