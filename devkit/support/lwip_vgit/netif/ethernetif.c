/**
 * @file
 * Ethernet Interface Skeleton
 *
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *
 */

#include "lwip/opt.h"

#include "lwip/def.h"
#include "lwip/mem.h"
#include "lwip/pbuf.h"
#include "lwip/stats.h"
#include "lwip/snmp.h"
#include "lwip/ethip6.h"
#include "netif/etharp.h"
#include "netif/ppp/pppoe.h"

#include "main.h"
#include "stm32f4x7_eth.h"

/* Define those to better describe your network interface. */
#define IFNAME0 's'
#define IFNAME1 't'

#define netifINTERFACE_TASK_STACK_SIZE		( 350 )

#define netifINTERFACE_TASK_PRIORITY		osPriorityHigh

#define netifGUARD_BLOCK_TIME			    ( 250 )

/* The time to block waiting for input. */
#define emacBLOCK_TIME_WAITING_FOR_INPUT	( ( portTickType ) 100 )

static struct netif *s_pxNetIf = NULL;
xSemaphoreHandle s_xSemaphore = NULL;
          
struct ethernetif {
  struct eth_addr *ethaddr;
  /* Add whatever per-interface state that is needed here. */
};

/* Ethernet Rx & Tx DMA Descriptors */
extern ETH_DMADESCTypeDef  DMARxDscrTab[ETH_RXBUFNB], DMATxDscrTab[ETH_TXBUFNB];

/* Ethernet Receive buffers  */
extern uint8_t Rx_Buff[ETH_RXBUFNB][ETH_RX_BUF_SIZE]; 

/* Ethernet Transmit buffers */
extern uint8_t Tx_Buff[ETH_TXBUFNB][ETH_TX_BUF_SIZE]; 

/* Global pointers to track current transmit and receive descriptors */
extern ETH_DMADESCTypeDef  *DMATxDescToSet;
extern ETH_DMADESCTypeDef  *DMARxDescToGet;

/* Global pointer for last received frame infos */
extern ETH_DMA_Rx_Frame_infos *DMA_RX_FRAME_infos;

/* Forward declarations. */
static void  ethernetif_input(struct netif *netif);
static void  arp_timer(void *arg);

/**
 * In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *        for this ethernetif
 */
static void
low_level_init(struct netif *netif)
{
    uint32_t i;
    struct ethernetif *ethernetif = netif->state;

    /* set MAC hardware address length */
    netif->hwaddr_len = ETHARP_HWADDR_LEN;

    /* set netif MAC hardware address */
    netif->hwaddr[0] =  MAC_ADDR0;
    netif->hwaddr[1] =  MAC_ADDR1;
    netif->hwaddr[2] =  MAC_ADDR2;
    netif->hwaddr[3] =  MAC_ADDR3;
    netif->hwaddr[4] =  MAC_ADDR4;
    netif->hwaddr[5] =  MAC_ADDR5;

    /* maximum transfer unit */
    netif->mtu = 1500;

    /* device capabilities */
    /* don't set NETIF_FLAG_ETHARP if this device is not an ethernet one */
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

#if LWIP_IPV6 && LWIP_IPV6_MLD
    /*
    * For hardware/netifs that implement MAC filtering.
    * All-nodes link-local is handled by default, so we must let the hardware know
    * to allow multicast packets in.
    * Should set mld_mac_filter previously. */
    if (netif->mld_mac_filter != NULL) {
        ip6_addr_t ip6_allnodes_ll;
        ip6_addr_set_allnodes_linklocal(&ip6_allnodes_ll);
        netif->mld_mac_filter(netif, &ip6_allnodes_ll, MLD6_ADD_MAC_FILTER);
    }
#endif /* LWIP_IPV6 && LWIP_IPV6_MLD */
    
    
    // Set the global static netif instance so the ethernet_input task can use it
    s_pxNetIf = netif;
 
    /* create binary semaphore used for informing ethernetif of frame reception */
    if (s_xSemaphore == NULL)
    {
        s_xSemaphore= xSemaphoreCreateCounting(20,0);
    }

    /* initialize MAC address in ethernet MAC */ 
    ETH_MACAddressConfig(ETH_MAC_Address0, netif->hwaddr); 

    /* Initialize Tx Descriptors list: Chain Mode */
    ETH_DMATxDescChainInit(DMATxDscrTab, &Tx_Buff[0][0], ETH_TXBUFNB);
    /* Initialize Rx Descriptors list: Chain Mode  */
    ETH_DMARxDescChainInit(DMARxDscrTab, &Rx_Buff[0][0], ETH_RXBUFNB);

    /* Enable Ethernet Rx interrrupt */
    { 
        for(i = 0; i < ETH_RXBUFNB; i++)
        {
            ETH_DMARxDescReceiveITConfig(&DMARxDscrTab[i], ENABLE);
        }
    }

#ifdef CHECKSUM_BY_HARDWARE
    /* Enable the checksum insertion for the Tx frames */
    {
        for(i=0; i<ETH_TXBUFNB; i++)
        {
            ETH_DMATxDescChecksumInsertionConfig(&DMATxDscrTab[i], ETH_DMATxDesc_ChecksumTCPUDPICMPFull);
        }
    }
#endif
  
    /* create the task that handles the ETH_MAC */
    osThreadDef(TNAME, (os_pthread)ethernetif_input, netifINTERFACE_TASK_PRIORITY, 1, netifINTERFACE_TASK_STACK_SIZE);
    osThreadCreate(osThread(TNAME), NULL);
  
    /* Enable MAC and DMA transmission and reception */
    ETH_Start();
}

/**
 * This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *         an err_t value if the packet couldn't be sent
 *
 * @note Returning ERR_MEM here if a DMA queue of your MAC is full can lead to
 *       strange results. You might consider waiting for space in the DMA queue
 *       to become available since the stack doesn't retry to send a packet
 *       dropped because of memory failure (except for the TCP timers).
 */

static err_t
low_level_output(struct netif *netif, struct pbuf *p)
{
    static xSemaphoreHandle xTxSemaphore = NULL;
    struct pbuf *q;
    uint32_t l = 0;
    u8 *buffer ;

    if (xTxSemaphore == NULL)
    {
        vSemaphoreCreateBinary (xTxSemaphore);
    } 

    if (xSemaphoreTake(xTxSemaphore, netifGUARD_BLOCK_TIME))
    {
        buffer =  (u8 *)(DMATxDescToSet->Buffer1Addr);
        for(q = p; q != NULL; q = q->next) 
        {
            memcpy((u8_t*)&buffer[l], q->payload, q->len);
            l = l + q->len;
        }
        ETH_Prepare_Transmit_Descriptors(l);
        xSemaphoreGive(xTxSemaphore);
    }

    return ERR_OK;
}

/**
 * Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return a pbuf filled with the received packet (including MAC header)
 *         NULL on memory error
 */
static struct pbuf * low_level_input(struct netif *netif)
{
    struct ethernetif *ethernetif = netif->state;
    struct pbuf *p, *q;
    u16_t len;
    uint32_t l=0,i =0;
    FrameTypeDef frame;
    u8 *buffer;
    __IO ETH_DMADESCTypeDef *DMARxNextDesc;
    
    p = NULL;
    
    /* Get received frame */
    frame = ETH_Get_Received_Frame_interrupt();
    
    if ((frame.descriptor->Status & ETH_DMARxDesc_ES) == (uint32_t)RESET)
    {
        /* Obtain the size of the packet and put it into the "len"
         variable. */
        len = frame.length;
        buffer = (u8 *)frame.buffer;
        
#if ETH_PAD_SIZE
        len += ETH_PAD_SIZE; /* allow room for Ethernet padding */
#endif

        /* We allocate a pbuf chain of pbufs from the pool. */
        p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        /* Copy received frame from ethernet driver buffer to stack buffer */
        if (p != NULL) {

#if ETH_PAD_SIZE
            pbuf_header(p, -ETH_PAD_SIZE); /* drop the padding word */
#endif

            /* We iterate over the pbuf chain until we have read the entire
             * packet into the pbuf. */
            for (q = p; q != NULL; q = q->next) {
              /* Read enough bytes to fill this pbuf in the chain. The
               * available data in the pbuf is given by the q->len
               * variable.
               * This does not necessarily have to be a memcpy, you can also preallocate
               * pbufs for a DMA-enabled MAC and after receiving truncate it to the
               * actually received size. In this case, ensure the tot_len member of the
               * pbuf is the sum of the chained pbuf len members.
               */
                memcpy((u8_t*)q->payload, (u8_t*)&buffer[l], q->len);
                l = l + q->len;
            }

#if ETH_PAD_SIZE
            pbuf_header(p, ETH_PAD_SIZE); /* reclaim the padding word */
#endif

            LINK_STATS_INC(link.recv);
        } else {
            //drop packet();
            LINK_STATS_INC(link.memerr);
            LINK_STATS_INC(link.drop);
        }
    }

    /* Release descriptors to DMA */
    /* Check if received frame with multiple DMA buffer segments */
    if (DMA_RX_FRAME_infos->Seg_Count > 1)
    {
        DMARxNextDesc = DMA_RX_FRAME_infos->FS_Rx_Desc;
    }
    else
    {
        DMARxNextDesc = frame.descriptor;
    }

    /* Set Own bit in Rx descriptors: gives the buffers back to DMA */
    for (i=0; i<DMA_RX_FRAME_infos->Seg_Count; i++)
    {  
        DMARxNextDesc->Status = ETH_DMARxDesc_OWN;
        DMARxNextDesc = (ETH_DMADESCTypeDef *)(DMARxNextDesc->Buffer2NextDescAddr);
    }

    /* Clear Segment_Count */
    DMA_RX_FRAME_infos->Seg_Count =0;


    /* When Rx Buffer unavailable flag is set: clear it and resume reception */
    if ((ETH->DMASR & ETH_DMASR_RBUS) != (u32)RESET)
    {
        /* Clear RBUS ETHERNET DMA flag */
        ETH->DMASR = ETH_DMASR_RBUS;

        /* Resume DMA reception */
        ETH->DMARPDR = 0;
    }
    
  return p;
}

/**
 * This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface. Then the type of the received packet is determined and
 * the appropriate input function is called.
 *
 * @param netif the lwip network interface structure for this ethernetif
 */
static void
ethernetif_input(struct netif *netif)
{
  struct ethernetif *ethernetif;
  struct eth_hdr *ethhdr;
  struct pbuf *p;

    while(1) {
        // If you can take the "data waiting" semaphore....
        if (xSemaphoreTake(s_xSemaphore, emacBLOCK_TIME_WAITING_FOR_INPUT) == pdTRUE)
        {
            ethernetif = s_pxNetIf->state;

            /* move received packet into a new pbuf */
            p = low_level_input(s_pxNetIf);
            /* no packet could be read, silently ignore this */
            if (p == NULL) return;
            /* points to packet payload, which starts with an Ethernet header */
            ethhdr = p->payload;

            switch (htons(ethhdr->type)) {
                /* IP or ARP packet? */
                case ETHTYPE_IP:
                case ETHTYPE_IPV6:
                case ETHTYPE_ARP:
#if PPPOE_SUPPORT
                /* PPPoE packet? */
                case ETHTYPE_PPPOEDISC:
                case ETHTYPE_PPPOE:
#endif /* PPPOE_SUPPORT */
                    /* full packet send to tcpip_thread to process */
                    if (s_pxNetIf->input(p, s_pxNetIf) != ERR_OK)
                    { 
                        LWIP_DEBUGF(NETIF_DEBUG, ("ethernetif_input: IP input error\n"));
                        pbuf_free(p);
                        p = NULL;
                    }
                    break;

                default:
                    pbuf_free(p);
                    p = NULL;
                    break;
            }
        }
    }
}

/**
 * Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 *         any other err_t on error
 */
err_t ethernetif_init(struct netif *netif)
{
  struct ethernetif *ethernetif;

  LWIP_ASSERT("netif != NULL", (netif != NULL));

  ethernetif = mem_malloc(sizeof(struct ethernetif));
  if (ethernetif == NULL) {
    LWIP_DEBUGF(NETIF_DEBUG, ("ethernetif_init: out of memory\n"));
    return ERR_MEM;
  }

#if LWIP_NETIF_HOSTNAME
  /* Initialize interface hostname */
  netif->hostname = "lwip";
#endif /* LWIP_NETIF_HOSTNAME */

  /*
   * Initialize the snmp variables and counters inside the struct netif.
   * The last argument should be replaced with your link speed, in units
   * of bits per second.
   */
  MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, LINK_SPEED_OF_YOUR_NETIF_IN_BPS);

  netif->state = ethernetif;
  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  /* We directly use etharp_output() here to save a function call.
   * You can instead declare your own function an call etharp_output()
   * from it if you have to do some checks before sending (e.g. if link
   * is available...) */
  netif->output = etharp_output;
#if LWIP_IPV6
  netif->output_ip6 = ethip6_output;
#endif /* LWIP_IPV6 */
  netif->linkoutput = low_level_output;

  ethernetif->ethaddr = (struct eth_addr *)&(netif->hwaddr[0]);

  /* initialize the hardware */
  low_level_init(netif);
  
  etharp_init();
  sys_timeout(ARP_TMR_INTERVAL, arp_timer, NULL);

  return ERR_OK;
}

static void arp_timer(void *arg)
{
    etharp_tmr();
    sys_timeout(ARP_TMR_INTERVAL, arp_timer, NULL);
}
