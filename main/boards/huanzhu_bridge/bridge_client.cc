// 桥接 HTTP 客户端实现 —— 见 bridge_client.h 的接口说明。
// 2026-11-06 重写：改用工程自带的 Http 抽象（Board::GetNetwork()->CreateHttp，
// 即 ota.cc / assets.cc 下载用的同一套，78/esp-ml307 组件提供），
// 不再依赖 esp_http_client —— IDF v6.1 已把它移出核心组件，头文件不可用。
// 注意：本文件不做 JSON 全解析，只用最简单的字符串查找取 id / text / reply /
// has_audio，够桥接协议用。服务端返回的 text/reply 里若含转义引号，解析会提前
// 截断 —— MOSS 回复里极少出现裸引号，可接受；若要严谨解析可换成 cJSON，接口不变。
//
// 本文件 = a549b58（#15 时钟修复版，板上现用固件）的 bridge_client.cc
//         + 信息屏改动（2026-11-18）：新增 json_long / json_double / json_bool
//         和 bridge_fetch_info（GET /esp/info）。其余与 a549b58 完全一致。
// ⚠️ 注意：main 分支在 a549b58 之后已被 dsh 改过（bridge_ping /
//    bridge_fetch_usage / bridge_fetch_weather，见 README 的“与 main 分支的冲突”）。
//    本文件以 a549b58 为基线，不含 dsh 的改动；合入 main 前必须先解决冲突。
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

// 长整型（token 数会越长越大，int 不够用时用这个）。信息屏改动新增。
static bool json_long(const char* json, const char* key, long* out) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (p == NULL) return false;
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) return false;
    *out = atol(p + 1);
    return true;
}

// 浮点数（金额）。服务端发的是 JSON 数字，如 0.89。信息屏改动新增。
static bool json_double(const char* json, const char* key, double* out) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (p == NULL) return false;
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) return false;
    *out = strtod(p + 1, NULL);
    return true;
}

// 布尔值：找 "key":true。信息屏改动新增。
static bool json_bool(const char* json, const char* key) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* p = strstr(json, pattern);
    if (p == NULL) return false;
    p = strchr(p + strlen(pattern), ':');
    if (p == NULL) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return strncmp(p, "true", 4) == 0;
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

// ---------------- 信息屏：GET /esp/info（2026-11-18 新增） ----------------
// 一次拿天气 + 两个 AI 用量。服务端用扁平 JSON，板端只做字符串查找，不拉 JSON 库。
// 任一字段缺失时对应 *_ok=false，调用方显示“未配置”/“未同步”，不报错、不清屏。
// 注意：Http 抽象的超时由 CreateHttp(0) 的参数控制（0=默认）；/esp/info 走
// 局域网桥接，默认超时够用，不需要 BRIDGE_INFO_HTTP_TIMEOUT_MS 的单独控制，
// 该常量只留作桥接侧参考（见 bridge_config.h）。
bool bridge_fetch_info(bridge_info_t* out) {
    if (out == NULL) return false;
    memset(out, 0, sizeof(*out));
    std::string url = build_url("/esp/info");
    auto http = new_http();
    if (auto opened = http->Open("GET", url); !opened) {
        ESP_LOGE(TAG, "GET /esp/info open failed: %s", opened.error().ToString().c_str());
        return false;
    }
    auto status = http->GetStatusCode();
    if (!status) {
        ESP_LOGE(TAG, "GET /esp/info status failed: %s", status.error().ToString().c_str());
        return false;
    }
    std::string resp = http->ReadAll();
    if (*status != 200) {
        ESP_LOGE(TAG, "GET /esp/info HTTP %d: %s", *status, resp.c_str());
        return false;
    }
    const char* json = resp.c_str();
    // 天气 3 天（wx0=今天，wx1=明天，wx2=后天）。有一天缺字段即整组判 wx_ok=false。
    out->wx_ok = true;
    for (int i = 0; i < 3; i++) {
        char key[32];
        snprintf(key, sizeof(key), "wx%d_date", i);
        if (!json_string(json, key, out->wx_date[i], sizeof(out->wx_date[i])))
            out->wx_ok = false;
        snprintf(key, sizeof(key), "wx%d_cond", i);
        if (!json_string(json, key, out->wx_cond[i], sizeof(out->wx_cond[i])))
            out->wx_ok = false;
        snprintf(key, sizeof(key), "wx%d_high", i);
        if (!json_int(json, key, &out->wx_high[i])) out->wx_ok = false;
        snprintf(key, sizeof(key), "wx%d_low", i);
        if (!json_int(json, key, &out->wx_low[i])) out->wx_ok = false;
        snprintf(key, sizeof(key), "wx%d_hum", i);
        if (!json_int(json, key, &out->wx_hum[i])) out->wx_ok = false;
        snprintf(key, sizeof(key), "wx%d_wind", i);
        if (!json_int(json, key, &out->wx_wind[i])) out->wx_ok = false;
    }
    // 我的用量（Claude Code）。my_ok=false 表示桥接还没同步到用量文件。
    out->my_ok = json_bool(json, "my_ok");
    if (out->my_ok) {
        json_long(json, "my_tokens_total", &out->my_tokens_total);
        json_long(json, "my_tokens_today", &out->my_tokens_today);
        json_double(json, "my_cost_usd", &out->my_cost_usd);
    }
    // Muse 免费版每周限额（Muse 应用本身，subscription-status 命令）。
    // muse_ok=false 表示桥接还没同步到 muse_usage.json。
    out->muse_ok = json_bool(json, "muse_ok");
    if (out->muse_ok) {
        json_int(json, "muse_weekly_pct", &out->muse_weekly_pct);
        json_string(json, "muse_reset_date", out->muse_reset_date,
                    sizeof(out->muse_reset_date));
        json_int(json, "muse_extra_pct", &out->muse_extra_pct);
        json_int(json, "muse_extra_left_b", &out->muse_extra_left_b);
    }
    // mengapi 用量。meng_configured=false 表示桥接 config.json 里没配令牌。
    out->meng_configured = json_bool(json, "meng_configured");
    out->meng_ok = json_bool(json, "meng_ok");
    if (out->meng_ok) {
        json_double(json, "meng_balance_usd", &out->meng_balance_usd);
        json_double(json, "meng_used_usd", &out->meng_used_usd);
    }
    ESP_LOGI(TAG, "info: wx_ok=%d my_ok=%d muse_ok=%d meng_cfg=%d meng_ok=%d",
             (int)out->wx_ok, (int)out->my_ok, (int)out->muse_ok,
             (int)out->meng_configured, (int)out->meng_ok);
    return true;
}
