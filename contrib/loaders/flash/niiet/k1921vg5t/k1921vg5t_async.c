

#include <stdint.h>

#define MAIN_REGION             0
#define NVR_REGION              1
/*-- FLASH ------------------------------------------------------------------*/
#define FLASH_PAGE_SIZE        1024
#define FLASH_PAGE_TOTAL       512
#define FLASH_WORD_WIDTH       2
#define FLASH_BASE             ((void*)0x50002000)
#define FLASH_BANK_ADDR        0x00000000

#define FLASH_ADDR             (*(volatile uint32_t*)(0x50002000u))
#define FLASH_DATA0            (*(volatile uint32_t*)(0x50002010u))
#define FLASH_DATA1            (*(volatile uint32_t*)(0x50002014u))
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


#define FIFO_RP (buffer_start[1])
#define FIFO_RP_PTR ((uint32_t*)FIFO_RP)
#define FIFO_WP_PTR ((uint32_t*)(buffer_start[0]))

void flash_write(
    volatile uint32_t write_cmd, volatile uint32_t count,
    volatile uint32_t *buffer_start, volatile uint32_t *buffer_end,
    volatile uint32_t *target_addr); //__attribute__((naked,noreturn,noinline));

//function needs a stack
void flash_write(volatile uint32_t write_cmd,
		volatile uint32_t count,
		volatile uint32_t *buffer_start,
		volatile uint32_t *buffer_end,
		volatile uint32_t *target_addr)
{
	do {
		if(FIFO_WP_PTR == 0){/* abort if wp == 0 */
				  break;
		}
 
		while (FIFO_RP_PTR == FIFO_WP_PTR);

		FLASH_ADDR = (uint32_t)target_addr;
		FLASH_DATA0 = *FIFO_RP_PTR;
		FLASH_DATA1 = *(FIFO_RP_PTR + 1);
		FLASH_CMD = write_cmd; // start write

		while (FLASH_STAT & FLASH_STAT_BUSY);//wait for write done

		target_addr+=FLASH_WORD_WIDTH;
		FIFO_RP += FLASH_WORD_WIDTH*4;
		if (FIFO_RP_PTR >= buffer_end){
			FIFO_RP = (uint32_t)(buffer_start+2);
		}
	} while (--count);
	asm("ebreak");
} 
