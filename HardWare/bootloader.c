#include "bootloader.h"

/*
 * bootloader_rx_buffer:
 *   UART空闲中断接收缓冲区，单次回调内收到的数据会先放到该缓冲区。
 */
uint8_t bootloader_rx_buffer[BOOTLOADER_RX_BUFFER_LEN];

/* 本次UART回调实际接收长度（单位：字节） */
uint16_t bootloader_rx_len = 0;

/* 累计接收总长度（单位：字节），用于与上位机声明长度做一致性校验 */
uint32_t bootloader_rx_len_sum = 0;

/* Flash写入偏移地址（相对APP_START_ADDR，单位：字节） */
uint32_t flash_write_offset = 0;

/* 最近一次收到有效数据的系统tick，用于超时判断“接收结束” */
uint32_t last_receive_time = 0;

/*
 * last_byte_flag:
 *   标记上一包是否遗留了1个无法凑成半字写入的字节。
 * last_byte:
 *   保存该遗留字节，等待下一包首字节拼成16bit后再写入。
 */
uint8_t last_byte_flag = 0;
uint8_t last_byte = 0;

/* 内部写入函数声明 */
static void Flash_Write_With_Last(void);
static void Flash_Write_No_Last(void);

/*
 * 根据地址计算STM32F407扇区号。
 * 说明：APP区擦除时会用到起止地址对应扇区。
 */
uint32_t Bootloader_GetSector(uint32_t address)
{
    if (address < 0x08004000U)
    {
        return FLASH_SECTOR_0;
    }
    else if (address < 0x08008000U)
    {
        return FLASH_SECTOR_1;
    }
    else if (address < 0x0800C000U)
    {
        return FLASH_SECTOR_2;
    }
    else if (address < 0x08010000U)
    {
        return FLASH_SECTOR_3;
    }
    else if (address < 0x08020000U)
    {
        return FLASH_SECTOR_4;
    }
    else if (address < 0x08040000U)
    {
        return FLASH_SECTOR_5;
    }
    else if (address < 0x08060000U)
    {
        return FLASH_SECTOR_6;
    }
    else if (address < 0x08080000U)
    {
        return FLASH_SECTOR_7;
    }
    else
    {
        return FLASH_SECTOR_7; /* 超出范围时默认返回最后一个扇区 */
    }
}

uint32_t Get_bootloader_rx_len_sum(void)
{
    /* 对外提供累计接收长度 */
    return bootloader_rx_len_sum;
}

uint32_t Get_last_receive_time(void)
{
    /* 对外提供最近接收时间，用于上层状态机超时判定 */
    return last_receive_time;
}

/*
 * 场景：上一包存在遗留字节（last_byte_flag = 1）。
 * 处理：
 *   - 首次写入时用 last_byte 与当前缓冲区第0字节拼成一个半字；
 *   - 后续按照 [i-1, i] 两字节继续写入。
 */
static void Flash_Write_With_Last(void)
{
    for (uint16_t i = 0; i < bootloader_rx_len; i += 2)
    {
        if (i == 0)
        {
            /* 低8位来自上一包遗留字节，高8位来自当前包第1个字节 */
            uint16_t data_16 = (last_byte | (bootloader_rx_buffer[i] << 8));
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, APP_START_ADDR + flash_write_offset + i, data_16) != HAL_OK)
            {
                /* Flash写入失败，交给统一错误处理 */
                Error_Handler();
            }
            continue;
        }
        else
        {
            /* 常规按小端拼半字：低字节在前，高字节在后 */
            uint16_t data_16 = (bootloader_rx_buffer[i - 1] | (bootloader_rx_buffer[i] << 8));
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, APP_START_ADDR + flash_write_offset + i, data_16) != HAL_OK)
            {
                /* Flash写入失败，交给统一错误处理 */
                Error_Handler();
            }
        }
    }
}

/*
 * 场景：上一包无遗留字节（last_byte_flag = 0）。
 * 处理：当前包直接按 [i, i+1] 两字节一组写入Flash。
 */
static void Flash_Write_No_Last(void)
{
    for (uint16_t i = 0; i + 1 < bootloader_rx_len; i += 2)
    {
        /* 按小端拼接16位数据并写入 */
        uint16_t data_16 = (bootloader_rx_buffer[i] | (bootloader_rx_buffer[i + 1] << 8));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, APP_START_ADDR + flash_write_offset + i, data_16) != HAL_OK)
        {
            /* Flash写入失败，交给统一错误处理 */
            Error_Handler();
        }
    }
}

/*
 * Bootloader接收准备：
 * 1) 清理串口溢出/空闲标志，避免历史状态影响新一轮升级；
 * 2) 启动“接收到空闲即回调”的中断接收模式。
 */
void Bootloader_Receive_Prepare(void)
{
    /* 清除UART的溢出和空闲标志 */
    __HAL_UART_CLEAR_OREFLAG(&huart1);
    __HAL_UART_CLEAR_IDLEFLAG(&huart1);

    /* 开启UART空闲接收中断 */
    HAL_UARTEx_ReceiveToIdle_IT(&huart1, bootloader_rx_buffer, BOOTLOADER_RX_BUFFER_LEN);
}

/*
 * 跳转到应用程序入口。
 * 返回值：
 *   0 - 跳转流程执行（理论上不会返回）
 *   1 - 参数非法或前置校验失败
 */
uint8_t Bootloader_Jump_To_App(void)
{
    typedef void (*pFunction)(void);

    /*
     * 若还遗留1个字节，跳转前补写：高字节填0xFF（擦除态），
     * 确保最后一字节不会丢失。
     */
    if (last_byte_flag == 1)
    {
        HAL_FLASH_Unlock();
        uint16_t data_16 = (uint16_t)(last_byte | (0xFFU << 8));
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, APP_START_ADDR + flash_write_offset, data_16) != HAL_OK)
        {
            Error_Handler();
        }
        HAL_FLASH_Lock();
        flash_write_offset += 1;
        last_byte_flag = 0;
    }

    /* APP向量表前两个字：初始MSP 与 Reset_Handler地址 */
    uint32_t app_stack = *(volatile uint32_t *)APP_START_ADDR;
    uint32_t app_reset_handler = *(volatile uint32_t *)(APP_START_ADDR + 4);

    /* 校验APP初始栈地址是否落在SRAM合法范围 */
    if ((app_stack < STACK_ADDR) || (app_stack > SRAM_END_ADDR))
    {
        printf("stack error\r\n");
        return 1;
    }

    /* 校验Reset_Handler地址是否在片内Flash合法范围 */
    if ((app_reset_handler < APP_START_ADDR) || (app_reset_handler > APP_END_ADDR))
    {
        printf("reset handler address error\r\n");
        return 1;
    }
    printf("Jumping to app\r\n");

    /* 全局关中断，准备切换执行上下文 跳转到APP后要由APP自行决定是否开启中断 */
    __disable_irq();

    NVIC_DisableIRQ(USART1_IRQn); /* 禁用USART1 */
    NVIC_DisableIRQ(EXTI0_IRQn);  /* 禁用按键中断 */

    /* 反初始化HAL外设状态 */
    HAL_DeInit();

    /* 切换到APP声明的主栈指针 */
    __set_MSP(app_stack);

    /* 中断向量表重定位到APP起始地址 */
    SCB->VTOR = APP_START_ADDR;

    __DSB(); /* 数据同步屏障，确保指令执行顺序 */
    __ISB(); /* 指令同步屏障，确保指令流更新 */

    /* 跳转到APP复位入口 */
    pFunction jump_to_app = (pFunction)app_reset_handler;
    jump_to_app();

    return 0;
}

/*
 * UART空闲接收回调：
 * 1) 记录接收长度/时间；
 * 2) 处理半字对齐写入（含遗留字节拼接）；
 * 3) 更新Flash写偏移；
 * 4) 重新挂载下一轮接收。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if (huart->Instance == USART1)
    {
        if (Size == 0)
        {
            HAL_UARTEx_ReceiveToIdle_IT(&huart1, bootloader_rx_buffer, BOOTLOADER_RX_BUFFER_LEN);
            return;
        }

        /* 更新最近接收时间（用于超时判定“接收结束”） */
        last_receive_time = HAL_GetTick();

        /* 记录本次长度与累计长度 */
        bootloader_rx_len = Size;
        bootloader_rx_len_sum += bootloader_rx_len;

        /* 写Flash前先解锁 */
        HAL_FLASH_Unlock();

        /*
         * (当前长度 + 遗留标志) 为偶数：本轮写完后无遗留字节。
         * (当前长度 + 遗留标志) 为奇数：本轮写完后仍会遗留1字节。
         */
        if ((bootloader_rx_len + last_byte_flag) % 2 == 0)
        {
            if (last_byte_flag == 1)
            {
                /* 上轮有遗留，本轮补齐后可完整写完，无新遗留 */
                Flash_Write_With_Last();

                /*
                 * 偏移增加：
                 * 本轮bootloader_rx_len字节 + 上轮遗留1字节（已参与本轮首半字）
                 */
                flash_write_offset += (bootloader_rx_len + 1);
            }
            else
            {
                /* 上轮无遗留，本轮偶数字节，直接整包写完 */
                Flash_Write_No_Last();
                flash_write_offset += bootloader_rx_len;
            }
            last_byte_flag = 0;
        }
        else
        {
            if (last_byte_flag == 1)
            {
                /* 上轮有遗留，本轮写完后末尾仍会再遗留1字节 */
                Flash_Write_With_Last();
                last_byte = bootloader_rx_buffer[bootloader_rx_len - 1];
                flash_write_offset += (bootloader_rx_len);
            }
            else
            {
                /* 上轮无遗留，本轮奇数字节，最后1字节先暂存 */
                Flash_Write_No_Last();
                last_byte = bootloader_rx_buffer[bootloader_rx_len - 1];
                flash_write_offset += (bootloader_rx_len - 1);
            }
            last_byte_flag = 1;
        }

        /* 写入结束后重新上锁Flash */
        HAL_FLASH_Lock();

        /* 清空缓冲区，减少残留数据对调试观察的干扰 */
        memset(bootloader_rx_buffer, 0, BOOTLOADER_RX_BUFFER_LEN);

        /* 继续挂起下一次空闲接收 */
        HAL_UARTEx_ReceiveToIdle_IT(&huart1, bootloader_rx_buffer, BOOTLOADER_RX_BUFFER_LEN);
    }
}
