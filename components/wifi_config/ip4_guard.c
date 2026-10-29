/* IPv4 strong host model, called by lwIP (tcpip thread) for every received IPv4 packet. */
#include "lwip/def.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip4.h"
#include "lwip/stats.h"

int app_ip4_input_guard(struct pbuf *p, struct netif *inp)
{
    if (p == NULL || inp == NULL || p->len < IP_HLEN) {
        return 0; /* malformed packets are rejected by ip4_input() itself */
    }
    const struct ip_hdr *iph = (const struct ip_hdr *)p->payload;
    ip4_addr_t dest;
    ip4_addr_copy(dest, iph->dest);
    if (ip4_addr_isany_val(dest) || ip4_addr_ismulticast(&dest) || ip4_addr_isbroadcast(&dest, inp) ||
        ip4_addr_cmp(&dest, netif_ip4_addr(inp))) {
        return 0;
    }
    struct netif *n;
    NETIF_FOREACH(n) {
        if (n != inp && !ip4_addr_isany_val(*netif_ip4_addr(n)) && ip4_addr_cmp(&dest, netif_ip4_addr(n))) {
            /* addressed to another interface of this device (e.g. the SoftAP address arriving
             * from the home LAN): not for us on this interface */
            IP_STATS_INC(ip.drop);
            pbuf_free(p);
            return 1;
        }
    }
    return 0;
}
