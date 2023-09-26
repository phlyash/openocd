/***************************************************************************
 *   Copyright (C) 2023 by Alexander Dykhno                                *
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
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/riscv/riscv.h>

#define FLASH_DRIVER_VER    0x00010000

/*==============================================================================
 *                    K1921VG015 CONTROL REGS
 *==============================================================================
 */
/*-- SIU ---------------------------------------------------------------------*/
#define SIU_CHIPID_K1921VG015   0xdeadbee1
#define SIU_BASE                0x3000F000
#define SIU_CHIPID              (SIU_BASE + 0x100)
#define SIU_SERVCTL             (SIU_BASE + 0x104)
#define SIU_UID0                (SIU_BASE + 0x110)
#define SIU_UID1                (SIU_BASE + 0x114)
#define SIU_UID2                (SIU_BASE + 0x118)
#define SIU_UID3                (SIU_BASE + 0x11C)

/*---- SIU->SERVCTL: Service mode control register */
#define SIU_SERVCTL_CHIPCLR     (1<<0)              /* Start full clear of all embedded flash memories */
#define SIU_SERVCTL_DONE        (1<<8)              /* Full clear done flag */
#define SIU_SERVCTL_SERVEN      (1<<0)              /* Service mode enable flag */

/*-- FLASH ------------------------------------------------------------------*/
#define MAIN_REGION            0
#define NVR_REGION             1
#define FLASH_PAGE_SIZE        4096
#define FLASH_MAIN_PAGE_TOTAL  256
#define FLASH_NVR_PAGE_TOTAL   1
#define FLASH_WORD_WIDTH       4
#define FLASH_BASE             0x3000D000
#define FLASH_ADDR             (FLASH_BASE + 0x00)
#define FLASH_DATA0            (FLASH_BASE + 0x04)
#define FLASH_DATA1            (FLASH_BASE + 0x08)
#define FLASH_DATA2            (FLASH_BASE + 0x0C)
#define FLASH_DATA3            (FLASH_BASE + 0x10)
#define FLASH_CMD              (FLASH_BASE + 0x44)
#define FLASH_STAT             (FLASH_BASE + 0x48)
/*---- FLASH->CMD: Command register */
#define FLASH_CMD_RD           (1<<0)              /* Read data in region */
#define FLASH_CMD_WR           (1<<1)              /* Write data in region */
#define FLASH_CMD_ERSEC        (1<<2)              /* Sector erase in region */
#define FLASH_CMD_ERALL        (1<<3)              /* Erase all sectors in region */
#define FLASH_CMD_NVRON        (1<<8)              /* Select NVR region for command operation */
#define FLASH_CMD_KEY          (0xC0DE<<16)        /* Command enable key */
/*---- FLASH->STAT: Status register */
#define FLASH_STAT_BUSY        (1<<0)              /* Flag operation busy */

/*---- CFGWORD (in FLASH NVR)----------------------------------------------- */
#define CFGWORD_PAGE                0
#define CFGWORD_ADDR_OFFSET         0x1FF0
#define CFGWORD_ADDR                (FLASH_PAGE_SIZE*CFGWORD_PAGE+CFGWORD_ADDR_OFFSET)
#define CFGWORD_JTAGEN              (1<<2)         /* Enable JTAG interface */
#define CFGWORD_CFGWE               (1<<1)         /* FLASH NVR region write enable */
#define CFGWORD_FLASHWE             (1<<0)         /* FLASH main region write enable */

/**
 * Private data for flash driver.
 */
struct k1921vg015_flash_bank {
    /* target params */
    bool probed;
    char *chip_name;
    char chip_brief[4096];
//    bool bmodedis;
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
static int k1921vg015_flash_waitdone(struct target *target)
{
    int retval;
    int timeout = 5000;

    uint32_t flash_status;
    retval = target_read_u32(target, FLASH_STAT, &flash_status);
    if (retval != ERROR_OK)
        return retval;

    while ((flash_status & FLASH_STAT_BUSY) == FLASH_STAT_BUSY) {
        retval = target_read_u32(target, FLASH_STAT, &flash_status);
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
static int k1921vg015_flash_erase(struct target *target, int page_num, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t flash_cmd = FLASH_CMD_KEY | FLASH_CMD_ERSEC;
    if (region == NVR_REGION)
        flash_cmd |= FLASH_CMD_NVRON;

    retval = target_write_u32(target, FLASH_ADDR, page_num*FLASH_PAGE_SIZE);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, FLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vg015_flash_waitdone(target);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/**
 * Mass flash erase
 */
static int k1921vg015_flash_mass_erase(struct target *target, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t flash_cmd = FLASH_CMD_KEY | FLASH_CMD_ERALL;
    if (region == NVR_REGION)
        flash_cmd |= FLASH_CMD_NVRON;

    retval = target_write_u32(target, FLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vg015_flash_waitdone(target);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/**
 * Read flash address
 */
static int k1921vg015_flash_read(struct target *target, uint32_t addr, uint32_t *data, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t flash_cmd = FLASH_CMD_KEY | FLASH_CMD_RD;
    if (region == NVR_REGION)
        flash_cmd |= FLASH_CMD_NVRON;

    retval = target_write_u32(target, FLASH_ADDR, addr);
    if (retval != ERROR_OK)
        return retval;
    retval = target_write_u32(target, FLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vg015_flash_waitdone(target);
    if (retval != ERROR_OK)
        return retval;
    for (int i = 0; i < FLASH_WORD_WIDTH; i++) {
        retval = target_read_u32(target, FLASH_DATA0 + i*4, &data[i]);
        if (retval != ERROR_OK)
            return retval;
    }

    return retval;
}

/**
 * Write flash address
 */
static int k1921vg015_flash_write(struct target *target, uint32_t addr, uint32_t *data, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t flash_cmd = FLASH_CMD_KEY | FLASH_CMD_WR;
    if (region == NVR_REGION)
        flash_cmd |= FLASH_CMD_NVRON;

    retval = target_write_u32(target, FLASH_ADDR, addr);
    if (retval != ERROR_OK)
        return retval;
    for (int i = 0; i < FLASH_WORD_WIDTH; i++) {
        retval = target_write_u32(target, FLASH_DATA0 + i*4, data[i]);
        if (retval != ERROR_OK)
            return retval;
    }
    retval = target_write_u32(target, FLASH_CMD, flash_cmd);
    if (retval != ERROR_OK)
        return retval;
    retval = k1921vg015_flash_waitdone(target);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/**
 * Dump flash sector.
 */
static int k1921vg015_flash_sector_dump(struct target *target, uint32_t *dump, int page_num, uint32_t region)
{
    int retval = ERROR_OK;

    uint32_t data[FLASH_WORD_WIDTH];
    int first = page_num * FLASH_PAGE_SIZE;
    int last = first + FLASH_PAGE_SIZE;

    for (int i = first; i < last; i+=FLASH_WORD_WIDTH*4) {
        retval = k1921vg015_flash_read(target, i, data, region);
        if (retval != ERROR_OK)
            return retval;
        for (int j = 0; j < FLASH_WORD_WIDTH; j++) {
            dump[(i%FLASH_PAGE_SIZE)/4+j] = data[j];
        }
    }

    return retval;
}

/**
 * Load flash sector dump back to memory
 */
static int k1921vg015_flash_sector_load(struct target *target, uint32_t *dump, int page_num, uint32_t region)
{
    int i;
    int retval = ERROR_OK;

    uint32_t data[FLASH_WORD_WIDTH];
    int first = page_num*FLASH_PAGE_SIZE;
    int last = first + FLASH_PAGE_SIZE;

    retval = k1921vg015_flash_erase(target, page_num, region);
    if (retval != ERROR_OK)
        return retval;

    for (i = first; i < last; i+=FLASH_WORD_WIDTH*4) {
        for (int j = 0; j < FLASH_WORD_WIDTH; j++) {
            data[j] = dump[(i%FLASH_PAGE_SIZE)/4+j];
        }
        retval = k1921vg015_flash_write(target, i, data, region);
        if (retval != ERROR_OK)
            return retval;
    }

    return retval;
}

/**
 * Read CFGWORD
 */
static int k1921vg015_flash_read_cfgword(struct target *target, uint32_t *cfgword)
{
    int retval = ERROR_OK;

    uint32_t data[FLASH_WORD_WIDTH];

    retval = k1921vg015_flash_read(target, CFGWORD_ADDR, data, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    *cfgword = data[0];

    return retval;
}

/**
 * Modify CFGWORD
 */
static int k1921vg015_flash_modify_cfgword(struct target *target, uint32_t enable, uint32_t param_mask)
{
    int retval = ERROR_OK;

    /* dump */
    uint32_t flash_dump[FLASH_PAGE_SIZE/4];
    retval = k1921vg015_flash_sector_dump(target, flash_dump, CFGWORD_PAGE, NVR_REGION);
    if (retval != ERROR_OK)
        return retval;

    /* modify dump */
    if (enable) /* we need to clear bit to enable */
        flash_dump[CFGWORD_ADDR_OFFSET] &= ~param_mask;
    else
        flash_dump[CFGWORD_ADDR_OFFSET] |= param_mask;

    /* write dump to flash */
    /* retval = k1921vg015_flash_sector_load(target, flash_dump, CFGWORD_PAGE, NVR_REGION); */
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

/*==============================================================================
 *                          FLASH DRIVER COMMANDS
 *==============================================================================
 */
COMMAND_HANDLER(k1921vg015_handle_read_command)
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
    uint32_t flash_data[FLASH_WORD_WIDTH];
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[1], flash_addr);

    retval = k1921vg015_flash_read(target, flash_addr, flash_data, region);
    if (retval != ERROR_OK)
        return retval;
    command_print(cmd,  "Read FLASH %s region:\n"
                            "    addr = 0x%04x, data = 0x%04x\n"
                            "    addr = 0x%04x, data = 0x%04x\n"
                            "    addr = 0x%04x, data = 0x%04x\n"                            
                            "    addr = 0x%04x, data = 0x%04x", (region == NVR_REGION) ? "NVR" : "main",
                                                                flash_addr,    flash_data[0],
                                                                flash_addr+4,  flash_data[1],
                                                                flash_addr+8,  flash_data[2],                                                                
                                                                flash_addr+12, flash_data[3]);
    return retval;
}

COMMAND_HANDLER(k1921vg015_handle_write_command)
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
    uint32_t flash_data[FLASH_WORD_WIDTH];
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[2], flash_addr);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[3], flash_data[0]);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[4], flash_data[1]);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[5], flash_data[2]);
    COMMAND_PARSE_NUMBER(uint, CMD_ARGV[6], flash_data[3]);    

    int page_num = flash_addr/FLASH_PAGE_SIZE;

    command_print(cmd, "Write FLASH %s region%s:\n"
                           "    addr = 0x%04x, data = 0x%04x,\n"
                           "    addr = 0x%04x, data = 0x%04x,\n"
                           "    addr = 0x%04x, data = 0x%04x,\n"
                           "    addr = 0x%04x, data = 0x%04x,\n"                           
                           "    Please wait ... ", (region == NVR_REGION) ? "NVR" : "main",
                                                   save_sector ? " (save sector data)" :
                                                                 erase_sector ? " (erase sector data)" : "",
                                                   flash_addr,    flash_data[0],
                                                   flash_addr+4,  flash_data[1], 
                                                   flash_addr+8,  flash_data[2],                                                                                                      
                                                   flash_addr+12, flash_data[3]);
    if (save_sector) {
        /* dump */
        uint32_t flash_dump[FLASH_PAGE_SIZE];
        retval = k1921vg015_flash_sector_dump(target, flash_dump, page_num, region);
        if (retval != ERROR_OK)
            return retval;

        /* modify dump */
        flash_dump[(flash_addr%FLASH_PAGE_SIZE)/4]   = flash_data[0];
        flash_dump[(flash_addr%FLASH_PAGE_SIZE)/4+1] = flash_data[1];
        flash_dump[(flash_addr%FLASH_PAGE_SIZE)/4+2] = flash_data[2];
        flash_dump[(flash_addr%FLASH_PAGE_SIZE)/4+3] = flash_data[3];        

        /* write dump to userflash */
        retval = k1921vg015_flash_sector_load(target, flash_dump, page_num, region);
        if (retval != ERROR_OK)
            return retval;
    } else {
        if (erase_sector) {
            retval = k1921vg015_flash_erase(target, page_num, region);
            if (retval != ERROR_OK)
                return retval;
        }
        retval = k1921vg015_flash_write(target, flash_addr, flash_data, region);
        if (retval != ERROR_OK)
            return retval;
    }

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vg015_handle_mass_erase_command)
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

    command_print(cmd, "Mass erase FLASH %s region\n"
                           "Please wait ... ", (region == NVR_REGION) ? "NVR" : "main");

    retval = k1921vg015_flash_mass_erase(target, region);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vg015_handle_erase_command)
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

    command_print(cmd, "Erase FLASH %s region sectors %d through %d\n"
                           "Please wait ... ", (region == NVR_REGION) ? "NVR" : "main", first, last);

    for (unsigned int i = first; i <= last; i++) {
        retval = k1921vg015_flash_erase(target, i, region);
        if (retval != ERROR_OK)
            return retval;
    }

    command_print(cmd, "done!");

    return retval;
}

COMMAND_HANDLER(k1921vg015_handle_protect_command)
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

    command_print(cmd, "Try to %s FLASH %s region write protection\n"
                           "Please wait ... ", protect_enable ? "enable" : "disable",
                                               (region == NVR_REGION) ? "NVR" : "main");

    uint32_t param_mask = (region == NVR_REGION) ? CFGWORD_CFGWE : CFGWORD_FLASHWE;
    retval = k1921vg015_flash_modify_cfgword(target, protect_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vg015_handle_debug_command)
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

    uint32_t param_mask = CFGWORD_JTAGEN;
    retval = k1921vg015_flash_modify_cfgword(target, debug_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vg015_handle_jtag_command)
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
    retval = k1921vg015_flash_modify_cfgword(target, jtag_enable, param_mask);
    if (retval != ERROR_OK)
        return retval;

    command_print(cmd, "done! Power on reset cycle is required for the new settings to take effect.");
    return retval;
}

COMMAND_HANDLER(k1921vg015_handle_srv_erase_command)
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

COMMAND_HANDLER(k1921vg015_handle_driver_info_command)
{
    int retval = ERROR_OK;

    command_print(cmd, "K1921VG015 flash driver\n"
                           "version: %d.%d\n"
                           "author: Alexander Dykhno\n"
                           "mail: dykhno@niiet.ru",
                           FLASH_DRIVER_VER>>16,
                           FLASH_DRIVER_VER&0xFFFF);

    return retval;
}

COMMAND_HANDLER(k1921vg015_handle_get_uid_command)
{
    struct target *target = get_current_target(CMD_CTX);
    int retval;
    uint32_t chipid;
    uint32_t uid[4];

    retval = ERROR_OK;

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    retval = target_read_u32(target, SIU_CHIPID, &chipid);
    if ((retval != ERROR_OK) || (chipid != SIU_CHIPID_K1921VG015)) {
        LOG_INFO("CHIPID error");
        return ERROR_FAIL;
    }

    /* read device uid register */
    retval = target_read_u32(target, SIU_UID0, &uid[0]);
    retval = target_read_u32(target, SIU_UID1, &uid[1]);
    retval = target_read_u32(target, SIU_UID2, &uid[2]);
    retval = target_read_u32(target, SIU_UID3, &uid[3]);

    command_print(cmd, "K1921VG015 UID[3:0](HEX) = %X %X %X %X\n",
                           uid[3],
                           uid[2],
                           uid[1],
                           uid[0]);

    return retval;
}

static const struct command_registration k1921vg015_exec_command_handlers[] = {
    {
        .name = "read",
        .handler = k1921vg015_handle_read_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr) address",
        .help = "Read two 32-bit words from FLASH main or NVR region address. Address should be 8 bytes aligned.",
    },
    {
        .name = "write",
        .handler = k1921vg015_handle_write_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr) (erase|save|none) address data0 data1 data2 data3",
        .help = "Write four 32-bit words to FLASH main or NVR region address. Address should be 8 bytes aligned. There is option that selects between to erase modified sector, to save all data and to do nothing - only write.",
    },
    {
        .name = "mass_erase",
        .handler = k1921vg015_handle_mass_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr)",
        .help = "Erase entire FLASH main or NVR region",
    },
    {
        .name = "erase",
        .handler = k1921vg015_handle_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr) first_sector_num last_sector_num",
        .help = "Erase sectors of FLASH main or NVR region, starting at sector first up to and including last",
    },
    {
        .name = "protect",
        .handler = k1921vg015_handle_protect_command,
        .mode = COMMAND_EXEC,
        .usage = "(main|nvr) (enable|disable)",
        .help = "FLASH main or NVR region write protect control. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "debug",
        .handler = k1921vg015_handle_debug_command,
        .mode = COMMAND_EXEC,
        .usage = "(enable|disable)",
        .help = "Control core debug function. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "jtag",
        .handler = k1921vg015_handle_jtag_command,
        .mode = COMMAND_EXEC,
        .usage = "(enable|disable)",
        .help = "Control JTAG interface. Power on reset cycle is required for the new settings to take effect.",
    },
    {
        .name = "srv_erase",
        .handler = k1921vg015_handle_srv_erase_command,
        .mode = COMMAND_EXEC,
        .usage = "",
        .help = "Perform mass erase of all chip flash memories. Power on reset cycle and SERVEN pin tied low are required for the return to normal operation mode.",
    },
    {
        .name = "driver_info",
        .handler = k1921vg015_handle_driver_info_command,
        .mode = COMMAND_EXEC,
        .usage = "",
        .help = "Show information about flash driver",
    },
    {
        .name = "get_uid",
        .handler = k1921vg015_handle_get_uid_command,
        .mode = COMMAND_EXEC,
        .usage = "",
        .help = "Show UID 128 bit",
    },    
    COMMAND_REGISTRATION_DONE
};

static const struct command_registration k1921vg015_command_handlers[] = {
    {
        .name = "k1921vg015",
        .mode = COMMAND_ANY,
        .help = "k1921vg015 flash command group",
        .usage = "",
        .chain = k1921vg015_exec_command_handlers,
    },
    COMMAND_REGISTRATION_DONE
};

/*==============================================================================
 *                          FLASH INTERFACE
 *==============================================================================
 */

FLASH_BANK_COMMAND_HANDLER(k1921vg015_flash_bank_command)
{
    struct k1921vg015_flash_bank *k1921vg015_info;

    if (CMD_ARGC < 6)
        return ERROR_COMMAND_SYNTAX_ERROR;

    k1921vg015_info = malloc(sizeof(struct k1921vg015_flash_bank));

    bank->driver_priv = k1921vg015_info;

    /* information will be updated by probing */
    k1921vg015_info->probed = false;
    k1921vg015_info->chip_name = "K1921VG015";
    //k1921vg015_info->bmodedis = true;
    k1921vg015_info->flashwe = true;
    k1921vg015_info->nvrwe = true;

    return ERROR_OK;
}

static int k1921vg015_protect_check(struct flash_bank *bank)
{
    struct k1921vg015_flash_bank *k1921vg015_info = bank->driver_priv;

    uint32_t protect_enable;
  /*      LOG_WARNING("Function:  k1921vg015_protect_check");
    if ((k1921vg015_info->bmodedis && !k1921vg015_info->flashwe) ||
        (!k1921vg015_info->bmodedis && !k1921vg015_info->nvrwe))
        protect_enable = 1;
    else
        protect_enable = 0;

    for (unsigned int i = 0; i < bank->num_sectors; i++)
        bank->sectors[i].is_protected = protect_enable;
*/
    return ERROR_OK;
}

static int k1921vg015_mass_erase(struct flash_bank *bank)
{
    struct k1921vg015_flash_bank *k1921vg015_info = bank->driver_priv;

    struct target *target = bank->target;
    int retval;
    uint32_t region;

        LOG_WARNING("Function:  k1921vg015_mass_erase");
    //if (k1921vg015_info->bmodedis)
        region = MAIN_REGION;
 /*   else
        region = NVR_REGION;*/

    retval = k1921vg015_flash_mass_erase(target, region);
    if (retval != ERROR_OK)
        return retval;

    for (unsigned int i = 0; i <= bank->num_sectors; i++) {
        bank->sectors[i].is_erased = 1;
    }

    return retval;
}

static int k1921vg015_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
    struct k1921vg015_flash_bank *k1921vg015_info = bank->driver_priv;
    struct target *target = bank->target;

    int retval = ERROR_FLASH_OPERATION_FAILED;
    uint32_t region;
    LOG_WARNING("Function:  k1921vg015_erase");
/*    LOG_INFO("Write_block Addr = 0x%x; param[0] = 0x%x;  param[1] = 0x%x; param[2] = 0x%x; param[3] = 0x%x; param[4] = 0x%x;", address, reg_params[0].value,reg_params[1].value,reg_params[2].value,reg_params[3].value,reg_params[4].value); */

  //  if (k1921vg015_info->bmodedis)
        region = MAIN_REGION;
  /*  else
        region = NVR_REGION;*/

    if (bank->target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    if ((first == 0) && (last == (bank->num_sectors - 1))) {
        retval = k1921vg015_mass_erase(bank);
        if (retval != ERROR_OK)
            return retval;
    } else {
        /* erasing pages */
        for (unsigned int i = first; i <= last; i++) {
            retval = k1921vg015_flash_erase(target, i, region);
            if (retval != ERROR_OK)
                return retval;
            bank->sectors[i].is_erased = 1;
        }
    }

    return retval;
}

static int k1921vg015_protect(struct flash_bank *bank, int set, int first, int last)
{
    struct target *target = bank->target;
    struct k1921vg015_flash_bank *k1921vg015_info = bank->driver_priv;

    LOG_WARNING("Function:  k1921vg015_protect");

    int retval;
    uint32_t region;
//    if (k1921vg015_info->bmodedis)
        region = MAIN_REGION;
/*    else
        region = NVR_REGION;*/

    if (target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    LOG_INFO("Plese wait ...");

    uint32_t param_mask = (region == NVR_REGION) ? CFGWORD_CFGWE : CFGWORD_FLASHWE;
    retval = k1921vg015_flash_modify_cfgword(target, set, param_mask);
    if (retval != ERROR_OK)
        return retval;

    return retval;
}

static int k1921vg015_write_block(struct flash_bank *bank, const uint8_t *buffer,
        uint32_t offset, uint32_t count)
{
    struct target *target = bank->target;
    struct k1921vg015_flash_bank *k1921vg015_info = bank->driver_priv;
    uint32_t buffer_size = 4096 + 16; /* 8 bytes for rp and wp */
    struct working_area *write_algorithm;
    struct working_area *source;
    uint32_t address = bank->base + offset;
    struct reg_param reg_params[4];
    //struct armv7m_algorithm armv7m_info;
    int retval = ERROR_OK;
    uint32_t reg_a0,reg_a1,reg_a2,reg_a3,reg_a4,reg_a5,reg_s5,reg_s6,reg_s7,save_buff_size;
    

    LOG_WARNING("Function:  k1921vg015_write_block");
    LOG_WARNING("ADDR buffer 0x%"PRIx32"\n"
                  "buffer[0] 0x%"PRIx32"\n"  
                  "buffer[1] 0x%"PRIx32"\n"  
                  "buffer[2] 0x%"PRIx32"\n"  
                  "buffer[3] 0x%"PRIx32"\n"  
                  "offset 0x%"PRIx32"\n"
                  "count 0x%"PRIx32"\n"
                  "buffer_size 0x%"PRIx32"\n****************\n",
                buffer,
                buffer[0],
                buffer[1],
                buffer[2],
                buffer[3],                                
                offset,
                count,
                buffer_size);

//    LOG_INFO("Plese wait ...");
    static const uint8_t k1921vg015_flash_write_code[] = {
#include "../../../contrib/loaders/flash/niietrv/k1921vg015.inc"
/*  0x37, 0xDC, 0x00, 0x30, 0x11, 0xA0, 0x02, 0x90, 0x36, 0x8B, 
  0x63, 0x0C, 0x0B, 0x04, 0xB2, 0x8A, 0xB3, 0x07, 0x5B, 0x41, 
  0xF5, 0xDB, 0x03, 0xAB, 0x0A, 0x00, 0x23, 0x22, 0x6C, 0x01, 
  0x91, 0x0A, 0x03, 0xAB, 0x0A, 0x00, 0x23, 0x24, 0x6C, 0x01, 
  0x91, 0x0A, 0x03, 0xAB, 0x0A, 0x00, 0x23, 0x26, 0x6C, 0x01, 
  0x91, 0x0A, 0x03, 0xAB, 0x0A, 0x00, 0x23, 0x28, 0x6C, 0x01, 
  0x91, 0x0A, 0x23, 0x20, 0xDC, 0x00, 0xC1, 0x06, 0x23, 0x22, 
  0xAC, 0x04, 0x83, 0x2B, 0x8C, 0x04, 0x93, 0xF7, 0x1B, 0x00, 
  0xE5, 0xFF, 0xA1, 0x0A, 0x41, 0x06, 0xFD, 0x15, 0x89, 0xC5, 
  0x7D, 0xB7, 0x13, 0x05, 0x00, 0x00, 0x48, 0xC2, 0x5E, 0x85, 
  0x4D, 0xB7*/
    };
    
    save_buff_size = sizeof(k1921vg015_flash_write_code);
    LOG_WARNING("k1921vg015_write_block : <flash write code>");

    /*LOG_WARNING("ADDR write_algorithm 0x%"PRIx32"\n"
                  "Size write_algorithm 0x%"PRIx32"\n"
                  "Addr struct write_algorithm 0x%"PRIx32"\n"
                  "Addr struct k1921vg015_flash_write_code 0x%"PRIx32"\n****************\n",
                write_algorithm->address,
                sizeof(k1921vg015_flash_write_code),
                &write_algorithm,
                &k1921vg015_flash_write_code);*/
    /* flash write code */
    if (target_alloc_working_area(target, sizeof(k1921vg015_flash_write_code),
            &write_algorithm) != ERROR_OK) {
        LOG_WARNING("no working area available, can't do block memory writes");
        return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
    }

    retval = target_write_buffer(target, write_algorithm->address,
            sizeof(k1921vg015_flash_write_code), k1921vg015_flash_write_code);
    if (retval != ERROR_OK)
        return retval;

    LOG_WARNING("k1921vg015_write_block : <memory buffer>");
    LOG_WARNING("ADDR write_algorithm 0x%"PRIx32"\n"
                  "Size write_algorithm 0x%"PRIx32"\n"
                  "Size write_algorithm 0x%"PRIx32"\n"
                  "Addr struct write_algorithm 0x%"PRIx32"\n"
                  "Addr struct k1921vg015_flash_write_code 0x%"PRIx32"\n****************\n",
                write_algorithm->address,
                sizeof(k1921vg015_flash_write_code),
                save_buff_size,
                &write_algorithm,
                &k1921vg015_flash_write_code);

    /* memory buffer */
    while (target_alloc_working_area_try(target, buffer_size, &source) != ERROR_OK) {
        buffer_size /= 2;
        buffer_size &= ~(FLASH_WORD_WIDTH*4-1); /* Make sure it's aligned */
        buffer_size += 8; /* And 8 bytes for WP and RP */
        if (buffer_size <= 256) {
            /* we already allocated the writing code, but failed to get a
             * buffer, free the algorithm */
            target_free_working_area(target, write_algorithm);

            LOG_WARNING("no large enough working area available, can't do block memory writes");
            return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
        }
    }

    LOG_WARNING("k1921vg015_write_block : <init_reg_param>");
    init_reg_param(&reg_params[0], "a0", 32, PARAM_IN_OUT); /* write_cmd base (in), status (out) */
    init_reg_param(&reg_params[1], "a1", 32, PARAM_OUT);    /* count (64bit) */
    init_reg_param(&reg_params[2], "a2", 32, PARAM_OUT);    /* buffer start */
    //init_reg_param(&reg_params[3], "a3", 32, PARAM_OUT);    /* buffer end */
    init_reg_param(&reg_params[3], "a3", 32, PARAM_IN_OUT); /* target address */

    uint32_t flash_cmd;
    flash_cmd = FLASH_CMD_KEY | FLASH_CMD_WR;
    //if (!k1921vg015_info->bmodedis)
    //    flash_cmd |= FLASH_CMD_NVRON;

    LOG_WARNING("k1921vg015_write_block : <buf_set_u32>");


    buf_set_u32(reg_params[0].value, 0, 32, flash_cmd);
    buf_set_u32(reg_params[1].value, 0, 32, count);
    buf_set_u32(reg_params[2].value, 0, 32, source->address);
    //buf_set_u32(reg_params[3].value, 0, 32, source->address + source->size);
    buf_set_u32(reg_params[3].value, 0, 32, address);

    LOG_WARNING("reg_params[0].value 0x%"PRIx32"\n"
                  "reg_params[1].value 0x%"PRIx32"\n"
                  "reg_params[2].value 0x%"PRIx32"\n"
                  "reg_params[3].value 0x%"PRIx32"\n****************\n",
                reg_params[0].value,
                reg_params[1].value,
                reg_params[2].value,
                reg_params[3].value);    

//LOG_INFO("Write_block Addr = 0x%x; param[0] = 0x%x;  param[1] = 0x%x; param[2] = 0x%x; param[3] = 0x%x; ", address, reg_params[0].value,reg_params[1].value,reg_params[2].value,reg_params[3].value);

/*    armv7m_info.common_magic = ARMV7M_COMMON_MAGIC;
    armv7m_info.core_mode = ARM_MODE_THREAD;
*/
    /*retval = target_run_flash_async_algorithm(target, buffer, count, FLASH_WORD_WIDTH*4,
            0, NULL,
            5, reg_params,
            source->address, source->size,
            write_algorithm->address, 0,
            NULL); */

        riscv_get_register(target, &reg_a0, GDB_REGNO_A0);
        riscv_get_register(target, &reg_a1, GDB_REGNO_A1);
        riscv_get_register(target, &reg_a2, GDB_REGNO_A2);
        riscv_get_register(target, &reg_a3, GDB_REGNO_A3);
        riscv_get_register(target, &reg_a4, GDB_REGNO_A4);
        riscv_get_register(target, &reg_a5, GDB_REGNO_A5);
        riscv_get_register(target, &reg_s5, GDB_REGNO_S5);
        riscv_get_register(target, &reg_s6, GDB_REGNO_S6);
        riscv_get_register(target, &reg_s7, GDB_REGNO_S7); 

        LOG_WARNING("ADDR write_algorithm 0x%"PRIx32"\n"
                  "Size write_algorithm 0x%"PRIx32"\n"
                   "a0= 0x%"PRIx32"; \n" 
                   "a1= 0x%"PRIx32"; \n" 
                   "a2= 0x%"PRIx32"; \n" 
                   "a3= 0x%"PRIx32"; \n" 
                   "a4= 0x%"PRIx32"; \n" 
                   "a5= 0x%"PRIx32"; \n"                    
                   "s5= 0x%"PRIx32"; \n" 
                   "s6= 0x%"PRIx32"; \n" 
                   "s7= 0x%"PRIx32"; \n",
                write_algorithm->address,
                save_buff_size,
                reg_a0,
                reg_a1,
                reg_a2,
                reg_a3,
                reg_a4,
                reg_a5,
                reg_s5,
                reg_s6,
                reg_s7);

        riscv_set_register(target, GDB_REGNO_A0, 0x01010101);
        riscv_set_register(target, GDB_REGNO_A1, 0x02020202);
        riscv_set_register(target, GDB_REGNO_A2, 0x03030303);
        riscv_set_register(target, GDB_REGNO_A3, 0x04040404);
        
        
        
        riscv_get_register(target, &reg_a0, GDB_REGNO_A0);
        riscv_get_register(target, &reg_a1, GDB_REGNO_A1);
        riscv_get_register(target, &reg_a2, GDB_REGNO_A2);
        riscv_get_register(target, &reg_a3, GDB_REGNO_A3);
        riscv_get_register(target, &reg_a4, GDB_REGNO_A4);
        riscv_get_register(target, &reg_a5, GDB_REGNO_A5);
        riscv_get_register(target, &reg_s5, GDB_REGNO_S5);
        riscv_get_register(target, &reg_s6, GDB_REGNO_S6);
        riscv_get_register(target, &reg_s7, GDB_REGNO_S7); 

        LOG_WARNING("ADDR write_algorithm 0x%"PRIx32"\n"
                  "Size write_algorithm 0x%"PRIx32"\n"
                  "Registers after manual writing:\n"
                   "a0= 0x%"PRIx32"; \n" 
                   "a1= 0x%"PRIx32"; \n" 
                   "a2= 0x%"PRIx32"; \n" 
                   "a3= 0x%"PRIx32"; \n" 
                   "a4= 0x%"PRIx32"; \n" 
                   "a5= 0x%"PRIx32"; \n"                    
                   "s5= 0x%"PRIx32"; \n" 
                   "s6= 0x%"PRIx32"; \n" 
                   "s7= 0x%"PRIx32"; \n",
                write_algorithm->address,
                save_buff_size,
                reg_a0,
                reg_a1,
                reg_a2,
                reg_a3,
                reg_a4,
                reg_a5,
                reg_s5,
                reg_s6,
                reg_s7);

    LOG_WARNING("k1921vg015_write_block : <target_run_algorithm>");
    retval = target_run_algorithm(target, 0, NULL, 4, reg_params,
                write_algorithm->address, write_algorithm->address+sizeof(k1921vg015_flash_write_code) - 4,
                10000, NULL);


    if (retval == ERROR_FLASH_OPERATION_FAILED){
        riscv_get_register(target, &reg_a0, GDB_REGNO_A0);
        riscv_get_register(target, &reg_a1, GDB_REGNO_A1);
        riscv_get_register(target, &reg_a2, GDB_REGNO_A2);
        riscv_get_register(target, &reg_a3, GDB_REGNO_A3);
        riscv_get_register(target, &reg_a4, GDB_REGNO_A4);
        riscv_get_register(target, &reg_a5, GDB_REGNO_A5);
        riscv_get_register(target, &reg_s5, GDB_REGNO_S5);
        riscv_get_register(target, &reg_s6, GDB_REGNO_S6);
        riscv_get_register(target, &reg_s7, GDB_REGNO_S7);        
        LOG_WARNING("flash write failed at address 0x%"PRIx32"\n"
                   "a0= 0x%"PRIx32"; \n" 
                   "a1= 0x%"PRIx32"; \n" 
                   "a2= 0x%"PRIx32"; \n" 
                   "a3= 0x%"PRIx32"; \n" 
                   "a4= 0x%"PRIx32"; \n" 
                   "a5= 0x%"PRIx32"; \n"                    
                   "s5= 0x%"PRIx32"; \n" 
                   "s6= 0x%"PRIx32"; \n" 
                   "s7= 0x%"PRIx32"; \n",
                buf_get_u32(reg_params[3].value, 0, 32),
                reg_a0,
                reg_a1,
                reg_a2,
                reg_a3,
                reg_a4,
                reg_a5,
                reg_s5,
                reg_s6,
                reg_s7);

        
       // LOG_INFO("a0= 0x%"PRIx32"; a1= 0x%"PRIx32"; a2= 0x%"PRIx32"; a3= 0x%"PRIx32"; ", reg_a0,reg_a1,reg_a2,reg_a3);

        
    }
    LOG_WARNING("k1921vg015_write_block : <target_free_working_area>");
    target_free_working_area(target, source);
    target_free_working_area(target, write_algorithm);

    destroy_reg_param(&reg_params[0]);
    destroy_reg_param(&reg_params[1]);
    destroy_reg_param(&reg_params[2]);
    destroy_reg_param(&reg_params[3]);
    //destroy_reg_param(&reg_params[4]);

    return retval;
}

static int k1921vg015_write(struct flash_bank *bank, const uint8_t *buffer,
        uint32_t offset, uint32_t count)
{
    struct target *target = bank->target;
    struct k1921vg015_flash_bank *k1921vg015_info = bank->driver_priv;
    uint8_t *new_buffer = NULL;

    LOG_WARNING("Function:  k1921vg015_write");
    LOG_WARNING("ADDR buffer 0x%"PRIx32"\n"
                  "buffer[0] 0x%"PRIx32"\n"  
                  "buffer[1] 0x%"PRIx32"\n"  
                  "buffer[2] 0x%"PRIx32"\n"  
                  "buffer[3] 0x%"PRIx32"\n"  
                  "offset 0x%"PRIx32"\n"
                  "count 0x%"PRIx32"\n"
                  "Bank Base 0x%"PRIx32"\n"
                  "Bank Size 0x%"PRIx32"\n"
                  "Bank num_sectors 0x%"PRIx32"\n****************\n",
                buffer,
                buffer[0],
                buffer[1],
                buffer[2],
                buffer[3],                                
                offset,
                count,
                bank->base,
                bank->size,
                bank->num_sectors);     
    if (bank->target->state != TARGET_HALTED) {
        LOG_ERROR("Target not halted");
        return ERROR_TARGET_NOT_HALTED;
    }

    if (offset & 0x0F) {
        LOG_ERROR("offset 0x%" PRIx32 " breaks required 4-word alignment", offset);
        return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
    }

    /* If there's an odd number of words, the data has to be padded. Duplicate
     * the buffer and use the normal code path with a single block write since
     * it's probably cheaper than to special case the last odd write using
     * discrete accesses. */

    int rem = count % FLASH_WORD_WIDTH*4;
    if (rem) {
        new_buffer = malloc(count + FLASH_WORD_WIDTH*4 - rem);
        if (new_buffer == NULL) {
            LOG_ERROR("Odd number of words to write and no memory for padding buffer");
            return ERROR_FAIL;
        }
        LOG_INFO("Odd number of words to write, padding with 0xFFFFFFFF");
        buffer = memcpy(new_buffer, buffer, count);
        while (rem < FLASH_WORD_WIDTH*4) {
            new_buffer[count++] = 0xff;
            rem++;
        }
    }

    int retval;

    /* TODO: Write buffer with registers */
    LOG_INFO("Flash programm size = %d bytes with registers",count);
    uint32_t new_flash_cmd = FLASH_CMD_KEY | FLASH_CMD_WR;
    uint32_t new_addr = offset;
    uint32_t new_count = count;
    uint8_t *new_mybuffer = *buffer;
    for (unsigned int i = 0; i < (count+15)/16; i ++) {
        LOG_INFO("Flash programm: Write Addr = 0x%" PRIx32 "",new_addr);
        retval = ERROR_OK;
        retval = target_write_u32(target, FLASH_ADDR, new_addr);
        if (retval != ERROR_OK)
            return retval;
        for (int i = 0; i < FLASH_WORD_WIDTH; i++) {
            retval = target_write_u32(target, FLASH_DATA0 + i*4, *((volatile uint32_t *) (new_mybuffer + i*4)));
            if (retval != ERROR_OK)
                return retval;
        }
        retval = target_write_u32(target, FLASH_CMD, new_flash_cmd);
        if (retval != ERROR_OK)
            return retval;
        retval = k1921vg015_flash_waitdone(target);
        if (retval != ERROR_OK)
            return retval;
        
        new_addr += 16;
        new_mybuffer+= 16;
    }    
    LOG_INFO("End Flash programm size = %d bytes with registers",count);
    //k1921vg015_flash_write();
    return retval;
    /* TODO: End*/
    /* try using block write */
    retval = k1921vg015_write_block(bank, buffer, offset, count/(FLASH_WORD_WIDTH*4));
    uint32_t flash_addr, flash_cmd, flash_data;

    if (retval == ERROR_TARGET_RESOURCE_NOT_AVAILABLE) {
        /* if block write failed (no sufficient working area),
         * we use normal (slow) single halfword accesses */
        LOG_WARNING("Can't use block writes, falling back to single memory accesses");
        LOG_INFO("Plese wait ..."); /* it`s quite a long process */

        flash_cmd = FLASH_CMD_KEY | FLASH_CMD_WR;
        /* chose between main and nvr region */

        /*if (!k1921vg015_info->bmodedis)
            flash_cmd |= FLASH_CMD_NVRON;*/

        /* write multiple bytes per try */
        for (unsigned int i = 0; i < count; i += FLASH_WORD_WIDTH*4) {
            /* current addr */
            LOG_INFO("%d byte of %d", i, count);
            flash_addr = offset + i;
            retval = target_write_u32(target, FLASH_ADDR, flash_addr);
            if (retval != ERROR_OK)
                goto free_buffer;

            /* Prepare data */
            uint32_t value[FLASH_WORD_WIDTH];
            memcpy(&value, buffer + i, FLASH_WORD_WIDTH*sizeof(uint32_t));

            /* place in reg data */
            for (int j = 0; j < FLASH_WORD_WIDTH; j++) {
                flash_data = value[j];
                retval = target_write_u32(target, FLASH_DATA0 + j*4, flash_data);
                if (retval != ERROR_OK)
                    goto free_buffer;
            }

            /* write start */
            retval = target_write_u32(target, FLASH_CMD, flash_cmd);
            if (retval != ERROR_OK)
                goto free_buffer;

            /* status check */
            retval = k1921vg015_flash_waitdone(target);
            if (retval != ERROR_OK)
                goto free_buffer;
        }

    }

free_buffer:
    if (new_buffer)
        free(new_buffer);

    return retval;
}

static int k1921vg015_probe(struct flash_bank *bank)
{
    struct k1921vg015_flash_bank *k1921vg015_info = bank->driver_priv;
    struct target *target = bank->target;

    if (bank->sectors) {
        free(bank->sectors);
        bank->sectors = NULL;
    }
    uint32_t retval;
    uint32_t chipid;
    uint32_t uid[4];

    retval = target_read_u32(target, SIU_CHIPID, &chipid);
    if ((retval != ERROR_OK) || (chipid != SIU_CHIPID_K1921VG015)) {
        LOG_INFO("CHIPID error");
        return ERROR_FAIL;
    }

    LOG_INFO("K1921VG015 detected");

	/* read device uid register */
	retval = target_read_u32(target, SIU_UID0, &uid[0]);
	retval = target_read_u32(target, SIU_UID1, &uid[1]);
	retval = target_read_u32(target, SIU_UID2, &uid[2]);
	retval = target_read_u32(target, SIU_UID3, &uid[3]);
     

	LOG_INFO("device uid = 0x%08" PRIx32 " 0x%08 " PRIx32 " 0x%08 " PRIx32 " 0x%08 " PRIx32 "", uid[0], uid[1], uid[2], uid[3]);

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
        LOG_INFO("Normal mode");
        uint32_t cfgword = 0xFFFFFFFF;
        /* read CFGWORD */
        retval = k1921vg015_flash_read_cfgword(target, &cfgword);
        if (retval != ERROR_OK)
            return retval;

        if (!(cfgword & CFGWORD_FLASHWE))
            k1921vg015_info->flashwe = false;
        if (!(cfgword & CFGWORD_CFGWE))
            k1921vg015_info->nvrwe = false;

        bank->base = 0x80000000;
        bank->size = FLASH_PAGE_SIZE*FLASH_MAIN_PAGE_TOTAL;  //1048576;    /* FLASH_PAGE_SIZE*FLASH_MAIN_PAGE_TOTAL;*/
        bank->num_sectors = FLASH_MAIN_PAGE_TOTAL; //256; /* FLASH_MAIN_PAGE_TOTAL;*/

        snprintf(k1921vg015_info->chip_brief,
                sizeof(k1921vg015_info->chip_brief),
                "\n"
                "[CFGWORD]\n"
                "FLASH main region write protection :\n"
                "    %s\n"
                "FLASH NVR region write protection :\n"
                "    %s\n",
                k1921vg015_info->flashwe ? "disable" : "enable",
                k1921vg015_info->nvrwe ? "disable" : "enable");     
    } else {
        LOG_INFO("Service mode activate");
        bank->size = FLASH_PAGE_SIZE*FLASH_MAIN_PAGE_TOTAL; //1048576;    /* FLASH_PAGE_SIZE*FLASH_MAIN_PAGE_TOTAL;*/
        bank->num_sectors = FLASH_MAIN_PAGE_TOTAL; //256; /* FLASH_MAIN_PAGE_TOTAL */;

        sprintf(k1921vg015_info->chip_brief,
                "\n"
                "SERVEN was HIGH during startup. Device entered service mode.\n"
                "All flash memories were locked and can not be readen.\n"
                "If you want to perform emergency erase (erase all entire memory),\n"
                "please use \"srv_erase\" command and reset device.\n"
                "Do not forget, SERVEN should be pulled down during reset for returning to normal operation mode.\n"
                );
        LOG_INFO("\nSERVEN was HIGH during startup. Device entered service mode.\n All flash memories were locked and can not be readen.\n If you want to perform emergency erase (erase all entire memory),\n please use \"srv_erase\" command and reset device.\n Do not forget, SERVEN should be pulled down during reset for returning to normal operation mode.\n");        
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
    LOG_INFO("Bank: base = 0x%08" PRIx32 " size = 0x%08 " PRIx32 " num_sectors = 0x%08 " PRIx32 " page_total = 0x%08 " PRIx32 " page_size = 0x%08 " PRIx32 "", bank->base, bank->size, bank->num_sectors, bank->num_sectors, page_size);
    k1921vg015_info->probed = true;

    return ERROR_OK;
}

static int k1921vg015_auto_probe(struct flash_bank *bank)
{
    struct k1921vg015_flash_bank *k1921vg015_info = bank->driver_priv;
    if (k1921vg015_info->probed)
        return ERROR_OK;
    return k1921vg015_probe(bank);
}

static int get_k1921vg015_info(struct flash_bank *bank, struct command_invocation *cmd)
{
    struct k1921vg015_flash_bank *k1921vg015_info = bank->driver_priv;
    LOG_INFO("\nNIIET %s\n%s", k1921vg015_info->chip_name, k1921vg015_info->chip_brief);
    command_print_sameline(cmd, " ");

    return ERROR_OK;
}


const struct flash_driver k1921vg015_flash = {
    .name = "k1921vg015",
    .usage = "flash bank <name> k1921vg015 <base> <size> 0 0 <target#>",
    .commands = k1921vg015_command_handlers,
    .flash_bank_command = k1921vg015_flash_bank_command,
    .erase = k1921vg015_erase,
    .protect = k1921vg015_protect,
    .write = k1921vg015_write,
    .read = default_flash_read,
    .probe = k1921vg015_probe,
    .auto_probe = k1921vg015_auto_probe,
    .erase_check = default_flash_blank_check,
    .protect_check = k1921vg015_protect_check,
    .info = get_k1921vg015_info,
	.free_driver_priv = default_flash_free_driver_priv,     
};
