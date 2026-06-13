/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_mcp_client_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cap_mcp_server.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/ip_addr.h"
#include "mdns.h"

static const char *TAG = "mcp_discover_core";

static const char *cap_mcp_find_txt_value(const mdns_result_t *result, const char *key)
{
    size_t i;

    if (!result || !key) {
        return NULL;
    }

    for (i = 0; i < result->txt_count; i++) {
        if (result->txt[i].key && strcmp(result->txt[i].key, key) == 0) {
            return result->txt[i].value;
        }
    }

    return NULL;
}

static const char *cap_mcp_pick_ip_string(const mdns_result_t *result,
                                          char *buf,
                                          size_t buf_size)
{
    const mdns_ip_addr_t *addr = NULL;

    if (!buf || buf_size == 0) {
        return NULL;
    }
    buf[0] = '\0';

    if (!result || !result->addr) {
        return NULL;
    }

    addr = result->addr;
    while (addr) {
        if (ipaddr_ntoa_r((const ip_addr_t *)&addr->addr, buf, buf_size)) {
            return buf;
        }
        addr = addr->next;
    }

    return NULL;
}

static esp_err_t cap_mcp_append_device(cJSON *devices,
                                       const char *instance,
                                       const char *hostname,
                                       const char *ip,
                                       const char *url_host,
                                       uint16_t port,
                                       const char *endpoint)
{
    cJSON *device = NULL;
    char server_url[320];
    char url[384];
    const char *host_for_url = NULL;

    if (!devices || !hostname || !hostname[0] || !endpoint || !endpoint[0] || port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    host_for_url = (url_host && url_host[0]) ? url_host : ((ip && ip[0]) ? ip : hostname);
    snprintf(server_url, sizeof(server_url), "http://%s:%u", host_for_url, (unsigned int)port);
    snprintf(url, sizeof(url), "%s/%s", server_url, endpoint);

    device = cJSON_CreateObject();
    if (!device) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(device, "instance", (instance && instance[0]) ? instance : "(unknown)");
    cJSON_AddStringToObject(device, "hostname", hostname);
    cJSON_AddStringToObject(device, "ip", (ip && ip[0]) ? ip : "(unresolved)");
    cJSON_AddNumberToObject(device, "port", port);
    cJSON_AddStringToObject(device, "endpoint", endpoint);
    cJSON_AddStringToObject(device, "server_url", server_url);
    cJSON_AddStringToObject(device, "url", url);
    cJSON_AddItemToArray(devices, device);
    return ESP_OK;
}

static bool cap_mcp_devices_has_match(const cJSON *devices,
                                      const char *hostname,
                                      uint16_t port,
                                      const char *endpoint)
{
    const cJSON *device = NULL;

    if (!cJSON_IsArray((cJSON *)devices) || !hostname || !hostname[0] || !endpoint || !endpoint[0]) {
        return false;
    }

    cJSON_ArrayForEach(device, (cJSON *)devices) {
        const cJSON *hostname_item = cJSON_GetObjectItemCaseSensitive((cJSON *)device, "hostname");
        const cJSON *port_item = cJSON_GetObjectItemCaseSensitive((cJSON *)device, "port");
        const cJSON *endpoint_item = cJSON_GetObjectItemCaseSensitive((cJSON *)device, "endpoint");

        if (!cJSON_IsString((cJSON *)hostname_item) || !cJSON_IsNumber((cJSON *)port_item) ||
                !cJSON_IsString((cJSON *)endpoint_item)) {
            continue;
        }
        if (strcmp(hostname_item->valuestring, hostname) == 0 &&
                (uint16_t)port_item->valueint == port &&
                strcmp(endpoint_item->valuestring, endpoint) == 0) {
            return true;
        }
    }

    return false;
}

static const char *cap_mcp_get_self_ip_string(char *buf, size_t buf_size)
{
    esp_netif_t *netif = NULL;
    esp_netif_ip_info_t ip_info = {0};

    if (!buf || buf_size == 0) {
        return NULL;
    }
    buf[0] = '\0';

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        return NULL;
    }
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return NULL;
    }

    snprintf(buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
    return buf;
}

static esp_err_t cap_mcp_append_self_device_if_needed(cJSON *devices, size_t *found)
{
    cap_mcp_server_config_t config = {0};
    bool started = false;
    char hostname_local[96];
    char ip_buf[64];
    const char *hostname = NULL;
    const char *instance = NULL;
    const char *endpoint = NULL;
    const char *ip = NULL;
    esp_err_t err;

    if (!devices || !found) {
        return ESP_ERR_INVALID_ARG;
    }

    err = cap_mcp_server_get_config(&config, &started);
    if (err != ESP_OK || !started) {
        return err;
    }

    hostname = (config.hostname && config.hostname[0]) ? config.hostname : CAP_MCP_SERVER_DEFAULT_HOSTNAME;
    instance = (config.instance_name && config.instance_name[0]) ? config.instance_name : CAP_MCP_SERVER_DEFAULT_INSTANCE;
    endpoint = (config.endpoint && config.endpoint[0]) ? config.endpoint : CAP_MCP_DEFAULT_ENDPOINT;
    ip = cap_mcp_get_self_ip_string(ip_buf, sizeof(ip_buf));

    if (cap_mcp_devices_has_match(devices, hostname, config.server_port, endpoint)) {
        return ESP_OK;
    }

    snprintf(hostname_local, sizeof(hostname_local), "%s.local", hostname);
    err = cap_mcp_append_device(devices,
                                instance,
                                hostname,
                                ip,
                                ip ? ip : hostname_local,
                                config.server_port,
                                endpoint);
    if (err == ESP_OK) {
        (*found)++;
    }
    return err;
}

static esp_err_t cap_mcp_parse_discover_options(const char *input_json,
                                                int *timeout_ms,
                                                bool *include_self)
{
    cJSON *input = NULL;

    if (!timeout_ms || !include_self) {
        return ESP_ERR_INVALID_ARG;
    }
    *timeout_ms = CAP_MCP_DISCOVER_TIMEOUT_MS;
    *include_self = true;

    if (!input_json || !input_json[0]) {
        return ESP_OK;
    }

    input = cJSON_Parse(input_json);
    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *timeout_item = cJSON_GetObjectItem(input, "timeout_ms");
    if (cJSON_IsNumber(timeout_item) && timeout_item->valueint > 0) {
        *timeout_ms = timeout_item->valueint;
    }

    cJSON *self_item = cJSON_GetObjectItem(input, "include_self");
    if (cJSON_IsBool(self_item)) {
        *include_self = cJSON_IsTrue(self_item);
    }

    cJSON_Delete(input);
    return ESP_OK;
}

esp_err_t cap_mcp_discover_services(const char *input_json, cJSON **result_out)
{
    int timeout_ms = CAP_MCP_DISCOVER_TIMEOUT_MS;
    bool include_self = true;
    mdns_result_t *results = NULL;
    cJSON *root = NULL;
    cJSON *devices = NULL;
    size_t found = 0;
    esp_err_t err;

    if (!result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;

    err = cap_mcp_parse_discover_options(input_json, &timeout_ms, &include_self);
    if (err != ESP_OK) {
        return err;
    }

    err = mdns_query_ptr(CAP_MCP_MDNS_SERVICE_TYPE,
                         CAP_MCP_MDNS_SERVICE_PROTO,
                         timeout_ms,
                         20,
                         &results);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_query_ptr failed: %s", esp_err_to_name(err));
        return err;
    }

    root = cJSON_CreateObject();
    devices = cJSON_CreateArray();
    if (!root || !devices) {
        mdns_query_results_free(results);
        cJSON_Delete(root);
        cJSON_Delete(devices);
        return ESP_ERR_NO_MEM;
    }

    for (mdns_result_t *result = results; result; result = result->next) {
        char ip_buf[64];
        const char *ip;
        const char *endpoint;
        const char *hostname;
        const char *instance;
        esp_err_t append_err;

        if (!include_self && result->hostname && strcmp(result->hostname, CAP_MCP_SERVER_DEFAULT_HOSTNAME) == 0) {
            continue;
        }

        ip = cap_mcp_pick_ip_string(result, ip_buf, sizeof(ip_buf));
        endpoint = cap_mcp_find_txt_value(result, "endpoint");
        if (!endpoint || !endpoint[0]) {
            endpoint = CAP_MCP_DEFAULT_ENDPOINT;
        }

        hostname = (result->hostname && result->hostname[0]) ? result->hostname : "(unknown)";
        instance = (result->instance_name && result->instance_name[0]) ?
                   result->instance_name : "(unknown)";

        append_err = cap_mcp_append_device(devices,
                                           instance,
                                           hostname,
                                           ip,
                                           NULL,
                                           result->port,
                                           endpoint);
        if (append_err != ESP_OK) {
            mdns_query_results_free(results);
            cJSON_Delete(root);
            return append_err;
        }
        found++;
    }

    mdns_query_results_free(results);
    if (include_self) {
        err = cap_mcp_append_self_device_if_needed(devices, &found);
        if (err != ESP_OK) {
            cJSON_Delete(root);
            return err;
        }
    }
    cJSON_AddNumberToObject(root, "count", (double)found);
    cJSON_AddItemToObject(root, "devices", devices);
    *result_out = root;
    return ESP_OK;
}
