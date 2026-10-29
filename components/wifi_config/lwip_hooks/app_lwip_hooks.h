#pragma once

/*
 * Additional lwIP hooks of this application, injected into the lwIP component through
 * ESP_IDF_LWIP_HOOK_FILENAME (ESP-IDF "Customized lwIP Hooks", see the top-level CMakeLists.txt).
 *
 * LWIP_HOOK_IP4_INPUT: strong host model for IPv4. lwIP accepts a unicast packet addressed to ANY
 * local address on ANY interface (ip4_input() walks all netifs), so a host on the home LAN could
 * reach 192.168.4.1 (the configuration portal) through the station interface. The hook drops
 * unicast packets whose destination is the address of another interface than the one they
 * arrived on. Implemented in components/wifi_config/ip4_guard.c.
 */

struct pbuf;
struct netif;

int app_ip4_input_guard(struct pbuf *p, struct netif *inp);

#define LWIP_HOOK_IP4_INPUT(p, inp) app_ip4_input_guard((p), (inp))
