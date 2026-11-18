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
#include <time.h>

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

// 字幕停留（2026-11-09）：回复播完后字幕保留几分钟再消失；新对话开始直接替换。
static bool g_reply_shown = false;
static TickType_t g_reply_shown_tick = 0;

// 记一次活动：刷新空闲计时；若屏幕已关则重新点亮。
static void touch_activity(void) {
    g_last_activity_tick = xTaskGetTickCount();
    if (g_screen_off) {
        g_screen_off = false;
        bridge_hal_set_backlight(true);
        ESP_LOGI(TAG, "wake: backlight on");
    }
}

// ---------------- 时钟（2026-11-17）：空闲时状态栏显示时间+日期 ----------------
// 取代一直卡住的“正在初始化...”。时间由 SNTP 对时（bridge_main.cc，ntp.aliyun.com），
// 时区 CST-8（Asia/Shanghai，UTC+8）。只在 ST_IDLE 且时间已对上时显示时钟；
// 时间还没对上（SNTP 未完成）时显示“待命”，绝不显示 1970 年的错误时间。
// 刷新：进 IDLE 时立即刷一次（bridge_set_state）；IDLE 循环里每分钟变了就刷
// （main_task 末尾的 IDLE 分支，每 50ms 检查一次，分钟没变直接返回）。
// 对话中（RECORDING/UPLOADING/WAITING/PLAYING）不显示时钟，状态栏改显示当前阶段；
// 表情 + 字幕不受影响（仍由 bridge_set_state / bridge_hal_show_text 控制）。
static const char* const kWeekdayZh[7] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
// 上次显示的分钟（HH*60+MM）；-1=尚未显示/需立即刷新；-2=时间未对上，正显示“待命”。
static int g_last_clock_min = -1;

// ---------------- IDLE 顶栏四页（2026-11-17 / 11-18）----------------
// 页面由中键短按手动循环切换，不再自动轮换：
//   0=聊天/时钟  1=服务状态  2=天气  3=AI 用量
// usage_task 后台定时拉 beichenapi /v1/usage（BRIDGE_USAGE_KEY 由 CI secret 注入）
// 与 wttr.in 天气，并每 60 秒 ping 一次桥接服务；结果缓存到下面全局变量。
// 未配置 key 时（默认编译 / 不带 secret）AI 用量页自动跳过，不影响其它三页。
static long g_usage_balance_cents = -1;   // 余额，单位分（-1=尚未拉取/失败）
static long g_usage_today_tokens = -1;    // 今日 token 数
static int  g_usage_today_requests = -1;  // 今日请求数
static bool g_usage_valid = false;
static int  g_weather_temp_c = 0;         // 当前温度（摄氏度）
static int  g_weather_code = -1;          // WMO 天气代码
static bool g_weather_valid = false;
static bool g_service_online = false;     // 桥接服务可达（BRIDGE_HOST:BRIDGE_PORT）
static int  g_status_page = 0;            // 0=时钟 1=服务状态 2=天气 3=AI 用量

// 各页上次显示值，避免 50ms 空闲循环里重复刷 LVGL；切页时统一重置。
static long s_last_bal = -2, s_last_tok = -2;
static bool s_last_valid = false;
static int  s_last_temp = 999, s_last_code = -999;
static bool s_last_wx_valid = false;
static bool s_last_service = false;
static bool s_last_service_shown = false;

static void bridge_status_invalidate(void) {
    s_last_bal = -2; s_last_tok = -2; s_last_valid = false;
    s_last_temp = 999; s_last_code = -999; s_last_wx_valid = false;
    s_last_service = false; s_last_service_shown = false;
    g_last_clock_min = -1;
}

// 天气代码 → 中文短描述（顶栏单行，控制在 4 字内）。
// wttr.in 的 weatherCode 基于 WMO：0-99 标准；霾类扩展码(>100)按 wttr.in 常见值补。
static const char* weather_text(int code) {
    if (code == 0) return "晴";
    if (code == 1) return "少云";
    if (code == 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if (code >= 51 && code <= 57) return "毛毛雨";
    if (code >= 61 && code <= 67) return "雨";
    if (code >= 71 && code <= 77) return "雪";
    if (code >= 80 && code <= 82) return "阵雨";
    if (code >= 85 && code <= 86) return "阵雪";
    if (code == 95) return "雷雨";
    if (code >= 96) return "雷暴";
    // wttr.in 扩展码：霾/沙尘等（如 149=Smoky haze）。
    if (code >= 100 && code <= 149) return "霾";
    return "天气";
}

static void usage_task(void* arg) {
    (void)arg;
    // 开机后等网络与 SNTP 起来（bridge_main 已启动 SNTP）再拉第一次，避免 DNS 未就绪。
    vTaskDelay(pdMS_TO_TICKS(20000));
    int minute_tick = 0;  // 每 60s 探一次服务；每 15 分钟拉一次用量/天气
    for (;;) {
        // 服务状态：只探桥接服务本身（局域网，毫秒级），失败不影响其它页。
        bool svc = bridge_ping();
        if (svc != g_service_online) {
            g_service_online = svc;
            ESP_LOGI(TAG, "bridge service %s", svc ? "ONLINE" : "OFFLINE");
        }
        if (minute_tick == 0) {
            long bal = -1, tok = -1;
            int req = -1;
            if (bridge_fetch_usage(&bal, &tok, &req)) {
                g_usage_balance_cents = bal;
                g_usage_today_tokens = tok;
                g_usage_today_requests = req;
                g_usage_valid = true;
            } else {
                g_usage_valid = false;  // 拉取失败：用量页显示"用量暂不可用"，不影响其它页
            }
            int temp = 0, code = -1;
            if (bridge_fetch_weather(&temp, &code)) {
                g_weather_temp_c = temp;
                g_weather_code = code;
                g_weather_valid = true;
            } else {
                g_weather_valid = false;
            }
        }
        if (++minute_tick >= BRIDGE_USAGE_REFRESH_MINUTES) minute_tick = 0;
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    }
}

static void bridge_clock_refresh(bool force) {
    if (g_state != ST_IDLE) return;  // 只在空闲显示时钟
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 2025 - 1900) {
        // 时间还没对上（SNTP 未完成）：显示“待命”，不再是“正在初始化”。
        if (force || g_last_clock_min != -2) {
            g_last_clock_min = -2;
            bridge_hal_set_status("待命");
        }
        return;
    }
    int cur_min = tm_now.tm_hour * 60 + tm_now.tm_min;
    // 分钟没变不重复刷；但刚从“待命”切过来时（g_last_clock_min==-2，
    // 时间刚对上），cur_min != -2，一定会刷，时钟在对时后 50ms 内即出现。
    if (!force && cur_min == g_last_clock_min) return;
    g_last_clock_min = cur_min;
    char time_str[16];
    char date_str[32];
    strftime(time_str, sizeof(time_str), "%H:%M", &tm_now);
    strftime(date_str, sizeof(date_str), "%m月%d日", &tm_now);
    char buf[64];
    // 例："14:35 11月17日 周二"。状态栏宽 270px，这串约 170px，单行放得下，不滚动。
    snprintf(buf, sizeof(buf), "%s %s %s", time_str, date_str, kWeekdayZh[tm_now.tm_wday]);
    bridge_hal_set_status(buf);
}

// IDLE 顶栏统一入口：页面由中键短按手动切换，这里只负责「当前页」的显示与重绘。
// force=true 表示立即重绘（切页 / 回 IDLE），不重置页码。
static void bridge_status_update(bool force) {
    if (g_state != ST_IDLE) return;

    if (g_status_page == 0) {
        bridge_clock_refresh(force);
        return;
    }

    char buf[64];

    // 页 1：服务状态。usage_task 每 60 秒 ping 一次桥接服务。
    if (g_status_page == 1) {
        if (force || !s_last_service_shown || g_service_online != s_last_service) {
            s_last_service = g_service_online;
            s_last_service_shown = true;
            snprintf(buf, sizeof(buf), "服务%s %s",
                     g_service_online ? "在线" : "离线", BRIDGE_HOST);
            bridge_hal_set_status(buf);
        }
        return;
    }

    // 页 2：天气。
    if (g_status_page == 2) {
        if (force || g_weather_valid != s_last_wx_valid ||
            (g_weather_valid && (g_weather_temp_c != s_last_temp || g_weather_code != s_last_code))) {
            s_last_temp = g_weather_temp_c;
            s_last_code = g_weather_code;
            s_last_wx_valid = g_weather_valid;
            if (g_weather_valid) {
                snprintf(buf, sizeof(buf), "海安 %d°C %s", g_weather_temp_c, weather_text(g_weather_code));
            } else {
                snprintf(buf, sizeof(buf), "天气暂不可用");
            }
            bridge_hal_set_status(buf);
        }
        return;
    }

    // 页 3：AI 用量（无 key 时切屏逻辑会跳过，不会进到这里）。
    if (force || g_usage_valid != s_last_valid ||
        (g_usage_valid && (g_usage_balance_cents != s_last_bal || g_usage_today_tokens != s_last_tok))) {
        s_last_bal = g_usage_balance_cents;
        s_last_tok = g_usage_today_tokens;
        s_last_valid = g_usage_valid;
        if (g_usage_valid && g_usage_balance_cents >= 0) {
            long today_k = g_usage_today_tokens >= 0 ? g_usage_today_tokens / 1000 : 0;
            snprintf(buf, sizeof(buf), "$%ld.%02ld 今日%ldk tok",
                     g_usage_balance_cents / 100, g_usage_balance_cents % 100, today_k);
        } else {
            snprintf(buf, sizeof(buf), "用量暂不可用");
        }
        bridge_hal_set_status(buf);
    }
}

// 统一改状态：换表情 + 记一次活动 + 更新状态栏。
static void bridge_set_state(bridge_state_t s) {
    g_state = s;
    if (s == ST_RECORDING) g_reply_shown = false;  // 新对话：旧回复字幕不再停留
    bridge_hal_set_emotion(emotion_for_state(s));
    // 状态栏：空闲显示时钟，对话中显示当前阶段。替代卡住的“正在初始化”。
    // 注意：这里只改顶部状态条；表情（上）和字幕（下）由各自的调用负责，不受影响。
    switch (s) {
        case ST_IDLE:
            // 对话结束回到首页（聊天/时钟）；手动切到的页不跨对话保留。
            g_status_page = 0;
            bridge_status_invalidate();
            bridge_status_update(true);
            break;
        case ST_RECORDING:
            bridge_hal_set_status("聆听中");
            break;
        case ST_UPLOADING:
            bridge_hal_set_status("上传中");
            break;
        case ST_WAITING:
            bridge_hal_set_status("等待回复");
            break;
        case ST_PLAYING:
            bridge_hal_set_status("播放中");
            break;
    }
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
} bridge_key_t;

static bridge_key_t g_keys[3] = {
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
            bridge_key_t* k = &g_keys[i];
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
                        // 松开：按按住时长分档触发。中键三档，上下键维持两档。
                        TickType_t held_ms =
                            (xTaskGetTickCount() - k->press_tick) * portTICK_PERIOD_MS;
                        if (i == 1) {
                            // 中键：<800ms 切屏；800ms~3s 配网；>=3s 已在按住时重启。
                            if (held_ms < KEY_LONG_PRESS_MS) {
#if !BRIDGE_KEY_LOG_ONLY
                                on_key_short(i);
#endif
                            } else if (held_ms < KEY_RESTART_HOLD_MS && !k->long_fired) {
#if !BRIDGE_KEY_LOG_ONLY
                                on_key_long(i);
#endif
                            }
                        } else if (!k->long_fired && held_ms < KEY_LONG_PRESS_MS) {
#if !BRIDGE_KEY_LOG_ONLY
                            on_key_short(i);
#endif
                        }
                    }
                }
            } else {
                pending_level[i] = level;
                k->debounce_count = 0;
                // 按住中：上下拨 800ms 触发音量；中键要满 3 秒才直接重启，
                // 800ms~3s 之间不动作，松开时由上面分支进入配网。
                if (level == 0 && !k->long_fired) {
                    TickType_t held_ms =
                        (xTaskGetTickCount() - k->press_tick) * portTICK_PERIOD_MS;
                    TickType_t threshold = (i == 1) ? KEY_RESTART_HOLD_MS : KEY_LONG_PRESS_MS;
                    if (held_ms >= threshold) {
                        k->long_fired = true;
                        log_raw_levels(k->name);
#if !BRIDGE_KEY_LOG_ONLY
                        if (i == 1) {
                            ESP_LOGI(TAG, "key PRESS held 3s: restart");
                            bridge_hal_restart();
                        } else {
                            on_key_long(i);
                        }
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
    } else {  // 中间短按：循环切屏（聊天/时钟 → 服务状态 → 天气 → AI 用量 → 回聊天）
        bool has_key = (BRIDGE_USAGE_KEY[0] != '\0');
        int next = g_status_page + 1;
        if (next == 3 && !has_key) next = 0;  // 没配 key：跳过 AI 用量页
        if (next > 3) next = 0;
        g_status_page = next;
        bridge_status_invalidate();
        bridge_status_update(true);  // 非 IDLE 时内部直接返回，回 IDLE 后显示新页
        ESP_LOGI(TAG, "key PRESS short: status page %d", g_status_page);
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
        ESP_LOGI(TAG, "key PRESS long 800ms~3s: enter wifi config");
        bridge_hal_enter_wifi_config();
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
                    g_reply_shown = true;
                    g_reply_shown_tick = xTaskGetTickCount();
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
                    // 播完不立刻清字幕：回复保留 BRIDGE_SUBTITLE_HOLD_MINUTES 分钟，
                    // 到时由 IDLE 分支清回"你好MOSS"；新对话会直接替换。
                    bridge_set_state(ST_IDLE);
                }
                g_stop_playback = false;
            }
            // 若按键在 WAITING/PLAYING 期间把状态改成了 RECORDING/IDLE，上面分支
            // 已按 g_state 现值处理，不再强制覆盖。
        } else {
            // IDLE / UPLOADING（上传在 RECORDING 分支里同步做完，不会停留）
            // 字幕停留：回复播完后保留几分钟再消失（回"你好MOSS"）；新对话会替换它。
            if (BRIDGE_SUBTITLE_HOLD_MINUTES > 0 && g_state == ST_IDLE && g_reply_shown) {
                TickType_t shown_ms =
                    (xTaskGetTickCount() - g_reply_shown_tick) * portTICK_PERIOD_MS;
                if (shown_ms >= (TickType_t)BRIDGE_SUBTITLE_HOLD_MINUTES * 60 * 1000) {
                    g_reply_shown = false;
                    bridge_hal_show_text("你好MOSS");
                    ESP_LOGI(TAG, "subtitle hold %d min expired, back to greeting",
                             BRIDGE_SUBTITLE_HOLD_MINUTES);
                }
            }
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
            // 空闲顶栏：显示中键短按选中的当前页；bridge_status_update 内部判刷新时机。
            // 屏幕已关（自动睡眠）时也照常更新 label，唤醒后即当前值。
            bridge_status_update(false);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void bridge_app_start(void) {
    xTaskCreate(key_task, "bridge_keys", 8192, NULL, 5, NULL);  // 栈加深：中键长按会在本任务里同步开配网 AP
    xTaskCreate(main_task, "bridge_main", 8192, NULL, 5, NULL);
    // 后台探测任务：每 60s ping 桥接服务；每 15 分钟拉天气/用量（无 key 时用量内部跳过）。
    xTaskCreate(usage_task, "bridge_usage", 8192, NULL, 5, NULL);
}

int bridge_app_volume(void) {
    return g_volume;
}
