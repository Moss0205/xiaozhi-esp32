// 桥接 HTTP 客户端 —— 板子 ↔ Mac mini bridge_v2.py
// 只依赖 ESP-IDF 的 esp_http_client（上游 Xiaozhi 工程自带），不依赖小智协议。
// 对应服务端接口（bridge_v2.py，2026-11-05 已实测通）：
//   POST /esp/voice  body=16k 单声道 PCM 裸数据，返回 {"id":N,"text":"...","status":"queued"}
//   GET  /esp/reply?after=<id>  返回 {"id":N,"reply":"...","has_audio":true} 或 204 无新回复
//   GET  /esp/audio?id=<id>    返回 24k 单声道 PCM 裸数据（响应头 X-Audio-Rate: 24000）
// 所有请求都带 X-Bridge-Token 头。服务端返回非 200 时函数返回 false 并打印状态码。
#ifndef _BRIDGE_CLIENT_H_
#define _BRIDGE_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 上传一段录音（16k PCM），成功返回 true 并填 out_id / out_text（听写结果）。
// out_text 缓冲由调用方提供，长度含结尾 0；听写为空（没说话）时返回 true 但 out_id=-1。
bool bridge_post_voice(const int16_t* pcm, size_t sample_count,
                       int* out_id, char* out_text, size_t text_cap);

// 轮询 after_id 之后的回复。有新回复返回 true 并填 out_*；无新回复（204）返回 false
// 且 out_id 不变。调用方用返回值区分「无新回复」和「请求失败」时看日志即可，
// 失败时同样返回 false（日志里会写清 HTTP 状态码），轮询循环继续重试。
bool bridge_poll_reply(int after_id, int* out_id,
                       char* out_text, size_t text_cap, bool* out_has_audio);

// 下载一条回复的音频。PCM 数据写进调用方提供的缓冲（24k 单声道，16-bit）。
// 返回实际写入的样本数；缓冲不够时返回 -1（调用方应加大缓冲后重试该 id，
// 服务端音频文件还在，不会丢）。0 表示该 id 没有音频（纯文字回复）。
int bridge_download_audio(int id, int16_t* pcm_out, size_t max_samples);

// ---------------- 信息屏（2026-11-18）：GET /esp/info ----------------
// 板子不直接调外部接口，只问桥接要一次数据。桥接一次返回天气 + 两个 AI 用量。
// 服务端用扁平 JSON（便于板端用下面的轻量解析，不拉 JSON 库）：
//   天气 3 天：wx0_date/wx0_cond/wx0_high/wx0_low/wx0_hum/wx0_wind，wx1_*, wx2_*
//   我的用量（Claude Code）：my_ok, my_tokens_total, my_tokens_today, my_cost_usd
//   Muse 免费版每周限额：muse_ok, muse_weekly_pct, muse_reset_date,
//     muse_extra_pct, muse_extra_left_b（来自 VM 的 subscription-status 命令，
//     不是 Claude Code 中转用量）
//   mengapi：meng_configured, meng_ok, meng_balance_usd, meng_used_usd
// 字段缺失或取不到时对应 *_ok=false，调用方显示“未配置”/“未同步”，不要当错误。
typedef struct {
    // 天气（近 3 天，wx0=今天，wx1=明天，wx2=后天）
    bool wx_ok;
    char wx_date[3][16];    // "11-18"（月-日）
    char wx_cond[3][16];    // "晴"/"多云"/"阴"/"毛毛雨"/"雨"/"雪"/"雾"/"雷阵雨"
    int  wx_high[3];        // 最高温 °C
    int  wx_low[3];         // 最低温 °C
    int  wx_hum[3];         // 平均湿度 %
    int  wx_wind[3];        // 最大风速 km/h
    // 我的用量（Claude Code 经 beichenapi 中转；数据由 VM 同步脚本定期写到桥接）
    bool my_ok;             // false = 桥接还没同步到用量文件（显示“未同步”）
    long my_tokens_total;   // 累计 token（输入+输出，不含缓存）
    long my_tokens_today;   // 今日 token（输入+输出）
    double my_cost_usd;     // 累计花费（美元，官方价估算；中转实际扣得更少）
    // Muse 免费版每周限额（Muse 应用本身，subscription-status 命令；
    // 不是 Claude Code 中转用量，不要和上面混淆）
    bool muse_ok;           // false = 桥接还没同步到 muse_usage.json（显示“未同步”）
    int  muse_weekly_pct;   // 每周限额已使用百分比，如 27
    char muse_reset_date[16]; // 重置日期，中文，如“11月22日”
    int  muse_extra_pct;    // 额外额度已使用百分比，如 0
    int  muse_extra_left_b; // 额外额度剩余 token 数（单位：十亿），如 31
    // mengapi 用量（桥接读 mengapi 面板接口；未配令牌时 meng_configured=false）
    bool meng_configured;   // 桥接 config.json 里有没有 MENGAPI_TOKEN / MENGAPI_USER_ID
    bool meng_ok;           // 令牌有效且接口通
    double meng_balance_usd;// 剩余（美元，quota / quota_per_unit）
    double meng_used_usd;   // 累计已用（美元）
} bridge_info_t;

// 拉取一次 /esp/info，填 out。成功返回 true；HTTP 失败/超时返回 false，
// 调用方保留上次缓存继续显示，不要清屏。out 里 *_ok 字段由服务端如实填写。
bool bridge_fetch_info(bridge_info_t* out);

#ifdef __cplusplus
}
#endif

#endif  // _BRIDGE_CLIENT_H_
