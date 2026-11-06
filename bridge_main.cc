// 桥接版入口 —— 替代上游 main.cc（该变体构建时从 SOURCES 移除 main.cc）。
// 引导链：nvs → 板级构造（屏/codec/背光/hal 绑定）→ SetupUI（圆屏几何修正）
//        → Assets::Apply（表情/字体沿用分区现状）→ StartNetwork → bridge_app。
// 不跑 Application::Run()：小智 MQTT 对话链路/唤醒词在桥接版整体停用。
#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "board.h"
#include "application.h"
#include "assets.h"
#include "display.h"
#include "bridge_hal.h"
#include "bridge_app.h"
#include "bridge_config.h"

#define TAG "bridge_main"

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
    board.SetNetworkEventCallback([](NetworkEvent event, const std::string&) {
        if (event == NetworkEvent::Connected) {
            Application::GetInstance().SetDeviceState(kDeviceStateStarting);
        }
    });

    board.StartNetwork();

    bridge_hal_init();
    bridge_app_start();
    ESP_LOGI(TAG, "huanzhu bridge boot ok (host=%s port=%d, key_log_only=%d)",
             BRIDGE_HOST, BRIDGE_PORT, BRIDGE_KEY_LOG_ONLY);
}
