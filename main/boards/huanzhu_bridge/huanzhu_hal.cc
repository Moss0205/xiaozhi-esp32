// 桥接 HAL 接线模板 —— dsh 集成时按 ADAPT 点核对 3 处上游 API，其余不动。
// 本文件假设的集成位置：Xiaozhi 工程里 fork 的 main/boards/huanzhu/ 目录，
// 与 huanzhu_board.cc / huanzhu_display / huanzhu_audio_codec 同工程编译，
// 因此能 include 到上游的 display / audio_codec 头文件。
#include "bridge_hal.h"

#include <esp_log.h>
#include <esp_system.h>

// ADAPT-1：头文件名按你工程里的实际路径改。fork 里 board.cc 用的是
//   #include "display/lcd_display.h"（SpiLcdDisplay）和 "audio_codec.h"（AudioCodec 基类）。
#include "display/lcd_display.h"
#include "audio_codec.h"
#include "huanzhu_audio_codec.h"
#include "board.h"

#define TAG "bridge_hal"

static Display* g_display = nullptr;
static AudioCodec* g_codec = nullptr;
static void (*g_wifi_config_fn)(void) = nullptr;

void bridge_hal_bind(void* display, void* codec) {
    g_display = (Display*)display;
    g_codec = (AudioCodec*)codec;
    ESP_LOGI(TAG, "hal bound: display=%p codec=%p", (void*)g_display, (void*)g_codec);
}

void bridge_hal_set_wifi_config_handler(void (*fn)(void)) {
    g_wifi_config_fn = fn;
}

void bridge_hal_init(void) {
    if (g_codec != nullptr) {
        // 幻珠 codec 的 ADC 必须 EnableInput(true) 才出数据（Read 在未 running 时恒返 0）；
        // 喇叭同理要 EnableOutput(true)。小智版里这两步由 AudioService 做，桥接版必须自己开。
        g_codec->EnableInput(true);
        g_codec->EnableOutput(true);
        ESP_LOGI(TAG, "hal init: codec input/output enabled");
    } else {
        ESP_LOGE(TAG, "hal init: codec not bound!");
    }
}

void bridge_hal_show_text(const char* text) {
    if (g_display == nullptr) {
        ESP_LOGW(TAG, "show_text: display not bound, text=%s", text);
        return;
    }
    // ADAPT-2：显示 API。fork 的 display.cc 注释提到上游 SetChatMessage，
    // 若你工程里的签名是 SetChatMessage(role, content)，把下面一行改成
    //   g_display->SetChatMessage("assistant", text);
    // 字幕落在内切圆里是 HuanzhuRoundDisplay::SetupUI 的几何修正，与这行无关。
    g_display->SetChatMessage("assistant", text);
}

void bridge_hal_set_emotion(const char* emotion) {
    if (g_display == nullptr) {
        ESP_LOGW(TAG, "set_emotion: display not bound, emotion=%s", emotion);
        return;
    }
    if (!g_display->SetEmotion(emotion)) {
        ESP_LOGW(TAG, "set_emotion('%s') failed (no emoji in assets?)", emotion);
    }
}

void bridge_hal_set_backlight(bool on) {
    auto* backlight = Board::GetInstance().GetBacklight();
    if (backlight == nullptr) {
        ESP_LOGW(TAG, "set_backlight: no backlight");
        return;
    }
    backlight->SetBrightness(on ? 100 : 0);
    ESP_LOGI(TAG, "backlight %s", on ? "on" : "off");
}

int bridge_hal_mic_read(int16_t* buf, int samples) {
    if (g_codec == nullptr) return 0;
    // ADAPT-3：读麦 API。fork 的 huanzhu_audio_codec.h 里 Read 是
    //   int Read(int16_t* dest, int samples) override;
    // 若在你工程里它是 protected、外部调不到，改成调 AudioCodec 基类对外
    // 暴露的公开读方法（在 audio_codec.h 里找与 Read 对应的公开方法名）。
    return static_cast<HuanzhuAudioCodec*>(g_codec)->ReadPcm(buf, samples);
}

void bridge_hal_spk_write(const int16_t* buf, int samples) {
    if (g_codec == nullptr) return;
    // fork 注释：输出复用上游 NoAudioCodec 的 Write/EnableOutput（I2S TX）。
    // 若 Write 签名与 (const int16_t*, int) 不符，按 audio_codec.h 改这一行。
    static_cast<HuanzhuAudioCodec*>(g_codec)->WritePcm(buf, samples);
}

void bridge_hal_enter_wifi_config(void) {
    if (g_wifi_config_fn != nullptr) {
        g_wifi_config_fn();
        return;
    }
    // 未接线时的默认行为：只记日志，不重启、不改 WiFi —— dsh 接线后这支日志
    // 就不会再出现。千万别在这里直接 esp_restart()，否则中间短按会变重启，
    // 与「中间短按=配网、中间长按=重启」的最终表冲突。
    ESP_LOGW(TAG, "enter_wifi_config: handler not bound yet, ignoring");
}

void bridge_hal_restart(void) {
    ESP_LOGI(TAG, "restart requested (press long)");
    esp_restart();
}
