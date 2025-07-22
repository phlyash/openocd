

#include <stdint.h>

#define MAIN_REGION             0
#define NVR_REGION              1
/*-- MFLASH ------------------------------------------------------------------*/
#define MFLASH_PAGE_SIZE        4096
#define MFLASH_PAGE_TOTAL       256
#define MFLASH_WORD_WIDTH       4
#define MFLASH_BASE             ((void*)0x3000D000)
#define MFLASH_BANK_ADDR        0x80000000


#define MFLASH_ADDR             (*(volatile uint32_t*)(0x3000D000u))
#define MFLASH_DATA0            (*(volatile uint32_t*)(0x3000D004u))
#define MFLASH_DATA1            (*(volatile uint32_t*)(0x3000D008u))
#define MFLASH_DATA2            (*(volatile uint32_t*)(0x3000D00Cu))
#define MFLASH_DATA3            (*(volatile uint32_t*)(0x3000D010u))
#define MFLASH_CMD              (*(volatile uint32_t*)(0x3000D044u))
#define MFLASH_STAT             (*(volatile uint32_t*)(0x3000D048u))

/*---- MFLASH->CMD: Command register */
#define MFLASH_CMD_RD           (1<<0)              /* Read data in region */
#define MFLASH_CMD_WR           (1<<1)              /* Write data in region */
#define MFLASH_CMD_ERSEC        (1<<2)              /* Sector erase in region */
#define MFLASH_CMD_ERALL        (1<<3)              /* Erase all sectors in region */
#define MFLASH_CMD_NVRON        (1<<8)              /* Select NVR region for command operation */
#define MFLASH_CMD_KEY          (0xC0DE<<16)        /* Command enable key */
/*---- MFLASH->STAT: Status register */
#define MFLASH_STAT_BUSY        (1<<0)              /* Flag operation busy */


void flash_write(
    volatile uint32_t write_cmd, volatile uint32_t count,
    volatile uint32_t *buffer_start,
    volatile uint32_t *target_addr); //__attribute__((naked,noreturn,noinline));

//function needs a stack
void flash_write(volatile uint32_t write_cmd,
		volatile uint32_t count,
		volatile uint32_t *buffer_start,
		volatile uint32_t *target_addr)
{
	while ((MFLASH_STAT & MFLASH_STAT_BUSY) == MFLASH_STAT_BUSY);// wait for write done
  for (int i = 0; i < count*MFLASH_WORD_WIDTH; i+=MFLASH_WORD_WIDTH) {
    MFLASH_ADDR = (uint32_t)target_addr;
    MFLASH_DATA0 = buffer_start[i];
    MFLASH_DATA1 = buffer_start[i+1];
    MFLASH_DATA2 = buffer_start[i+2];
    MFLASH_DATA3 = buffer_start[i+3];
    MFLASH_CMD =  MFLASH_CMD_KEY | MFLASH_CMD_WR; // start write
	asm("nop");
	asm("nop");
	asm("nop");
	asm("nop");
	asm("nop");
	while ((MFLASH_STAT & MFLASH_STAT_BUSY) == MFLASH_STAT_BUSY);// wait for write done


    target_addr += MFLASH_WORD_WIDTH;
  }
  __asm__ volatile("ebreak");
} 