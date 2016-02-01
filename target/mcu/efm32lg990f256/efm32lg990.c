/*
 * emb6 is licensed under the 3-clause BSD license. This license gives everyone
 * the right to use and distribute the code, either in binary or source code
 * format, as long as the copyright license is retained in the source code.
 *
 * The emb6 is derived from the Contiki OS platform with the explicit approval
 * from Adam Dunkels. However, emb6 is made independent from the OS through the
 * removal of protothreads. In addition, APIs are made more flexible to gain
 * more adaptivity during run-time.
 *
 * The license text is:
 *
 * Copyright (c) 2015,
 * Hochschule Offenburg, University of Applied Sciences
 * Laboratory Embedded Systems and Communications Electronics.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */
/*============================================================================*/
/**
 * \addtogroup bsp
 * @{
 * \addtogroup mcu MCU HAL library
 * @{
 * \addtogroup efm32
 * @{
 */
/*! \file   efm32lg990f256/efm32lg990.c

    \author Manuel Schappacher manuel.schappacher@hs-offenburg.de

    \brief  Hardware dependent initialization for EFM32LG990F256.

   \version 0.0.1
*/
/*============================================================================*/

/*==============================================================================
                                 INCLUDE FILES
==============================================================================*/

#include "emb6.h"

#include "target.h"
#include "hwinit.h"
#include "bsp.h"
#include "board_conf.h"

#include "em_device.h"
#include "em_chip.h"
#include "em_cmu.h"
#include "em_wdog.h"
#include "em_int.h"
#include "em_gpio.h"
#include "spidrv.h"
#include "em_usart.h"
#include "em_timer.h"
#include "gpiointerrupt.h"

#include "lib_tmr.h"

/*==============================================================================
                                     MACROS
==============================================================================*/
#define     LOGGER_ENABLE           LOGGER_HAL
#include    "logger.h"

#ifndef EFM32_LED_ACTIVE_HIGH
#define EFM32_LED_ACTIVE_HIGH       TRUE
#endif /* #ifndef EFM32_LED_ACTIVE_HIGH */

#if EFM32_LED_ACTIVE_HIGH
#define EFM32_LED_OUT_VAL           0
#else /* #ifndef EFM32_LED_ACTIVE_HIGH */
#define EFM32_LED_OUT_VAL           1
#endif /* #ifndef EFM32_LED_ACTIVE_HIGH */

/*==============================================================================
                                     ENUMS
==============================================================================*/

/*==============================================================================
                         STRUCTURES AND OTHER TYPEDEFS
==============================================================================*/


/**
 * \brief    Description of a single Pin.
 */
typedef struct
{
    /** Port */
    GPIO_Port_TypeDef port;
    /** pin */
    uint8_t pin;
    /** mode */
    GPIO_Mode_TypeDef mode;
    /** direction */
    uint8_t val;

} s_hal_gpio_pin_t;


/**
 * \brief    Decription of an SPI interface.
 */
typedef struct
{
    /** handle to the SPI driver */
    SPIDRV_HandleData_t hndl;
    /** handle pointer to the SPI driver */
    SPIDRV_Handle_t pHndl;
    /** chip select pin */
    s_hal_gpio_pin_t csPin;

} s_hal_spiDrv;


/**
 * \brief    All available GPIOs.
 */
typedef enum
{
    /** RF reset pin */
    e_hal_gpios_rf_rst,
    /** RF IRQ pin */
    e_hal_gpios_rf_irq,
    /** RF slp pin */
    e_hal_gpios_rf_slp,

    /** RF CC112x/CC120x GPIO0 pin */
    e_hal_gpios_rf_isq_0,
    /** RF CC112x/CC120x GPIO2 pin */
    e_hal_gpios_rf_isq_1,
    /** RF CC112x/CC120x GPIO3 pin */
    e_hal_gpios_rf_isq_2,

    e_hal_gpios_max

}e_hal_gpios_t;

/*==============================================================================
                           LOCAL FUNCTION PROTOTYPES
==============================================================================*/

/* initialization of the watchdog */
static void _hal_wdcInit( void );

/* initialization of the tick counter */
static void _hal_tcInit( void );

/* initialization of clocks */
static void _hal_clksInit( void );

/* callback for the Tick Counter */
static void _hal_tcCb( void );

/* callback for the radio interrupt */
static void _hal_radioCb(uint8_t pin);

/*==============================================================================
                          VARIABLE DECLARATIONS
==============================================================================*/

/** Ticks since startup */
static clock_time_t volatile l_hal_tick;
/** Seconds since startup */
static clock_time_t volatile l_hal_sec;

/** Definition of the SPI interface */
static s_hal_spiDrv s_hal_spi = {
        .pHndl = NULL,
        .csPin = {EFM32_IO_PORT_USART_CS, EFM32_IO_PIN_USART_CS, gpioModePushPull, 0}
};

s_hal_gpio_pin_t s_hal_gpio[e_hal_gpios_max] =
{
    {EFM32_IO_PORT_RF_RST, EFM32_IO_PIN_RF_RST, gpioModePushPull,  0},  /* e_hal_gpios_rf_rst */
    {EFM32_IO_PORT_RF_IRQ, EFM32_IO_PIN_RF_IRQ, gpioModeInputPull, 0},  /* e_hal_gpios_rf_irq */
    {EFM32_IO_PORT_RF_SLP, EFM32_IO_PIN_RF_SLP, gpioModePushPull,  0},  /* e_hal_gpios_rf_slp */
};

/** registered callback function for the radio interrupt */
pfn_intCallb_t pf_hal_radioCb = NULL;

/** External interrupt GPIOs table */
s_hal_gpio_pin_t s_hal_exti_gpio[E_TARGET_EXT_INT_MAX] =
{
    {EFM32_IO_PORT_RF_IRQ_0, EFM32_IO_PIN_RF_IRQ_0, gpioModeInputPull, 0},  /* E_TARGET_EXT_INT_0 */
    {EFM32_IO_PORT_RF_IRQ_2, EFM32_IO_PIN_RF_IRQ_2, gpioModeInputPull, 0},  /* E_TARGET_EXT_INT_1 */
    {EFM32_IO_PORT_RF_IRQ_3, EFM32_IO_PIN_RF_IRQ_3, gpioModeInputPull, 0},  /* E_TARGET_EXT_INT_2 */

    {0, 0, 0, 0},  /* E_TARGET_EXT_INT_3 is not supported */
};

/** User IOs */
s_hal_gpio_pin_t s_hal_userio[] =
{
    {EFM32_IO_PORT_LED0, EFM32_IO_PIN_LED0, gpioModePushPull,  EFM32_LED_OUT_VAL},  /* LED0 */
    {EFM32_IO_PORT_LED1, EFM32_IO_PIN_LED1, gpioModePushPull, EFM32_LED_OUT_VAL},   /* LED1 */
};

/** External interrupt handler table */
pfn_intCallb_t pf_hal_exti[E_TARGET_EXT_INT_MAX] = {0};

/*==============================================================================
                                LOCAL CONSTANTS
==============================================================================*/

/** mac address */
//const uint8_t mac_address[8] = {RADIO_MAC_ADDR};

/*==============================================================================
                                LOCAL FUNCTIONS
==============================================================================*/

/**
 *    \brief    Timer1 interrupt handler.
 */
void TIMER1_IRQHandler()
{
    uint32_t flags;
    INT_Disable();

    flags = TIMER_IntGet( TIMER1 );
    _hal_tcCb();
    TIMER_IntClear( TIMER1, flags );

    INT_Enable();
}

/**
 *    \brief    Initializes Watchdog.
 */
static void _hal_wdcInit(void)
{
    WDOG_Init_TypeDef wd = {
            FALSE, FALSE, FALSE, FALSE, FALSE,
            FALSE, wdogClkSelLFRCO, wdogPeriod_256k
    };

    /* disable watchdog at initialization*/
    wd.enable = 0;
    WDOG_Init( &wd );

} /* _hal_tcInit() */


/**
 *    \brief    Initializes SysTick.
 */
static void _hal_tcInit(void)
{
    uint32_t l_ticks;
    TIMER_TypeDef* ps_timer = TIMER1;
    TIMER_Init_TypeDef s_timerInit = TIMER_INIT_DEFAULT;

    s_timerInit.enable = true;
    s_timerInit.prescale = timerPrescale2;
    s_timerInit.riseAction = timerInputActionReloadStart;

    /* Calculate ticks */
    l_ticks = SystemHFClockGet() / 2 / 1000;

    /* configure timer for 1ms */
    TIMER_TopSet( ps_timer, l_ticks );
    /* enable timer interrupts */
    NVIC_DisableIRQ( TIMER1_IRQn );
    NVIC_ClearPendingIRQ( TIMER1_IRQn );
    NVIC_EnableIRQ( TIMER1_IRQn );
    TIMER_IntEnable( ps_timer, TIMER_IEN_OF );

    /* initialize and start timer */
    TIMER_Init( ps_timer, &s_timerInit );

} /* _hal_tcInit() */


/**
 *    \brief    Initializes IOs.
 */
static void _hal_clksInit( void )
{
    /* enable required clocks */
    CMU_ClockEnable( cmuClock_HFPER, true );
    CMU_ClockEnable( cmuClock_GPIO, true );
    CMU_ClockEnable( cmuClock_TIMER1, true );
}


/**
 * \brief    SysTick handler to increase internal ticks.
 */
static void _hal_tcCb( void )
{
    /* Indicate timer update to the emb6 timer */
    if( l_hal_tick % CONF_TICK_SEC == 0 )
        l_hal_sec++;
    l_hal_tick++;

    Tmr_Update();
} /* _isr_tc_interrupt() */


/**
 * \brief    Internal radio interrupt callback function.
 */
static void _hal_radioCb( uint8_t pin )
{
    if( pf_hal_radioCb != NULL )
        pf_hal_radioCb( NULL );
}

/**
 * \brief   External interrupt handler
 */
static void _hal_extiCb(uint8_t pin)
{
    uint8_t ix;

    for (ix = 0; ix < E_TARGET_EXT_INT_MAX; ix++) {
        if (s_hal_exti_gpio[ix].pin == pin) {
            if (pf_hal_exti[ix]) {
                pf_hal_exti[ix](NULL);
            }
            break;
        }
    }
}


/*==============================================================================
                                 API FUNCTIONS
==============================================================================*/

/*==============================================================================
  hal_enterCritical()
==============================================================================*/
void hal_enterCritical( void )
{
    /* enter critical section */
    INT_Disable();

} /* hal_enterCritical() */

/*==============================================================================
  hal_exitCritical()
==============================================================================*/
void hal_exitCritical( void )
{
    /* exit critical section */
    INT_Enable();

}/* hal_exitCritical() */

/*==============================================================================
  hwinit_init()
==============================================================================*/
int8_t hal_init (void)
{
    uint8_t ix;

    /* reset callback */
    pf_hal_radioCb = NULL;
    for (ix = 0; ix < E_TARGET_EXT_INT_MAX; ix++) {
        pf_hal_exti[ix] = 0;
    }

    /* initialize Chip */
    CHIP_Init();

    /* initialize clocks */
    _hal_clksInit();

    /* initialize watchdog */
    _hal_wdcInit();

    /* initialize SysTicks */
    _hal_tcInit();

    /* initialize user IOs */
    for( ix = 0; ix < sizeof( s_hal_userio); ix++ )
    {
        s_hal_gpio_pin_t* p_gpioPin = &s_hal_userio[ix];
#if EFM32_LED_ACTIVE_HIGH
        GPIO_PinModeClear( p_gpioPin->port, p_gpioPin->pin, p_gpioPin->mode, p_gpioPin->val );
#else
        GPIO_PinModeSet( p_gpioPin->port, p_gpioPin->pin, p_gpioPin->mode, p_gpioPin->val );
#endif /* #if EFM32_LED_ACTIVE_HIGH */
    }


    return 1;
}/* hal_init() */


/*==============================================================================
  hal_getrand()
==============================================================================*/
uint8_t hal_getrand(void)
{
    // TODO implement this function
    return 0;
}/* hal_getrand() */


/*==============================================================================
  hal_ledOff()
==============================================================================*/
void hal_ledOff(uint16_t ui_led)
{
    s_hal_gpio_pin_t* p_gpioPin = NULL;

    switch( ui_led )
    {
        case 0:
            p_gpioPin = &s_hal_userio[0];
            break;
        case 1:
            p_gpioPin = &s_hal_userio[1];
            break;
        default:
            break;
    }

    if( p_gpioPin != NULL )
#if EFM32_LED_ACTIVE_HIGH
        GPIO_PinOutClear( p_gpioPin->port, p_gpioPin->pin );
#else
        GPIO_PinOutSet( p_gpioPin->port, p_gpioPin->pin );
#endif /* EFM32_LED_ACTIVE_HIGH */
} /* hal_ledOff() */


/*==============================================================================
  hal_ledOn()
==============================================================================*/
void hal_ledOn(uint16_t ui_led)
{
    s_hal_gpio_pin_t* p_gpioPin = NULL;

    switch( ui_led )
    {
        case 0:
            p_gpioPin = &s_hal_userio[0];
            break;
        case 1:
            p_gpioPin = &s_hal_userio[1];
            break;
        default:
            break;
    }

    if( p_gpioPin != NULL )
#if EFM32_LED_ACTIVE_HIGH
        GPIO_PinOutSet( p_gpioPin->port, p_gpioPin->pin );
#else
    GPIO_PinOutClear( p_gpioPin->port, p_gpioPin->pin );
#endif /* EFM32_LED_ACTIVE_HIGH */
} /* hal_ledOn() */


/*==============================================================================
  hal_extiClear()
 =============================================================================*/
void hal_extiRegister(en_targetExtInt_t     e_extInt,
                      en_targetIntEdge_t    e_edge,
                      pfn_intCallb_t        pfn_intCallback)
{
#if NETSTK_CFG_ARG_CHK_EN
    if (pfn_intCallback == NULL) {
        return;
    }
#endif


    s_hal_gpio_pin_t *p_gpioPin = NULL;


    /*
     * This functions initializes the dispatcher register. Typically
     * GPIOINT_Init() is called once in your startup code.
     */
    GPIOINT_Init();


    switch (e_extInt) {
        case E_TARGET_RADIO_INT:

            pf_hal_radioCb = pfn_intCallback;

            p_gpioPin = &s_hal_gpio[e_hal_gpios_rf_irq];
            /* configure pin */
            GPIO_PinModeSet(p_gpioPin->port, p_gpioPin->pin,
                    p_gpioPin->mode, p_gpioPin->val);
            /* Register callbacks before setting up and enabling pin interrupt. */
            GPIOINT_CallbackRegister(p_gpioPin->pin, _hal_radioCb);
            /* Set falling edge interrupt */
            GPIO_IntConfig(p_gpioPin->port, p_gpioPin->pin, true, false,
                    true);
            break;

        case E_TARGET_USART_INT:
            break;

        case E_TARGET_EXT_INT_0:
        case E_TARGET_EXT_INT_1:
        case E_TARGET_EXT_INT_2:
            /*
             * Treat external interrupt configurations in same manner
             */
            p_gpioPin = &s_hal_exti_gpio[e_extInt];

            /* store interrupt callback */
            pf_hal_exti[e_extInt] = pfn_intCallback;

            /* configure external interrupt GPIOs */
            GPIO_PinModeSet(p_gpioPin->port,
                            p_gpioPin->pin,
                            p_gpioPin->mode,
                            p_gpioPin->val);

            /* configure edge detection */
            if (e_edge == E_TARGET_INT_EDGE_FALLING) {
                GPIO_IntConfig(p_gpioPin->port,
                               p_gpioPin->pin,
                               false,
                               true,
                               true);
            } else {
                GPIO_IntConfig(p_gpioPin->port,
                               p_gpioPin->pin,
                               true,
                               false,
                               true);
            }

            /* register callback function */
            GPIOINT_CallbackRegister(p_gpioPin->pin, _hal_extiCb);

            /* disable interrupt by default */
            hal_extiDisable(e_extInt);
            break;

        default:
            break;
    }
} /* hal_extiRegister() */


/*==============================================================================
  hal_extiClear()
 =============================================================================*/
void hal_extiClear(en_targetExtInt_t e_extInt)
{
    uint32_t pin_msk;

    pin_msk = 1 << (s_hal_exti_gpio[e_extInt].pin);
    GPIO_IntClear(pin_msk);
} /* hal_extiClear() */


/*==============================================================================
  hal_extiEnable()
 =============================================================================*/
void hal_extiEnable(en_targetExtInt_t e_extInt)
{
    uint32_t pin_msk;

    pin_msk = 1 << (s_hal_exti_gpio[e_extInt].pin);
    GPIO_IntEnable(pin_msk);
} /* hal_extiEnable() */

/*==============================================================================
  hal_extiDisable()
 =============================================================================*/
void hal_extiDisable(en_targetExtInt_t e_extInt)
{
    uint32_t pin_msk;

    pin_msk = 1 << (s_hal_exti_gpio[e_extInt].pin);
    GPIO_IntDisable(pin_msk);
} /* hal_extiDisable() */


/*==============================================================================
  hal_delay_us()
 =============================================================================*/
void hal_delay_us( uint32_t i_delay )
{
    volatile int i = 0;
    uint32_t l_ticks = ((SystemCoreClockGet() / 1000000) * i_delay) / 10;
    for( i = 0; i < l_ticks; i++ );

} /* hal_delay_us() */

/*==============================================================================
  hal_pinInit()
 =============================================================================*/
void* hal_ctrlPinInit(en_targetExtPin_t e_pinType)
{
    s_hal_gpio_pin_t* p_gpioPin = NULL;

    switch (e_pinType){
        case E_TARGET_RADIO_RST:

            p_gpioPin = &s_hal_gpio[e_hal_gpios_rf_rst];
            /* configure pin */
            GPIO_PinModeSet( p_gpioPin->port, p_gpioPin->pin, p_gpioPin->mode, p_gpioPin->val );
            /* set pin */
            GPIO_PinOutSet( p_gpioPin->port, p_gpioPin->pin );
            break;

        case E_TARGET_RADIO_SLPTR:

            p_gpioPin = &s_hal_gpio[e_hal_gpios_rf_slp];
            /* configure pin */
            GPIO_PinModeSet( p_gpioPin->port, p_gpioPin->pin, p_gpioPin->mode, p_gpioPin->val );
            /* set pin */
            GPIO_PinOutSet( p_gpioPin->port, p_gpioPin->pin );
            break;

        default:

            p_gpioPin = NULL;
            break;
    }

    return p_gpioPin;
} /* hal_ctrlPinInit() */

/*==============================================================================
  hal_pinInit()
 =============================================================================*/
void* hal_pinInit(en_targetExtPin_t e_pinType)
{
    s_hal_gpio_pin_t* p_gpioPin = NULL;

    switch (e_pinType){
        case E_TARGET_RADIO_RST:

            p_gpioPin = &s_hal_gpio[e_hal_gpios_rf_rst];
            /* configure pin */
            GPIO_PinModeSet( p_gpioPin->port, p_gpioPin->pin, p_gpioPin->mode, p_gpioPin->val );
            /* set pin */
            GPIO_PinOutSet( p_gpioPin->port, p_gpioPin->pin );
            break;

        case E_TARGET_RADIO_SLPTR:

            p_gpioPin = &s_hal_gpio[e_hal_gpios_rf_slp];
            /* configure pin */
            GPIO_PinModeSet( p_gpioPin->port, p_gpioPin->pin, p_gpioPin->mode, p_gpioPin->val );
            /* set pin */
            GPIO_PinOutSet( p_gpioPin->port, p_gpioPin->pin );
            break;

        default:

            p_gpioPin = NULL;
            break;
    }

    return p_gpioPin;
} /* hal_pinInit() */

/*==============================================================================
  hal_pinSet()
 =============================================================================*/
void hal_pinSet(void * p_pin)
{
    s_hal_gpio_pin_t* p_gpioPin;

    if( p_pin != NULL ) {
        p_gpioPin = ( s_hal_gpio_pin_t* )p_pin;
        p_gpioPin->val = 1;
        GPIO_PinOutSet( p_gpioPin->port, p_gpioPin->pin );
    }
} /* hal_pinSet() */

/*==============================================================================
  hal_pinClr()
 =============================================================================*/
void hal_pinClr(void * p_pin)
{
    s_hal_gpio_pin_t* p_gpioPin;

    if( p_pin != NULL ) {
        p_gpioPin = (s_hal_gpio_pin_t*)p_pin;
        p_gpioPin->val = 0;
        GPIO_PinOutClear( p_gpioPin->port, p_gpioPin->pin );
    }
} /* hal_pinClr() */

/*==============================================================================
  hal_pinGet()
 =============================================================================*/
uint8_t    hal_pinGet(void * p_pin)
{
    s_hal_gpio_pin_t* p_gpioPin;

    if(p_pin == NULL) {
        return 0;
    }
    p_gpioPin = (s_hal_gpio_pin_t*)p_pin;
    if( p_gpioPin->mode != gpioModeInput )
        return p_gpioPin->val;
    else
        return GPIO_PinInGet( p_gpioPin->port, p_gpioPin->pin );
} /* hal_pinGet() */

/*==============================================================================
  hal_spiInit()
 =============================================================================*/
void* hal_spiInit(void)
{
    /* configure SPI */
    SPIDRV_Init_t spiInit = EFM32_USART;
    spiInit.portLocation = EFM32_USART_LOC;
    spiInit.csControl = spidrvCsControlApplication;

    /* configure SPI clock frequency of 4MHz */
    spiInit.bitRate = 4000000;

    /* initialize SPI */
    s_hal_spi.pHndl = &s_hal_spi.hndl;
    SPIDRV_Init( s_hal_spi.pHndl, &spiInit );

    /* configure manual chip select pin */
    GPIO_PinModeSet( s_hal_spi.csPin.port, s_hal_spi.csPin.pin,
            s_hal_spi.csPin.mode, s_hal_spi.csPin.val );
    /* set chip select pin */
    GPIO_PinOutSet( s_hal_spi.csPin.port, s_hal_spi.csPin.pin );

    return &s_hal_spi;
} /* hal_spiInit() */

/*==============================================================================
  hal_spiSlaveSel()
 =============================================================================*/
uint8_t hal_spiSlaveSel(void * p_spi, bool action)
{
    s_hal_spiDrv* p_spiHal;

    if (p_spi == NULL) {
        LOG_ERR("SPI was not initialized!");
        return 0;
    }

    p_spiHal = (s_hal_spiDrv*)p_spi;

    if (action) {
        //! [select_slave]
        /* clear chip select pin */
        hal_enterCritical();
        GPIO_PinOutClear( p_spiHal->csPin.port, p_spiHal->csPin.pin );
        //! [select_slave]
    }
    else {
        /* set chip select pin */
        GPIO_PinOutSet( p_spiHal->csPin.port, p_spiHal->csPin.pin );
        hal_exitCritical();
    }
    return 1;
} /* hal_spiSlaveSel() */

/*==============================================================================
  hal_spiRead()
 =============================================================================*/
uint8_t hal_spiRead(uint8_t * p_reg, uint16_t i_length)
{
    //SPIDRV_MReceiveB( s_hal_spi.pHndl, p_reg, i_length );
    for( int i = 0; i < i_length; i++ )
        p_reg[i] = USART_SpiTransfer( s_hal_spi.pHndl->initData.port, 0xff );
    return *p_reg;
} /* hal_spiRead() */

/*==============================================================================
  hal_spiWrite()
 =============================================================================*/
void hal_spiWrite(uint8_t * c_value, uint16_t i_length)
{
    //SPIDRV_MTransmitB( s_hal_spi.pHndl, c_value, i_length );
    for( int i = 0; i < i_length; i++ )
        USART_SpiTransfer( s_hal_spi.pHndl->initData.port, c_value[i] );
} /* hal_spiWrite() */


void hal_spiTxRx(uint8_t *p_tx, uint8_t *p_rx, uint16_t len)
{
    uint16_t ix;

    for (ix = 0; ix < len; ix++) {
        p_rx[ix] = USART_SpiTransfer(s_hal_spi.pHndl->initData.port, p_tx[ix]);
    }

}

/*==============================================================================
  hal_spiTranRead()
 =============================================================================*/
void hal_watchdogReset(void)
{
    WDOG_Feed();
} /* hal_watchdogReset() */

/*==============================================================================
  hal_spiTranRead()
 =============================================================================*/
void hal_watchdogStart(void)
{
    WDOG_Enable( true );
} /* hal_watchdogStart() */

/*==============================================================================
  hal_spiTranRead()
 =============================================================================*/
void hal_watchdogStop(void)
{
    WDOG_Enable( false );
} /* hal_watchdogStop() */

/*==============================================================================
  hal_getTick()
 =============================================================================*/
clock_time_t hal_getTick(void)
{
    return l_hal_tick;
} /* hal_getTick() */

/*==============================================================================
  hal_getSec()
 =============================================================================*/
clock_time_t hal_getSec(void)
{
    return l_hal_sec;
} /* hal_getSec() */


/*==============================================================================
  hal_getTRes()
 =============================================================================*/
clock_time_t hal_getTRes(void)
{
    return CLOCK_SECOND;
} /* hal_getSec() */


/** @} */
/** @} */
/** @} */
