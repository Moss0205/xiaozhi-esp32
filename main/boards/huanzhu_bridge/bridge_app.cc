// 桥接应用状态机实现 —— 见 bridge_app.h 的状态图。
// 录音与播报缓冲都放在 PSRAM（8MB 板子够用）：录音 30s×16k×2B=960KB，
// 播报 20s×24k×2B=960KB；两块不同时满，峰值约 1.9MB。
#include "bridge_app.h"
#include "bridge_client.h"
#include "bridge_config.h"
#include "bridge_hal.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <string.h>
#include <stdlib.h>

#define TAG "bridge_app"

typedef enum {
    ST_IDLE = 0,
    ST_RECORDING,
    ST_UPLOADING,
    ST_WAITING,
    ST_PLAYING,
} bridge_state_t;

static volatile bridge_state_t g_state = ST_IDLE;
static volatile bool g_stop_playback = false;
static volatile int g_volume = BRIDGE_VOLUME_DEFAULT;
static int g_last_reply_id = 0;

// ---------------- 表情 + 自动睡眠（2026-11-09） ----------------
// 表情映射（名字见 assets 分区 index.json；精简包 5 个大脸：
   // delicious、silly、funny、sleepy、confused）：
static const char* emotion_for_state(bridge_state_t s) {
    switch (s) {
        case ST_RECORDING: return "funny";     // 在听
        case ST_UPLOADING: return "confused";  // 上传中（短暂）
        case ST_WAITING:   return "confused";  // 等 MOSS 回
        case ST_PLAYING:   return "silly";     // 在说
        case ST_IDLE:
        default:           return "sleepy";    // 待机
    }
}

static TickType_t g_last_activity_tick = 0;
static bool g_screen_off = false;

// 记一次活动：刷新空闲计时；若屏幕已关则重新点亮。
static void touch_activity(void) {
    g_last_activity_tick = xTaskGetTickCount();
    if (g_screen_off) {
        g_screen_off = false;
        bridge_hal_set_backlight(true);
        ESP_LOGI(TAG, "wake: backlight on");
    }
}

// 统一改状态：换表情 + 记一次活动。
static void bridge_set_state(bridge_state_t s) {
    g_state = s;
    bridge_hal_set_emotion(emotion_for_state(s));
    touch_activity();
}

// ---------------- 按键：GPIO 轮询 + 消抖 + 短/长按 ----------------
typedef struct {
    gpio_num_t gpio;
    const char* name;
    int last_level;       // 上次稳定电平（1=松开，0=按下，低有效）
    int debounce_count;   // 连续同电平计数
    TickType_t press_tick;  // 按下时刻
    bool long_fired;      // 长按已触发过（松开时不再发短按）
} key_t;

static key_t g_keys[3] = {
    {KEY_UP_GPIO, "UP", 1, 0, 0, false},
    {KEY_PRESS_GPIO, "PRESS", 1, 0, 0, false},
    {KEY_DOWN_GPIO, "DOWN", 1, 0, 0, false},
};

static void log_raw_levels(const char* why) {
    ESP_LOGI(TAG, "KEY RAW %s: GPIO3=%d GPIO4=%d GPIO5=%d",
             why, gpio_get_level(GPIO_NUM_3), gpio_get_level(GPIO_NUM_4),
             gpio_get_level(GPIO_NUM_5));
}

static void on_key_short(int idx);
static void on_key_long(int idx);

static void key_task(void* arg) {
    (void)arg;
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << KEY_UP_GPIO) | (1ULL << KEY_PRESS_GPIO) |
                           (1ULL << KEY_DOWN_GPIO);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;  // 原理图已有 10k 上拉，这里再开内部上拉无害
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    log_raw_levels("boot");

    const int debounce_ticks = KEY_DEBOUNCE_MS / KEY_POLL_MS;
    for (;;) {
        for (int i = 0; i < 3; i++) {
            key_t* k = &g_keys[i];
            int level = gpio_get_level(k->gpio);
            if (level != k->last_level) {
                // 电平变了：先消抖，连续 debounce_ticks 次同电平才认。
                // 简化实现：本次先记数，下次循环再确认（见下方计数逻辑）。
            }
            // 用「连续计数」做消抖：level 与 last_level 不同时计数，否则清零。
            // 为保持代码短，这里用一个静态的 pending 计数存在 debounce_count 里：
            // last_level 是「已确认电平」，debounce_count 是「新电平连续次数」。
            static int pending_level[3] = {1, 1, 1};
            if (level != k->last_level) {
                if (level != pending_level[i]) {
                    pending_level[i] = level;
                    k->debounce_count = 1;
                } else {
                    k->debounce_count++;
                }
                if (k->debounce_count >= debounce_ticks) {
                    // 确认一次电平翻转
                    int old = k->last_level;
                    k->last_level = level;
                    k->debounce_count = 0;
                    if (old == 1 && level == 0) {
                        // 按下
                        k->press_tick = xTaskGetTickCount();
                        k->long_fired = false;
                        touch_activity();  // 睡眠中按下的第一下先唤醒屏幕，再走正常语义
                        log_raw_levels(k->name);
#if BRIDGE_KEY_LOG_ONLY
                        ESP_LOGI(TAG, "KEY_LOG_ONLY: %s pressed, no action", k->name);
#endif
                    } else if (old == 0 && level == 1) {
                        // 松开：若长按没触发过，按住时长 < 阈值 = 短按
                        TickType_t held_ms =
                            (xTaskGetTickCount() - k->press_tick) * portTICK_PERIOD_MS;
                        if (!k->long_fired && held_ms < KEY_LONG_PRESS_MS) {
#if !BRIDGE_KEY_LOG_ONLY
                            on_key_short(i);
#endif
                        }
                    }
                }
            } else {
                pending_level[i] = level;
                k->debounce_count = 0;
                // 按住中：到阈值且未触发过 = 长按（只触发一次）
                if (level == 0 && !k->long_fired) {
                    TickType_t held_ms =
                        (xTaskGetTickCount() - k->press_tick) * portTICK_PERIOD_MS;
                    if (held_ms >= KEY_LONG_PRESS_MS) {
                        k->long_fired = true;
                        log_raw_levels(k->name);
#if !BRIDGE_KEY_LOG_ONLY
                        on_key_long(i);
#endif
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
}

// 按键语义（bridge_config.h 的最终表；方向映射只由 KEY_*_GPIO 三行决定）。
static void on_key_short(int idx) {
    if (idx == 0) {  // 上拨短按：开始对话（播放中 = 打断并重录）
        if (g_state == ST_PLAYING) g_stop_playback = true;
        if (g_state == ST_IDLE || g_state == ST_PLAYING) {
            bridge_set_state(ST_RECORDING);
            bridge_hal_show_text("在听…");
            ESP_LOGI(TAG, "key UP short: start recording");
        }
    } else if (idx == 2) {  // 下拨短按：结束对话
        if (g_state == ST_RECORDING) {
            bridge_set_state(ST_UPLOADING);  // 主任务检测到后上传并进 WAITING
            ESP_LOGI(TAG, "key DOWN short: stop recording, upload");
        } else if (g_state == ST_WAITING) {
            bridge_set_state(ST_IDLE);
            bridge_hal_show_text("你好MOSS");
            ESP_LOGI(TAG, "key DOWN short: cancel waiting");
        }
    } else {  // 中间短按：配网
        ESP_LOGI(TAG, "key PRESS short: enter wifi config");
        bridge_hal_enter_wifi_config();
    }
}

static void on_key_long(int idx) {
    if (idx == 0) {
        if (g_volume + BRIDGE_VOLUME_STEP <= 100) g_volume += BRIDGE_VOLUME_STEP;
        ESP_LOGI(TAG, "key UP long: volume %d", g_volume);
    } else if (idx == 2) {
        if (g_volume - BRIDGE_VOLUME_STEP >= 0) g_volume -= BRIDGE_VOLUME_STEP;
        ESP_LOGI(TAG, "key DOWN long: volume %d", g_volume);
    } else {
        ESP_LOGI(TAG, "key PRESS long: restart");
        bridge_hal_restart();
    }
}

// ---------------- 主状态机 ----------------
static void play_pcm_with_gain(const int16_t* pcm, int samples) {
    // 软件音量：逐样本乘 volume%，避免动 codec 的音量曲线。
    const int chunk = 480;  // 24k 下 20ms 一 chunk
    int16_t buf[chunk];
    for (int off = 0; off < samples && !g_stop_playback; off += chunk) {
        int n = samples - off < chunk ? samples - off : chunk;
        for (int i = 0; i < n; i++) {
            int32_t v = (int32_t)pcm[off + i] * g_volume / 100;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            buf[i] = (int16_t)v;
        }
        bridge_hal_spk_write(buf, n);
    }
}

static void main_task(void* arg) {
    (void)arg;
    // 录音缓冲：30s × 16k，PSRAM
    const size_t rec_max = BRIDGE_RECORD_MAX_S * AUDIO_INPUT_SAMPLE_RATE;
    int16_t* rec = (int16_t*)heap_caps_malloc(rec_max * 2,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // 播报缓冲：20s × 24k，PSRAM
    const size_t play_max = 20 * AUDIO_OUTPUT_SAMPLE_RATE;
    int16_t* play = (int16_t*)heap_caps_malloc(play_max * 2,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (rec == NULL || play == NULL) {
        ESP_LOGE(TAG, "PSRAM alloc failed, bridge app cannot run");
        vTaskDelete(NULL);
        return;
    }
    bridge_hal_show_text("你好MOSS");
    bridge_hal_set_emotion("sleepy");
    g_last_activity_tick = xTaskGetTickCount();
    ESP_LOGI(TAG, "bridge app ready, state=IDLE");

    for (;;) {
        if (g_state == ST_RECORDING) {
            g_stop_playback = false;  // 上次播报若被打断，标志必须清掉，否则下次播报会立刻停
            // 录音循环：每次读 320 样本（16k 下 20ms），直到下拨短按切到 UPLOADING、
            // 或 30 秒上限。注意：录音时主任务独占，轮询按键靠 key_task 并行跑。
            size_t got = 0;
            int16_t tmp[320];
            while (g_state == ST_RECORDING && got < rec_max) {
                int n = bridge_hal_mic_read(tmp, 320);
                if (n > 0) {
                    if (got + (size_t)n > rec_max) n = (int)(rec_max - got);
                    memcpy(rec + got, tmp, n * 2);
                    got += n;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            if (g_state == ST_RECORDING) {
                // 30 秒上限到，同样走上传
                bridge_set_state(ST_UPLOADING);
            }
            // UPLOADING：上传录音，拿听写文本（服务端已入队，MOSS 轮询后会回）
            bridge_hal_show_text("在想…");
            int voice_id = -1;
            char heard[512];
            heard[0] = '\0';
            if (got > 0 && bridge_post_voice(rec, got, &voice_id, heard, sizeof(heard))) {
                if (voice_id < 0 || heard[0] == '\0') {
                    bridge_hal_show_text("没听清，再说一次");
                    bridge_set_state(ST_IDLE);
                } else {
                    ESP_LOGI(TAG, "heard id=%d text=%s", voice_id, heard);
                    g_last_reply_id = voice_id;
                    bridge_set_state(ST_WAITING);
                }
            } else {
                bridge_hal_show_text("没连上桥接服务");
                bridge_set_state(ST_IDLE);
            }
        } else if (g_state == ST_WAITING) {
            // 等 MOSS 回复：每秒轮询一次，最长 120 秒；超时回 IDLE，不卡死。
            int waited_ms = 0;
            bool got_reply = false;
            while (g_state == ST_WAITING && waited_ms < 120000) {
                int rid = 0;
                char reply[1024];
                reply[0] = '\0';
                bool has_audio = false;
                if (bridge_poll_reply(g_last_reply_id, &rid, reply, sizeof(reply),
                                      &has_audio)) {
                    ESP_LOGI(TAG, "reply id=%d has_audio=%d text=%s",
                             rid, (int)has_audio, reply);
                    bridge_hal_show_text(reply);
                    if (has_audio) {
                        int samples = bridge_download_audio(rid, play, play_max);
                        if (samples > 0) {
                            bridge_set_state(ST_PLAYING);
                            g_stop_playback = false;
                            play_pcm_with_gain(play, samples);
                        }
                    }
                    g_last_reply_id = rid;
                    got_reply = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(BRIDGE_POLL_INTERVAL_MS));
                waited_ms += BRIDGE_POLL_INTERVAL_MS;
            }
            if (g_state == ST_WAITING) {
                if (!got_reply) bridge_hal_show_text("MOSS 没回，看看桥接服务");
                bridge_set_state(ST_IDLE);
            } else if (g_state == ST_PLAYING) {
                // 播完（或被上拨打断）：若打断时 g_state 已被按键改成 RECORDING，
                // 这里不要覆盖回 IDLE。
                if (!g_stop_playback) {
                    bridge_set_state(ST_IDLE);
                    bridge_hal_show_text("你好MOSS");
                }
                g_stop_playback = false;
            }
            // 若按键在 WAITING/PLAYING 期间把状态改成了 RECORDING/IDLE，上面分支
            // 已按 g_state 现值处理，不再强制覆盖。
        } else {
            // IDLE / UPLOADING（上传在 RECORDING 分支里同步做完，不会停留）
            // 自动睡眠：IDLE 下无操作超过 BRIDGE_IDLE_SLEEP_MINUTES 分钟就关背光，
            // 任意按键（key_task 按下时 touch_activity）会重新点亮。
            if (BRIDGE_IDLE_SLEEP_MINUTES > 0 && g_state == ST_IDLE && !g_screen_off) {
                TickType_t idle_ms =
                    (xTaskGetTickCount() - g_last_activity_tick) * portTICK_PERIOD_MS;
                if (idle_ms >= (TickType_t)BRIDGE_IDLE_SLEEP_MINUTES * 60 * 1000) {
                    g_screen_off = true;
                    bridge_hal_set_backlight(false);
                    ESP_LOGI(TAG, "idle %d min: backlight off", BRIDGE_IDLE_SLEEP_MINUTES);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void bridge_app_start(void) {
    xTaskCreate(key_task, "bridge_keys", 8192, NULL, 5, NULL);  // 栈加深：中间短按会在本任务里同步开配网 AP
    xTaskCreate(main_task, "bridge_main", 8192, NULL, 5, NULL);
}

int bridge_app_volume(void) {
    return g_volume;
}
