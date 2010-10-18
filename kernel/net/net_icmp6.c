/* KallistiOS ##version##

   kernel/net/net_icmp6.c
   Copyright (C) 2010 Lawrence Sebald

*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <arch/timer.h>

#include "net_icmp6.h"
#include "net_ipv6.h"
#include "net_ipv4.h"   /* For net_ipv4_checksum() */

/*
This file implements RFC 4443, the Internet Control Message Protocol for IPv6.
All messages mentioned below are from that RFC, unless otherwise specified.
Currently implemented message types are:
    128 - Echo
    129 - Echo Reply
    133 - Router Solicitation (RFC 4861) -- Sending only
    134 - Router Advertisement (RFC 4861) -- Receiving only
    135 - Neighbor Solicitation (RFC 4861)
    136 - Neighbor Advertisement (RFC 4861)
    137 - Redirect (RFC 4861) -- partial

Message types that are not implemented yet (if ever):
    1   - Destination Unreachable
    2   - Packet Too Big
    3   - Time Exceeded
    4   - Parameter Problem
    Any other numbers not listed in the first list...
*/

struct ping_pkt {
    LIST_ENTRY(ping_pkt) pkt_list;
    struct in6_addr ip;
    uint8 *data;
    int data_sz;
    uint16 icmp_seq;
    uint64 usec;
};

LIST_HEAD(ping_pkt_list, ping_pkt);

static struct ping_pkt_list pings = LIST_HEAD_INITIALIZER(0);

static void icmp6_default_echo_cb(const struct in6_addr *ip, uint16 seq,
                                  uint64 delta_us, uint8 hlim,
                                  const uint8 *data, int data_sz) {
    char ipstr[INET6_ADDRSTRLEN];

    printf("%d bytes from %s, icmp_seq=%d hlim=%d time=%.3f ms\n", data_sz,
           inet_ntop(AF_INET6, ip, ipstr, INET6_ADDRSTRLEN), seq, hlim,
           delta_us / 1000.0);
}

/* The default echo (ping6) callback */
net6_echo_cb net_icmp6_echo_cb = icmp6_default_echo_cb;

/* Handle Echo Reply (ICMPv6 type 129) packets */
static void net_icmp6_input_129(netif_t *net, ipv6_hdr_t *ip, icmp6_hdr_t *icmp,
                                const uint8 *d, int s) {
    uint64 tmr;
    struct ping_pkt *ping;
    uint16 seq;

    tmr = timer_us_gettime64();
    seq = (d[7] | (d[6] << 8));

    LIST_FOREACH(ping, &pings, pkt_list) {
        if(ping->icmp_seq == seq) {
            if(net_icmp6_echo_cb) {
                net_icmp6_echo_cb(&ping->ip, seq, tmr - ping->usec,
                                  ip->hop_limit, d, s);
            }

            LIST_REMOVE(ping, pkt_list);
            free(ping->data);
            free(ping);

            return;
        }
    }
}

/* Handle Echo (ICMPv6 type 128) packets */
static void net_icmp6_input_128(netif_t *net, ipv6_hdr_t *ip, icmp6_hdr_t *icmp,
                                const uint8 *d, int s) {
    uint16 cs;
    struct in6_addr src, dst;

    memcpy(&src, &ip->dst_addr, sizeof(struct in6_addr));
    memcpy(&dst, &ip->src_addr, sizeof(struct in6_addr));

    /* Set type to echo reply */
    icmp->type = ICMP6_MESSAGE_ECHO_REPLY;

    /* Invert the addresses and fix the hop limit */
    if(IN6_IS_ADDR_MC_LINKLOCAL(&src)) {
        src = net->ip6_lladdr;
    }

    ip->src_addr = src;
    ip->dst_addr = dst;

    if(net->hop_limit) {
        ip->hop_limit = net->hop_limit;
    }
    else {
        ip->hop_limit = 255;
    }

    /* Recompute the ICMP header checksum */
    icmp->checksum = 0;
    cs = net_ipv6_checksum_pseudo(&src, &dst, ntohs(ip->length), IPV6_HDR_ICMP);
    icmp->checksum = net_ipv4_checksum((uint8 *)icmp, ntohs(ip->length), cs);

    /* Send the result away */
    net_ipv6_send_packet(net, ip, d, s);
}

static void dupdet(netif_t *net, const struct in6_addr *ip) {
    struct in6_addr dst = *ip;

    /* Send to the solicited nodes multicast group for the specified addr */
    dst.s6_addr[0] = 0xFF;
    dst.s6_addr[1] = 0x02;
    dst.__s6_addr.__s6_addr16[1] = 0x0000;
    dst.__s6_addr.__s6_addr16[2] = 0x0000;
    dst.__s6_addr.__s6_addr16[3] = 0x0000;
    dst.__s6_addr.__s6_addr16[4] = 0x0000;
    dst.s6_addr[10] = 0x00;
    dst.s6_addr[11] = 0x01;
    dst.s6_addr[12] = 0xFF;

    net_icmp6_send_nsol(net, &dst, ip, 1);
}

/* Handle Router Advertisement (ICMPv6 type 134) packets */
static void net_icmp6_input_134(netif_t *net, ipv6_hdr_t *ip, icmp6_hdr_t *icmp,
                                const uint8 *d, int s) {
    icmp6_router_adv_t *pkt = (icmp6_router_adv_t *)icmp;
    struct in6_addr src;
    uint16 len = ntohs(ip->length);
    int pos = 0;

    /* Ignore obviously bad packets */
    if(len <= sizeof(icmp6_router_adv_t)) {
        return;
    }

    /* Make sure that the source address is link-local */
    memcpy(&src, &ip->src_addr, sizeof(struct in6_addr));
    if(!IN6_IS_ADDR_LINKLOCAL(&src)) {
        return;
    }

    /* Make sure the hop limit and code are correct */
    if(ip->hop_limit != 255 || pkt->code) {
        return;
    }

    /* If this router isn't default, we don't care about it at all. */
    if(!pkt->router_lifetime) {
        return;
    }

    /* If this router isn't the first one, then ignore it */
    if(net->ip6_gateway.s6_addr[0]) {
        return;
    }

    /* Parse the options that are in the advertisement */
    while(pos + sizeof(icmp6_router_adv_t) < len) {
        /* Make sure the option is at least sane */
        if(!pkt->options[pos + 1]) {
            return;
        }

        switch(pkt->options[pos]) {
            case NDP_OPT_MTU:
                net->mtu6 = (pkt->options[pos + 4] << 24) |
                    (pkt->options[pos + 5] << 16) |
                    (pkt->options[pos + 6] << 8) | (pkt->options[pos + 7]);
                break;

            case NDP_OPT_PREFIX_INFO:
            {
                icmp6_ndp_prefix_t *pfx =
                    (icmp6_ndp_prefix_t *)&pkt->options[pos];
                void *tmp;
                struct in6_addr addr;
                int i;

                /* Make sure the option is of the right length */
                if(pfx->length != 4) {
                    break;
                }

                /* Make sure the prefix is of an appropriate length */
                if(pfx->prefix_length != 64) {
                    break;
                }

                /* Make sure we have the on-link and autonomous flags set */
                if((pfx->flags & 0xC0) != 0xC0) {
                    break;
                }

                /* Make sure we don't already have the prefix. */
                memcpy(&addr, &pfx->prefix, 8);
                memcpy(&addr.s6_addr[8], &net->ip6_lladdr.s6_addr[8], 8);

                for(i = 0; i < net->ip6_addr_count; ++i) {
                    if(!memcmp(&addr, &net->ip6_addrs[i],
                               sizeof(struct in6_addr))) {
                        goto out;
                    }
                }

                /* TODO: Handle preferred/valid lifetimes properly */
                /* Add the new address to our list */
                tmp = realloc(net->ip6_addrs, (net->ip6_addr_count + 1) *
                              sizeof(struct in6_addr));
                if(!tmp) {
                    break;
                }

                net->ip6_addrs = (struct in6_addr *)tmp;
                memcpy(&net->ip6_addrs[net->ip6_addr_count], &addr,
                       sizeof(struct in6_addr));

                ++net->ip6_addr_count;

                dupdet(net, &addr);
out:
                break;
            }

            case NDP_OPT_SOURCE_LINK_ADDR:
            {
                icmp6_nsol_lladdr_t *ll =
                    (icmp6_nsol_lladdr_t *)&pkt->options[pos];

                /* Make sure the length is sane */
                if(ll->length != 1) {
                    break;
                }

                net_ndp_insert(net, ll->mac, &src, 1);
                break;
            }
        }

        pos += pkt->options[pos + 1] << 3;
    }

    memcpy(&net->ip6_gateway, &src, sizeof(struct in6_addr));
    net->hop_limit = pkt->cur_hop_limit;
}

/* Handle Neighbor Solicitation (ICMPv6 type 135) packets */
static void net_icmp6_input_135(netif_t *net, ipv6_hdr_t *ip, icmp6_hdr_t *icmp,
                                const uint8 *d, int s) {
    icmp6_neighbor_sol_t *pkt = (icmp6_neighbor_sol_t *)icmp;
    icmp6_nsol_lladdr_t *ll;
    uint16 len = ntohs(ip->length);
    struct in6_addr target, src;
    int sol = 1, pos = 0, i;

    /* Ignore obviously bad packets */
    if(len < sizeof(icmp6_neighbor_sol_t)) {
        return;
    }

    /* Ignore neighbor solicitations for multicast addresses and ones that have
       a hop limit that's wrong */
    memcpy(&target, &pkt->target, sizeof(struct in6_addr));

    if(ip->hop_limit != 255 || IN6_IS_ADDR_MULTICAST(&target) || icmp->code) {
        return;
    }

    /* Make sure they're asking about this machine. */
    if(!memcmp(&target, &net->ip6_lladdr, sizeof(struct in6_addr))) {
        goto cont;
    }

    /* Check all non link-local prefixes we have */
    for(i = 0; i < net->ip6_addr_count; ++i) {
        if(!memcmp(&target, &net->ip6_addrs[i], sizeof(struct in6_addr))) {
            goto cont;
        }
    }

    /* If we get here, we haven't found it, so assume its not for us */
    return;

cont:
    /* Copy out the source address */
    memcpy(&src, &ip->src_addr, sizeof(struct in6_addr));

    if(IN6_IS_ADDR_UNSPECIFIED(&src)) {
        src = in6addr_linklocal_allnodes;
        sol = 0;
    }
    /* If its not unspecified, see if there's a link-layer address option */
    else {
        while(pos + sizeof(icmp6_neighbor_sol_t) < len) {
            /* Make sure the option is at least sane */
            if(!pkt->options[pos + 1]) {
                return;
            }

            if(pkt->options[pos] == NDP_OPT_SOURCE_LINK_ADDR) {
                ll = (icmp6_nsol_lladdr_t *)&pkt->options[pos];
                net_ndp_insert(net, ll->mac, &src, 1);
            }

            pos += pkt->options[pos + 1] << 3;
        }            
    }

    /* Send the neighbor advertisement */
    net_icmp6_send_nadv(net, &src, &target, sol);
}

/* Handle Neighbor Advertisement (ICMPv6 type 136) packets */
static void net_icmp6_input_136(netif_t *net, ipv6_hdr_t *ip, icmp6_hdr_t *icmp,
                                const uint8 *d, int s) {
    icmp6_neighbor_adv_t *pkt = (icmp6_neighbor_adv_t *)icmp;
    icmp6_nsol_lladdr_t *lladdr = (icmp6_nsol_lladdr_t *)pkt->options;
    uint16 len = ntohs(ip->length);
    struct in6_addr target, dest;
    uint32 flags;

    /* Ignore obviously bad packets */
    if(len < sizeof(icmp6_neighbor_adv_t)) {
        return;
    }

    /* Silently drop packets that don't have the neighbor's lower level addr */
    if(len < sizeof(icmp6_neighbor_adv_t) + sizeof(icmp6_nsol_lladdr_t)) {
        return;
    }

    /* If the option isn't the target link layer address option, ignore it */
    if(lladdr->type != NDP_OPT_TARGET_LINK_ADDR || lladdr->length != 1) {
        return;
    }

    /* Make sure the hop limit is right and this isn't a multicast addr */
    memcpy(&target, &pkt->target, sizeof(struct in6_addr));
    if(ip->hop_limit != 255 || IN6_IS_ADDR_MULTICAST(&target)) {
        return;
    }

    /* Make sure if the destination is multicast, the solicited flag is zero */
    memcpy(&dest, &ip->dst_addr, sizeof(struct in6_addr));
    flags = pkt->flags;
    if(IN6_IS_ADDR_MULTICAST(&dest) && (flags & 0x02)) {
        return;
    }

    net_ndp_insert(net, lladdr->mac, &target, !(flags & 0x02));
}

static void net_icmp6_input_137(netif_t *net, ipv6_hdr_t *ip, icmp6_hdr_t *icmp,
                                const uint8 *d, int s) {
    icmp6_redirect_t *pkt = (icmp6_redirect_t *)icmp;
    icmp6_nsol_lladdr_t *ll = (icmp6_nsol_lladdr_t *)pkt->options;
    struct in6_addr target, dest;
    char str[INET6_ADDRSTRLEN], str2[INET6_ADDRSTRLEN];
    int len = ntohs(ip->length), pos = 0;

    /* Reject obviously bad packets */
    if(len < sizeof(icmp6_redirect_t)) {
        return;
    }

    /* Copy out the addresses */
    memcpy(&target, &pkt->target, sizeof(struct in6_addr));
    memcpy(&dest, &pkt->dest, sizeof(struct in6_addr));

    dbglog(DBG_KDEBUG, "net_icmp6: Redirect:\n"
           "%s -> %s\n", inet_ntop(AF_INET6, &dest, str, INET6_ADDRSTRLEN),
           inet_ntop(AF_INET6, &target, str2, INET6_ADDRSTRLEN));

    /* Check the target and destination for equality -- if they're equal, update
       the NDP entry and move on. */
    if(!memcmp(&target, &dest, sizeof(struct in6_addr))) {
        while(pos + sizeof(icmp6_redirect_t) < len) {
            /* Make sure the option is at least sane */
            if(!pkt->options[pos + 1]) {
                return;
            }

            if(pkt->options[pos] == NDP_OPT_TARGET_LINK_ADDR) {
                ll = (icmp6_nsol_lladdr_t *)&pkt->options[pos];
                net_ndp_insert(net, ll->mac, &target, 0);
            }

            pos += pkt->options[pos + 1] << 3;
        }
    }
}

int net_icmp6_input(netif_t *net, ipv6_hdr_t *ip, const uint8 *d, int s) {
    icmp6_hdr_t *icmp;
    uint16 cs = net_ipv6_checksum_pseudo(&ip->src_addr, &ip->dst_addr,
                                         ntohs(ip->length), IPV6_HDR_ICMP);
    int i;

    /* Find the ICMPv6 header */
    icmp = (icmp6_hdr_t *)d;

    /* Check the checksum */
    i = net_ipv4_checksum(d, s, cs);

    if(i) {
        dbglog(DBG_KDEBUG, "net_icmp6: icmp with invalid checksum\n");
        return -1;
    }

    switch(icmp->type) {
        case ICMP6_MESSAGE_ECHO:
            net_icmp6_input_128(net, ip, icmp, d, s);
            break;

        case ICMP6_MESSAGE_ECHO_REPLY:
            net_icmp6_input_129(net, ip, icmp, d, s);
            break;

        case ICMP6_ROUTER_ADVERTISEMENT:
            net_icmp6_input_134(net, ip, icmp, d, s);
            break;

        case ICMP6_NEIGHBOR_SOLICITATION:
            net_icmp6_input_135(net, ip, icmp, d, s);
            break;

        case ICMP6_NEIGHBOR_ADVERTISEMENT:
            net_icmp6_input_136(net, ip, icmp, d, s);
            break;

        case ICMP6_REDIRECT:
            net_icmp6_input_137(net, ip, icmp, d, s);
            break;

        default:
            dbglog(DBG_KDEBUG, "net_icmp6: unknown icmp6 type: %d\n",
                   icmp->type);
    }

    return 0;
}

/* Send an ICMPv6 Echo (PING6) packet to the specified device */
int net_icmp6_send_echo(netif_t *net, const struct in6_addr *dst, uint16 ident,
                        uint16 seq, const uint8 *data, int size) {
    icmp6_echo_hdr_t *echo;
    struct ping_pkt *newping;
    uint8 databuf[sizeof(icmp6_hdr_t) + size + 4];
    struct in6_addr src;
    uint16 cs;

    if(!net) {
        if(!(net = net_default_dev)) {
            return -1;
        }
    }

    /* If we're sending to the loopback, set that as our source too */
    if(IN6_IS_ADDR_LOOPBACK(dst)) {
        src = in6addr_loopback;
    }
    else if(IN6_IS_ADDR_LINKLOCAL(dst) || IN6_IS_ADDR_MC_LINKLOCAL(dst)) {
        src = net->ip6_lladdr;
    }
    else if(net->ip6_addr_count) {
        src = net->ip6_addrs[0];
    }
    else {
        return -1;
    }

    echo = (icmp6_echo_hdr_t *)databuf;

    /* Fill in the ICMP Header */
    echo->type = ICMP6_MESSAGE_ECHO;
    echo->code = 0;
    echo->checksum = 0;
    echo->ident = htons(ident);
    echo->seq = htons(seq);
    memcpy(databuf + sizeof(icmp6_echo_hdr_t), data, size);

    /* Compute the ICMP Checksum */
    cs = net_ipv6_checksum_pseudo(&src, dst, sizeof(icmp6_echo_hdr_t) + size,
                                  IPV6_HDR_ICMP);
    echo->checksum = net_ipv4_checksum(databuf, sizeof(icmp6_echo_hdr_t) + size,
                                       cs);

    newping = (struct ping_pkt *) malloc(sizeof(struct ping_pkt));
    newping->data = (uint8 *)malloc(size);
    newping->data_sz = size;
    newping->icmp_seq = seq;
    memcpy(newping->data, data, size);
    newping->ip = *dst;
    LIST_INSERT_HEAD(&pings, newping, pkt_list);

    newping->usec = timer_us_gettime64();
    return net_ipv6_send(net, databuf, sizeof(icmp6_echo_hdr_t) + size, 0,
                         IPV6_HDR_ICMP, &src, dst);
}

/* Send a Neighbor Solicitation packet on the specified device */
int net_icmp6_send_nsol(netif_t *net, const struct in6_addr *dst,
                        const struct in6_addr *target, int dupdet) {
    icmp6_neighbor_sol_t *pkt;
    icmp6_nsol_lladdr_t *ll;
    uint8 databuf[sizeof(icmp6_neighbor_sol_t) + sizeof(icmp6_nsol_lladdr_t)];
    struct in6_addr src;
    int size = sizeof(icmp6_neighbor_sol_t);
    uint16 cs;

    if(!net) {
        if(!(net = net_default_dev)) {
            return -1;
        }
    }

    /* If we don't have a link-local address and we're not doing duplicate
       detection, bail out now. */
    if(!net->ip6_lladdr.__s6_addr.__s6_addr8[0] && !dupdet) {
        return -1;
    }

    pkt = (icmp6_neighbor_sol_t *)databuf;

    /* Fill in the ICMP Header */
    pkt->type = ICMP6_NEIGHBOR_SOLICITATION;
    pkt->code = 0;
    pkt->checksum = 0;
    pkt->reserved = 0;
    memcpy(&pkt->target, target, sizeof(struct in6_addr));

    /* If we're doing duplicate detection, send this on the unspecified address,
       otherwise, on the link-local address. Also, if we're not doing duplicate
       detection, add a neighbor solicitation link-layer address option. */
    if(dupdet) {
        src = in6addr_any;
    }
    else {
        if(IN6_IS_ADDR_LINKLOCAL(target)) {
            src = net->ip6_lladdr;
        }
        else if(net->ip6_addr_count) {
            src = net->ip6_addrs[0];
        }
        else {
            return -1;
        }

        ll = (icmp6_nsol_lladdr_t *)(databuf + sizeof(icmp6_neighbor_sol_t));
        ll->type = NDP_OPT_SOURCE_LINK_ADDR;
        ll->length = 1;
        memcpy(ll->mac, net->mac_addr, 6);
        size += sizeof(icmp6_nsol_lladdr_t);
    }

    /* Compute the ICMP Checksum */
    cs = net_ipv6_checksum_pseudo(&src, dst, size, IPV6_HDR_ICMP);
    pkt->checksum = net_ipv4_checksum(databuf, size, cs);

    return net_ipv6_send(net, databuf, size, 255, IPV6_HDR_ICMP, &src, dst);
}

/* Send a Neighbor Advertisement packet on the specified device */
int net_icmp6_send_nadv(netif_t *net, const struct in6_addr *dst,
                        const struct in6_addr *target, int sol) {
    icmp6_neighbor_adv_t *pkt;
    icmp6_nsol_lladdr_t *ll;
    uint8 databuf[sizeof(icmp6_neighbor_adv_t) + sizeof(icmp6_nsol_lladdr_t)];
    struct in6_addr src;
    int size = sizeof(icmp6_neighbor_adv_t) + sizeof(icmp6_nsol_lladdr_t);
    uint16 cs;

    if(!net) {
        if(!(net = net_default_dev)) {
            return -1;
        }
    }

    pkt = (icmp6_neighbor_adv_t *)databuf;

    /* Fill in the ICMP Header */
    pkt->type = ICMP6_NEIGHBOR_ADVERTISEMENT;
    pkt->code = 0;
    pkt->checksum = 0;
    pkt->reserved[0] = pkt->reserved[1] = pkt->reserved[2] = 0;
    pkt->flags = 0x40;    /* Set the override flag */
    memcpy(&pkt->target, target, sizeof(struct in6_addr));

    /* If this is a solicited request, handle it a little bit differently. */
    if(sol) {
        pkt->flags |= 0x20; /* Set the solicited flag */
    }

    memcpy(&src, target, sizeof(struct in6_addr));

    ll = (icmp6_nsol_lladdr_t *)(databuf + sizeof(icmp6_neighbor_adv_t));
    ll->type = NDP_OPT_TARGET_LINK_ADDR;
    ll->length = 1;
    memcpy(ll->mac, net->mac_addr, 6);

    /* Compute the ICMP Checksum */
    cs = net_ipv6_checksum_pseudo(&src, dst, size, IPV6_HDR_ICMP);
    pkt->checksum = net_ipv4_checksum(databuf, size, cs);

    return net_ipv6_send(net, databuf, size, 255, IPV6_HDR_ICMP, &src, dst);
}

/* Send a Router Solicitation request on the specified interface */
int net_icmp6_send_rsol(netif_t *net) {
    icmp6_router_sol_t *pkt;
    icmp6_nsol_lladdr_t *ll;
    uint8 databuf[sizeof(icmp6_router_sol_t) + sizeof(icmp6_nsol_lladdr_t)];
    struct in6_addr src;
    int size = sizeof(icmp6_router_sol_t) + sizeof(icmp6_nsol_lladdr_t);
    uint16 cs;

    if(!net) {
        if(!(net = net_default_dev)) {
            return -1;
        }
    }

    pkt = (icmp6_router_sol_t *)databuf;

    /* Fill in the ICMP Header */
    pkt->type = ICMP6_ROUTER_SOLICITATION;
    pkt->code = 0;
    pkt->checksum = 0;
    pkt->reserved = 0;

    src = net->ip6_lladdr;

    /* If we're working on an unspecified address, then we don't include the
       source link layer address option */
    if(IN6_IS_ADDR_UNSPECIFIED(&src)) {
        size -= sizeof(icmp6_nsol_lladdr_t);
    }
    else {
        ll = (icmp6_nsol_lladdr_t *)pkt->options;
        ll->type = NDP_OPT_SOURCE_LINK_ADDR;
        ll->length = 1;
        memcpy(ll->mac, net->mac_addr, 6);
    }

    /* Compute the ICMP Checksum */
    cs = net_ipv6_checksum_pseudo(&src, &in6addr_linklocal_allrouters, size,
                                  IPV6_HDR_ICMP);
    pkt->checksum = net_ipv4_checksum(databuf, size, cs);

    return net_ipv6_send(net, databuf, size, 255, IPV6_HDR_ICMP, &src,
                         &in6addr_linklocal_allrouters);
}