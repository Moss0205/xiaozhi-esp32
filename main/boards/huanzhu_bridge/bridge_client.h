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

#ifdef __cplusplus
}
#endif

#endif  // _BRIDGE_CLIENT_H_
