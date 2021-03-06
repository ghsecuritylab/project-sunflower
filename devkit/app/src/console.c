#include "cmsis_os.h"
#include "stm32f4xx.h"
#include "uart.h"
#include "console.h"
#include "sunflower_app_header.h"
#include "debug.h"
#include "radio.h"
#include "radio_packets.h"
#include "xprintf.h"
#include "ftp.h"
#include "time_sync.h"
#include "si446x_api_lib.h"
#include "si446x_cmd.h"
#include "tcpecho.h"
#include "fw_update.h"
#include "crc.h"
#include <string.h>

osMessageQId        uartRxMsgQ;

uint8_t             rxBuff[CONSOLE_MAX_MSG_SIZE];
uint8_t             txBuff[CONSOLE_MAX_MSG_SIZE];
uint8_t             tmpHALRxBuff;

uint8_t             rxBuffPos = 0;
uint8_t             txBuffPos = 0;

uint8_t             console_task_started = 0;
uint16_t            selected_network_table = 0;

char                testString[] = {"TEST TEST TEST"};

static void         processString(char* str);
static uint8_t      string_len(char* str);
static void         processDebugCommand(char* str, uint8_t len);
static void         processRadioCommand(char* str, uint8_t len);
static void         consoleTxChar(unsigned char c);
static void         processFTPCommand(char* str, uint8_t len);


void consoleTxChar(unsigned char c)
{
    UART_CharTX(USARTn, c);
}

void ConsoleTaskHwInit(void)
{
    GPIO_InitTypeDef   GPIO_InitStructure;
    NVIC_InitTypeDef   NVIC_InitStructure;
    
    USARTn_TX_GPIO_CLK_ENABLE();
    USARTn_RX_GPIO_CLK_ENABLE();
    USARTn_CLK_ENABLE(); 
    
    GPIO_InitStructure.GPIO_Pin   = USARTn_TX_PIN;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd  = GPIO_PuPd_NOPULL;
    GPIO_Init(USARTn_TX_GPIO_PORT, &GPIO_InitStructure);
    
    GPIO_InitStructure.GPIO_Pin   = USARTn_RX_PIN;
    GPIO_Init(USARTn_RX_GPIO_PORT, &GPIO_InitStructure);
    
    GPIO_PinAFConfig(USARTn_TX_GPIO_PORT, USARTn_TX_PIN_SOURCE, USARTn_TX_AF);
    GPIO_PinAFConfig(USARTn_RX_GPIO_PORT, USARTn_RX_PIN_SOURCE, USARTn_RX_AF);    

    NVIC_InitStructure.NVIC_IRQChannel = USARTn_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = configLIBRARY_LOWEST_INTERRUPT_PRIORITY;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}

void ConsoleTaskOSInit(void)
{
    USART_InitTypeDef config;
    osMessageQDef(UARTRxMsgQueue, CONSOLE_MSG_Q_SIZE, char*);
    uartRxMsgQ = osMessageCreate(osMessageQ(UARTRxMsgQueue), NULL);
    
    config.USART_BaudRate               = 115200;
    config.USART_HardwareFlowControl    = USART_HardwareFlowControl_None;
    config.USART_Mode                   = USART_Mode_Rx | USART_Mode_Tx;
    config.USART_Parity                 = USART_Parity_No;
    config.USART_StopBits               = USART_StopBits_1;
    config.USART_WordLength             = USART_WordLength_8b;

    USART_Init(USARTn, &config);
    
    xdev_out(consoleTxChar);
}

void ConsoleTask(void)
{
    osEvent msgQueueEvent;
    char* rxChars = NULL;
    
    UART_Start(USARTn);
    
    while(1)
    {
        UART_StartRX(USARTn);
        
        console_task_started = 1;
        
        // Sleep until we receive some data
        msgQueueEvent = osMessageGet(uartRxMsgQ, osWaitForever);
        
        if(msgQueueEvent.status == osEventMessage)
        {
            rxChars = (char*)msgQueueEvent.value.p;
            processString(rxChars);
        }
        
        memset(rxBuff, 0, CONSOLE_MAX_MSG_SIZE);
        rxBuffPos = 0;
    }
}

void processString(char* str)
{
    uint8_t len = string_len(str);
    uint8_t i;
    
    (void)i;
    
    xprintf("\n");
    
    switch(str[0])
    {
        case 'f':
            processFTPCommand(str, len);
            break;
        case 'r':
            if(len >= 2)
            {
                if(str[1] == 'r')
                {
                    NVIC_SystemReset();
                }
            }
            else
            {
                xprintf("Reset commands\n");
                xprintf("rr: reset the microprocessor completely\n");
            }
            break;
        case 'd':
            processDebugCommand(str, len);
            break;
        
        case 'p':
            {
                generic_message_t fakeReport;
                fakeReport.cmd = SENSOR_MSG;
                fakeReport.dst = RadioGetMACAddress();
                fakeReport.src = 0xDEADBEEF;
                fakeReport.payload.sensor_message.chip_temp = 22;
                fakeReport.payload.sensor_message.moisture2 = 3700;
                fakeReport.payload.sensor_message.timestamp = GetUnixTime();
                
                EnqueueSensorTCP(&fakeReport);
                
                xprintf("Message enqueued\n");
            }
            break;
            
        case 't':
            if(len >= 2)
            {
                if(str[1] == 's')
                {
                    xprintf("Syncing Time...\n");
                    TimeSync();
                }
                else if(str[1] == 'p')
                {
                    xprintf("Current UNIX Time: %d\n", GetUnixTime());
                }
            }
            else
            {
                xprintf("Time commands\n");
                xprintf("ts: sync the RTC with the remote server\n");
                xprintf("tp: print the current RTC time\n");
            }
            break;
            
        case 'x':
            processRadioCommand(str, len);
            break;
        
        case 'v':
            xprintf("SUNFLOWER OS V %d.%d, HW Rev %d, %s\n", (SUNFLOWER_APP_VERSION >> 24) & 0xFF, 
                                                             (SUNFLOWER_APP_VERSION >> 16) & 0xFF,
                                                             (SUNFLOWER_APP_VERSION >> 8)  & 0xFF,
                                                             ((SUNFLOWER_APP_VERSION >> 0)  & 0xFF) == 0x01 ? "DEBUG" : "PRODUCTION" );
        
            xprintf("BUILD DATE: %s @ %s\n\n", __DATE__, __TIME__);
            break;
        
        case 'u':
            if(len >= 2)
            {
                if(str[1] == 'd')
                {
                    xprintf("Dandelion image: %s\n", Is_Dandelion_Image_Valid() ? "VALID" : "INVALID" );
                }
                else if(str[1] == 's')
                {
                    xprintf("Sunflower image (backup): %s\n", Is_Sunflower_Image_Valid(false) ? "VALID" : "INVALID" );
                    xprintf("Sunflower image (main): %s\n", Is_Sunflower_Image_Valid(true) ? "VALID" : "INVALID" );
                }
                else if(str[1] == 'u')
                {
                    uint32_t unit_test = 0xDEADBEEF;
                    xprintf("CRC32 of 0xDEADBEEF: 0x%x\n", crc32(0x00000000, (uint8_t*)(&unit_test), sizeof(uint32_t)));
                }
                else if(str[1] == 't')
                {
                    // This function blocks until the firmware update is finished
                    TransmitFwUpdate();
                }
            }
            else
            {
                xprintf("Firmware Image Commands\n");
                xprintf("ud: check dandelion image valid\n");
                xprintf("us: check sunflower image valid\n");
                xprintf("uu: run CRC unit test\n");
                xprintf("ut: send a dandelion firmware update to all field units\n");
            }
            
        
        default:
            xprintf("h : print help\n");
            xprintf("x : radio commands\n");
            xprintf("t : time commands\n");
            xprintf("v : print version info\n");
            xprintf("d : debug information\n");
            xprintf("u : firmware update commands\n");
            xprintf("r : reset commands\n");
            xprintf("p : add a fake sensor report to the TCP buffer\n");
            break;
    }
    
    xprintf("> ");
}

void processFTPCommand(char* str, uint8_t len)
{   
    ip_addr_t       ipaddr;
    long            port;
    long            ip_temp_a;
    long            ip_temp_b;
    long            ip_temp_c;
    long            ip_temp_d;
    char*           first_num;
    
    if(len >= 2)
    {
        switch(str[1])
        {
            case 'i':
                FTP_Init();
                return;
            
            case 'f':
                FTP_DownloadFirmware();
                return;
            
            case 'd':
                IP4_ADDR(&ipaddr, 169, 254, 129, 99);
                FTP_Connect(&ipaddr, 21);
                return;
            
            case 'l':
                FTP_GetFwVersions(NULL, NULL, 0);
                return;
            
            case 'c':
                // Replace all '.' in the string with ' '
                for(uint8_t i = 0; i < len; i++)
                {
                    if(str[i] == '.')
                    {
                        str[i] = ' ';
                    }
                }
                
                first_num = &str[3];
                
                xatoi(&first_num, &ip_temp_a);
                xatoi(&first_num, &ip_temp_b);
                xatoi(&first_num, &ip_temp_c);
                xatoi(&first_num, &ip_temp_d);
                xatoi(&first_num, &port);
                IP4_ADDR(&ipaddr, ip_temp_a, ip_temp_b, ip_temp_c, ip_temp_d);
                FTP_Connect(&ipaddr, port);
                return;
        }
    }
    
    xprintf("FTP Commands\n");
    xprintf("fi : initialize FTP\n");
    xprintf("fd : connect to default FTP host\n");
    xprintf("fc <ip> <port>: connect to an FTP server at 'ip' on 'port'\n");
    xprintf("ff : perform a full firmware download cycle from waterloo.autom8ed.com\n");
}

void processRadioCommand(char* str, uint8_t len)
{
    generic_message_t* generic_msg;
    
    if(len >= 2)
    {
        switch(str[1])
        {
            case 'g':
            {
                generic_msg = pvPortMalloc(sizeof(generic_message_t));
        
                // TODO: check we didn't run out of RAM (we should catch this in the 
                //       application Malloc failed handler, but just in case)
            
                generic_msg->cmd = DEVICE_INFO;
                generic_msg->dst = 0xFFFFFFFF;
                SendToBroadcast((uint8_t*)generic_msg, sizeof(generic_message_t));
                return;
            }
            
            case 'r':
            {
                uint32_t current_mac = RadioGetDeviceMAC(selected_network_table);
                if(current_mac != 0x00000000)
                {
                    generic_msg = pvPortMalloc(sizeof(generic_message_t));
            
                    // TODO: check we didn't run out of RAM (we should catch this in the 
                    //       application Malloc failed handler, but just in case)
                
                    generic_msg->cmd = RSSI;
                    generic_msg->dst = current_mac;
                    SendToDevice((uint8_t*)generic_msg, sizeof(generic_message_t), current_mac);
                }
                else
                {
                    xprintf("Error selecting MAC address!\r\n");
                }
                return;
            }
            
            case 'p':
                generic_msg = pvPortMalloc(sizeof(generic_message_t));
            
                // TODO: check we didn't run out of RAM (we should catch this in the 
                //       application Malloc failed handler, but just in case)
            
                generic_msg->cmd = PING;
            
                SendToBroadcast((uint8_t*)generic_msg, sizeof(generic_message_t));
                return;
            
            case 'l':
            {
                uint32_t current_mac = RadioGetDeviceMAC(selected_network_table);
                if(current_mac != 0x00000000)
                {
                    xprintf("Currently Selected MAC: 0x%08x\r\n", current_mac);
                }
                else
                {
                    xprintf("Currently Selected MAC: NONE\r\n");
                }
                xprintf("Currently connected devices: \r\n");
                RadioPrintConnectedDevices();
                return;
            }
            case 'i':
                xprintf("Radio MAC: 0x%08x\n", RadioGetMACAddress());
                return;
            
            case 't':
                if(len > 4)
                {
                    long  polling_rate;
                    char* first_num;
                    first_num = &str[3];
                    xatoi(&first_num, &polling_rate);
                    
                    if(polling_rate < 500 || polling_rate > (24 * 60 * 60 * 1000))
                    {
                        ERR("Minimum polling rate is 500ms, maximum rate is %d\n", (24 * 60 * 60 * 1000));
                        return;
                    }
                    
                    uint32_t current_mac = RadioGetDeviceMAC(selected_network_table);
                    if(current_mac != 0x00000000)
                    {
                        generic_msg = pvPortMalloc(sizeof(generic_message_t));
                
                        // TODO: check we didn't run out of RAM (we should catch this in the 
                        //       application Malloc failed handler, but just in case)
                    
                        generic_msg = pvPortMalloc(sizeof(generic_message_t));
                        generic_msg->cmd = SENSOR_CMD;
                        
                        
                        generic_msg->payload.sensor_cmd.sensor_polling_period = polling_rate;
                        generic_msg->payload.sensor_cmd.valid_fields = 0x1;
                        SendToDevice((uint8_t*)generic_msg, sizeof(generic_message_t), current_mac);
                    }
                    else
                    {
                        xprintf("Error selecting MAC address!\r\n");
                    }
                }
                return;
            
            case 'd':
            {
                if(len > 4)
                {                
                    char* first_num;
                    long  network_position;
                    first_num = &str[3];
                    xatoi(&first_num, &network_position);
                    
                    selected_network_table = network_position;
                    
                    uint32_t current_mac = RadioGetDeviceMAC(selected_network_table);
                    if(current_mac != 0x00000000)
                    {
                        xprintf("Selected MAC: 0x%08x\r\n", current_mac);
                    }
                    else
                    {
                        xprintf("Error selecting MAC address!\r\n");
                    }
                    return;
                }
            }
            
            case 's':
            {
                generic_msg = pvPortMalloc(sizeof(generic_message_t));
        
                // TODO: check we didn't run out of RAM (we should catch this in the 
                //       application Malloc failed handler, but just in case)
            
                generic_msg = pvPortMalloc(sizeof(generic_message_t));
                generic_msg->cmd = SENSOR_CMD;
                
                generic_msg->payload.sensor_cmd.valid_fields = 0x80000000;
                SendToBroadcast((uint8_t*)generic_msg, sizeof(generic_message_t));
                return;
            }
            
            case 'z':
            {
                generic_msg = pvPortMalloc(sizeof(generic_message_t));
        
                // TODO: check we didn't run out of RAM (we should catch this in the 
                //       application Malloc failed handler, but just in case)
            
                generic_msg = pvPortMalloc(sizeof(generic_message_t));
                generic_msg->cmd = SENSOR_CMD;
                
                generic_msg->payload.sensor_cmd.valid_fields = 0x40000000;
                SendToBroadcast((uint8_t*)generic_msg, sizeof(generic_message_t));
                return;
            }
        }
    }
    
    xprintf("Radio Commands\n");
    xprintf("xs : reset remote unit\n");
    xprintf("xr : request RSSI info from remote unit\n");
    xprintf("xt <miliseconds> : set sensor polling period\n");
    xprintf("xi : print radio info\n");
    xprintf("xp : send a radio ping packet\n");
    xprintf("xl : print a list of connected / currently selected nodes\n");
    xprintf("xd <device num> : select a device for command targets\n");
    xprintf("xg : get device info from target device\n");
    xprintf("xz : order all sensors to enter sleep mode\n");
}

void processDebugCommand(char* str, uint8_t len)
{
    if(len >= 2)
    {
        if(str[1] == 'd')
        {
            ToggleDebug();
            return;
        }
        else if(str[1] == 'i')
        {
            ToggleInfo();
            return;
        }
        else if(str[1] == 'w')
        {
            ToggleWarn();
            return;
        }
        else if(str[1] == 'e')
        {
            ToggleError();
            return;
        }
        else if(str[1] == 's')
        {
            xprintf("Debug = %s\n", DebugEnabled() ? "TRUE" : "FALSE");
            xprintf("Info  = %s\n", InfoEnabled() ? "TRUE" : "FALSE");
            xprintf("Warn  = %s\n", WarnEnabled() ? "TRUE" : "FALSE");
            xprintf("Error = %s\n", ErrorEnabled() ? "TRUE" : "FALSE");
            return;
        }
    }
    
    xprintf("Debug commands\n");
    xprintf("ds : print enabled message status\n");
    xprintf("dd : toggle debug messages\n");
    xprintf("di : toggle info messages\n");
    xprintf("dw : toggle warn messages\n");
    xprintf("de : toggle error messages\n");
}

uint8_t string_len(char* str)
{
    uint8_t i = 0;
    
    while(str[i] != '\0' && i < CONSOLE_MAX_MSG_SIZE)
    {
        i++;
    }
    
    return i;
}

void ConsolePrint(char* text)
{
    uint8_t i = string_len(text);
        
    while(!console_task_started)
    {
        osDelay(10);
    }
    
    // While the UART is not ready for TX, spin
    while(UART_ReadyTX(USARTn) != UART_OK)
    {
        osDelay(10);
    }
    
    UART_StartTX(USARTn, text, i);
}

void ConsoleGetChar(char c)
{
    // Silently drop chars if buffer is full: OS is dealing with the message
    if(rxBuffPos >= CONSOLE_MAX_MSG_SIZE)
    {
        return;
    }
    
    switch(c)
    {        
        case '\r':
        case '\n':
            osMessagePut(uartRxMsgQ, (uint32_t)rxBuff, osWaitForever);
            break;
        
        default:
            rxBuff[rxBuffPos++] = c;
    }
    
    if(rxBuffPos >= CONSOLE_MAX_MSG_SIZE)
    {
        osMessagePut(uartRxMsgQ, (uint32_t)rxBuff, osWaitForever);
    }
    
    UART_CharTX(USARTn, c);
}
