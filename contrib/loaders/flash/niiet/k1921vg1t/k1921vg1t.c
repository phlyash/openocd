

#include <stdint.h>

#define MAIN_REGION             0
#define NVR_REGION              1
/*-- FLASH ------------------------------------------------------------------*/
#define FLASH_PAGE_SIZE        2048
#define FLASH_PAGE_TOTAL       512
#define FLASH_WORD_WIDTH       4
#define FLASH_BASE             ((void*)0x50002000)
#define FLASH_BANK_ADDR        0x00000000


#define FLASH_ADDR             (*(volatile uint32_t*)(0x50002000u))
#define FLASH_DATA0            (*(volatile uint32_t*)(0x50002010u))
#define FLASH_DATA1            (*(volatile uint32_t*)(0x50002014u))
#define FLASH_DATA2            (*(volatile uint32_t*)(0x50002018u))
#define FLASH_DATA3            (*(volatile uint32_t*)(0x5000201Cu))
#define FLASH_CMD              (*(volatile uint32_t*)(0x50002040u))
#define FLASH_STAT             (*(volatile uint32_t*)(0x50002044u))

/*---- FLASH->CMD: Command register */
#define FLASH_CMD_RD           (1<<0)              /* Read data in region */
#define FLASH_CMD_WR           (1<<1)              /* Write data in region */
#define FLASH_CMD_ERSEC        (1<<2)              /* Sector erase in region */
#define FLASH_CMD_ERALL        (1<<3)              /* Erase all sectors in region */
#define FLASH_CMD_NVRON        (1<<8)              /* Select NVR region for command operation */
#define FLASH_CMD_KEY          (0xC0DE<<16)        /* Command enable key */
/*---- FLASH->STAT: Status register */
#define FLASH_STAT_BUSY        (1<<0)              /* Flag operation busy */


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
	while ((FLASH_STAT & FLASH_STAT_BUSY) == FLASH_STAT_BUSY);// wait for write done
  for (int i = 0; i < count*FLASH_WORD_WIDTH; i+=FLASH_WORD_WIDTH) {
    FLASH_ADDR = (uint32_t)target_addr;
    FLASH_DATA0 = buffer_start[i];
    FLASH_DATA1 = buffer_start[i+1];
    FLASH_DATA2 = buffer_start[i+2];
    FLASH_DATA3 = buffer_start[i+3];
    FLASH_CMD =  FLASH_CMD_KEY | FLASH_CMD_WR; // start write
	asm("nop");
	asm("nop");
	asm("nop");
	asm("nop");
	asm("nop");
	while ((FLASH_STAT & FLASH_STAT_BUSY) == FLASH_STAT_BUSY);// wait for write done


    target_addr += FLASH_WORD_WIDTH;
  }
  __asm__ volatile("ebreak");
} 