/**
 * \file
 * \brief           k1921vg5t flash driver
 * \copyright       NIIET, 2025
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "imp.h"
#include <helper/binarybuffer.h>
#include <target/algorithm.h>
#include <target/armv7m.h>

#define FLASH_DRIVER_VER    0x00010000

/*==============================================================================
 *                    K1921VG5T CONTROL REGS
 *==============================================================================
 */
/*-- SIU ---------------------------------------------------------------------*/
#define SIU_CHIPID_K1921VG5T    0x04e4c300
#define SIU_BASE                0x50003000
#define SIU_SERVCTL             (SIU_BASE + 0x04)
#define SIU_CHIPID              (SIU_BASE + 0x00)
/*---- SIU->SERVCTL: Service mode control register */
#define SIU_SERVCTL_DONE        (1<<8)              /* Full clear done flag */
#define SIU_SERVCTL_SERVEN      (1<<0)             /* Service mode enable flag */

#define MAIN_REGION             0
#define NVR_REGION              1
/*-- FLASH ------------------------------------------------------------------*/
#define FLASH_PAGE_SIZE        1024
#define FLASH_PAGE_TOTAL       512
#define FLASH_WORD_WIDTH       2
#define FLASH_BASE             0x50002000
#define FLASH_BANK_ADDR        0x00000000

#define FLASH_ADDR             (FLASH_BASE + 0x00)
#define FLASH_DATA0            (FLASH_BASE + 0x10)
#define FLASH_DATA1            (FLASH_BASE + 0x14)
#define FLASH_CMD              (FLASH_BASE + 0x40)
#define FLASH_STAT             (FLASH_BASE + 0x44)

/*---- FLASH->CMD: Command register */
#define FLASH_CMD_RD           (1<<0)              /* Read data in region */
#define FLASH_CMD_WR           (1<<1)              /* Write data in region */
#define FLASH_CMD_ERSEC        (1<<2)              /* Sector erase in region */
#define FLASH_CMD_ERALL        (1<<3)              /* Erase all sectors in region */
#define FLASH_CMD_DPD          (1<<4)              /* Enter/Exit power down mode */ 
#define FLASH_CMD_NVRON        (1<<8)              /* Select NVR region for command operation */
#define FLASH_CMD_KEY          (0xC0DE<<16)        /* Command enable key */
/*---- FLASH->STAT: Status register */
#define FLASH_STAT_BUSY        (1<<0)              /* Flag operation busy */

/*---- CFGWORD (in FLASH NVR)----------------------------------------------- */
#define CFGWORD_PAGE                15
#define CFGWORD_ADDR_OFFSET         0x3F0
#define CFGWORD_ADDR                (FLASH_PAGE_SIZE*CFGWORD_PAGE+CFGWORD_ADDR_OFFSET)
//#define CFGWORD_ADDR                0x3FF8 
#define CFGWORD_JTAGEN              (1<<2)          /* Enable JTAG interface */
#define CFGWORD_CFGWE               (1<<1)          /* FLASH NVR region write enable */
#define CFGWORD_FLASHWE             (1<<0)          /* FLASH main region write enable */

/**
 * Private data for flash driver.
 */
struct k1921vg5t_flash_bank {
	/* target params */
	bool probed;
	char *chip_name;
	char chip_brief[4096];
	bool flashwe;
	bool cfgwe;
};


/*==============================================================================
 *                     FLASH HARDWARE CONTROL FUNCTIONS
 *==============================================================================
 */

/**
 * Wait while operation with flash being performed
 */
static int k1921vg5t_flash_waitdone(struct target *target)
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
static int k1921vg5t_flash_erase(struct target *target, int page_num, uint32_t region)
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
	retval = k1921vg5t_flash_waitdone(target);
	if (retval != ERROR_OK)
		return retval;

	return retval;
}

/**
 * Mass flash erase
 */
static int k1921vg5t_flash_mass_erase(struct target *target, uint32_t region)
{
	int retval = ERROR_OK;

	uint32_t flash_cmd = FLASH_CMD_KEY | FLASH_CMD_ERALL| FLASH_CMD_ERSEC;
	if (region == NVR_REGION)
		flash_cmd |= FLASH_CMD_NVRON;

	retval = target_write_u32(target, FLASH_CMD, flash_cmd);
	if (retval != ERROR_OK)
		return retval;
	retval = k1921vg5t_flash_waitdone(target);
	if (retval != ERROR_OK)
		return retval;

	return retval;
}

/**
 * Read flash address
 */
static int k1921vg5t_flash_read(struct target *target, uint32_t addr, uint32_t *data, uint32_t region)
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
	retval = k1921vg5t_flash_waitdone(target);
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
static int k1921vg5t_flash_write(struct target *target, uint32_t addr, uint32_t *data, uint32_t region)
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
	retval = k1921vg5t_flash_waitdone(target);
	if (retval != ERROR_OK)
		return retval;

	return retval;
}

/**
 * Dump flash sector.
 */
static int k1921vg5t_flash_sector_dump(struct target *target, uint32_t *dump, int page_num, uint32_t region)
{
	int retval = ERROR_OK;

	uint32_t data[FLASH_WORD_WIDTH];
	int first = page_num * FLASH_PAGE_SIZE;
	int last = first + FLASH_PAGE_SIZE;

	for (int i = first; i < last; i+=FLASH_WORD_WIDTH*4) {
		retval = k1921vg5t_flash_read(target, i, data, region);
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
static int k1921vg5t_flash_sector_load(struct target *target, uint32_t *dump, int page_num, uint32_t region)
{
	int i;
	int retval = ERROR_OK;

	uint32_t data[FLASH_WORD_WIDTH];
	int first = page_num*FLASH_PAGE_SIZE;
	int last = first + FLASH_PAGE_SIZE;

	retval = k1921vg5t_flash_erase(target, page_num, region);
	if (retval != ERROR_OK)
		return retval;

	for (i = first; i < last; i+=FLASH_WORD_WIDTH*4) {
		for (int j = 0; j < FLASH_WORD_WIDTH; j++) {
			data[j] = dump[(i%FLASH_PAGE_SIZE)/4+j];
		}
		retval = k1921vg5t_flash_write(target, i, data, region);
		if (retval != ERROR_OK)
			return retval;
	}

	return retval;
}

/**
 * Read CFGWORD
 */
static int k1921vg5t_flash_read_cfgword(struct target *target, uint32_t *cfgword)
{
	int retval = ERROR_OK;

	uint32_t data[FLASH_WORD_WIDTH];

	retval = k1921vg5t_flash_read(target, CFGWORD_ADDR, data, NVR_REGION);
	if (retval != ERROR_OK)
		return retval;

	*cfgword = data[0];

	return retval;
}

/**
 * Modify CFGWORD
 */
static int k1921vg5t_flash_modify_cfgword(struct target *target, uint32_t enable, uint32_t param_mask)
{
	int retval = ERROR_OK;

	/* dump */
	uint32_t flash_dump[FLASH_PAGE_SIZE/4];
	retval = k1921vg5t_flash_sector_dump(target, flash_dump, CFGWORD_PAGE, NVR_REGION);
	if (retval != ERROR_OK)
		return retval;

	/* modify dump */
	if (enable) /* we need to clear bit to enable */
		flash_dump[CFGWORD_ADDR_OFFSET/4] &= ~param_mask;
	else
		flash_dump[CFGWORD_ADDR_OFFSET/4] |= param_mask;

	/* write dump to flash */
	retval = k1921vg5t_flash_sector_load(target, flash_dump, CFGWORD_PAGE, NVR_REGION);
	if (retval != ERROR_OK)
		return retval;

	return retval;
}

/*==============================================================================
 *                          FLASH DRIVER COMMANDS
 *==============================================================================
 */
COMMAND_HANDLER(k1921vg5t_handle_read_command)
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

	retval = k1921vg5t_flash_read(target, flash_addr, flash_data, region);
	if (retval != ERROR_OK)
		return retval;
	command_print(CMD,  "Read FLASH %s region:\n"
							"    addr = 0x%04x, data = 0x%04x\n"
							"    addr = 0x%04x, data = 0x%04x", 
							(region == NVR_REGION) ? "nvr" : "main",
							flash_addr,   flash_data[0],
							flash_addr+4, flash_data[1]);
	return retval;
}

COMMAND_HANDLER(k1921vg5t_handle_write_command)
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

	int page_num = flash_addr/FLASH_PAGE_SIZE;

	command_print(CMD, "Write FLASH %s region%s:\n"
						   "    addr = 0x%04x, data = 0x%04x,\n"
						   "    addr = 0x%04x, data = 0x%04x,\n"
						   "    Please wait ... ", (region == NVR_REGION) ? "nvr" : "main",
												   save_sector ? " (save sector data)" :
																 erase_sector ? " (erase sector data)" : "",
												   flash_addr,   flash_data[0],
												   flash_addr+4, flash_data[1]);
	if (save_sector) {
		/* dump */
		uint32_t flash_dump[FLASH_PAGE_SIZE];
		retval = k1921vg5t_flash_sector_dump(target, flash_dump, page_num, region);
		if (retval != ERROR_OK)
			return retval;

		/* modify dump */
		flash_dump[(flash_addr%FLASH_PAGE_SIZE)/4]   = flash_data[0];
		flash_dump[(flash_addr%FLASH_PAGE_SIZE)/4+1] = flash_data[1];

		/* write dump to userflash */
		retval = k1921vg5t_flash_sector_load(target, flash_dump, page_num, region);
		if (retval != ERROR_OK)
			return retval;
	} else {
		if (erase_sector) {
			retval = k1921vg5t_flash_erase(target, page_num, region);
			if (retval != ERROR_OK)
				return retval;
		}
		retval = k1921vg5t_flash_write(target, flash_addr, flash_data, region);
		if (retval != ERROR_OK)
			return retval;
	}

	command_print(CMD, "done!");

	return retval;
}

COMMAND_HANDLER(k1921vg5t_handle_mass_erase_command)
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

	command_print(CMD, "Mass erase FLASH %s region\n"
						   "Please wait ... ", (region == NVR_REGION) ? "nvr" : "main");

	retval = k1921vg5t_flash_mass_erase(target, region);
	if (retval != ERROR_OK)
		return retval;

	command_print(CMD, "done!");

	return retval;
}

COMMAND_HANDLER(k1921vg5t_handle_erase_command)
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

	command_print(CMD, "Erase FLASH %s region sectors %d through %d\n"
						   "Please wait ... ", (region == NVR_REGION) ? "nvr" : "main", first, last);

	for (unsigned int i = first; i <= last; i++) {
		retval = k1921vg5t_flash_erase(target, i, region);
		if (retval != ERROR_OK)
			return retval;
	}

	command_print(CMD, "done!");

	return retval;
}

COMMAND_HANDLER(k1921vg5t_handle_protect_command)
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

	command_print(CMD, "Try to %s FLASH %s region write protection\n"
						   "Please wait ... ", protect_enable ? "enable" : "disable",
											   (region == NVR_REGION) ? "NVR" : "main");

	uint32_t param_mask = (region == NVR_REGION) ? CFGWORD_CFGWE : CFGWORD_FLASHWE;
	retval = k1921vg5t_flash_modify_cfgword(target, protect_enable, param_mask);
	if (retval != ERROR_OK)
		return retval;

	command_print(CMD, "done! Power on reset cycle is required for the new settings to take effect.");
	return retval;
}


COMMAND_HANDLER(k1921vg5t_handle_jtag_command)
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

	command_print(CMD, "Try to %s JTAG interface\n"
						   "Please wait ... ", jtag_enable ? "enable" : "disable");

	uint32_t param_mask = CFGWORD_JTAGEN;
	retval = k1921vg5t_flash_modify_cfgword(target, jtag_enable, param_mask);
	if (retval != ERROR_OK)
		return retval;

	command_print(CMD, "done! Power on reset cycle is required for the new settings to take effect.");
	return retval;
}

COMMAND_HANDLER(k1921vg5t_handle_srv_erase_command)
{
	int retval;
	struct target *target = get_current_target(CMD_CTX);

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	command_print(CMD, "Try to perform service mode erase - all flash memories will be erased\n"
						   "Please wait ... ");

	retval = target_write_u32(target, SIU_SERVCTL, SIU_SERVCTL_DONE);
	if (retval != ERROR_OK)
		return retval;

	int timeout = 500;
	uint32_t status;

	retval = target_read_u32(target, SIU_SERVCTL, &status);
	if (retval != ERROR_OK)
		return retval;

	if((status & SIU_SERVCTL_SERVEN) != SIU_SERVCTL_SERVEN){
		LOG_ERROR("Service mode erase turned off, SERVEN is HIGH. Operation failed.");
		return ERROR_FLASH_OPERATION_FAILED;
	}

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
	command_print(CMD, "done! Power on reset cycle and SERVEN low are required for the return to normal operation mode.");

	return retval;
}

COMMAND_HANDLER(k1921vg5t_handle_driver_info_command)
{
	int retval = ERROR_OK;

	command_print(CMD, "k1921vg5t flash driver\n"
						   "version: %d.%d\n"
						   "copyright: AO NIIET, 2025\n",
						   FLASH_DRIVER_VER>>16,
						   FLASH_DRIVER_VER&0xFFFF);

	return retval;
}

static const struct command_registration k1921vg5t_exec_command_handlers[] = {
	{
		.name = "read",
		.handler = k1921vg5t_handle_read_command,
		.mode = COMMAND_EXEC,
		.usage = "(main|nvr) address",
		.help = "Read two 32-bit words from FLASH main or NVR region address. Address should be 8 bytes aligned.",
	},
	{
		.name = "write",
		.handler = k1921vg5t_handle_write_command,
		.mode = COMMAND_EXEC,
		.usage = "(main|nvr) (erase|save|none) address data0 data1 data2 data3",
		.help = "Write two 32-bit words to FLASH main or NVR region address. Address should be 8 bytes aligned. There is option that selects between to erase modified sector, to save all data and to do nothing - only write.",
	},
	{
		.name = "mass_erase",
		.handler = k1921vg5t_handle_mass_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "(main|nvr)",
		.help = "Erase entire FLASH main or NVR region",
	},
	{
		.name = "erase",
		.handler = k1921vg5t_handle_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "(main|nvr) first_sector_num last_sector_num",
		.help = "Erase sectors of FLASH main or NVR region, starting at sector first up to and including last",
	},
	{
		.name = "protect",
		.handler = k1921vg5t_handle_protect_command,
		.mode = COMMAND_EXEC,
		.usage = "(main|nvr) (enable|disable)",
		.help = "FLASH main or NVR region write protect control. Power on reset cycle is required for the new settings to take effect.",
	},
	{
		.name = "jtag",
		.handler = k1921vg5t_handle_jtag_command,
		.mode = COMMAND_EXEC,
		.usage = "(enable|disable)",
		.help = "Control JTAG interface. Power on reset cycle is required for the new settings to take effect.",
	},
	{
		.name = "srv_erase",
		.handler = k1921vg5t_handle_srv_erase_command,
		.mode = COMMAND_EXEC,
		.usage = "",
		.help = "Perform mass erase of all chip flash memories. Power on reset cycle and SERVEN pin tied low are required for the return to normal operation mode.",
	},
	{
		.name = "driver_info",
		.handler = k1921vg5t_handle_driver_info_command,
		.mode = COMMAND_EXEC,
		.usage = "",
		.help = "Show information about flash driver",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration k1921vg5t_command_handlers[] = {
	{
		.name = "k1921vg5t",
		.mode = COMMAND_ANY,
		.help = "k1921vg5t flash command group",
		.usage = "",
		.chain = k1921vg5t_exec_command_handlers,
	},
	COMMAND_REGISTRATION_DONE
};

/*==============================================================================
 *                          FLASH INTERFACE
 *==============================================================================
 */

FLASH_BANK_COMMAND_HANDLER(k1921vg5t_flash_bank_command)
{
	struct k1921vg5t_flash_bank *k1921vg5t_info;

	if (CMD_ARGC < 6)
		return ERROR_COMMAND_SYNTAX_ERROR;

	k1921vg5t_info = malloc(sizeof(struct k1921vg5t_flash_bank));

	bank->driver_priv = k1921vg5t_info;

	/* information will be updated by probing */
	k1921vg5t_info->probed = false;
	k1921vg5t_info->chip_name = "k1921vg5t";
	k1921vg5t_info->flashwe = true;
	k1921vg5t_info->cfgwe = true;

	return ERROR_OK;
}

static int k1921vg5t_protect_check(struct flash_bank *bank)
{
	struct k1921vg5t_flash_bank *k1921vg5t_info = bank->driver_priv;

	uint32_t protect_enable;

	protect_enable = !k1921vg5t_info->flashwe ? 1 : 0;

	for (unsigned int i = 0; i < bank->num_sectors; i++)
		bank->sectors[i].is_protected = protect_enable;

	return ERROR_OK;
}

static int k1921vg5t_mass_erase(struct flash_bank *bank)
{
	struct target *target = bank->target;
	int retval;

	retval = k1921vg5t_flash_mass_erase(target, MAIN_REGION);
	if (retval != ERROR_OK)
		return retval;

	for (unsigned int i = 0; i < bank->num_sectors; i++) {
		bank->sectors[i].is_erased = 1;
	}

	return retval;
}

static int k1921vg5t_erase(struct flash_bank *bank, unsigned int first, unsigned int last)
{
	struct target *target = bank->target;
	int retval = ERROR_FLASH_OPERATION_FAILED;
	LOG_INFO("Erasing sectors from %u to %u", first, last);
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if ((first == 0) && (last == (bank->num_sectors - 1))) {
		retval = k1921vg5t_mass_erase(bank);
		if (retval != ERROR_OK)
			return retval;
	} else {
		/* erasing pages */
		for (unsigned int i = first; i <= last; i++) {
			retval = k1921vg5t_flash_erase(target, i, MAIN_REGION);
			if (retval != ERROR_OK)
				return retval;
			bank->sectors[i].is_erased = 1;
		}
	}

	return retval;
}

static int k1921vg5t_protect(struct flash_bank *bank, int set, unsigned int first, unsigned int last)
{
	struct target *target = bank->target;
	struct k1921vg5t_flash_bank *k1921vg5t_info = bank->driver_priv;

	int retval;

	if (target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	LOG_INFO("Plese wait ...");

	uint32_t param_mask = CFGWORD_FLASHWE;
	retval = k1921vg5t_flash_modify_cfgword(target, set, param_mask);
	if (retval != ERROR_OK)
		return retval;
	k1921vg5t_info->flashwe = false;
	return retval;
}

static int k1921vg5t_write_block(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	const uint32_t block_size = FLASH_WORD_WIDTH * 4;
	const uint32_t stack_size = 128;
	const uint32_t stack_align = 16;
	target_addr_t stack_aligned_addr = 0;
	struct target *target = bank->target;
	uint32_t buffer_size;
	struct working_area *write_algorithm;
	struct working_area *stack_area;
	struct working_area *source;
	uint32_t target_address;
	struct reg_param reg_params[5];
	int retval = ERROR_OK;

	static const uint8_t k1921vg5t_flash_write_code[] = {
#include "../../../contrib/loaders/flash/niiet/k1921vg5t/k1921vg5t.inc"
	};

	LOG_INFO("Start block write");
	/* flash write code */
	if (target_alloc_working_area(target, sizeof(k1921vg5t_flash_write_code),
			&write_algorithm) != ERROR_OK) {
		LOG_WARNING("no working area available, can't do block memory writes");
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	retval = target_write_buffer(target, write_algorithm->address,
			sizeof(k1921vg5t_flash_write_code),k1921vg5t_flash_write_code);
	if (retval != ERROR_OK){
		target_free_working_area(target, write_algorithm);
		return retval;
	}

	/* alloc stack memory */
	if (target_alloc_working_area(target, stack_size + stack_align,
			&stack_area) != ERROR_OK) {
		LOG_WARNING("no working area available, can't do block memory writes");
		target_free_working_area(target, write_algorithm);
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}
	stack_aligned_addr = (stack_area->address + stack_align) & ~(stack_align - 1); // align to 16

	buffer_size = target_get_working_area_avail(target);
	buffer_size = MIN(count * block_size, MAX(buffer_size, FLASH_PAGE_SIZE)); 
	buffer_size &= ~(FLASH_WORD_WIDTH*4-1); /* Make sure it's aligned */
	LOG_INFO("buffer_size %d, count %d", buffer_size, count);


	retval = target_alloc_working_area(target, buffer_size, &source);
	/* Allocated size is always 32-bit word aligned */
	if (retval != ERROR_OK) {
		target_free_working_area(target, write_algorithm);
		target_free_working_area(target, stack_area);
		LOG_WARNING("no large enough working area available, can't do block memory writes");
		/* target_alloc_working_area() may return ERROR_FAIL if area backup fails:
		 * convert any error to ERROR_TARGET_RESOURCE_NOT_AVAILABLE
		 */
		return ERROR_TARGET_RESOURCE_NOT_AVAILABLE;
	}

	init_reg_param(&reg_params[0], "a0", 32, PARAM_IN_OUT); /* write_cmd base (in), status (out) */
	init_reg_param(&reg_params[1], "a1", 32, PARAM_OUT);    /* count (4*4 bytes) */
	init_reg_param(&reg_params[2], "a2", 32, PARAM_OUT);    /* buffer start */
	init_reg_param(&reg_params[3], "a3", 32, PARAM_IN_OUT); /* target address */
	init_reg_param(&reg_params[4], "sp", 32, PARAM_OUT);
	uint32_t flash_cmd;
	flash_cmd = FLASH_CMD_KEY | FLASH_CMD_WR;
	uint32_t thisrun_count= 0;
	for (unsigned int i = 0; i < count; i += thisrun_count) {
		thisrun_count = MIN(buffer_size/block_size, count - i);
		target_address = offset + i*block_size;
		LOG_INFO("thisrun_count %d, target_address 0x%x", thisrun_count,target_address);
		buf_set_u32(reg_params[0].value, 0, 32, flash_cmd);
		buf_set_u32(reg_params[1].value, 0, 32, thisrun_count);
		buf_set_u32(reg_params[2].value, 0, 32, source->address);
		buf_set_u32(reg_params[3].value, 0, 32, target_address);
		buf_set_u32(reg_params[4].value, 0, 32, stack_aligned_addr + stack_size);/* write stack pointer */
		retval = target_write_buffer(target, source->address, thisrun_count * block_size, buffer + i * block_size);
		if (retval != ERROR_OK){
			break;
		}

		retval = target_run_algorithm(target,
				0, NULL,
				5, reg_params,
				write_algorithm->address,
				write_algorithm->address + 0x24,//exit point "asm("ebreak");"
				10000, NULL);

		if (retval != ERROR_OK) {
			LOG_ERROR("Failed to execute algorithm at target address 0x%x",
					target_address); 
			break;
		}

	}
	

	target_free_working_area(target, source);
	target_free_working_area(target, stack_area);
	target_free_working_area(target, write_algorithm);

	destroy_reg_param(&reg_params[0]);
	destroy_reg_param(&reg_params[1]);
	destroy_reg_param(&reg_params[2]);
	destroy_reg_param(&reg_params[3]);
	destroy_reg_param(&reg_params[4]);

	return retval;
}


static int k1921vg5t_write(struct flash_bank *bank, const uint8_t *buffer,
		uint32_t offset, uint32_t count)
{
	struct target *target = bank->target;
	uint8_t *new_buffer = NULL;
	if (bank->target->state != TARGET_HALTED) {
		LOG_ERROR("Target not halted");
		return ERROR_TARGET_NOT_HALTED;
	}

	if (offset & 0xF) {
		LOG_ERROR("offset 0x%" PRIx32 " breaks required 4-word alignment", offset);
		return ERROR_FLASH_DST_BREAKS_ALIGNMENT;
	}

	/* If there's an odd number of words, the data has to be padded. Duplicate
	 * the buffer and use the normal code path with a single block write since
	 * it's probably cheaper than to special case the last odd write using
	 * discrete accesses. */

	int rem = count % (FLASH_WORD_WIDTH*4);
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

	/* try using block write */
	retval = k1921vg5t_write_block(bank, buffer, offset, count/(FLASH_WORD_WIDTH*4));
	uint32_t flash_addr, flash_cmd, flash_data;
	if (retval != ERROR_OK) {
				/* if block write failed (no sufficient working area),
				 * we use normal (slow) register accesses */
		LOG_INFO("Block write failed, use slow mode"); /* it`s quite a long process */

		flash_cmd = FLASH_CMD_KEY | FLASH_CMD_WR;

		/* write multiple bytes per try */
		for (unsigned int i = 0; i < count; i += FLASH_WORD_WIDTH*4) {
			/* current addr */
			
			flash_addr = offset + i;
			LOG_INFO("%d byte of %d addr 0x%x", i, count,flash_addr);
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
			retval = k1921vg5t_flash_waitdone(target);
			if (retval != ERROR_OK)
				goto free_buffer;
		}
	}

free_buffer:
	if (new_buffer)
		free(new_buffer);

	return retval;
}

static int k1921vg5t_probe(struct flash_bank *bank)
{
	struct k1921vg5t_flash_bank *k1921vg5t_info = bank->driver_priv;
	struct target *target = bank->target;

	if (bank->sectors) {
		free(bank->sectors);
		bank->sectors = NULL;
	}
	uint32_t retval;
	uint32_t chipid;

	retval = target_read_u32(target, SIU_CHIPID, &chipid);
	chipid = chipid & 0xFFFFFFF0; // reset number of revision bits
	if ((retval != ERROR_OK) || (chipid != SIU_CHIPID_K1921VG5T)) {
		LOG_INFO("CHIPID error %d, CHIPID=%x", retval,chipid);
		return ERROR_FAIL;
	}

	LOG_INFO("k1921vg5t detected");
	bank->base = FLASH_BANK_ADDR;
	bank->size = FLASH_PAGE_SIZE*FLASH_PAGE_TOTAL;
	bank->num_sectors = FLASH_PAGE_TOTAL;

	/* check if we in service mode */
	uint32_t service_mode;
	retval = target_read_u32(target, SIU_SERVCTL, &service_mode);
	if (retval != ERROR_OK)
		return retval;
	service_mode = service_mode & SIU_SERVCTL_SERVEN ? 1 : 0;



	if (!service_mode) {
		uint32_t cfgword = 0xFFFFFFFF;
		/* read CFGWORD */
		retval = k1921vg5t_flash_read_cfgword(target, &cfgword);
		if (retval != ERROR_OK)
			return retval;


		k1921vg5t_info->flashwe = (cfgword & CFGWORD_FLASHWE) == CFGWORD_FLASHWE;
		k1921vg5t_info->cfgwe = (cfgword & CFGWORD_CFGWE) == CFGWORD_CFGWE;



		snprintf(k1921vg5t_info->chip_brief,
				sizeof(k1921vg5t_info->chip_brief),
				"\n"
				"[MEMORY CONFIGURATION]\n"
				"Memory mapped to 0x%"PRIu64" (will be used for writing and debugging):\n"
				"\n"
				"[CFGWORD]\n"
				"FLASH main region write protection :\n"
				"    %s\n"
				"FLASH NVR region write protection :\n"
				"    %s\n",
				bank->base,
				k1921vg5t_info->flashwe ? "disable" : "enable",
				k1921vg5t_info->cfgwe ? "disable" : "enable");
	} else {
		sprintf(k1921vg5t_info->chip_brief,
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

	k1921vg5t_info->probed = true;

	return ERROR_OK;
}

static int k1921vg5t_auto_probe(struct flash_bank *bank)
{
	struct k1921vg5t_flash_bank *k1921vg5t_info = bank->driver_priv;
	if (k1921vg5t_info->probed)
		return ERROR_OK;
	return k1921vg5t_probe(bank);
}

static int get_k1921vg5t_info(struct flash_bank *bank, struct command_invocation *cmd)
{
	struct k1921vg5t_flash_bank *k1921vg5t_info = bank->driver_priv;
	command_print_sameline(cmd, "\nNIIET %s\n%s", k1921vg5t_info->chip_name, k1921vg5t_info->chip_brief);

	return ERROR_OK;
}


const struct flash_driver k1921vg5t_flash = {
	.name = "k1921vg5t",
	.usage = "flash bank <name> k1921vg5t <base> <size> 0 0 <target#>",
	.commands = k1921vg5t_command_handlers,
	.flash_bank_command = k1921vg5t_flash_bank_command,
	.erase = k1921vg5t_erase,
	.protect = k1921vg5t_protect,
	.write = k1921vg5t_write,
	.read = default_flash_read,
	.probe = k1921vg5t_probe,
	.auto_probe = k1921vg5t_auto_probe,
	.erase_check = default_flash_blank_check,
	.protect_check = k1921vg5t_protect_check,
	.info = get_k1921vg5t_info,
	.free_driver_priv = default_flash_free_driver_priv,
};
