#include "app_bootloader.h"

#define BOOTLOADER_AUTO_FINISH 0

/*
 * 启动命令接收缓冲区。
 * 期望格式："start:len"，例如 "start:65536"。
 */
uint8_t app_rec_start_buf[64] = {0};

/* 启动命令实际接收长度 */
uint16_t app_rec_start_len = 0;

/* 上位机声明的APP总长度（单位：字节） */
uint32_t app_rec_total_len = 0;

/* Bootloader应用层状态机初始状态 */
APP_BOOTLOADER_STATUS app_bootloader_status = APP_BOOTLOADER_INIT;

/* Bootloader发送完成标签 */
uint8_t finish_flag = 0;

/* 内部状态处理函数声明 */
static void App_Bootloader_Run(void);
static void App_Bootloader_Receive_Prepare(void);
static void App_Bootloader_Recv_Data(void);
static uint8_t App_Bootloader_Check_Data(void);
static uint8_t App_Bootloader_Jump_App(void);

/*
 * Bootloader应用层初始化入口。
 * 作用：打印引导提示并切换到等待启动命令状态。
 */
void App_Bootloader_Init(void)
{
    printf("[BL] Bootloader start\r\n");
    printf("[BL] Waiting host command: start:len\r\n");
    printf("[BL] Example command: start:65536\r\n");

    /* 进入命令等待阶段 */
    app_bootloader_status = APP_BOOTLOADER_START;
}

/*
 * 状态：APP_BOOTLOADER_START
 * 阻塞等待上位机发送启动命令，解析总长度并推进状态。
 */
static void App_Bootloader_Run(void)
{
    /* 每次进入先清空缓存，避免旧数据干扰解析 */
    memset(app_rec_start_buf, 0, sizeof(app_rec_start_buf));
    app_rec_start_len = 0;

    /* 阻塞接收直到串口空闲（HAL_MAX_DELAY表示一直等待） */
    HAL_UARTEx_ReceiveToIdle(&huart1, app_rec_start_buf, sizeof(app_rec_start_buf), &app_rec_start_len, HAL_MAX_DELAY);
    if (app_rec_start_len > 0)
    {
        /* 查找关键字 "start:" */
        char *start_str = strstr((char *)app_rec_start_buf, "start:");
        if (start_str != NULL)
        {
            /* 提取长度字段，start: 后面应为十进制数字 */
            app_rec_total_len = atoi(start_str + 6);
            if (app_rec_total_len > 0)
            {
                printf("[BL] APP length accepted: %d, preparing receive\r\n", app_rec_total_len);

                /* 长度合法，进入擦除并准备接收阶段 */
                app_bootloader_status = APP_BOOTLOADER_REC_PREPARE;
            }
            else
            {
                /* 长度非法（0或非数字） */
                printf("[BL] Invalid length, resend 'start:len'\r\n");
            }
        }
        else
        {
            /* 格式不匹配 */
            printf("[BL] Invalid command format, resend 'start:len'\r\n");
        }
    }
}

/**
 * @brief 擦除APP区对应的Flash扇区。
 *
 */
static void App_Bootloader_Erase_Flash(void)
{
    HAL_FLASH_Unlock();

    /* 根据APP起止地址计算扇区范围 */
    uint32_t start_sector = Bootloader_GetSector(APP_START_ADDR);
    uint32_t end_sector = Bootloader_GetSector(APP_END_ADDR);
    uint32_t page_error = 0;

    FLASH_EraseInitTypeDef erase_init;
    erase_init.TypeErase = FLASH_TYPEERASE_SECTORS;
    erase_init.Sector = start_sector;
    erase_init.NbSectors = (end_sector - start_sector) + 1U;
    erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;

    /* 擦除失败进入统一错误处理 */
    if (HAL_FLASHEx_Erase(&erase_init, &page_error) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_FLASH_Lock();
}

/*
 * 状态：APP_BOOTLOADER_REC_PREPARE
 * 1) 擦除APP区对应Flash扇区；
 * 2) 启动底层UART中断接收；
 * 3) 切换到接收数据状态。
 */
static void App_Bootloader_Receive_Prepare(void)
{
    /* 擦除APP区Flash，准备写入 */
    App_Bootloader_Erase_Flash();

    /* 启动底层接收准备（中断+空闲检测） */
    Bootloader_Receive_Prepare();
    printf("[BL] Start receiving firmware stream\r\n");

    /* 进入数据接收监控阶段 */
    app_bootloader_status = APP_BOOTLOADER_REC_DATA;
}

/*
 * 状态：APP_BOOTLOADER_REC_DATA
 * 通过“最近一次接收时间”判断是否接收结束：
 * - 超过2秒无新数据，认为本次传输完成，进入校验阶段。
 */
static void App_Bootloader_Recv_Data(void)
{
    uint32_t last_receive_time = Get_last_receive_time();
#if BOOTLOADER_AUTO_FINISH
    /* 2秒没有数据接收则认为接收完成 */
    if ((HAL_GetTick() - last_receive_time > 2000) && (last_receive_time > 0))
    {
        app_bootloader_status = APP_BOOTLOADER_CHECK_DATA;
    }
#endif
    /* 按键触发完成标志，进入校验阶段 */
    if (finish_flag == 1)
    {
        app_bootloader_status = APP_BOOTLOADER_CHECK_DATA;
        finish_flag = 0;
    }
}

/*
 * 状态：APP_BOOTLOADER_CHECK_DATA
 * 校验“累计接收长度”与“上位机声明总长度”是否一致。
 * 返回值：0=校验通过，1=校验失败。
 */
static uint8_t App_Bootloader_Check_Data(void)
{
    uint32_t bootloader_rx_len_sum = Get_bootloader_rx_len_sum();

    if (bootloader_rx_len_sum == app_rec_total_len)
    {
        /* 长度一致，准备跳转APP */
        printf("[BL] Receive OK, total len: %d\r\n", bootloader_rx_len_sum);
        app_bootloader_status = APP_BOOTLOADER_JUMP_APP;
        return 0;
    }
    else
    {
        /* 长度不一致，提示重发 */
        printf("[BL] Receive length mismatch, send: %d recv: %d\r\n", app_rec_total_len, bootloader_rx_len_sum);
        printf("[BL] Please resend 'start:len'\r\n");
        return 1;
    }
}

/*
 * 状态：APP_BOOTLOADER_JUMP_APP
 * 调用底层跳转接口进入APP。
 */
static uint8_t App_Bootloader_Jump_App(void)
{
    uint8_t ret = 0;
    ret = Bootloader_Jump_To_App();
    return ret;
}

/*
 * Bootloader主状态机调度函数。
 * 建议在主循环中周期调用。
 */
void App_Bootloader_Work(void)
{
    switch (app_bootloader_status)
    {
    case APP_BOOTLOADER_START:
        /* 等待并解析启动命令 */
        App_Bootloader_Run();
        break;

    case APP_BOOTLOADER_REC_PREPARE:
        finish_flag = 0;
        /* 擦除APP区并启动中断接收 */
        App_Bootloader_Receive_Prepare();
        break;

    case APP_BOOTLOADER_REC_DATA:
        /* 监控接收超时，判断传输结束 */
        App_Bootloader_Recv_Data();
        break;

    case APP_BOOTLOADER_CHECK_DATA:
        /* 长度校验失败则复位系统，重新进入Bootloader流程 */
        if (App_Bootloader_Check_Data())
        {
            printf("[BL] Data check failed, system reset\r\n");
            NVIC_SystemReset();
        }
        break;

    case APP_BOOTLOADER_JUMP_APP:
        /* 跳转失败时复位系统，避免停留在异常状态 */
        if (App_Bootloader_Jump_App())
        {
            printf("[BL] Jump to APP failed, system reset\r\n");
            NVIC_SystemReset();
        }
        break;

    default:
        /* 其他状态不处理 */
        break;
    }
}

/* 按键接收回调函数 */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == GPIO_PIN_0)
    {
        finish_flag = 1;
    }
}
