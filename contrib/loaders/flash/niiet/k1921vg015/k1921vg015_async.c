

#include <stdint.h>

#define MAIN_REGION             0
#define NVR_REGION              1
/*-- MFLASH ------------------------------------------------------------------*/
#define MFLASH_PAGE_SIZE        4096
#define MFLASH_PAGE_TOTAL       256
#define MFLASH_WORD_WIDTH       4
#define MFLASH_BASE             ((void*)0x3000D000)
#define MFLASH_BANK_ADDR        0x80000000

#define MFLASH_ADDR             *(uint32_t*)(MFLASH_BASE + 0x00)
#define MFLASH_DATA0            *(uint32_t*)(MFLASH_BASE + 0x04)
#define MFLASH_DATA1            *(uint32_t*)(MFLASH_BASE + 0x08)
#define MFLASH_DATA2            *(uint32_t*)(MFLASH_BASE + 0x0C)
#define MFLASH_DATA3            *(uint32_t*)(MFLASH_BASE + 0x10)
#define MFLASH_CMD              *(uint32_t*)(MFLASH_BASE + 0x44)
#define MFLASH_STAT             *(uint32_t*)(MFLASH_BASE + 0x48)

/*---- MFLASH->CMD: Command register */
#define MFLASH_CMD_RD           (1<<0)              /* Read data in region */
#define MFLASH_CMD_WR           (1<<1)              /* Write data in region */
#define MFLASH_CMD_ERSEC        (1<<2)              /* Sector erase in region */
#define MFLASH_CMD_ERALL        (1<<3)              /* Erase all sectors in region */
#define MFLASH_CMD_NVRON        (1<<8)              /* Select NVR region for command operation */
#define MFLASH_CMD_KEY          (0xC0DE<<16)        /* Command enable key */
/*---- MFLASH->STAT: Status register */
#define MFLASH_STAT_BUSY        (1<<0)              /* Flag operation busy */


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

		MFLASH_ADDR = (uint32_t)target_addr;
		MFLASH_DATA0 = *FIFO_RP_PTR;
		MFLASH_DATA1 = *(FIFO_RP_PTR + 1);
		MFLASH_DATA2 = *(FIFO_RP_PTR + 2);
		MFLASH_DATA3 = *(FIFO_RP_PTR + 3);
		MFLASH_CMD = write_cmd; // start write

		while (MFLASH_STAT & MFLASH_STAT_BUSY);//wait for write done

		target_addr+=MFLASH_WORD_WIDTH;
		FIFO_RP += MFLASH_WORD_WIDTH*4;
		if (FIFO_RP_PTR >= buffer_end){
			FIFO_RP = (uint32_t)(buffer_start+2);
		}
	} while (--count);
	asm("ebreak");
} 