/* KallistiOS ##version##

   kernel/net/net_ipv6.h
   Copyright (C) 2010 Lawrence Sebald

*/

#ifndef __LOCAL_NET_IPV6_H
#define __LOCAL_NET_IPV6_H

#include <netinet/in.h>

#ifdef PACKED
#undef PACKED
#endif

#define PACKED __attribute__((packed))

typedef struct ipv6_hdr_s {
    uint8           version_lclass;
    uint8           hclass_lflow;
    uint16          lclass;
    uint16          length;
    uint8           next_header;
    uint8           hop_limit;
    struct in6_addr src_addr;
    struct in6_addr dst_addr;
} PACKED ipv6_hdr_t;

typedef struct ipv6_ext_hdr_s {
    uint8       next_header;
    uint8       ext_length;
    uint8       data[];
} PACKED ipv6_ext_hdr_t;

typedef struct ipv6_pseudo_hdr_s {
    struct in6_addr src_addr;
    struct in6_addr dst_addr;
    uint32          upper_layer_len;
    uint8           zero[3];
    uint8           next_header;
} PACKED ipv6_pseudo_hdr_t;

#undef PACKED

#define IPV6_HDR_EXT_HOP_BY_HOP     0
#define IPV6_HDR_EXT_ROUTING        43
#define IPV6_HDR_EXT_FRAGMENT       44
#define IPV6_HDR_ENCAP_SEC_PAYLOAD  50
#define IPV6_HDR_AUTHENTICATION     51
#define IPV6_HDR_ICMP               58
#define IPV6_HDR_NONE               59
#define IPV6_HDR_EXT_DESTINATION    60

int net_ipv6_send_packet(netif_t *net, ipv6_hdr_t *hdr, const uint8 *data,
                         int data_size);
int net_ipv6_send(netif_t *net, const uint8 *data, int data_size, int hop_limit,
                  int proto, const struct in6_addr *src,
                  const struct in6_addr *dst);
int net_ipv6_input(netif_t *src, const uint8 *pkt, int pktsize);
uint16 net_ipv6_checksum_pseudo(const struct in6_addr *src,
                                const struct in6_addr *dst,
                                uint32 upper_len, uint8 next_hdr);

extern const struct in6_addr in6addr_linklocal_allnodes;
extern const struct in6_addr in6addr_linklocal_allrouters;

/* Init and Shutdown */
int net_ipv6_init();
void net_ipv6_shutdown();

#endif /* !__LOCAL_NET_IPV6_H */