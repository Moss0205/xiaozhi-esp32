// 桥接 HTTP 客户端实现 —— 见 bridge_client.h 的接口说明。
// 注意：本文件不做 JSON 全解析，只用最简单的字符串查找取 id / text / reply /
// has_audio，够桥接协议用，也避免在板端拉一个 JSON 库。服务端返回的 text/reply
// 里若含转义引号，解析会提前截断 —— MOSS 回复里极少出现裸引号，可接受；
// 若 dsh 想做严谨解析，在这里换成 cJSON 即可，接口不变。
#include "bridge_client.h"
#include "bridge_config.h"

#include <esp_log.h>
#include <esp_http_client.h>
#include <string.h>
#include <stdlib.h>

#define TAG "bridge_client"

static void build_url(char* buf, size_t cap, const char* path) {
    snprintf(buf, cap, "http://%s:%d%s", BRIDGE_HOST, BRIDGE_PORT, path);
}

// ---- 轻量 JSON 取值：找 "key":value，字符串取引号内容，数字/布尔取到分隔符 ----
static bool json_string(const char* json, const char* key, char* out, size_t cap) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (p == NULL) return false;
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p != '\0' && *p != '"' && i + 1 < cap) {
        // 服务端用 json.dumps(ensure_ascii=False) 输出，中文是 UTF-8 原文；
        // 只有 \" 和 \\ 需要还原，其余字节原样抄。
        if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) p++;
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

static bool json_int(const char* json, const char* key, int* out) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (p == NULL) return false;
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) return false;
    *out = atoi(p + 1);
    return true;
}

// 响应收集：esp_http_client 事件回调把 body 攒进这块缓冲。
typedef struct {
    char* buf;
    size_t cap;
    size_t len;
} resp_buf_t;

static esp_err_t collect_event(esp_http_client_event_t* evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    resp_buf_t* r = (resp_buf_t*)evt->user_data;
    size_t copy = evt->data_len;
    if (r->len + copy >= r->cap) copy = r->cap - r->len - 1;
    memcpy(r->buf + r->len, evt->data, copy);
    r->len += copy;
    r->buf[r->len] = '\0';
    return ESP_OK;
}

static esp_http_client_handle_t new_client(const char* url, resp_buf_t* resp) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = collect_event;
    cfg.user_data = resp;
    cfg.timeout_ms = BRIDGE_HTTP_TIMEOUT_MS;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "X-Bridge-Token", BRIDGE_TOKEN);
    return h;
}

bool bridge_post_voice(const int16_t* pcm, size_t sample_count,
                       int* out_id, char* out_text, size_t text_cap) {
    char url[256];
    build_url(url, sizeof(url), "/esp/voice");
    char resp_json[2048];
    resp_buf_t resp = {resp_json, sizeof(resp_json), 0};
    resp_json[0] = '\0';
    esp_http_client_handle_t h = new_client(url, &resp);
    esp_http_client_set_method(h, HTTP_METHOD_POST);
    esp_http_client_set_header(h, "Content-Type", "audio/L16");
    esp_http_client_set_post_field(h, (const char*)pcm, (int)(sample_count * 2));
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "POST /esp/voice failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "POST /esp/voice HTTP %d: %s", status, resp_json);
        return false;
    }
    if (out_text != NULL && text_cap > 0) {
        if (!json_string(resp_json, "text", out_text, text_cap)) out_text[0] = '\0';
    }
    if (out_id != NULL) {
        // 服务端没说话时返回 id:null，atoi 得 0；调用方用 out_text 为空判断更稳，
        // 这里把 0 统一成 -1 表示「无有效 id」。
        int id = 0;
        json_int(resp_json, "id", &id);
        *out_id = (id == 0) ? -1 : id;
    }
    ESP_LOGI(TAG, "voice uploaded: %s", resp_json);
    return true;
}

bool bridge_poll_reply(int after_id, int* out_id,
                       char* out_text, size_t text_cap, bool* out_has_audio) {
    char path[64];
    snprintf(path, sizeof(path), "/esp/reply?after=%d", after_id);
    char url[256];
    build_url(url, sizeof(url), path);
    char resp_json[2048];
    resp_buf_t resp = {resp_json, sizeof(resp_json), 0};
    resp_json[0] = '\0';
    esp_http_client_handle_t h = new_client(url, &resp);
    esp_err_t err = esp_http_client_perform(h);
    int status = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET /esp/reply failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status == 204) return false;  // 无新回复，不是错误
    if (status != 200) {
        ESP_LOGE(TAG, "GET /esp/reply HTTP %d: %s", status, resp_json);
        return false;
    }
    int id = 0;
    if (!json_int(resp_json, "id", &id)) return false;
    if (out_id != NULL) *out_id = id;
    if (out_text != NULL && text_cap > 0) {
        if (!json_string(resp_json, "reply", out_text, text_cap)) out_text[0] = '\0';
    }
    if (out_has_audio != NULL) {
        *out_has_audio = (strstr(resp_json, "\"has_audio\": true") != NULL ||
                          strstr(resp_json, "\"has_audio\":true") != NULL);
    }
    return true;
}

int bridge_download_audio(int id, int16_t* pcm_out, size_t max_samples) {
    char path[64];
    snprintf(path, sizeof(path), "/esp/audio?id=%d", id);
    char url[256];
    build_url(url, sizeof(url), path);
    // 音频不走 collect_event（body 可能几百 KB），直接用 read 循环流式取。
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = BRIDGE_HTTP_TIMEOUT_MS;
    cfg.buffer_size = 4096;
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "X-Bridge-Token", BRIDGE_TOKEN);
    esp_err_t err = esp_http_client_open(h, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GET /esp/audio open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(h);
        return -1;
    }
    int status = esp_http_client_get_status_code(h);
    if (status == 404) {
        ESP_LOGI(TAG, "audio id %d: no audio (text-only reply)", id);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return 0;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "GET /esp/audio HTTP %d", status);
        esp_http_client_close(h);
        esp_http_client_cleanup(h);
        return -1;
    }
    size_t total_bytes = 0;
    size_t max_bytes = max_samples * 2;
    char chunk[2048];
    int n;
    while ((n = esp_http_client_read(h, chunk, sizeof(chunk))) > 0) {
        if (total_bytes + (size_t)n > max_bytes) {
            ESP_LOGE(TAG, "audio id %d too large for buffer (%zu bytes)", id, max_bytes);
            esp_http_client_close(h);
            esp_http_client_cleanup(h);
            return -1;
        }
        memcpy((char*)pcm_out + total_bytes, chunk, n);
        total_bytes += n;
    }
    esp_http_client_close(h);
    esp_http_client_cleanup(h);
    ESP_LOGI(TAG, "audio id %d: %zu bytes (%.1fs @24k)", id, total_bytes,
             total_bytes / 2.0 / 24000);
    return (int)(total_bytes / 2);
}
