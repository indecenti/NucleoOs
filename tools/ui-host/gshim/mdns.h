// ui-host gallery shim (IDF mdns): declarations only; a query never finds anything on the host.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#define MDNS_TYPE_PTR 0x000C
#define MDNS_TYPE_A 0x0001
typedef struct mdns_search_once_s mdns_search_once_t;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { union { esp_ip4_addr_t ip4; } u_addr; int type; } esp_ip_addr_t;
typedef struct mdns_ip_addr_s { esp_ip_addr_t addr; struct mdns_ip_addr_s *next; } mdns_ip_addr_t;
typedef struct mdns_result_s { struct mdns_result_s *next; void *esp_netif; int tcpip_if, ip_protocol; char *instance_name, *service_type, *proto, *hostname;
                               uint16_t port; void *txt; uint8_t *txt_value_len; size_t txt_count; mdns_ip_addr_t *addr; } mdns_result_t;
#ifdef __cplusplus
extern "C" {
#endif
mdns_search_once_t *mdns_query_async_new(const char *name, const char *service, const char *proto, uint16_t type, uint32_t timeout, size_t max, void *notifier);
bool mdns_query_async_get_results(mdns_search_once_t *s, uint32_t timeout, mdns_result_t **res, uint8_t *n);
esp_err_t mdns_query_async_delete(mdns_search_once_t *s);
void mdns_query_results_free(mdns_result_t *r);
esp_err_t mdns_query_ptr(const char *service, const char *proto, uint32_t timeout, size_t max, mdns_result_t **res);
#ifdef __cplusplus
}
#endif
#define ESP_IPADDR_TYPE_V4 0
#ifndef IPSTR
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(a) (int)((a)->addr & 0xff), (int)(((a)->addr >> 8) & 0xff), (int)(((a)->addr >> 16) & 0xff), (int)(((a)->addr >> 24) & 0xff)
#endif
