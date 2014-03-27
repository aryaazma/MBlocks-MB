/* Copyright (c) 2009 Nordic Semiconductor. All Rights Reserved.
 *
 * The information contained herein is property of Nordic Semiconductor ASA.
 * Terms and conditions of usage are described in detail in NORDIC
 * SEMICONDUCTOR STANDARD SOFTWARE LICENSE AGREEMENT.
 *
 * Licensees are granted free, non-transferable use of the information. NO
 * WARRANTY of ANY KIND is provided. This heading must NOT be removed from
 * the file.
 *
 */

#include "twi_master.h"
#include "twi_master_config.h"
#include <stdbool.h>
#include <stdint.h>
#include "nrf.h"
#include "nrf_soc.h"
#include "nrf_delay.h"
#include "nrf_gpio.h"
#include "nrf_error.h"
#include "nrf_assert.h"

#include "global.h"

/* Max cycles approximately to wait on RXDREADY and TXDREADY event, 
 * This is optimized way instead of using timers, this is not power aware. */
#define MAX_TIMEOUT_LOOPS             (20000UL)        /**< MAX while loops to wait for RXD/TXD event */


static bool twi_master_write(uint8_t *data, uint8_t data_length, bool issue_stop_condition)
{
    uint32_t timeout = MAX_TIMEOUT_LOOPS;   /* max loops to wait for EVENTS_TXDSENT event*/

    if (data_length == 0)
    {
        /* Return false for requesting data of size 0 */
        return false;
    }

    NRF_TWI1->TXD           = *data++;
    NRF_TWI1->TASKS_STARTTX = 1;

    while (true)
    {
        while(NRF_TWI1->EVENTS_TXDSENT == 0 && (--timeout))
        {
            // Do nothing.
        }

        if (timeout == 0)
        {
#if (0)
            NRF_TWI1->EVENTS_STOPPED = 0; 
            NRF_TWI1->TASKS_STOP     = 1; 
            
            /* Wait until stop sequence is sent */
            while(NRF_TWI1->EVENTS_STOPPED == 0) 
            { 
                // Do nothing.
            }
#endif
            
            /* Timeout before receiving event*/
            return false;
        }

        NRF_TWI1->EVENTS_TXDSENT = 0;
        if (--data_length == 0)
        {
            break;
        }

        NRF_TWI1->TXD = *data++;
    }
    
    if (issue_stop_condition) 
    { 
        NRF_TWI1->EVENTS_STOPPED = 0; 
        NRF_TWI1->TASKS_STOP     = 1; 
        /* Wait until stop sequence is sent */ 
        while(NRF_TWI1->EVENTS_STOPPED == 0) 
        { 
            // Do nothing.
        } 
    }
    return true;
}


/** @brief Function for read by twi_master. 
 */
static bool twi_master_read(uint8_t *data, uint8_t data_length, bool issue_stop_condition)
{
    uint32_t timeout = MAX_TIMEOUT_LOOPS;   /* max loops to wait for RXDREADY event*/

    if (data_length == 0)
    {
        /* Return false for requesting data of size 0 */
        return false;
    }
    else if (data_length == 1)
    {
        sd_ppi_channel_assign(TWI_MASTER_PPI_CHANNEL,
        		&(NRF_TWI1->EVENTS_BB), &(NRF_TWI1->TASKS_STOP));
    }
    else
    {
        sd_ppi_channel_assign(TWI_MASTER_PPI_CHANNEL,
        		&(NRF_TWI1->EVENTS_BB), &(NRF_TWI1->TASKS_SUSPEND));
    }
    
    sd_ppi_channel_enable_set(TWI_MASTER_PPI_CHEN_MASK);

    NRF_TWI1->TASKS_STARTRX   = 1;
    
    while (true)
    {
        while((NRF_TWI1->EVENTS_RXDREADY == 0) && (--timeout))
        {    
            // Do nothing.
        }

        if (timeout == 0)
        {
            NRF_TWI1->EVENTS_STOPPED = 0;
            NRF_TWI1->TASKS_STOP     = 1;

            /* Wait until stop sequence is sent */
            while(NRF_TWI1->EVENTS_STOPPED == 0)
            {
                // Do nothing.
            }

            return false;
        }

        NRF_TWI1->EVENTS_RXDREADY = 0;
        *data++ = NRF_TWI1->RXD;

        /* Configure PPI to stop TWI master before we get last BB event */
        if (--data_length == 1)
        {
            sd_ppi_channel_assign(TWI_MASTER_PPI_CHANNEL,
            		&(NRF_TWI1->EVENTS_BB), &(NRF_TWI1->TASKS_STOP));
        }

        if (data_length == 0)
        {
            break;
        }
        
        NRF_TWI1->TASKS_RESUME = 1;
    }

    /* Wait until stop sequence is sent */
    while(NRF_TWI1->EVENTS_STOPPED == 0)
    {
        // Do nothing.
    }
    NRF_TWI1->EVENTS_STOPPED = 0;

    sd_ppi_channel_enable_clr(TWI_MASTER_PPI_CHEN_MASK);
    return true;
}


/**
 * @brief Function for detecting stuck slaves (SDA = 0 and SCL = 1) and tries to clear the bus.
 *
 * @return
 * @retval false Bus is stuck.
 * @retval true Bus is clear.
 */
static bool twi_master_clear_bus(void)
{
    uint32_t twi_state;
    bool bus_clear;
    uint32_t clk_pin_config;
    uint32_t data_pin_config;
        
    // Save and disable TWI hardware so software can take control over the pins
    twi_state        = NRF_TWI1->ENABLE;
    NRF_TWI1->ENABLE = TWI_ENABLE_ENABLE_Disabled << TWI_ENABLE_ENABLE_Pos;
    
    clk_pin_config                                        =  \
            NRF_GPIO->PIN_CNF[TWI_MASTER_CONFIG_CLOCK_PIN_NUMBER];    
    NRF_GPIO->PIN_CNF[TWI_MASTER_CONFIG_CLOCK_PIN_NUMBER] =   \
            (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos) \
          | (GPIO_PIN_CNF_DRIVE_S0D1     << GPIO_PIN_CNF_DRIVE_Pos) \
          | (GPIO_PIN_CNF_PULL_Pullup    << GPIO_PIN_CNF_PULL_Pos)  \
          | (GPIO_PIN_CNF_INPUT_Connect  << GPIO_PIN_CNF_INPUT_Pos) \
          | (GPIO_PIN_CNF_DIR_Output     << GPIO_PIN_CNF_DIR_Pos);    

    data_pin_config                                      = \
        NRF_GPIO->PIN_CNF[TWI_MASTER_CONFIG_DATA_PIN_NUMBER];
    NRF_GPIO->PIN_CNF[TWI_MASTER_CONFIG_DATA_PIN_NUMBER] = \
        (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos) \
      | (GPIO_PIN_CNF_DRIVE_S0D1     << GPIO_PIN_CNF_DRIVE_Pos) \
      | (GPIO_PIN_CNF_PULL_Pullup    << GPIO_PIN_CNF_PULL_Pos)  \
      | (GPIO_PIN_CNF_INPUT_Connect  << GPIO_PIN_CNF_INPUT_Pos) \
      | (GPIO_PIN_CNF_DIR_Output     << GPIO_PIN_CNF_DIR_Pos);    
      
    TWI_SDA_HIGH();
    TWI_SCL_HIGH();
    TWI_DELAY();

    if ((TWI_SDA_READ() == 1) && (TWI_SCL_READ() == 1))
    {
        bus_clear = true;
    }
    else
    {
        uint_fast8_t i;
        bus_clear = false;

        // Clock max 18 pulses worst case scenario(9 for master to send the rest of command and 9 for slave to respond) to SCL line and wait for SDA come high
        for (i=18; i--;)
        {
            TWI_SCL_LOW();
            TWI_DELAY();
            TWI_SCL_HIGH();
            TWI_DELAY();

            if (TWI_SDA_READ() == 1)
            {
                bus_clear = true;
                break;
            }
        }
    }
    
    NRF_GPIO->PIN_CNF[TWI_MASTER_CONFIG_CLOCK_PIN_NUMBER] = clk_pin_config;
    NRF_GPIO->PIN_CNF[TWI_MASTER_CONFIG_DATA_PIN_NUMBER]  = data_pin_config;

    NRF_TWI1->ENABLE = twi_state;

    return bus_clear;
}


/** @brief Function for initializing the twi_master.
 */
bool twi_master_init(void)
{
    /* To secure correct signal levels on the pins used by the TWI
       master when the system is in OFF mode, and when the TWI master is 
       disabled, these pins must be configured in the GPIO peripheral.
    */
	uint32_t err_code = 0;

    NRF_GPIO->PIN_CNF[TWI_MASTER_CONFIG_CLOCK_PIN_NUMBER] =     \
        (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos) \
      | (GPIO_PIN_CNF_DRIVE_S0D1     << GPIO_PIN_CNF_DRIVE_Pos) \
      | (GPIO_PIN_CNF_PULL_Pullup    << GPIO_PIN_CNF_PULL_Pos)  \
      | (GPIO_PIN_CNF_INPUT_Connect  << GPIO_PIN_CNF_INPUT_Pos) \
      | (GPIO_PIN_CNF_DIR_Input      << GPIO_PIN_CNF_DIR_Pos);   

    NRF_GPIO->PIN_CNF[TWI_MASTER_CONFIG_DATA_PIN_NUMBER] =      \
        (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos) \
      | (GPIO_PIN_CNF_DRIVE_S0D1     << GPIO_PIN_CNF_DRIVE_Pos) \
      | (GPIO_PIN_CNF_PULL_Pullup    << GPIO_PIN_CNF_PULL_Pos)  \
      | (GPIO_PIN_CNF_INPUT_Connect  << GPIO_PIN_CNF_INPUT_Pos) \
      | (GPIO_PIN_CNF_DIR_Input      << GPIO_PIN_CNF_DIR_Pos);    

    NRF_TWI1->EVENTS_RXDREADY = 0;
    NRF_TWI1->EVENTS_TXDSENT  = 0;
    NRF_TWI1->PSELSCL         = TWI_MASTER_CONFIG_CLOCK_PIN_NUMBER;
    NRF_TWI1->PSELSDA         = TWI_MASTER_CONFIG_DATA_PIN_NUMBER;
    NRF_TWI1->FREQUENCY       = TWI_FREQUENCY_FREQUENCY_K100 << TWI_FREQUENCY_FREQUENCY_Pos;

    err_code = sd_ppi_channel_assign(TWI_MASTER_PPI_CHANNEL,
                                     &(NRF_TWI1->EVENTS_BB),
                                     &(NRF_TWI1->TASKS_SUSPEND));
    ASSERT(err_code == NRF_SUCCESS);

    err_code = sd_ppi_channel_enable_clr(TWI_MASTER_PPI_CHEN_MASK);
    ASSERT(err_code == NRF_SUCCESS);

    NRF_TWI1->ENABLE          = TWI_ENABLE_ENABLE_Enabled << TWI_ENABLE_ENABLE_Pos;

    return twi_master_clear_bus();
}


/** @brief  Function for transfer by twi_master.
 */ 
bool twi_master_transfer(uint8_t address, uint8_t *data, uint8_t data_length, bool issue_stop_condition)
{
    bool transfer_succeeded = false;
    if (data_length > 0 && twi_master_clear_bus())
    {
        NRF_TWI1->ADDRESS = (address >> 1);

        if ((address & TWI_READ_BIT))
        {
            transfer_succeeded = twi_master_read(data, data_length, issue_stop_condition);
        }
        else
        {
            transfer_succeeded = twi_master_write(data, data_length, issue_stop_condition);
        }
    }
    return transfer_succeeded;
}

/*lint --flb "Leave library region" */
