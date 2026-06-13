/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "cap_mcp_client_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "mcp_client_core";

#define CAP_MCP_REQUEST_ID_CALL    1
#define CAP_MCP_REQUEST_ID_LIST    2
#define CAP_MCP_RESPONSE_BUF_SIZE  (8 * 1024)
#define CAP_MCP_HTTP_TIMEOUT_MS    20000

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} cap_mcp_buf_t;

static esp_err_t cap_mcp_http_event_handler(esp_http_client_event_t *event)
{
    cap_mcp_buf_t *buf = (cap_mcp_buf_t *)event->user_data;
    size_t needed;

    if (!buf || event->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }

    needed = buf->len + event->data_len;
    if (needed < buf->cap) {
        memcpy(buf->data + buf->len, event->data, event->data_len);
        buf->len += event->data_len;
        buf->data[buf->len] = '\0';
    }
    return ESP_OK;
}

static void cap_mcp_build_full_url(const char *server_url,
                                   const char *endpoint,
                                   char *full_url,
                                   size_t full_url_size)
{
    size_t length = strnlen(server_url, 256);

    if (length == 0 || length >= full_url_size) {
        full_url[0] = '\0';
        return;
    }

    while (length > 0 && server_url[length - 1] == '/') {
        length--;
    }
    memcpy(full_url, server_url, length);
    full_url[length] = '\0';

    if (endpoint && endpoint[0] != '\0') {
        const char *trimmed = endpoint[0] == '/' ? endpoint + 1 : endpoint;

        if (*trimmed) {
            snprintf(full_url + length, full_url_size - length, "/%s", trimmed);
        }
    }
}

static esp_err_t cap_mcp_http_post(const char *url,
                                   const char *body,
                                   cap_mcp_buf_t *buf)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = cap_mcp_http_event_handler,
        .user_data = buf,
        .timeout_ms = CAP_MCP_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = NULL;
    esp_err_t err;
    int status;

    client = esp_http_client_init(&config);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status %d", status);
        return ESP_ERR_HTTP_CONNECT;
    }
    return ESP_OK;
}

static esp_err_t cap_mcp_parse_common_input(const char *input_json,
                                            char *server_url_buf,
                                            size_t server_url_buf_size,
                                            char *endpoint_buf,
                                            size_t endpoint_buf_size,
                                            char *cursor_buf,
                                            size_t cursor_buf_size,
                                            char *tool_name_buf,
                                            size_t tool_name_buf_size,
                                            cJSON **arguments_out)
{
    cJSON *input = cJSON_Parse(input_json);

    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *server_url_item = cJSON_GetObjectItem(input, "server_url");
    if (!cJSON_IsString(server_url_item) || !server_url_item->valuestring[0]) {
        cJSON_Delete(input);
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(server_url_buf, server_url_item->valuestring, server_url_buf_size);

    if (endpoint_buf && endpoint_buf_size > 0) {
        const char *endpoint = CAP_MCP_DEFAULT_ENDPOINT;
        cJSON *endpoint_item = cJSON_GetObjectItem(input, "endpoint");
        if (cJSON_IsString(endpoint_item) && endpoint_item->valuestring[0]) {
            endpoint = endpoint_item->valuestring;
        }
        strlcpy(endpoint_buf, endpoint, endpoint_buf_size);
    }

    if (cursor_buf && cursor_buf_size > 0) {
        cJSON *cursor_item = cJSON_GetObjectItem(input, "cursor");

        cursor_buf[0] = '\0';
        if (cJSON_IsString(cursor_item) && cursor_item->valuestring[0]) {
            strlcpy(cursor_buf, cursor_item->valuestring, cursor_buf_size);
        }
    }

    if (tool_name_buf && tool_name_buf_size > 0) {
        cJSON *tool_name_item = cJSON_GetObjectItem(input, "tool_name");
        if (!cJSON_IsString(tool_name_item) || !tool_name_item->valuestring[0]) {
            cJSON_Delete(input);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(tool_name_buf, tool_name_item->valuestring, tool_name_buf_size);
    }

    if (arguments_out) {
        cJSON *arguments = cJSON_GetObjectItem(input, "arguments");

        if (!arguments || !cJSON_IsObject(arguments)) {
            *arguments_out = cJSON_CreateObject();
        } else {
            *arguments_out = cJSON_Duplicate(arguments, 1);
        }

        if (!*arguments_out) {
            cJSON_Delete(input);
            return ESP_ERR_NO_MEM;
        }
    }

    cJSON_Delete(input);
    return ESP_OK;
}

static esp_err_t cap_mcp_execute_json_rpc(const char *full_url,
                                          cJSON *request,
                                          cJSON **response_out)
{
    cap_mcp_buf_t response_buf = {0};
    char *body = NULL;
    esp_err_t err;

    *response_out = NULL;
    body = cJSON_PrintUnformatted(request);
    if (!body) {
        return ESP_FAIL;
    }

    response_buf.data = malloc(CAP_MCP_RESPONSE_BUF_SIZE);
    if (!response_buf.data) {
        free(body);
        return ESP_ERR_NO_MEM;
    }
    response_buf.cap = CAP_MCP_RESPONSE_BUF_SIZE;
    response_buf.data[0] = '\0';

    err = cap_mcp_http_post(full_url, body, &response_buf);
    free(body);
    if (err != ESP_OK) {
        free(response_buf.data);
        return err;
    }

    *response_out = cJSON_Parse(response_buf.data);
    free(response_buf.data);
    if (!*response_out) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t cap_mcp_list_remote_tools(const char *input_json, cJSON **result_out)
{
    char server_url_buf[256];
    char endpoint_buf[64];
    char cursor_buf[128];
    char full_url[384];
    cJSON *params = NULL;
    cJSON *request = NULL;
    cJSON *response = NULL;
    cJSON *root = NULL;
    cJSON *tools_out = NULL;
    cJSON *error_obj = NULL;
    cJSON *result = NULL;
    cJSON *tools_array = NULL;
    cJSON *tool = NULL;
    esp_err_t err;

    if (!input_json || !result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;

    err = cap_mcp_parse_common_input(input_json,
                                     server_url_buf,
                                     sizeof(server_url_buf),
                                     endpoint_buf,
                                     sizeof(endpoint_buf),
                                     cursor_buf,
                                     sizeof(cursor_buf),
                                     NULL,
                                     0,
                                     NULL);
    if (err != ESP_OK) {
        return err;
    }

    cap_mcp_build_full_url(server_url_buf, endpoint_buf, full_url, sizeof(full_url));
    if (full_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    params = cJSON_CreateObject();
    request = cJSON_CreateObject();
    if (!params || !request) {
        cJSON_Delete(params);
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }

    if (cursor_buf[0]) {
        cJSON_AddStringToObject(params, "cursor", cursor_buf);
    }
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", "tools/list");
    cJSON_AddItemToObject(request, "params", params);
    cJSON_AddNumberToObject(request, "id", CAP_MCP_REQUEST_ID_LIST);

    err = cap_mcp_execute_json_rpc(full_url, request, &response);
    cJSON_Delete(request);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    tools_out = cJSON_CreateArray();
    if (!root || !tools_out) {
        cJSON_Delete(response);
        cJSON_Delete(root);
        cJSON_Delete(tools_out);
        return ESP_ERR_NO_MEM;
    }

    error_obj = cJSON_GetObjectItem(response, "error");
    if (cJSON_IsObject(error_obj)) {
        cJSON *message = cJSON_GetObjectItem(error_obj, "message");

        cJSON_AddStringToObject(root,
                                "error_message",
                                cJSON_IsString(message) ? message->valuestring : "Unknown MCP error");
        cJSON_AddItemToObject(root, "tools", tools_out);
        cJSON_Delete(response);
        *result_out = root;
        return ESP_OK;
    }

    result = cJSON_GetObjectItem(response, "result");
    if (!cJSON_IsObject(result)) {
        cJSON_Delete(response);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    tools_array = cJSON_GetObjectItem(result, "tools");
    if (cJSON_IsArray(tools_array)) {
        cJSON_ArrayForEach(tool, tools_array) {
            cJSON *duplicate = cJSON_Duplicate(tool, 1);

            if (!duplicate) {
                cJSON_Delete(response);
                cJSON_Delete(root);
                return ESP_ERR_NO_MEM;
            }
            cJSON_AddItemToArray(tools_out, duplicate);
        }
    }
    cJSON_AddItemToObject(root, "tools", tools_out);

    cJSON *next_cursor = cJSON_GetObjectItem(result, "nextCursor");
    if (cJSON_IsString(next_cursor) && next_cursor->valuestring[0]) {
        cJSON_AddStringToObject(root, "nextCursor", next_cursor->valuestring);
    }

    cJSON_Delete(response);
    *result_out = root;
    return ESP_OK;
}

esp_err_t cap_mcp_call_remote_tool(const char *input_json, cJSON **result_out)
{
    char server_url_buf[256];
    char endpoint_buf[64];
    char tool_name_buf[128];
    char full_url[384];
    cJSON *arguments = NULL;
    cJSON *params = NULL;
    cJSON *request = NULL;
    cJSON *response = NULL;
    cJSON *root = NULL;
    cJSON *error_obj = NULL;
    cJSON *result = NULL;
    esp_err_t err;

    if (!input_json || !result_out) {
        return ESP_ERR_INVALID_ARG;
    }
    *result_out = NULL;

    err = cap_mcp_parse_common_input(input_json,
                                     server_url_buf,
                                     sizeof(server_url_buf),
                                     endpoint_buf,
                                     sizeof(endpoint_buf),
                                     NULL,
                                     0,
                                     tool_name_buf,
                                     sizeof(tool_name_buf),
                                     &arguments);
    if (err != ESP_OK) {
        cJSON_Delete(arguments);
        return err;
    }

    cap_mcp_build_full_url(server_url_buf, endpoint_buf, full_url, sizeof(full_url));
    if (full_url[0] == '\0') {
        cJSON_Delete(arguments);
        return ESP_ERR_INVALID_ARG;
    }

    params = cJSON_CreateObject();
    request = cJSON_CreateObject();
    if (!params || !request) {
        cJSON_Delete(arguments);
        cJSON_Delete(params);
        cJSON_Delete(request);
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(params, "name", tool_name_buf);
    cJSON_AddItemToObject(params, "arguments", arguments);
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", "tools/call");
    cJSON_AddItemToObject(request, "params", params);
    cJSON_AddNumberToObject(request, "id", CAP_MCP_REQUEST_ID_CALL);

    err = cap_mcp_execute_json_rpc(full_url, request, &response);
    cJSON_Delete(request);
    if (err != ESP_OK) {
        return err;
    }

    root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(response);
        return ESP_ERR_NO_MEM;
    }

    error_obj = cJSON_GetObjectItem(response, "error");
    if (cJSON_IsObject(error_obj)) {
        cJSON *message = cJSON_GetObjectItem(error_obj, "message");

        cJSON_AddStringToObject(root,
                                "error_message",
                                cJSON_IsString(message) ? message->valuestring : "Unknown MCP error");
        cJSON_Delete(response);
        *result_out = root;
        return ESP_OK;
    }

    result = cJSON_GetObjectItem(response, "result");
    if (!cJSON_IsObject(result)) {
        cJSON_Delete(response);
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *content = cJSON_GetObjectItem(result, "content");
    cJSON *is_error = cJSON_GetObjectItem(result, "isError");
    cJSON_AddItemToObject(root, "content", content ? cJSON_Duplicate(content, 1) : cJSON_CreateArray());
    if (cJSON_IsBool(is_error)) {
        cJSON_AddBoolToObject(root, "isError", cJSON_IsTrue(is_error));
    }

    cJSON_Delete(response);
    *result_out = root;
    return ESP_OK;
}
