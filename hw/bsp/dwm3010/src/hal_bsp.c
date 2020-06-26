/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */
#include <assert.h>

#include "bsp/bsp.h"
#include "os/mynewt.h"

#if MYNEWT_VAL(TRNG)
#include "trng/trng.h"
#include "trng_stm32/trng_stm32.h"
#endif

#if MYNEWT_VAL(UART_0)
#include <uart/uart.h>
#include <uart_hal/uart_hal.h>
#endif

#include <hal/hal_bsp.h>
#include <hal/hal_gpio.h>
#include "hal/hal_i2c.h"
#include <hal/hal_flash_int.h>
#include <hal/hal_system.h>
#include <hal/hal_timer.h>

#include <hal/hal_spi.h>

#if MYNEWT_VAL(DW3000_DEVICE_0)
#include "dw3000/dw3000_dev.h"
#include "dw3000/dw3000_hal.h"
#endif

#include <stm32f756xx.h>
#include <stm32f7xx_hal_gpio_ex.h>
#include "mcu/stm32_hal.h"
#include <mcu/stm32f7_bsp.h>
#include <mcu/stm32f7xx_mynewt_hal.h>

#if MYNEWT_VAL(ETH_0)
#include <stm32_eth/stm32_eth.h>
#include <stm32_eth/stm32_eth_cfg.h>
#endif

#if PWM_CNT
#include <pwm_stm32/pwm_stm32.h>
static struct pwm_dev stm32_pwm_dev_driver[PWM_CNT];
static const char *stm32_pwm_dev_name[PWM_CNT] = {
#if PWM_CNT > 0
    "pwm0",
#endif
#if PWM_CNT > 1
    "pwm1",
#endif
#if PWM_CNT > 2
    "pwm2",
#endif
};

static struct stm32_pwm_conf  stm32_pwm_config[PWM_CNT] = {
#if MYNEWT_VAL(PWM_0)
    { TIM3, TIM3_IRQn },
#endif
#if MYNEWT_VAL(PWM_1)
    { TIM4, TIM4_IRQn },
#endif
#if MYNEWT_VAL(PWM_2)
    { TIM11, TIM1_TRG_COM_TIM11_IRQn },
#endif
};

#endif /* PWM_CNT */

const uint32_t stm32_flash_sectors[] = {
    0x08000000,     /* 32kB  */
    0x08008000,     /* 32kB  */
    0x08010000,     /* 32kB  */
    0x08018000,     /* 32kB  */
    0x08020000,     /* 128kB */
    0x08040000,     /* 256kB */
    0x08080000,     /* 256kB */
    0x080c0000,     /* 256kB */
    0x08100000,     /* End of flash */
};

#define SZ (sizeof(stm32_flash_sectors) / sizeof(stm32_flash_sectors[0]))
static_assert(MYNEWT_VAL(STM32_FLASH_NUM_AREAS) + 1 == SZ,
        "STM32_FLASH_NUM_AREAS does not match flash sectors");

#if MYNEWT_VAL(TRNG)
static struct trng_dev os_bsp_trng;
#endif

#if MYNEWT_VAL(UART_0)
static struct uart_dev hal_uart0;

static const struct stm32_uart_cfg uart_cfg[UART_CNT] = {
    [0] = {
        .suc_uart = USART3,
        .suc_rcc_reg = &RCC->APB1ENR,
        .suc_rcc_dev = RCC_APB1ENR_USART3EN,
        .suc_pin_tx = MCU_GPIO_PORTD(8),     /* PD8 */
        .suc_pin_rx = MCU_GPIO_PORTD(9),     /* PD9 */
        .suc_pin_rts = -1,
        .suc_pin_cts = -1,
        .suc_pin_af = GPIO_AF7_USART3,
        .suc_irqn = USART3_IRQn,
    }
};
#endif

#if MYNEWT_VAL(SPI_0_MASTER)
struct stm32_hal_spi_cfg spi0m_cfg = {
    .sck_pin      = MCU_GPIO_PORTA(5),
    .mosi_pin     = MCU_GPIO_PORTB(5),
    .miso_pin     = MCU_GPIO_PORTA(6),
    .irq_prio = 2,
};
struct dpl_sem g_spi0_sem;
#endif

#if MYNEWT_VAL(SPI_3_MASTER)
struct stm32_hal_spi_cfg spi3m_cfg = {
    .sck_pin  = MCU_GPIO_PORTE(2),
    .miso_pin = MCU_GPIO_PORTE(5),
    .mosi_pin = MCU_GPIO_PORTE(6),
    .irq_prio = 3,
};
struct dpl_sem g_spi3_sem;
#endif

#if MYNEWT_VAL(I2C_0)
/*
 * The PB8 and PB9 pins are connected through jumpers in the board to
 * both ADC_IN and I2C pins. To enable I2C functionality SB147/SB157 need
 * to be removed (they are the default connections) and SB138/SB143 need
 * to be shorted.
 */
static struct stm32_hal_i2c_cfg i2c_cfg0 = {
    .hic_i2c = I2C1,
    .hic_rcc_reg = &RCC->APB1ENR,
    .hic_rcc_dev = RCC_APB1ENR_I2C1EN,
    .hic_pin_sda = MCU_GPIO_PORTB(9),    /* D14 on CN7 */
    .hic_pin_scl = MCU_GPIO_PORTB(8),    /* D15 on CN7 */
    .hic_pin_af = GPIO_AF4_I2C1,
    .hic_10bit = 0,
    .hic_timingr = 0x30420F13,    /* 100KHz at 16MHz of SysCoreClock */
};
#endif

#if MYNEWT_VAL(ETH_0)
static const struct stm32_eth_cfg eth_cfg = {
    /*
     * PORTA
     *   PA1 - ETH_RMII_REF_CLK
     *   PA2 - ETH_RMII_MDIO
     *   PA7 - ETH_RMII_CRS_DV
     */
    .sec_port_mask[0] = (1 << 1) | (1 << 2) | (1 << 7),

    /*
     * PORTB
     *   PB13 - ETH_RMII_TXD1
     */
    .sec_port_mask[1] = (1 << 13),

    /*
     * PORTC
     *   PC1 - ETH_RMII_MDC
     *   PC4 - ETH_RMII_RXD0
     *   PC5 - ETH_RMII_RXD1
     */
    .sec_port_mask[2] = (1 << 1) | (1 << 4) | (1 << 5),

    /*
     * PORTG
     *   PG11 - ETH_RMII_TXEN
     *   PG13 - ETH_RMII_TXD0
     */
    .sec_port_mask[6] = (1 << 11) | (1 << 13),
    .sec_phy_type = LAN_8742_RMII,
    .sec_phy_irq = -1
};
#endif

/* FIXME */
static const struct hal_bsp_mem_dump dump_cfg[] = {
    [0] = {
        .hbmd_start = &_ram_start,
        .hbmd_size = RAM_SIZE,
    },
    [1] = {
        .hbmd_start = &_dtcmram_start,
        .hbmd_size = DTCMRAM_SIZE,
    },
    [2] = {
        .hbmd_start = &_itcmram_start,
        .hbmd_size = ITCMRAM_SIZE,
    },
};

extern const struct hal_flash stm32_flash_dev;
const struct hal_flash *
hal_bsp_flash_dev(uint8_t id)
{
    /*
     * Internal flash mapped to id 0.
     */
    if (id != 0) {
        return NULL;
    }
    return &stm32_flash_dev;
}

const struct hal_bsp_mem_dump *
hal_bsp_core_dump(int *area_cnt)
{
    *area_cnt = sizeof(dump_cfg) / sizeof(dump_cfg[0]);
    return dump_cfg;
}

#if MYNEWT_VAL(DW3000_DEVICE_0)
/*
 * dw3000 device structure defined in dw3000_hal.c
 */
static struct _dw3000_dev_instance_t * dw3000_0 = 0;
static const struct dw3000_dev_cfg dw3000_0_cfg = {
    .spi_sem = &g_spi0_sem,
    .spi_baudrate = MYNEWT_VAL(DW3000_DEVICE_BAUDRATE),
    .spi_num = MYNEWT_VAL(DW3000_DEVICE_SPI_IDX),
    .rst_pin  = MYNEWT_VAL(DW3000_DEVICE_0_RST),
    .irq_pin  = MYNEWT_VAL(DW3000_DEVICE_0_IRQ),
    .ss_pin = MYNEWT_VAL(DW3000_DEVICE_0_SS),
    .rx_antenna_delay = MYNEWT_VAL(DW3000_DEVICE_0_RX_ANT_DLY),
    .tx_antenna_delay = MYNEWT_VAL(DW3000_DEVICE_0_TX_ANT_DLY),
    .ext_clock_delay = 0
};
#endif


void
hal_bsp_init(void)
{
    int rc;
    (void)rc;

    hal_system_clock_start();

#if MYNEWT_VAL(TRNG)
    rc = os_dev_create(&os_bsp_trng.dev, "trng", OS_DEV_INIT_KERNEL,
                       OS_DEV_INIT_PRIO_DEFAULT, stm32_trng_dev_init, NULL);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(UART_0)
    rc = os_dev_create((struct os_dev *) &hal_uart0, "uart0",
      OS_DEV_INIT_PRIMARY, 0, uart_hal_init, (void *)&uart_cfg[0]);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(SPI_0_MASTER)
    rc = hal_spi_init(0, &spi0m_cfg, HAL_SPI_TYPE_MASTER);
    assert(rc == 0);
    dpl_error_t err = dpl_sem_init(&g_spi0_sem, 0x1);
    assert(err == DPL_OK);
#endif

#if MYNEWT_VAL(SPI_3_MASTER)
    rc = hal_spi_init(3, &spi3m_cfg, HAL_SPI_TYPE_MASTER);
    assert(rc == 0);
    dpl_error_t err = dpl_sem_init(&g_spi3_sem, 0x1);
    assert(err == DPL_OK);
#endif

#if MYNEWT_VAL(DW3000_DEVICE_0)
    dw3000_0 = hal_dw3000_inst(0);
    rc = os_dev_create((struct os_dev *) dw3000_0, "dw3000_0",
            OS_DEV_INIT_PRIMARY, 0, dw3000_dev_init, (void *)&dw3000_0_cfg);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(I2C_0)
    rc = hal_i2c_init(0, &i2c_cfg0);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(TIMER_0)
    hal_timer_init(0, TIM9);
#endif

#if MYNEWT_VAL(TIMER_1)
    hal_timer_init(1, TIM10);
#endif

#if MYNEWT_VAL(TIMER_2)
    hal_timer_init(2, TIM11);
#endif

#if (MYNEWT_VAL(OS_CPUTIME_TIMER_NUM) >= 0)
    rc = os_cputime_init(MYNEWT_VAL(OS_CPUTIME_FREQ));
    assert(rc == 0);
#endif

#if MYNEWT_VAL(ETH_0)
    stm32_eth_init(&eth_cfg);
#endif

#if MYNEWT_VAL(PWM_0)
    rc = os_dev_create((struct os_dev *) &stm32_pwm_dev_driver[PWM_0_DEV_ID],
        (char*)stm32_pwm_dev_name[PWM_0_DEV_ID],
        OS_DEV_INIT_KERNEL,
        OS_DEV_INIT_PRIO_DEFAULT,
        stm32_pwm_dev_init,
        &stm32_pwm_config[PWM_0_DEV_ID]);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(PWM_1)
    rc = os_dev_create((struct os_dev *) &stm32_pwm_dev_driver[PWM_1_DEV_ID],
        (char*)stm32_pwm_dev_name[PWM_1_DEV_ID],
        OS_DEV_INIT_KERNEL,
        OS_DEV_INIT_PRIO_DEFAULT,
        stm32_pwm_dev_init,
        &stm32_pwm_config[PWM_1_DEV_ID]);
    assert(rc == 0);
#endif

#if MYNEWT_VAL(PWM_2)
    rc = os_dev_create((struct os_dev *) &stm32_pwm_dev_driver[PWM_2_DEV_ID],
        (char*)stm32_pwm_dev_name[PWM_2_DEV_ID],
        OS_DEV_INIT_KERNEL,
        OS_DEV_INIT_PRIO_DEFAULT,
        stm32_pwm_dev_init,
        &stm32_pwm_config[PWM_2_DEV_ID]);
    assert(rc == 0);
#endif
}

/**
 * Returns the configured priority for the given interrupt. If no priority
 * configured, return the priority passed in
 *
 * @param irq_num
 * @param pri
 *
 * @return uint32_t
 */
uint32_t
hal_bsp_get_nvic_priority(int irq_num, uint32_t pri)
{
    /* Add any interrupt priorities configured by the bsp here */
    return pri;
}
