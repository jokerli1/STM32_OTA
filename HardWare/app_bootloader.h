#ifndef APP_BOOTLOADER_H
#define APP_BOOTLOADER_H

#include "config.h"

typedef enum
{
    APP_BOOTLOADER_INIT = 0,
    APP_BOOTLOADER_START,
    APP_BOOTLOADER_REC_PREPARE,
    APP_BOOTLOADER_REC_DATA,
    APP_BOOTLOADER_CHECK_DATA,
    APP_BOOTLOADER_JUMP_APP,
} APP_BOOTLOADER_STATUS;

void App_Bootloader_Init(void);

void App_Bootloader_Work(void);

#endif
