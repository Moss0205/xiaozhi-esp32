// 桥接 HTTP 客户端实现 —— 见 bridge_client.h 的接口说明。
// 2026-11-06 重写：改用工程自带的 Http 抽象（Board::GetNetwork()->CreateHttp，
// 即 ota.cc / assets.cc 下载用的同一套，78/esp-ml307 组件提供），
// 不再依赖 esp_http_client —— IDF v6.1 已把它移出核心组件，头文件不可用。
// 注意：本文件不做 JSON 全解析，只用最简单的字符串查找取 id / text / reply /
// has_audio，够桥接协议用。服务端返回的 text/reply 里若含转义引号，解析会提前
// 截断 —— MOSS 回复里极少出现裸引号，可接受；若要严谨解析可换成 cJSON，接口不变。
#include "bridge_client.h"
#include "bridge_config.h"

#include "board.h"

#include <esp_log.h>
#include <string.h>
#include <stdlib.h>

#include <memory>
#include <string>

#define TAG "bridge_client"

static std::string build_url(const char* path) {
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d%s", BRIDGE_HOST, BRIDGE_PORT, path);
    return std::string(url);
}

// 每次请求新建一个 Http 实例（CreateHttp 轻量，与 Ota::SetupHttp 同一用法）
static std::unique_ptr<Http> new_http() {
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetHeader("X-Bridge-Token", BRIDGE_TOKEN);
    return http;
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
        // 只有 \" 和 \\\\ 需要还原，其余字节原样抄。
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
    p++;
    while (*p == ' ' || *p == '\t') p++;
    // wttr.in 把数字也当字符串返回（"temp_C":"15"），beichen 返回裸数字（"requests":0）。
    // 两种都支持：若有引号先跳过，再 atoi。atoi 遇非数字返回 0，调用方已初始化默认值。
    if (*p == '"') p++;
    *out = atoi(p);
    return true;
}

// 取浮点字段（余额 balance 是小数）。用 strtod 读到第一个非数字为止。
static bool json_double(const char* json, const char* key, double* out) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (p == NULL) return false;
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return false;  // 负数余额无意义，直接拒
    *out = atof(p);
    return true;
}

bool bridge_fetch_usage(long* balance_cents, long* today_tokens, int* today_requests) {
    if (balance_cents) *balance_cents = -1;
    if (today_tokens) *today_tokens = -1;
    if (today_requests) *today_requests = -1;

    const char* key = BRIDGE_USAGE_KEY;
    if (key == NULL || key[0] == '\0') {
        ESP_LOGD(TAG, "usage fetch skipped: no API key configured");
        return false;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    http->SetHeader("Authorization", auth);
    http->SetHeader("Accept", "application/json");

    if (auto opened = http->Open("GET", BRIDGE_USAGE_URL); !opened) {
        ESP_LOGE(TAG, "usage GET open failed: %s", opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "usage GET status failed: %s", status.error().ToString().c_str());
        return false;
    }
    std::string resp = http->ReadAll();
    if (*status != 200) {
        ESP_LOGE(TAG, "usage GET HTTP %d: %.200s", *status, resp.c_str());
        return false;
    }

    double balance = 0;
    if (!json_double(resp.c_str(), "balance", &balance)) {
        ESP_LOGE(TAG, "usage JSON: no balance field: %.200s", resp.c_str());
        return false;
    }
    if (balance_cents) *balance_cents = (long)(balance * 100.0 + 0.5);

    // usage.today：服务端把 today 放在 total 之前。用 "total" 的位置截断 today 段，
    // 避免在 daily_usage / model_stats 里撞到同名的 total_tokens / requests。
    const char* usage_p = strstr(resp.c_str(), "\"usage\"");
    const char* today_p = usage_p ? strstr(usage_p, "\"today\"") : NULL;
    if (today_p) {
        const char* total_p = strstr(today_p, "\"total\"");
        size_t seg_len = total_p ? (size_t)(total_p - today_p) : strlen(today_p);
        std::string today_seg(today_p, seg_len);
        int tok = 0;
        int req = 0;
        json_int(today_seg.c_str(), "total_tokens", &tok);
        json_int(today_seg.c_str(), "requests", &req);
        if (today_tokens) *today_tokens = tok;
        if (today_requests) *today_requests = req;
    }

    ESP_LOGI(TAG, "usage ok: balance_cents=%ld today_tokens=%ld requests=%d",
             balance_cents ? *balance_cents : -1,
             today_tokens ? *today_tokens : -1,
             today_requests ? *today_requests : -1);
    return true;
}

bool bridge_fetch_weather(int* temp_c, int* weather_code) {
    if (temp_c) *temp_c = 0;
    if (weather_code) *weather_code = -1;

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(0);
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", "curl/8.0");  // wttr.in 对默认 UA 偶有 403，伪装 curl

    if (auto opened = http->Open("GET", BRIDGE_WEATHER_URL); !opened) {
        ESP_LOGE(TAG, "weather GET open failed: %s", opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "weather GET status failed: %s", status.error().ToString().c_str());
        return false;
    }
    std::string resp = http->ReadAll();
    if (*status != 200) {
        ESP_LOGE(TAG, "weather GET HTTP %d: %.200s", *status, resp.c_str());
        return false;
    }

    // 只取 current_condition 数组第一个对象：从 "current_condition" 后的第一个 '['
    // 到第一个 ']'。避开 weather 数组其它日期/hourly 里同名的 temp_C / weatherCode。
    const char* cc = strstr(resp.c_str(), "\"current_condition\"");
    if (cc == NULL) {
        ESP_LOGE(TAG, "weather JSON: no current_condition");
        return false;
    }
    const char* lb = strchr(cc, '[');
    const char* rb = lb ? strchr(lb, ']') : NULL;
    if (lb == NULL || rb == NULL || rb <= lb) {
        ESP_LOGE(TAG, "weather JSON: bad current_condition array");
        return false;
    }
    std::string cur(lb, (size_t)(rb - lb));

    int t = 0, code = -1;
    // temp_C 是整数（wttr.in 当前温度不带小数），直接 json_int。
    if (!json_int(cur.c_str(), "temp_C", &t)) {
        ESP_LOGE(TAG, "weather JSON: no temp_C in current_condition");
        return false;
    }
    json_int(cur.c_str(), "weatherCode", &code);
    if (temp_c) *temp_c = t;
    if (weather_code) *weather_code = code;

    ESP_LOGI(TAG, "weather ok: temp_C=%d code=%d", t, code);
    return true;
}

// 服务状态探测：只要能连上 HTTP 并拿到状态码就算"在线"（200/204 都算），
// 与桥接 token 是否正确无关（token 错会返回 401，仍说明服务进程活着）。
// 用 INT_MAX 当 after id，服务端不会有任何新回复可发，纯探测。
bool bridge_ping(void) {
    char path[64];
    snprintf(path, sizeof(path), "/esp/reply?after=%d", 2147483647);
    std::string url = build_url(path);
    auto http = new_http();
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGD(TAG, "service ping open failed: %s", opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGD(TAG, "service ping status failed: %s", status.error().ToString().c_str());
        return false;
    }
    return true;
}

bool bridge_post_voice(const int16_t* pcm, size_t sample_count,
                       int* out_id, char* out_text, size_t text_cap) {
    std::string url = build_url("/esp/voice");
    auto http = new_http();
    http->SetHeader("Content-Type", "audio/L16");
    // 二进制 PCM 直接塞 std::string（可含 NUL），SetContent 按字节长度发送
    http->SetContent(std::string(reinterpret_cast<const char*>(pcm), sample_count * 2));
    if (auto opened = http->Open("POST", url); !opened) {
        ESP_LOGE(TAG, "POST /esp/voice open failed: %s", opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "POST /esp/voice status failed: %s", status.error().ToString().c_str());
        return false;
    }
    std::string resp = http->ReadAll();
    if (*status != 200) {
        ESP_LOGE(TAG, "POST /esp/voice HTTP %d: %s", *status, resp.c_str());
        return false;
    }
    if (out_text != NULL && text_cap > 0) {
        if (!json_string(resp.c_str(), "text", out_text, text_cap)) out_text[0] = '\0';
    }
    if (out_id != NULL) {
        // 服务端没说话时返回 id:null，atoi 得 0；调用方用 out_text 为空判断更稳，
        // 这里把 0 统一成 -1 表示「无有效 id」。
        int id = 0;
        json_int(resp.c_str(), "id", &id);
        *out_id = (id == 0) ? -1 : id;
    }
    ESP_LOGI(TAG, "voice uploaded: %s", resp.c_str());
    return true;
}

bool bridge_poll_reply(int after_id, int* out_id,
                       char* out_text, size_t text_cap, bool* out_has_audio) {
    char path[64];
    snprintf(path, sizeof(path), "/esp/reply?after=%d", after_id);
    std::string url = build_url(path);
    auto http = new_http();
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGE(TAG, "GET /esp/reply open failed: %s", opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "GET /esp/reply status failed: %s", status.error().ToString().c_str());
        return false;
    }
    if (*status == 204) return false;  // 无新回复，不是错误
    std::string resp = http->ReadAll();
    if (*status != 200) {
        ESP_LOGE(TAG, "GET /esp/reply HTTP %d: %s", *status, resp.c_str());
        return false;
    }
    int id = 0;
    if (!json_int(resp.c_str(), "id", &id)) return false;
    if (out_id != NULL) *out_id = id;
    if (out_text != NULL && text_cap > 0) {
        if (!json_string(resp.c_str(), "reply", out_text, text_cap)) out_text[0] = '\0';
    }
    if (out_has_audio != NULL) {
        *out_has_audio = (strstr(resp.c_str(), "\"has_audio\": true") != NULL ||
                          strstr(resp.c_str(), "\"has_audio\":true") != NULL);
    }
    return true;
}

int bridge_download_audio(int id, int16_t* pcm_out, size_t max_samples) {
    char path[64];
    snprintf(path, sizeof(path), "/esp/audio?id=%d", id);
    std::string url = build_url(path);
    auto http = new_http();
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGE(TAG, "GET /esp/audio open failed: %s", opened.error().ToString().c_str());
        return -1;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "GET /esp/audio status failed: %s", status.error().ToString().c_str());
        return -1;
    }
    if (*status == 404) {
        ESP_LOGI(TAG, "audio id %d: no audio (text-only reply)", id);
        return 0;
    }
    if (*status != 200) {
        ESP_LOGE(TAG, "GET /esp/audio HTTP %d", *status);
        return -1;
    }
    // 流式读取：Read 返回本次读到的字节数，0 表示结束（与 assets.cc 下载同一用法）
    size_t total_bytes = 0;
    size_t max_bytes = max_samples * 2;
    char chunk[2048];
    while (true) {
        auto ret = http->Read(chunk, sizeof(chunk));
        if (!ret) {
            ESP_LOGE(TAG, "GET /esp/audio read failed: %s", ret.error().ToString().c_str());
            return -1;
        }
        size_t n = *ret;
        if (n == 0) break;
        if (total_bytes + n > max_bytes) {
            ESP_LOGE(TAG, "audio id %d too large for buffer (%zu bytes)", id, max_bytes);
            return -1;
        }
        memcpy((char*)pcm_out + total_bytes, chunk, n);
        total_bytes += n;
    }
    ESP_LOGI(TAG, "audio id %d: %zu bytes (%.1fs @24k)", id, total_bytes,
             total_bytes / 2.0 / 24000);
    return (int)(total_bytes / 2);
}
