// 桥接版入口 —— 替代上游 main.cc（该变体构建时从 SOURCES 移除 main.cc）。
// 引导链：nvs → 板级构造（屏/codec/背光/hal 绑定）→ SetupUI（圆屏几何修正）
//        → Assets::Apply（表情/字体沿用分区现状）→ StartNetwork → bridge_app。
// 不跑 Application::Run()：小智 MQTT 对话链路/唤醒词在桥接版整体停用。
#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <stdlib.h>
#include <time.h>

#include "esp_netif_sntp.h"

#include "board.h"
#include "application.h"
#include "assets.h"
#include "display.h"
#include "bridge_hal.h"
#include "bridge_app.h"
#include "bridge_config.h"

#define TAG "bridge_main"

// ---------------- 时间同步（2026-11-17）：SNTP 对时 + CST-8 时区 ----------------
// 根因：桥接版不跑小智 OTA，而上游唯一的时间来源就是 OTA 的 server_time
// （ota.cc 用 settimeofday 设置系统时间）。所以桥接版的系统时间从未被设置过，
// 一直是 1970-01-01 + 开机时长。叠加桥接版把小智状态机钉在 kDeviceStateStarting，
// 上游 LvglDisplay::UpdateStatusBar() 里的空闲时钟（要求 kDeviceStateIdle）
// 永远不触发，于是 status_label_ 停留在 SetupUI 时的“正在初始化...”不动。
// 修复：在 WiFi 连上后启动 SNTP 对时，时区 Asia/Shanghai（UTC+8，无夏令时），
// 空闲时钟由 bridge_app.cc 的 bridge_clock_refresh() 驱动（见该文件）。
static bool g_time_sync_started = false;

static void bridge_time_sync_start(void) {
    if (g_time_sync_started) return;
    g_time_sync_started = true;
    // 时区：Asia/Shanghai。POSIX TZ 里 "CST-8" 表示本地 = UTC+8（符号与日常写法相反）。
    setenv("TZ", "CST-8", 1);
    tzset();
    // SNTP：只配一个服务器（ntp.aliyun.com，国内快）。ESP_NETIF_SNTP_DEFAULT_CONFIG
    // 用单服务器，避免 CONFIG_LWIP_SNTP_MAX_SERVERS 默认只有 1 个名额时数组越界。
    // start=true + wait_for_sync=true：后台自动对时，对上后系统时间即被设置；
    // 对不上时 bridge_clock_refresh() 会显示“待命”而不是错误时间。
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed: %s (clock stays at 待命 until time syncs)",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com, TZ=CST-8)");
    }
}

extern "C" void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    display->SetupUI();
    Assets::GetInstance().Apply();

    // WifiBoard::EnterWifiConfigMode() 只在 Starting 态直接开配网 AP，
    // 这里把小智状态机钉在 Starting（不跑 Run()，它不会自己往前走）。
    Application::GetInstance().SetDeviceState(kDeviceStateStarting);

    // 配网成功路径会把状态改成 WifiConfiguring 且不再复位（复位原本由小智
    // Application 事件循环做，桥接版没有）。连上网后钉回 Starting，保证
    // 「中间短按 = 再次进配网」在整个生命周期可用。
    // 连上网后同时启动 SNTP 对时（bridge_time_sync_start，内部只执行一次）。
    board.SetNetworkEventCallback([](NetworkEvent event, const std::string&) {
        if (event == NetworkEvent::Connected) {
            Application::GetInstance().SetDeviceState(kDeviceStateStarting);
            bridge_time_sync_start();
        }
    });

    board.StartNetwork();

    bridge_hal_init();
    bridge_app_start();
    ESP_LOGI(TAG, "huanzhu bridge boot ok (host=%s port=%d, key_log_only=%d)",
             BRIDGE_HOST, BRIDGE_PORT, BRIDGE_KEY_LOG_ONLY);
}
