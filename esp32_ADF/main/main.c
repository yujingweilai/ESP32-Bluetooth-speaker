/*
 * Copyright (c) 2020 Baidu.com, Inc. All Rights Reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License is distributed on
 * an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "app_control.h"
#include "app_sys_tools.h"
#include "display_service.h"
#include "audio_mem.h"

#include "audio_idf_version.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

#define TAG "MAIN"


void app_main(void)
{
    esp_err_t ret  = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());    
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
// #if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
//     ESP_ERROR_CHECK(esp_netif_init());
// #else
//     tcpip_adapter_init();
// #endif


    start_sys_monitor();
    app_enty();



}
