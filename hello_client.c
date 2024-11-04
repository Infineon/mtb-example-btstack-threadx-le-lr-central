
/******************************************************************************
* File Name: hello_client.c
*
* Description: This is the source code LE LR Central Application (GATT Client)
*              LE Long Range Central Example for ModusToolbox.
*
* The GATT Client application is designed to connect and access services
* of the Hello Sensor device using LE LR PHY. Because handles of the all attributes of
* the Hello Sensor are well known, GATT Client does not perform GATT
* discovery, but uses them directly.  GATT Client assumes
* that Hello Sensor advertises a special UUID and connects to the device
* which publishes it.
*
* Features demonstrated
*  - Registration with LE stack for various events
*  - Connection to a peripheral
*  - As a central processing notifications from the server
*  - Scan and connect to peer over LE Coded PHY
*  - Ability to switch between S=2 / S=8 coding post connection
*
* To demonstrate the app, work through the following steps.
* 1. Plug the AIROC eval board into your computer
* 2. Build and download the application (to the AIROC board)
* 3. Make sure that your peripheral device (hello_sensor) is up and advertising
* 4. Push the user button on the board to start the connection process.
* 5. Once connected, press the same user button again for 1 second to switch
*    between S=2/S=8 PHY coding algorithms
*
* Related Document: See README.md
*
********************************************************************************
* Copyright 2021-2023, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/

/*******************************************************************************
 * Header Files
 ******************************************************************************/
#include "wiced_bt_ble.h"
#include "wiced_bt_cfg.h"
#include "wiced_bt_dev.h"
#include "wiced_bt_gatt.h"
#include "wiced_bt_stack.h"
#include "wiced_bt_uuid.h"
#include "wiced_memory.h"
#include "wiced_result.h"
#include "wiced_timer.h"
#include "wiced_hal_nvram.h"

#include "cybsp.h"
#include "cyhal.h"
#include "cycfg_gap.h"
#include "cycfg_pins.h"
#include "cyhal_gpio.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "hello_client.h"
#include "app_bt_utils.h"

/*******************************************************************************
 * Macros
 ********************************************************************************/

#define HCLIENT_APP_TIMEOUT_IN_MS 1000 /* Hello Client App Timer Timeout in milli seconds  */

#define HELLO_CLIENT_PAIRED_KEYS_VS_ID_START 103
#define HELLO_CLIENT_PAIRED_KEYS_VS_ID_END 105

#define PAIRING_BUTTON_ALWAYS_ON 0
#define PAIRING_BUTTON_TOGGLE 1

#define __UUID_SERVICE_HELLO_SENSOR 0x38, 0x28, 0x2E, 0x5F, 0xA5, 0x1E, 0xC7, 0xA4, 0xC2, 0x46, 0x47, 0x74, 0xB6, 0xC7, 0x81, 0x2F
#define HDLD_HELLO_SENSOR_NOTIFY_CHAR_DESC 0x000A
#define OPCODE_VSC_SET_S8_ON_CONNECTION 0x01B7

/* Interrupt priority for GPIO connected to button */
#define GPIO_INTERRUPT_PRIORITY         (4)

/*******************************************************************************
 *                                Structures
 ******************************************************************************/

/* structure to store GATT attributes for read/write operations */
typedef struct
{
    uint16_t handle;
    uint16_t attr_len;
    const void *p_attr;
} gatt_attribute_t;

/* Peer Info */
typedef struct
{
    uint16_t conn_id;               // Connection Identifier
    uint8_t role;                   // central or peripheral in the current connection
    uint8_t addr_type;              // peer address type
    uint8_t transport;              // peer connected transport
    uint8_t peer_addr[BD_ADDR_LEN]; // Peer BD Address
} hello_client_peer_info_t;

/* Host information to be stored in NVRAM */
typedef struct
{
    wiced_bt_ble_address_t bdaddr;                // BD address of the bonded host
    uint16_t characteristic_client_configuration; // Current value of the client configuration descriptor
} hclient_host_info_t;

/* Hello client application info */
typedef struct
{
    uint32_t app_timer_count;                                   // App Timer Count
    uint16_t conn_id;                                           // Hold the peripheral connection id
    uint8_t num_connections;                                    // Number of connections
    uint16_t central_conn_id;                                    // Handle of the central connection
    uint8_t battery_level;                                      // fake battery level
    hclient_host_info_t host_info;                              // NVRAM save area
    hello_client_peer_info_t peer_info[CY_BT_SERVER_MAX_LINKS]; // Peer Info
    uint8_t is_s8_coding_active;                                // S = 8 Coding is being used
} hello_client_app_t;

typedef void (*pfn_free_buffer_t)(uint8_t *);

extern wiced_bt_dev_status_t wiced_bt_ble_cache_ext_conn_config(wiced_bt_ble_ext_conn_cfg_t *p_ext_conn_cfg);
void hello_client_interrupt_handler(void *user_data, cyhal_gpio_event_t event );
#ifdef AUTO_CONNECT
uint16_t last_conn_id = 0;
#endif // AUTO_CONNECT
/*******************************************************************************
 * Global Variables
 ********************************************************************************/

/* Holds the hello client app info */
hello_client_app_t g_hello_client;

wiced_bt_local_identity_keys_t hello_sensor_id_key_store;
wiced_bt_device_link_keys_t hello_sensor_paired_key_store;

/* Hello service UUID  */
const uint8_t hello_service[16] = {__UUID_SERVICE_HELLO_SENSOR};

/* Variable to indicate if the scan has to be started.
 * Set to 1 if the user pushes and holds the button for more than 5 seconds */
uint8_t start_scan = 0;

extern const wiced_bt_cfg_settings_t wiced_bt_cfg_settings;
extern wiced_bt_cfg_ble_t hello_sensor_cfg_ble;

wiced_timer_t hello_client_second_timer;


/* For button press interrupt */
cyhal_gpio_callback_data_t btn_cb_data =
{
    .callback     = hello_client_interrupt_handler,
    .callback_arg = NULL
};

/*******************************************************************************
 * Function Prototypes
 ********************************************************************************/

static void hello_client_app_init(void);
static wiced_result_t hello_client_management_cback(wiced_bt_management_evt_t event,
                                                    wiced_bt_management_evt_data_t *p_event_data);
static wiced_bt_gatt_status_t hello_client_gatt_callback(wiced_bt_gatt_evt_t event, wiced_bt_gatt_event_data_t *p_data);
static wiced_bt_gatt_status_t hello_client_gatt_connection_up(wiced_bt_gatt_connection_status_t *p_conn_status);
static wiced_bt_gatt_status_t hello_client_gatt_connection_down(wiced_bt_gatt_connection_status_t *p_conn_status);
static wiced_bt_gatt_status_t hello_client_gatt_op_comp_cb(wiced_bt_gatt_operation_complete_t *p_data);
static wiced_bt_gatt_status_t hello_client_gatt_req_cb(wiced_bt_gatt_attribute_request_t *p_data);

static void hello_client_app_timeout(WICED_TIMER_PARAM_TYPE arg);
static void hello_client_scan_result_cback(wiced_bt_ble_scan_results_t *p_scan_result, uint8_t *p_adv_data);
static void hello_client_encryption_changed(wiced_result_t result, uint8_t *p_bd_addr);
static void hello_client_add_peer_info(uint16_t conn_id,
                                       uint8_t *p_bd_addr,
                                       uint8_t role,
                                       uint8_t transport,
                                       uint8_t address_type);
static void hello_client_remove_peer_info(uint16_t conn_id);
static hello_client_peer_info_t *hello_client_get_peer_information(uint16_t conn_id);
static void hello_client_process_data_from_peripheral(int len, uint8_t *data);
static void hello_client_gatt_enable_notification(void);
static wiced_bool_t hello_client_is_device_bonded(wiced_bt_device_address_t bd_address);
static int hello_client_is_central(wiced_bt_device_address_t bda);
static void hello_client_rssi_cb(wiced_bt_dev_rssi_result_t *pdata);
static wiced_bool_t             hello_client_save_link_keys( wiced_bt_device_link_keys_t *p_keys);
static wiced_bool_t             hello_client_read_link_keys( wiced_bt_device_link_keys_t *p_keys);
static void                     hello_client_load_keys_to_addr_resolution_db( void );
/*******************************************************************************
* Function Definitions
*******************************************************************************/

/*******************************************************************************
 * Function Name: set_s_8_on_connection
 ********************************************************************************
 * Summary:
 * Send Vendor Specific Command to use S=8 coded PHY
 *
 * Parameters:
 *  None
 *
 * Return
 *  wiced_bt_dev_status_t  Result from BT_RESULT_LIST
 *
 *******************************************************************************/
wiced_bt_dev_status_t set_s_8_on_connection(void)
{
    wiced_bt_dev_status_t bt_status = WICED_BT_ERROR;
    uint8_t buffer[] = {0x01};

    bt_status = wiced_bt_dev_vendor_specific_command(OPCODE_VSC_SET_S8_ON_CONNECTION, sizeof(buffer), buffer, NULL);
    if (bt_status != WICED_BT_PENDING)
    {
        return bt_status;
    }

    return WICED_BT_SUCCESS;
}

/*
 *  Interrupt handler for user button.
 */
void hello_client_interrupt_handler(void *user_data, cyhal_gpio_event_t event )
{
    // if connected, switch coding algorithms
    // else, start scanning for hello sensor
    printf("[hello_client_interrupt_handler] \n");
    if (g_hello_client.num_connections)
    {
        wiced_bt_ble_phy_preferences_t phy_pref = {0};

#ifdef AUTO_CONNECT
        hello_client_peer_info_t *peer_info = hello_client_get_peer_information(last_conn_id);
        if (peer_info == NULL)
        {
            printf("Peer Info null \n");
            return;
        }
#else
        memcpy((void *)phy_pref.remote_bd_addr, (void *)g_hello_client.peer_info[0].peer_addr, BD_ADDR_LEN);
#endif // AUTO_CONNECT
        /* Switch mode to S2 if state is in S8 and vice versa */
        phy_pref.phy_opts = (g_hello_client.is_s8_coding_active) ? BTM_BLE_PREFER_LELR_S2 : BTM_BLE_PREFER_LELR_S8;
        phy_pref.tx_phys = BTM_BLE_PREFER_LELR_PHY;
        phy_pref.rx_phys = BTM_BLE_PREFER_LELR_PHY;

        wiced_bt_ble_set_phy(&phy_pref);

        g_hello_client.is_s8_coding_active ^= 1;
        printf("Switching for LE LR PHY coding [is_s8_coding_active : %d] \n",
               g_hello_client.is_s8_coding_active);
        //cyhal_gpio_write(CYBSP_USER_LED1, 1 - g_hello_client.is_s8_coding_active);
    }
    else
    {

#ifdef USE_S8_DEFAULT
        printf("Set Encoding Scheme to S8\n");
        set_s_8_on_connection();
        g_hello_client.is_s8_coding_active = 1;
#else
        g_hello_client.is_s8_coding_active = 0;
#endif
        printf("Start Scanning for Hello Sensor\n");
        start_scan = 1;

        cyhal_gpio_write(CYBSP_USER_LED1, CYBSP_LED_STATE_ON);
        cyhal_gpio_write(CYBSP_USER_LED2, CYBSP_LED_STATE_OFF);
        wiced_bt_ble_scan(BTM_BLE_SCAN_TYPE_HIGH_DUTY, WICED_TRUE, hello_client_scan_result_cback);
    }
}

static void hello_client_interrupt_config (void)
{
     /* Initialize the user button */
    cyhal_gpio_init(CYBSP_USER_BTN, CYHAL_GPIO_DIR_INPUT, CYHAL_GPIO_DRIVE_PULLUP, CYBSP_BTN_PRESSED);

    /* Configure GPIO interrupt */
    cyhal_gpio_register_callback(CYBSP_USER_BTN, &btn_cb_data);

    cyhal_gpio_enable_event(CYBSP_USER_BTN, CYHAL_GPIO_IRQ_FALL, GPIO_INTERRUPT_PRIORITY, true);
}
/*
 *  Entry point to the application. Set device configuration and start BT
 *  stack initialization.  The actual application initialization will happen
 *  when stack reports that BT device is ready.
 */
void application_start(void)
{
    // Register call back and configuration with stack
    wiced_bt_stack_init(hello_client_management_cback, &wiced_bt_cfg_settings);

    /* Create a buffer heap, make it the default heap.  */
    wiced_bt_create_heap("app", NULL, 0x1000, NULL, WICED_TRUE);
}

/**************************************************************************************************
 * Function Name: hello_client_management_cback
 ***************************************************************************************************
 * Summary:
 *   This is a Bluetooth stack event handler function to receive management events from
 *   the LE stack and process as per the application.
 *
 * Parameters:
 *   wiced_bt_management_evt_t event             : LE event code of one byte length
 *   wiced_bt_management_evt_data_t *p_event_data: Pointer to LE management event structures
 *
 * Return:
 *  wiced_result_t: Error code from WICED_RESULT_LIST or BT_RESULT_LIST
 *
 *************************************************************************************************/
wiced_result_t hello_client_management_cback(wiced_bt_management_evt_t event,
                                             wiced_bt_management_evt_data_t *p_event_data)
{
    wiced_result_t result = WICED_BT_SUCCESS;

    printf("[%s] Event:%s\n", __FUNCTION__, get_bt_event_name(event));

    switch (event)
    {
        /* Bluetooth  stack enabled */
    case BTM_ENABLED_EVT:
    {
        hello_client_app_init();
        hello_client_interrupt_config();
    }
    break;

    case BTM_DISABLED_EVT:
        break;

    case BTM_USER_CONFIRMATION_REQUEST_EVT:
    {
        printf("Numeric_value: %ld \n", (long)(p_event_data->user_confirmation_request.numeric_value));
        wiced_bt_dev_confirm_req_reply(WICED_BT_SUCCESS, p_event_data->user_confirmation_request.bd_addr);
    }
    break;

    case BTM_PASSKEY_NOTIFICATION_EVT:
    {
        printf("PassKey Notification: %ld \n",
               (long)(p_event_data->user_passkey_notification.passkey));
        wiced_bt_dev_confirm_req_reply(WICED_BT_SUCCESS, p_event_data->user_passkey_notification.bd_addr);
    }
    break;

    case BTM_PAIRING_IO_CAPABILITIES_BLE_REQUEST_EVT:
    {
        p_event_data->pairing_io_capabilities_ble_request.local_io_cap = BTM_IO_CAPABILITIES_NONE;
        p_event_data->pairing_io_capabilities_ble_request.oob_data = BTM_OOB_NONE;
        p_event_data->pairing_io_capabilities_ble_request.auth_req = BTM_LE_AUTH_REQ_SC_BOND;
        p_event_data->pairing_io_capabilities_ble_request.max_key_size = 0x10;
        p_event_data->pairing_io_capabilities_ble_request.init_keys =
            BTM_LE_KEY_PENC | BTM_LE_KEY_PID | BTM_LE_KEY_PCSRK | BTM_LE_KEY_LENC;
        p_event_data->pairing_io_capabilities_ble_request.resp_keys =
            BTM_LE_KEY_PENC | BTM_LE_KEY_PID | BTM_LE_KEY_PCSRK | BTM_LE_KEY_LENC;
    }
    break;

    case BTM_PAIRING_COMPLETE_EVT:
    {
        wiced_bt_dev_ble_pairing_info_t *p_info = &p_event_data->pairing_complete.pairing_complete_info.ble;

        printf("Pairing Complete: %d\n", p_info->reason);
    }
    break;

    case BTM_ENCRYPTION_STATUS_EVT:
    {
        wiced_bt_dev_encryption_status_t *p_status = &p_event_data->encryption_status;

        printf("encryption status: res %d\n", p_status->result);

        hello_client_encryption_changed(p_status->result, p_status->bd_addr);
    }
    break;

    case BTM_SECURITY_REQUEST_EVT:
    { /* Use the default security */
        wiced_bt_ble_security_grant(p_event_data->security_request.bd_addr, WICED_BT_SUCCESS);
    }
    break;

     case BTM_PAIRED_DEVICE_LINK_KEYS_UPDATE_EVT:
            hello_client_save_link_keys( &p_event_data->paired_device_link_keys_update );
            break;

    case  BTM_PAIRED_DEVICE_LINK_KEYS_REQUEST_EVT:
        if ( hello_client_read_link_keys( &p_event_data->paired_device_link_keys_request ) )
        {
            printf( "Key retrieval success\n" );
        }
        else
        {
            result = WICED_BT_ERROR;
            printf( "Key retrieval failure\n" );
        }
        break;

    case BTM_LOCAL_IDENTITY_KEYS_UPDATE_EVT:
        break;

    case BTM_LOCAL_IDENTITY_KEYS_REQUEST_EVT:
        result = WICED_BT_NO_RESOURCES;
        break;

    case BTM_BLE_SCAN_STATE_CHANGED_EVT:
    {
        printf("Scan State Change: %d\n", p_event_data->ble_scan_state_changed);
    }
    break;
    case BTM_BLE_PHY_UPDATE_EVT:
    {
        printf("PHY config is updated (1=1M, 2=2M, 3=LE coded) as TX_PHY : %d, RX_PHY : %d\n",
            p_event_data->ble_phy_update_event.tx_phy,
            p_event_data->ble_phy_update_event.rx_phy);
        if ( (p_event_data->ble_phy_update_event.tx_phy == 3) || (p_event_data->ble_phy_update_event.rx_phy == 3))
        {
            printf("Switching for LE LR PHY coding [is_s8_coding_active : %d] \n",
                       g_hello_client.is_s8_coding_active);
        }
    }
        break;
    default:
        break;
    }

    return result;
}

#ifdef ENABLE_BT_SPY_LOG
void hci_trace_cback(wiced_bt_hci_trace_type_t type, uint16_t length, uint8_t *p_data)
{
    cybt_debug_uart_send_hci_trace(type, length, p_data);
}
#endif

/*
 * WICED BT Init Complete.  This function is called when device initialization
 * has been completed.  Perform the App Initializations & Callback Registrations
 */
void hello_client_app_init(void)
{
    int index;
    wiced_bt_gatt_status_t gatt_status;
    wiced_result_t result;
    wiced_bt_ble_ext_scan_config_t scan_cfg;
    wiced_bt_ble_ext_conn_cfg_t conn_cfg;
    wiced_bt_ble_phy_preferences_t phy_preferences;

    printf("hello_client_app_init\n");

#ifdef ENABLE_BT_SPY_LOG
    wiced_bt_dev_register_hci_trace(hci_trace_cback);
#endif

    memset(&g_hello_client, 0, sizeof(g_hello_client));

    // reset connection information
    g_hello_client.num_connections = 0;
    for (index = 0; index < CY_BT_SERVER_MAX_LINKS; index++)
    {
        g_hello_client.peer_info[index].conn_id = 0;
    }

    /* Register with stack to receive GATT related events */
    gatt_status = wiced_bt_gatt_register(hello_client_gatt_callback);

    printf("wiced_bt_gatt_register status %d \n", gatt_status);

    /* Allow peer to pair */
    wiced_bt_set_pairable_mode(WICED_TRUE, 0);

    /* Load the address resolution DB with the keys stored in the NVRAM */
    hello_client_load_keys_to_addr_resolution_db();

    /* Starting the app timers , seconds timer and the ms timer  */
    if (wiced_init_timer(&hello_client_second_timer, hello_client_app_timeout, 0, WICED_MILLI_SECONDS_PERIODIC_TIMER) ==
        WICED_SUCCESS)
    {
        wiced_start_timer(&hello_client_second_timer, HCLIENT_APP_TIMEOUT_IN_MS);
    }

    /* Initialize the RGB LED */

    cyhal_gpio_init(CYBSP_USER_LED1, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_STRONG, CYBSP_LED_STATE_OFF);
    cyhal_gpio_init(CYBSP_USER_LED2, CYHAL_GPIO_DIR_OUTPUT, CYHAL_GPIO_DRIVE_STRONG, CYBSP_LED_STATE_OFF);

    /* set the default PHY to LE Coded PHY */
    phy_preferences.rx_phys = BTM_BLE_PREFER_LELR_PHY;
    phy_preferences.tx_phys = BTM_BLE_PREFER_LELR_PHY;
    wiced_bt_ble_set_default_phy(&phy_preferences);

    scan_cfg.scanning_phys = WICED_BT_BLE_EXT_ADV_PHY_LE_CODED_BIT;
    scan_cfg.duration = 60 * 100; // 6000*10ms=60s
    scan_cfg.period = 0;          // scan continuously

    scan_cfg.enc_phy_scan_type = 1;   // active scan
    scan_cfg.enc_phy_scan_int = 200;  // 200*0.625ms = 125ms
    scan_cfg.enc_phy_scan_win = 80;  // 100*0.625ms = 62.5ms

    result = wiced_bt_ble_cache_ext_scan_config(&scan_cfg);

    printf("[%s] Result: %d\n", __FUNCTION__, result);

    memset(&conn_cfg, 0, sizeof(conn_cfg));

    conn_cfg.initiating_phys = WICED_BT_BLE_EXT_ADV_PHY_LE_CODED_BIT;

    conn_cfg.scan_int[0] = conn_cfg.scan_int[1] = conn_cfg.scan_int[2] = 200;                    // 200*0.625ms=125ms
    conn_cfg.scan_window[0] = conn_cfg.scan_window[1] = conn_cfg.scan_window[2] = 80;           // 80*0.625ms=50ms
    conn_cfg.min_conn_int[0] = conn_cfg.min_conn_int[1] = conn_cfg.min_conn_int[2] = 100;         // 100*1.25ms=125ms
    conn_cfg.max_conn_int[0] = conn_cfg.max_conn_int[1] = conn_cfg.max_conn_int[2] = 200;         // 200*1.25ms=250ms
    conn_cfg.conn_latency[0] = conn_cfg.conn_latency[1] = conn_cfg.conn_latency[2] = 0;          // number of connection events
    conn_cfg.supervision_to[0] = conn_cfg.supervision_to[1] = conn_cfg.supervision_to[2] = 1000; // 1000*10ms10s

    result = wiced_bt_ble_cache_ext_conn_config(&conn_cfg);

    printf("[%s] Result: %d\n", __FUNCTION__, result);

    UNUSED_VARIABLE(result);
    UNUSED_VARIABLE(gatt_status);
}

/*
 * Callback function is executed to process various GATT events
 */
wiced_bt_gatt_status_t hello_client_gatt_callback(wiced_bt_gatt_evt_t event, wiced_bt_gatt_event_data_t *p_data)
{
    wiced_bt_gatt_status_t result = WICED_BT_SUCCESS;

    printf("[%s] event %d \n", __FUNCTION__, event);

    switch (event)
    {
    case GATT_CONNECTION_STATUS_EVT:
        if (p_data->connection_status.connected)
        {
            result = hello_client_gatt_connection_up(&p_data->connection_status);
        }
        else
        {
            result = hello_client_gatt_connection_down(&p_data->connection_status);
        }
        break;

    case GATT_DISCOVERY_RESULT_EVT:
        printf("[%s] Discovery result  Type: %d\n", __FUNCTION__, p_data->discovery_result.discovery_type);
        break;

    case GATT_DISCOVERY_CPLT_EVT:
        printf("[%s] Discovery complete, will now enable notifications\n", __FUNCTION__);
        break;

    case GATT_OPERATION_CPLT_EVT:
        result = hello_client_gatt_op_comp_cb(&p_data->operation_complete);
        break;

    case GATT_ATTRIBUTE_REQUEST_EVT:
        result = hello_client_gatt_req_cb(&p_data->attribute_request);
        break;

    case GATT_GET_RESPONSE_BUFFER_EVT:
        p_data->buffer_request.buffer.p_app_rsp_buffer =
            wiced_bt_get_buffer(p_data->buffer_request.len_requested);
        p_data->buffer_request.buffer.p_app_ctxt = (void *)wiced_bt_free_buffer;
        break;

    case GATT_APP_BUFFER_TRANSMITTED_EVT:
    {
        pfn_free_buffer_t pfn_free = (pfn_free_buffer_t)p_data->buffer_xmitted.p_app_ctxt;
        if (pfn_free)
            pfn_free(p_data->buffer_xmitted.p_app_data);
    }
    break;

    default:
        break;
    }

    return result;
}

/* This function will be called on every connection establishment */
/* This function is invoked when connection is established */
wiced_bt_gatt_status_t hello_client_gatt_connection_up(wiced_bt_gatt_connection_status_t *p_conn_status)
{
    if (g_hello_client.num_connections > CY_BT_SERVER_MAX_LINKS)
    {
        printf("[%s] max connect limit reached: %d!\n", __FUNCTION__, g_hello_client.num_connections);
        wiced_bt_gatt_disconnect(p_conn_status->conn_id);
        return WICED_BT_GATT_SUCCESS;
    }
    /* Make sure to update phy*/
    {
        wiced_bt_ble_phy_preferences_t phy_pref = {0};

        memcpy((void *)phy_pref.remote_bd_addr, (void *)p_conn_status->bd_addr, BD_ADDR_LEN);
        /* Switch mode to S2 if state is in S8 and vice versa */
#ifdef USE_S8_DEFAULT
        phy_pref.phy_opts = BTM_BLE_PREFER_LELR_S8;
#else
        phy_pref.phy_opts = BTM_BLE_PREFER_LELR_S2;
#endif
        phy_pref.tx_phys = BTM_BLE_PREFER_LELR_PHY;
        phy_pref.rx_phys = BTM_BLE_PREFER_LELR_PHY;

        wiced_bt_ble_set_phy(&phy_pref);
    }
    // Keep number of active connections
    g_hello_client.num_connections++;
#ifdef AUTO_CONNECT
    last_conn_id = p_conn_status->conn_id;
#endif // AUTO_CONNECT

    // Adding the peer info
    hello_client_add_peer_info(p_conn_status->conn_id,
                               p_conn_status->bd_addr,
                               p_conn_status->link_role,
                               p_conn_status->transport,
                               p_conn_status->addr_type);

    printf("[%s] Conn Id:%d Num conn:%d Role:%d\n",
           __FUNCTION__,
           p_conn_status->conn_id,
           g_hello_client.num_connections,
           p_conn_status->link_role);

    // This application supports single connection to central (phone) and multiple connections to peripherals (hello_sensors)
    if (p_conn_status->link_role == HCI_ROLE_CENTRAL)
    {
        g_hello_client.conn_id = p_conn_status->conn_id;

        /* Configure to receive notification from server */
        hello_client_gatt_enable_notification();
    }
    else // Connected as peripheral
    {
        // Update the connection handle to the central
        g_hello_client.central_conn_id = p_conn_status->conn_id;
    }

    cyhal_gpio_write(CYBSP_USER_LED1, CYBSP_LED_STATE_ON);
    cyhal_gpio_write(CYBSP_USER_LED2, CYBSP_LED_STATE_ON);
    return WICED_BT_GATT_SUCCESS;
}

/* This function will be called when connection goes down */
wiced_bt_gatt_status_t hello_client_gatt_connection_down(wiced_bt_gatt_connection_status_t *p_conn_status)
{

    printf("hello_client_connection_down %d \n", g_hello_client.num_connections);

    /* Check if the device is there in the peer info table */
    if ((g_hello_client.num_connections) && hello_client_get_peer_information(p_conn_status->conn_id))
    {
        // Decrement the number of  connections
        g_hello_client.num_connections--;
    }

    if (p_conn_status->link_role == HCI_ROLE_PERIPHERAL)
    {
        // Resetting the connection handle to the central
        g_hello_client.central_conn_id = 0;
    }

    // Remove the peer info
    hello_client_remove_peer_info(p_conn_status->conn_id);

    cyhal_gpio_write(CYBSP_USER_LED1, CYBSP_LED_STATE_OFF);
    cyhal_gpio_write(CYBSP_USER_LED2, CYBSP_LED_STATE_OFF);
#ifdef AUTO_CONNECT
    if (start_scan == 0)
    {
        printf("Starting scan \n");
        start_scan=1;
        wiced_bt_ble_scan(BTM_BLE_SCAN_TYPE_HIGH_DUTY, WICED_TRUE, hello_client_scan_result_cback);
    }
#endif // AUTO_CONNECT
    return WICED_BT_GATT_SUCCESS;
}

/*
 * GATT operation started by the client has been completed
 */
wiced_bt_gatt_status_t hello_client_gatt_op_comp_cb(wiced_bt_gatt_operation_complete_t *p_data)
{
    wiced_result_t status;
    hello_client_peer_info_t *p_peer_info = NULL;
    wiced_bt_ble_sec_action_type_t encryption_type = BTM_BLE_SEC_ENCRYPT;

    printf("hello_client_gatt_op_comp_cb conn %d op %d st %d\n", p_data->conn_id, p_data->op, p_data->status);

    switch (p_data->op)
    {
    case GATTC_OPTYPE_READ_HANDLE:
    case GATTC_OPTYPE_READ_BY_TYPE:
        printf("read_rsp status:%d\n", p_data->status);
        break;

    case GATTC_OPTYPE_WRITE_WITH_RSP:
    case GATTC_OPTYPE_WRITE_NO_RSP:
    case GATTC_OPTYPE_PREPARE_WRITE:
        printf("write_rsp status:%d\n", p_data->status);

        /* server puts authentication requirement. Encrypt the link */
        if ((p_data->status == WICED_BT_GATT_INSUF_AUTHENTICATION) &&
            (p_data->response_data.handle == HDLD_HELLO_SENSOR_NOTIFY_CHAR_DESC))
        {
            if ((p_peer_info = hello_client_get_peer_information(p_data->conn_id)) != NULL)
            {
                if (hello_client_is_device_bonded(p_peer_info->peer_addr))
                {
                    status = wiced_bt_dev_set_encryption(p_peer_info->peer_addr,
                                                         p_peer_info->transport,
                                                         &encryption_type);
                    printf("wiced_bt_dev_set_encryption %d \n", status);
                }
                else
                {
                    status = wiced_bt_dev_sec_bond(p_peer_info->peer_addr,
                                                   p_peer_info->addr_type,
                                                   p_peer_info->transport,
                                                   0,
                                                   NULL);
                    printf("wiced_bt_dev_sec_bond %d \n", status);
                }
            }
        }
        break;

    case GATTC_OPTYPE_CONFIG_MTU:
        printf("peer mtu:%d\n", p_data->response_data.mtu);
        break;

    case GATTC_OPTYPE_NOTIFICATION:
        hello_client_process_data_from_peripheral(p_data->response_data.att_value.len,
                                             p_data->response_data.att_value.p_data);
        if ((p_peer_info = hello_client_get_peer_information(p_data->conn_id)) != NULL)
        {
             wiced_bt_dev_read_rssi(p_peer_info->peer_addr, p_peer_info->transport, (wiced_bt_dev_cmpl_cback_t*) hello_client_rssi_cb);
        }
        break;

    case GATTC_OPTYPE_INDICATION:
        hello_client_process_data_from_peripheral(p_data->response_data.att_value.len,
                                             p_data->response_data.att_value.p_data);
        wiced_bt_gatt_client_send_indication_confirm(p_data->conn_id, p_data->response_data.handle);
        break;

    default:
        break;
    }

    UNUSED_VARIABLE(status);
    return WICED_BT_GATT_SUCCESS;
}
/*
 * This function handles callback for rssi result event
 */
void hello_client_rssi_cb(wiced_bt_dev_rssi_result_t *pdata)
{
    char rssi_val[8]={'0'};
    snprintf(rssi_val, sizeof(rssi_val),"%d", (int) pdata->rssi);
    printf("RSSI value is %d \r\n", pdata->rssi);
}
/*
 * This function handles notification/indication data received from the peripheral device
 */
void hello_client_process_data_from_peripheral(int len, uint8_t *data)
{
    char lenar[4]={'0'};
    printf("hello_client_process_data_from_peripheral len:%d central conn_id:%d ccc:%d\n",
           len,
           g_hello_client.central_conn_id,
           g_hello_client.host_info.characteristic_client_configuration);
    snprintf(lenar, sizeof(lenar),"%d", (int) len%100);
}

/*
 * Process various GATT requests received from the central
 */
wiced_bt_gatt_status_t hello_client_gatt_req_cb(wiced_bt_gatt_attribute_request_t *p_data)
{
    wiced_bt_gatt_status_t result = WICED_BT_GATT_SUCCESS;

    printf("hello_client_gatt_req_cb. conn %d, opcode %d\n", p_data->conn_id, p_data->opcode);

    switch (p_data->opcode)
    {
    case GATT_REQ_MTU:
        printf("peer mtu:%d\n", p_data->data.remote_mtu);
        wiced_bt_gatt_server_send_mtu_rsp(p_data->conn_id,
                                          p_data->data.remote_mtu,
                                          CY_BT_RX_PDU_SIZE);
        break;

    default:
        break;
    }

    return result;
}

/* The function invoked on timeout of app seconds timer. */
void hello_client_app_timeout(WICED_TIMER_PARAM_TYPE arg)
{
    wiced_result_t status = WICED_BT_SUCCESS;
    char time_value[5] = {'0'};

    g_hello_client.app_timer_count++;
    if (g_hello_client.app_timer_count % 10 == 0)
    {
        printf("[%s] Count: %ld\n", __FUNCTION__, (long)(g_hello_client.app_timer_count));

        snprintf(time_value, sizeof(time_value),"%ld",(long) g_hello_client.app_timer_count);

        if (start_scan && wiced_bt_ble_get_current_scan_state() == BTM_BLE_SCAN_TYPE_NONE)
        {
            status = wiced_bt_ble_scan(BTM_BLE_SCAN_TYPE_HIGH_DUTY, WICED_TRUE, hello_client_scan_result_cback);
            printf("wiced_bt_ble_scan: %d\n", status);
        }
        UNUSED_VARIABLE(status);
    }
}

/*
 * Process notification from the stack that encryption has been set.
 * If connected client is registered for notification or indication,
 * it is a good time to send it out
 */
void hello_client_encryption_changed(wiced_result_t result, uint8_t *p_bd_addr)
{
    printf("hello_client_encryption_changed %d\n", result);

    /* Bonding success */
    if (result == WICED_BT_SUCCESS)
    {
        // When we are connected as a central, we need to enable notifications from the peripheral
        // This needs to be done only once, because client configuration descriptor value
        // should be persistent across connections with bonded devices.
        if (hello_client_is_central(p_bd_addr))
        {
            hello_client_gatt_enable_notification();
        }
    }
#ifdef AUTO_CONNECT
    if ( g_hello_client.num_connections < CY_BT_SERVER_MAX_LINKS)
    {
        printf("Starting scan \n");
        start_scan = 1;
        wiced_bt_ble_scan(BTM_BLE_SCAN_TYPE_HIGH_DUTY, WICED_TRUE, hello_client_scan_result_cback);
    }
#endif // AUTO_CONNECT
}

/*
 * This function handles the scan results
 */
void hello_client_scan_result_cback(wiced_bt_ble_scan_results_t *p_scan_result, uint8_t *p_adv_data)
{
    wiced_result_t status;
    wiced_bool_t ret_status;
    uint8_t length;
    uint8_t *p_data;

    if (p_scan_result)
    {
        // Advertisement data from hello_server should have Advertisement type SERVICE_UUID_128
        p_data = wiced_bt_ble_check_advertising_data(p_adv_data, BTM_BLE_ADVERT_TYPE_128SRV_COMPLETE, &length);

        // Check if  the hello service uuid is there in the advertisement
        if ((p_data == NULL) || (length != LEN_UUID_128) || (memcmp(p_data, hello_service, LEN_UUID_128) != 0))
        {
            // wrong device
            return;
        }

        printf("[%s] Found Hello Sensor\n", __FUNCTION__);

        start_scan = 0;

        /* Stop the scan since the desired device is found */
        status = wiced_bt_ble_scan(BTM_BLE_SCAN_TYPE_NONE, WICED_TRUE, hello_client_scan_result_cback);

        printf("[%s] scan off status %d\n", __FUNCTION__, status);

        /* Initiate the connection */
        ret_status = wiced_bt_gatt_le_connect(p_scan_result->remote_bd_addr,
                                              p_scan_result->ble_addr_type,
                                              BLE_CONN_MODE_HIGH_DUTY,
                                              TRUE);

        printf("[%s]  wiced_bt_gatt_connect returned status %d\n", __FUNCTION__, ret_status);
    }
    else
    {
        printf("[%s] Scan completed:\n", __FUNCTION__);
    }
    UNUSED_VARIABLE(ret_status);
    UNUSED_VARIABLE(status);
}

/*
 * This function writes into peer's client configuration descriptor to enable notifications
 */
void hello_client_gatt_enable_notification(void)
{
    wiced_bt_gatt_status_t status;
    uint16_t u16 = GATT_CLIENT_CONFIG_NOTIFICATION;
    wiced_bt_gatt_write_hdr_t enable_notif = {0};

    // Allocating a buffer to send the write request
    uint8_t *val = wiced_bt_get_buffer(sizeof(uint16_t));

    if (val)
    {
        WICED_MEMCPY(val, &u16, sizeof(uint16_t));
        enable_notif.auth_req = GATT_AUTH_REQ_NONE;
        enable_notif.handle = HDLD_HELLO_SENSOR_NOTIFY_CHAR_DESC; /* hard coded server ccd */
        enable_notif.offset = 0;
        enable_notif.len = 2;

        // Register with the server to receive notification
        status = wiced_bt_gatt_client_send_write(g_hello_client.conn_id,
                                                 GATT_REQ_WRITE,
                                                 &enable_notif,
                                                 val,
                                                 (void *)wiced_bt_free_buffer);

        printf("wiced_bt_gatt_send_write %d\n", status);
    }
    UNUSED_VARIABLE(status);
}

/*
 * This function adds the peer information to the table
 */
void hello_client_add_peer_info(uint16_t conn_id,
                                uint8_t *p_bd_addr,
                                uint8_t role,
                                uint8_t transport,
                                uint8_t address_type)
{
    int index;

    for (index = 0; index < CY_BT_SERVER_MAX_LINKS; index++)
    {
        if (g_hello_client.peer_info[index].conn_id == 0)
        {
            g_hello_client.peer_info[index].conn_id = conn_id;
            g_hello_client.peer_info[index].role = role;
            g_hello_client.peer_info[index].transport = transport;
            g_hello_client.peer_info[index].addr_type = address_type;
            memcpy(g_hello_client.peer_info[index].peer_addr, p_bd_addr, BD_ADDR_LEN);
            break;
        }
    }
}

/*
 * This function removes the peer information from the table
 */
void hello_client_remove_peer_info(uint16_t conn_id)
{
    int index;

    for (index = 0; index < CY_BT_SERVER_MAX_LINKS; index++)
    {
        if (g_hello_client.peer_info[index].conn_id == conn_id)
        {
            g_hello_client.peer_info[index].conn_id = 0;
        }
    }
}

/*
 * This function gets the peer address from  the table
 */
hello_client_peer_info_t *hello_client_get_peer_information(uint16_t conn_id)
{
    int index;

    for (index = 0; index < CY_BT_SERVER_MAX_LINKS; index++)
    {
        if (g_hello_client.peer_info[index].conn_id == conn_id)
        {
            return &g_hello_client.peer_info[index];
        }
    }
    return NULL;
}

/*
 * Find out if specific device is connected as a central
 */
static int hello_client_is_central(wiced_bt_device_address_t bda)
{
    int index;

    for (index = 0; index < CY_BT_SERVER_MAX_LINKS; index++)
    {
        if (g_hello_client.peer_info[index].conn_id != 0)
        {
            if (memcmp(g_hello_client.peer_info[index].peer_addr, bda, BD_ADDR_LEN) == 0)
            {
                return (g_hello_client.peer_info[index].role == HCI_ROLE_CENTRAL);
            }
        }
    }
    return FALSE;
}


/*
 * This function is called to save keys generated as a result of pairing or keys update
 */
wiced_bool_t hello_client_save_link_keys( wiced_bt_device_link_keys_t *p_keys)
{
    uint8_t                     bytes_written, bytes_read;
    wiced_bt_device_link_keys_t temp_keys;
    uint16_t                    id = 0, i;
    wiced_result_t              result;

    // search through all available NVRAM IDs.
    for ( i = WICED_NVRAM_VSID_START; i < WICED_NVRAM_VSID_END; i++ )
    {
        bytes_read = wiced_hal_read_nvram( i, sizeof( temp_keys ), (uint8_t *)&temp_keys, &result );

        printf( "Read NVRAM at:%d bytes:%d result:%d\n", i, bytes_read, result );

        // if failed to read NVRAM, there is nothing saved at that location
        if ( ( result != WICED_SUCCESS ) || ( bytes_read != sizeof( temp_keys ) ) )
        {
            id = i;
            break;
        }
        else
        {
            if ( memcmp( temp_keys.bd_addr, p_keys->bd_addr, BD_ADDR_LEN ) == 0 )
            {
                // keys for this device have been saved, reuse the ID
                id = i;
                break;
            }
        }
    }
    if ( id == 0 )
    {
        // all NVRAM locations are already occupied.  Cann't save anything.
        printf( "Failed to save NVRAM\n" );
        return WICED_FALSE;
    }
    printf( "writing to id:%d\n", id );
    bytes_written = wiced_hal_write_nvram( id, sizeof( wiced_bt_device_link_keys_t ), (uint8_t *)p_keys, &result );
    printf( "Saved %d bytes at id:%d\n", bytes_written, id );
    UNUSED_VARIABLE(bytes_written);
    return WICED_TRUE;
}

/*
 * This function is called to read keys for specific bdaddr
 */
wiced_bool_t hello_client_read_link_keys( wiced_bt_device_link_keys_t *p_keys)
{
    wiced_bt_device_link_keys_t temp_keys;
    uint8_t                     bytes_read;
    uint16_t                    i;
    wiced_result_t              result;

    // search through all available NVRAM IDs.
    for ( i = WICED_NVRAM_VSID_START; i < WICED_NVRAM_VSID_END; i++ )
    {
        bytes_read = wiced_hal_read_nvram( i, sizeof( temp_keys ), (uint8_t *)&temp_keys, &result );

        printf(" [%s] read status %d bytes read %d \n", __FUNCTION__, result, bytes_read);

        // if failed to read NVRAM, there is nothing saved at that location
        if ( result == WICED_SUCCESS )
        {
            if ( memcmp( temp_keys.bd_addr, p_keys->bd_addr, BD_ADDR_LEN ) == 0 )
            {
                // keys for this device have been saved
                memcpy( &p_keys->key_data, &temp_keys.key_data, sizeof( temp_keys.key_data ) );
                return WICED_TRUE;
            }
        }
        else
        {
            break;
        }
    }
    UNUSED_VARIABLE(bytes_read);
    return WICED_FALSE;
}

void hello_client_load_keys_to_addr_resolution_db( void )
{
    uint8_t                     bytes_read;
    uint16_t                    i;
    wiced_result_t              result;
    wiced_bt_device_link_keys_t keys;

    // search through all available NVRAM IDs.
    for ( i = WICED_NVRAM_VSID_START; i < WICED_NVRAM_VSID_END; i++ )
    {
        bytes_read = wiced_hal_read_nvram( i, sizeof( keys ), (uint8_t *)&keys, &result );

        printf(" [%s] read status %d bytes read %d \n", __FUNCTION__, result, bytes_read);

        // if failed to read NVRAM, there is nothing saved at that location
        if ( result == WICED_SUCCESS )
        {
#ifdef CYW20706A2
            result = wiced_bt_dev_add_device_to_address_resolution_db( &keys, keys.key_data.ble_addr_type );
#else
            result = wiced_bt_dev_add_device_to_address_resolution_db( &keys );
#endif

            printf("Updated Addr Resolution DB:%d\n", result );
        }
        else
        {
            break;
        }
    }
    UNUSED_VARIABLE(bytes_read);
}

/* Check for device entry exists in NVRAM list */
wiced_bool_t hello_client_is_device_bonded( wiced_bt_device_address_t bd_address )
{
    wiced_bt_device_link_keys_t temp_keys;
    uint8_t                     bytes_read;
    uint16_t                    i;
    wiced_result_t              result;

    // search through all available NVRAM IDs.
    for ( i = WICED_NVRAM_VSID_START; i < WICED_NVRAM_VSID_END; i++ )
    {
        bytes_read = wiced_hal_read_nvram( i, sizeof( temp_keys ), (uint8_t *)&temp_keys, &result );

        printf(" [%s] read status %d bytes read %d \n", __FUNCTION__, result, bytes_read);

        // if failed to read NVRAM, there is nothing saved at that location
        if ( result == WICED_SUCCESS )
        {
            if ( memcmp( temp_keys.bd_addr, bd_address, BD_ADDR_LEN ) == 0 )
            {
                return WICED_TRUE;
            }
        }
        else
        {
            break;
        }
    }
    UNUSED_VARIABLE(bytes_read);
    return WICED_FALSE;
}

/* [] END OF FILE */
