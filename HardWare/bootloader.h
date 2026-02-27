#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include "config.h"

#define BOOTLOADER_RX_BUFFER_LEN 2048
#define APP_START_ADDR 0x08008000U
#define APP_END_ADDR 0x0807FFFFU
#define STACK_ADDR 0x20000000U
#define SRAM_END_ADDR 0x20020000U

 uint32_t Bootloader_GetSector(uint32_t address);

 uint32_t Get_bootloader_rx_len_sum(void);

 uint32_t Get_last_receive_time(void);

 void Bootloader_Receive_Prepare(void);

 uint8_t Bootloader_Jump_To_App(void);

#endif
