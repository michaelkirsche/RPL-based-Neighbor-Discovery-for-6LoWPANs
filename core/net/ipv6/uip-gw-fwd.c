/**
 * \file		pgw_fwd.c
 *
 * \brief		Forwarding/bridging-related functions for the 6LoWPAN-ND proxy-gateway 
 *
 * \author		Luis Maqueda <luis@sen.se>
 */
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include "net/ipv6/uip-gw-fwd.h"
#include "net/ipv6/uip-ds6.h"
#include "net/ipv6/uip-nd6.h"


#if NETSTACK_CONF_WITH_IPV6

//#define DEBUG DEBUG_NONE
#define DEBUG 0
#include "net/ip/uip-debug.h"
/*------------------------------------------------------------------*/
/** @{ */
/** \name Pointers to the header structures.
 *  All pointers except UIP_IP_BUF depend on uip_ext_len, which at
 *  packet reception, is the total length of the extension headers.
 *  
 *  The pointer to ND6 options header also depends on nd6_opt_offset,
 *  which we set in each function.
 *
 *  Care should be taken when manipulating these buffers about the
 *  value of these length variables
 */
#define ICMP6_NA                        136 

#define UIP_IP_BUF                ((struct uip_ip_hdr *)&uip_buf[UIP_LLH_LEN])  /**< Pointer to IP header */
#define UIP_ICMP_BUF            ((struct uip_icmp_hdr *)&uip_buf[uip_l2_l3_hdr_len])  /**< Pointer to ICMP header*/
/**@{  Pointers to messages just after icmp header */
#define UIP_ND6_NS_BUF            ((uip_nd6_ns *)&uip_buf[uip_l2_l3_icmp_hdr_len])
#define UIP_ND6_NA_BUF            ((uip_nd6_na *)&uip_buf[uip_l2_l3_icmp_hdr_len])
/** @} */
/** Pointer to ND option */
#define UIP_ND6_OPT_HDR_BUF  ((uip_nd6_opt_hdr *)&uip_buf[uip_l2_l3_icmp_hdr_len + nd6_opt_offset])
#define UIP_ND6_OPT_PREFIX_BUF ((uip_nd6_opt_prefix_info *)&uip_buf[uip_l2_l3_icmp_hdr_len + nd6_opt_offset])
#define UIP_ND6_OPT_MTU_BUF ((uip_nd6_opt_mtu *)&uip_buf[uip_l2_l3_icmp_hdr_len + nd6_opt_offset])
#define CURRENT_OPT_LENGTH		(UIP_ND6_OPT_HDR_BUF->len << 3)
/** @} */

static uint8_t nd6_opt_offset;                     /** Offset from the end of the icmpv6 header to the option in uip_buf*/
static uint8_t *nd6_opt_llao;   /**  Pointer to llao option in uip_buf */


//#if !UIP_CONF_ROUTER            // TBD see if we move it to ra_input
static uip_ipaddr_t ipaddr;
static uint8_t rtobtained;

static uip_ds6_addr_t *addr; /**  Pointer to an interface address */

uint8_t eth_lladdr_id[ETH_LLADDR_SIZE];
uint8_t eth_router_lladdr_id[ETH_LLADDR_SIZE];
uint8_t eth_llheader[UIP_ETH_LLH_LEN];
/*------------------------------------------------------------------*/

/*
 *  Global (extern) variables 
 */
interface_t incoming_if = UNDEFINED;
interface_t outgoing_if = UNDEFINED;

uint8_t if_send_to_slip;


/* 
 * Static Variables 
 */
/** \brief Bridge table */
static gw_nbr_table_t brigde_table;
/** \brief Bridge table entry*/
static gw_nbr_entry_t *lookup_result;
				
uip_ipaddr_t routeripaddr;				


void
uip_gw_fwd_init() 
{
	memset(&brigde_table, 0, sizeof(brigde_table));	
	rtobtained = 0;
	if_send_to_slip = 0;
//	uip_ip6addr(&routeripaddr, 0x2001, 0x1, 0x0, 0x0, 0xc801, 0x17ff, 0xfea4, 0x54);

}

void
gw_create_ethheader()
{
	uint8_t ieee[ETH_LLADDR_SIZE] = IEEE_8023_MAC_ADDRESS;
	uint8_t type[2] = ETH_IPV6_TYPE;
	int i;
	memcpy(eth_lladdr_id, ieee, ETH_LLADDR_SIZE);
	
	memset(eth_llheader, 0, 14);
//	uint8_t header[UIP_ETH_LLH_LEN] = {202,1,14,129,0,28,0,80,86,192,0,1,134,221};	
//	memcpy(eth_llheader, header, 14);
	memcpy(eth_llheader, eth_router_lladdr_id, ETH_LLADDR_SIZE);
	memcpy(eth_llheader + ETH_LLADDR_SIZE, eth_lladdr_id, ETH_LLADDR_SIZE);
	memcpy(eth_llheader + ETH_LLADDR_SIZE + ETH_LLADDR_SIZE, type, 2);   
	
	PRINTF("Create the ethernet header:\n");
	for(i = 0; i < UIP_ETH_LLH_LEN; i++) printf(" %02x",eth_llheader[i]); 
	PRINTF("\n");
}



void gw_lladdr_from_globladdr(uip_ipaddr_t* target, uip_ipaddr_t* source)
{
	uint8_t linklocal_prefix[2] = {0xfe, 0x80};
	uip_ipaddr_copy(target, source);
	memset(target, 0, 8);
	memcpy(target, linklocal_prefix, 2);
		
}


void gw_icmp6_input()
{
  uint8_t flags;
  PRINTF("GW Received NS from ");
  PRINT6ADDR(&UIP_IP_BUF->srcipaddr);
  PRINTF(" to ");
  PRINT6ADDR(&UIP_IP_BUF->destipaddr);
  PRINTF(" with target address");
  PRINT6ADDR((uip_ipaddr_t *) (&UIP_ND6_NS_BUF->tgtipaddr));
  PRINTF("\n");
  
#if UIP_CONF_IPV6_CHECKS
  if((UIP_IP_BUF->ttl != UIP_ND6_HOP_LIMIT) ||
     (uip_is_addr_mcast(&UIP_ND6_NS_BUF->tgtipaddr)) ||
     (UIP_ICMP_BUF->icode != 0)) {
    PRINTF("GW NS received is bad\n");
    goto discard;
  }
#endif /* UIP_CONF_IPV6_CHECKS */

  /* Options processing */
    nd6_opt_llao = NULL;
    nd6_opt_offset = UIP_ND6_NS_LEN;
  while(uip_l3_icmp_hdr_len + nd6_opt_offset < uip_len) {
#if UIP_CONF_IPV6_CHECKS
    if(UIP_ND6_OPT_HDR_BUF->len == 0) {
      PRINTF("GW NS received is bad\n");
      goto discard;
    }
#endif /* UIP_CONF_IPV6_CHECKS */
switch (UIP_ND6_OPT_HDR_BUF->type) {
    case UIP_ND6_OPT_SLLAO:
      nd6_opt_llao = &uip_buf[uip_l2_l3_icmp_hdr_len + nd6_opt_offset];
#if UIP_CONF_IPV6_CHECKS
      /* There must be NO option in a DAD NS */
      if(uip_is_addr_unspecified(&UIP_IP_BUF->srcipaddr)) {
        PRINTF("GW NS received is bad\n");
        goto discard;
      } 
       else {
#endif /*UIP_CONF_IPV6_CHECKS */
	if(rtobtained == 0){
      	 memcpy(eth_router_lladdr_id, &nd6_opt_llao[UIP_ND6_OPT_DATA_OFFSET], ETH_LLADDR_SIZE);
       	 gw_create_ethheader();
	 rtobtained = 1;
        }
     }

       break;
	         
#if UIP_CONF_IPV6_CHECKS  
#endif /*UIP_CONF_IPV6_CHECKS */
    default:
      PRINTF("GW ND option not supported in NS");
      break;
    }
    nd6_opt_offset += (UIP_ND6_OPT_HDR_BUF->len << 3);
  }
	addr = uip_ds6_addr_lookup(&UIP_ND6_NS_BUF->tgtipaddr);
#if UIP_CONF_IPV6_CHECKS
    if(uip_ds6_is_my_addr(&UIP_IP_BUF->srcipaddr)) {
        /**
         * \NOTE do we do something here? we both are using the same address.
         * If we are doing dad, we could cancel it, though we should receive a
         * NA in response of DAD NS we sent, hence DAD will fail anyway. If we
         * were not doing DAD, it means there is a duplicate in the network!
         */
      PRINTF("GW NS received is bad\n");
      goto discard;
    }
#endif /*UIP_CONF_IPV6_CHECKS */
   /* Address resolution case */
    if(uip_is_addr_solicited_node(&UIP_IP_BUF->destipaddr)) {
	  gw_nbr_entry_t *gwnbraddr;
	  gwnbraddr = gw_nbr_lookup(&UIP_ND6_NS_BUF->tgtipaddr);
	  if(gwnbraddr != NULL && gwnbraddr->state == GW_NBR_REACHABLE) {
		uip_ipaddr_copy(&UIP_IP_BUF->destipaddr, &UIP_IP_BUF->srcipaddr);
		if(!uip_is_addr_link_local(&UIP_IP_BUF->srcipaddr))
		uip_ipaddr_copy(&UIP_IP_BUF->srcipaddr, &UIP_ND6_NS_BUF->tgtipaddr);
		else
		{	
			gw_lladdr_from_globladdr(&UIP_IP_BUF->srcipaddr, &UIP_ND6_NS_BUF->tgtipaddr);
			if_send_to_slip = 1;		
		}
		flags = UIP_ND6_NA_FLAG_SOLICITED | UIP_ND6_NA_FLAG_OVERRIDE;
			goto create_na;
		}
    }

    /* NUD CASE */
    if(uip_ds6_addr_lookup(&UIP_IP_BUF->destipaddr) == addr) {
	  gw_nbr_entry_t *gwnbraddr;
	  gwnbraddr = gw_nbr_lookup(&UIP_ND6_NS_BUF->tgtipaddr);
	  if(gwnbraddr != NULL && gwnbraddr->state == GW_NBR_REACHABLE) {
		uip_ipaddr_copy(&UIP_IP_BUF->destipaddr, &UIP_IP_BUF->srcipaddr);
		uip_ipaddr_copy(&UIP_IP_BUF->srcipaddr, &UIP_ND6_NS_BUF->tgtipaddr);
		flags = GW_ND6_NA_FLAG_SOLICITED | GW_ND6_NA_FLAG_OVERRIDE;
		goto create_na;
		}
    } else {
#if UIP_CONF_IPV6_CHECKS
      PRINTF("GW NS received is bad\n");
      goto discard;
#endif /* UIP_CONF_IPV6_CHECKS */
    }
 
  return;
  create_na:
    /* If the node is a router it should set R flag in NAs */



 PRINTF("GW Prepare to send NA from ");
 PRINT6ADDR(&UIP_IP_BUF->srcipaddr);
 PRINTF(" to ");
 PRINT6ADDR(&UIP_IP_BUF->destipaddr);
 
  uip_gw_create_na(&UIP_IP_BUF->srcipaddr, &UIP_IP_BUF->destipaddr, &UIP_ND6_NS_BUF->tgtipaddr, flags);
	/* include TLLAO option */
  uip_gw_append_icmp_opt(UIP_ND6_OPT_TLLAO, eth_lladdr_id, 0, 0);
  UIP_ICMP_BUF->icmpchksum = 0;
  UIP_ICMP_BUF->icmpchksum = ~uip_icmp6chksum();
  UIP_STAT(++uip_stat.nd6.sent);
  PRINTF("GW Sending NA from ");
  PRINT6ADDR(&UIP_IP_BUF->srcipaddr);
  PRINTF(" to ");
  PRINT6ADDR(&UIP_IP_BUF->destipaddr);
  PRINTF(" with target address ");
  PRINT6ADDR(&UIP_ND6_NA_BUF->tgtipaddr);
  PRINTF("\n");
  return;

discard:
  uip_len = 0;
  return;
forward:
  return;
}

void
uip_gw_create_na(uip_ipaddr_t* src, uip_ipaddr_t* dst, uip_ipaddr_t* tgt, uint8_t flags)
{
		
	uip_ipaddr_t aux;
	
	uip_ipaddr_copy(&aux, src);
  	uip_ipaddr_copy(&UIP_IP_BUF->destipaddr, dst);
	uip_ipaddr_copy(&UIP_IP_BUF->srcipaddr, &aux);
        PRINTF("GW Create NA from ");
	PRINT6ADDR(&UIP_IP_BUF->srcipaddr);
	PRINTF(" to "); 
	PRINT6ADDR(&UIP_IP_BUF->destipaddr);
	
	uip_ext_len = 0;
	UIP_IP_BUF->vtc = 0x60;
	UIP_IP_BUF->tcflow = 0;
	UIP_IP_BUF->flow = 0;
	UIP_IP_BUF->len[0] = 0;       /* length will not be more than 255 */
	UIP_IP_BUF->len[1] = UIP_ICMPH_LEN + UIP_ND6_NA_LEN;
	UIP_IP_BUF->proto = UIP_PROTO_ICMP6;
	UIP_IP_BUF->ttl = UIP_ND6_HOP_LIMIT;

	UIP_ICMP_BUF->type = ICMP6_NA;
	UIP_ICMP_BUF->icode = 0;

	UIP_ND6_NA_BUF->flagsreserved = flags;
	
	uip_ipaddr_copy((uip_ipaddr_t *)&UIP_ND6_NA_BUF->tgtipaddr, tgt);

	uip_len =
    	UIP_IPH_LEN + UIP_ICMPH_LEN + UIP_ND6_NA_LEN;
}

#define UIP_ICMP_OPTS_APPEND ((uip_nd6_opt_hdr *)&uip_buf[UIP_LLH_LEN + uip_len])
void
uip_gw_append_icmp_opt(uint8_t type, void* data, uint8_t status, uint16_t lifetime)
{
	UIP_ICMP_OPTS_APPEND->type = type;
	/* Length depends on the specific type of option */
	switch (type) {
	case UIP_ND6_OPT_SLLAO:
	case UIP_ND6_OPT_TLLAO:
	#if (UIP_CONF_GW == 1) && (incoming_if == IEEE_802_3)
	UIP_ICMP_OPTS_APPEND->len = UIP_ND6_OPT_ETH_LLAO_LEN  >> 3;
  	memcpy((uint8_t*)(UIP_ICMP_OPTS_APPEND) + UIP_ND6_OPT_DATA_OFFSET, data, ETH_LLADDR_SIZE);
  	/* padding required */
  	memset((uint8_t*)(UIP_ICMP_OPTS_APPEND) + UIP_ND6_OPT_DATA_OFFSET + ETH_LLADDR_SIZE, 0,
    				UIP_ND6_OPT_ETH_LLAO_LEN - 2 - ETH_LLADDR_SIZE);
    	UIP_IP_BUF->len[1] += UIP_ND6_OPT_ETH_LLAO_LEN;
    	uip_len += UIP_ND6_OPT_ETH_LLAO_LEN;
	#endif
	break;
	}
}



gw_nbr_entry_t* gw_nbr_lookup(uip_ipaddr_t *addr) 
{
	int i;
	
	for (i = 0; i < brigde_table.elems; i++) {
		PRINTF("GW look up nbr: ");
		PRINT6ADDR(addr);
 		PRINTF("\n");
		PRINTF("compare it with nbr_table entry: ");
		PRINT6ADDR(&(brigde_table.table[i].addr));
 		PRINTF("\n");
		if (uip_ip6addr_cmp(addr,&(brigde_table.table[i].addr))) {
			PRINTF("GW has found this nbr! \n");
			return &(brigde_table.table[i]);
		}
	}
	PRINTF("GW fails to find this nbr!\n");
	return NULL;
}



void 
gw_nbr_add(uip_ipaddr_t *addr) 
{
	int i;
	int index;
	uint8_t flags;
	gw_nbr_entry_t* nbr_entry;


	nbr_entry = gw_nbr_lookup(addr);
	if (nbr_entry != NULL) {
		PRINTF("GW add new nbr: check that this one already exist: ");
		PRINT6ADDR(addr);
 		PRINTF("\n");
		PRINTF("change the state as REACHABLE.\n");
		nbr_entry->state = GW_NBR_REACHABLE;
		return;
			
	}

	if (brigde_table.elems != MAX_GW_NBR_ENTRIES) {
		index = brigde_table.elems;
		brigde_table.elems++;
	} else {
		/* pick victim which is unreachable */
		for (i = 0; i < brigde_table.elems; i++) {
		if (brigde_table.table[i].state = GW_NBR_GARBAGE_COLLECTABLE) {
			index = i;
			break;
		}
	}
	}
	brigde_table.table[index].state = GW_NBR_REACHABLE;
	memcpy(&(brigde_table.table[index].addr), addr, 16);
	PRINTF("GW add new nbr: ");
	PRINT6ADDR(&(brigde_table.table[index].addr));
 	PRINTF("\n");
	
  return;
	
}



void gw_nbr_delete(uip_ipaddr_t *addr)
{
	int i;
	for (i = 0; i < brigde_table.elems; i++) {
		if (uip_ip6addr_cmp(addr,&(brigde_table.table[i].addr))) {
			brigde_table.table[i].state = GW_NBR_GARBAGE_COLLECTABLE;
			PRINTF("GW delete a nbr: ");
			PRINT6ADDR(addr);
 			PRINTF("\n");
		}
	}	
}



#endif /* UIP_CONF_IPV6 */

/** @}*/
