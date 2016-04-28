/***********************************************************************************
Copyright (c) Nordic Semiconductor ASA
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

  1. Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright notice, this
  list of conditions and the following disclaimer in the documentation and/or
  other materials provided with the distribution.

  3. Neither the name of Nordic Semiconductor ASA nor the names of other
  contributors to this software may be used to endorse or promote products
  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
************************************************************************************/
#include <string.h>
#include "bootloader.h"
#include "bootloader_rtc.h"
#include "transport.h"
#include "bootloader_util.h"
#include "bootloader_info.h"
#include "bootloader_mesh.h"
#include "dfu_types_mesh.h"
#include "nrf_mbr.h"
#include "nrf_flash.h"
#include "mesh_aci.h"
#include "app_error.h"
/*****************************************************************************
* Local defines
*****************************************************************************/
#define IRQ_ENABLED                 (0x01)     /**< Field that identifies if an interrupt is enabled. */
#define MAX_NUMBER_INTERRUPTS       (32)       /**< Maximum number of interrupts available. */
/*****************************************************************************
* Local typedefs
*****************************************************************************/

/*****************************************************************************
* Static globals
*****************************************************************************/
static bl_if_cmd_handler_t m_cmd_handler;
/*****************************************************************************
* Static functions
*****************************************************************************/
static void set_timeout(uint32_t time)
{
#ifndef NO_TIMEOUTS
    NRF_RTC0->EVENTS_COMPARE[RTC_BL_STATE_CH] = 0;
    NRF_RTC0->CC[RTC_BL_STATE_CH] = (NRF_RTC0->COUNTER + time) & RTC_MASK;
    NRF_RTC0->INTENSET = (1 << (RTC_BL_STATE_CH + RTC_INTENSET_COMPARE0_Pos));
#endif
}

static void interrupts_disable(void)
{
    uint32_t interrupt_setting_mask;
    uint32_t irq;

    /* Fetch the current interrupt settings. */
    interrupt_setting_mask = NVIC->ISER[0];

    /* Loop from interrupt 0 for disabling of all interrupts. */
    for (irq = 0; irq < MAX_NUMBER_INTERRUPTS; irq++)
    {
        if (interrupt_setting_mask & (IRQ_ENABLED << irq))
        {
            /* The interrupt was enabled, hence disable it. */
            NVIC_DisableIRQ((IRQn_Type)irq);
        }
    }
}

/** Interrupt indicating new serial command */
#ifdef SERIAL
void SWI2_IRQHandler(void)
{
    mesh_aci_command_check();
}
#endif

static void rx_cb(mesh_packet_t* p_packet)
{
    mesh_adv_data_t* p_adv_data = mesh_packet_adv_data_get(p_packet);
    if (p_adv_data && p_adv_data->handle > RBC_MESH_APP_MAX_HANDLE)
    {
        bl_cmd_t rx_cmd;
        rx_cmd.type = BL_CMD_TYPE_RX;
        rx_cmd.params.rx.p_dfu_packet = (dfu_packet_t*) &p_adv_data->handle;
        rx_cmd.params.rx.length = p_adv_data->adv_data_length - 3;
        m_cmd_handler(&rx_cmd);
    }
}

static uint32_t bl_evt_handler(bl_evt_t* p_evt)
{
    bl_cmd_t rsp_cmd;
    switch (p_evt->type)
    {
        case BL_EVT_TYPE_ABORT:
            bootloader_abort(p_evt->params.abort.reason);
            break;
        case BL_EVT_TYPE_TX_RADIO:
            if (!transport_tx(mesh_packet_get_aligned(p_evt->params.tx.radio.p_dfu_packet),
                    p_evt->params.tx.radio.tx_count,
                    (tx_interval_type_t) p_evt->params.tx.radio.interval_type))
            {
                return NRF_ERROR_INTERNAL;
            }
            break;
        case BL_EVT_TYPE_TIMER_SET:
            set_timeout(p_evt->params.timer.set.delay_us);
            break;
        case BL_EVT_TYPE_FLASH_WRITE:
            nrf_flash_store((uint32_t*) p_evt->params.flash.write.start_addr,
                                        p_evt->params.flash.write.p_data,
                                        p_evt->params.flash.write.length, 0);

            /* respond immediately */
            rsp_cmd.type                            = BL_CMD_TYPE_FLASH_WRITE_COMPLETE;
            rsp_cmd.params.flash.write.start_addr   = p_evt->params.flash.write.start_addr;
            rsp_cmd.params.flash.write.p_data       = p_evt->params.flash.write.p_data;
            rsp_cmd.params.flash.write.length       = p_evt->params.flash.write.length;
            m_cmd_handler(&rsp_cmd);
            break;
        case BL_EVT_TYPE_FLASH_ERASE:
            nrf_flash_erase((uint32_t*) p_evt->params.flash.erase.start_addr,
                                        p_evt->params.flash.erase.length);

            /* respond immediately */
            rsp_cmd.type                            = BL_CMD_TYPE_FLASH_ERASE_COMPLETE;
            rsp_cmd.params.flash.erase.start_addr   = p_evt->params.flash.erase.start_addr;
            rsp_cmd.params.flash.erase.length       = p_evt->params.flash.erase.length;
            m_cmd_handler(&rsp_cmd);
            break;
        default:
            return NRF_ERROR_NOT_SUPPORTED;
    }
    return NRF_SUCCESS;
}
/*****************************************************************************
* Interface functions
*****************************************************************************/
void bootloader_init(void)
{
    m_cmd_handler = *((bl_if_cmd_handler_t*) (0x20000000 + ((uint32_t) (NRF_FICR->SIZERAMBLOCKS * NRF_FICR->NUMRAMBLOCK) - 4)));
    if (m_cmd_handler == NULL ||
        (uint32_t) m_cmd_handler >= 0x20000000)
    {
        m_cmd_handler = NULL;
        return;
    }

    rtc_init();

    bl_cmd_t init_cmd;
    init_cmd.type = BL_CMD_TYPE_INIT;
    init_cmd.params.init.bl_if_version = BL_IF_VERSION;
    init_cmd.params.init.event_callback = bl_evt_handler;
    init_cmd.params.init.timer_count = 1;
    m_cmd_handler(&init_cmd);

#ifdef SERIAL
    mesh_aci_init();
#endif

    transport_init(rx_cb, RBC_MESH_ACCESS_ADDRESS_BLE_ADV);
}

void bootloader_enable(void)
{
    bl_cmd_t enable_cmd;
    enable_cmd.type = BL_CMD_TYPE_ENABLE;
    m_cmd_handler(&enable_cmd);
    transport_start();
}

uint32_t bootloader_cmd_send(bl_cmd_t* p_bl_cmd)
{
    return m_cmd_handler(p_bl_cmd);
}

void bootloader_abort(bl_end_t end_reason)
{
    bl_info_entry_t* p_segment_entry = bootloader_info_entry_get((uint32_t*) BOOTLOADER_INFO_ADDRESS, BL_INFO_TYPE_FLAGS);
    switch (end_reason)
    {
        case BL_END_SUCCESS:
        case BL_END_ERROR_TIMEOUT:
        case BL_END_FWID_VALID:
        case BL_END_ERROR_MBR_CALL_FAILED:
            if (p_segment_entry && bootloader_app_is_valid((uint32_t*) p_segment_entry->segment.start))
            {
                interrupts_disable();

                sd_mbr_command_t com = {SD_MBR_COMMAND_INIT_SD, };

                volatile uint32_t err_code = sd_mbr_command(&com);
                APP_ERROR_CHECK(err_code);

                err_code = sd_softdevice_vector_table_base_set(p_segment_entry->segment.start);
                APP_ERROR_CHECK(err_code);

                bootloader_util_app_start(p_segment_entry->segment.start);
            }
            break;
        case BL_END_ERROR_INVALID_PERSISTENT_STORAGE:
            APP_ERROR_CHECK_BOOL(false);
        default:
            NVIC_SystemReset();
            break;
    }
}


bl_info_entry_t* info_entry_get(bl_info_type_t type)
{
    bl_cmd_t get_cmd;
    get_cmd.type = BL_CMD_TYPE_INFO_GET;
    get_cmd.params.info.get.type = type;
    get_cmd.params.info.get.p_entry = NULL;
    if (m_cmd_handler(&get_cmd) != NRF_SUCCESS)
    {
        return NULL;
    }
    
    return get_cmd.params.info.get.p_entry;
}
