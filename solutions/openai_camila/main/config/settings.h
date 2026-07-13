/* General settings

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

/**
 * @brief  Set used board name, see `codec_board` README.md for more details
 */
#define BOARD_NAME "ESP32_S3_BOX_3"

/**
 * @brief  If defined will use OPUS codec
 */
#define WEBRTC_SUPPORT_OPUS

/**
 * @brief  Whether enable data channel
 */
#define DATA_CHANNEL_ENABLED (true)

/**
 * @brief  Optional development WiFi SSID.
 *
 * Leave empty for normal BLE/NVS provisioning.
 */
#define WIFI_SSID ""

/**
 * @brief  Optional development WiFi password.
 *
 * Leave empty for normal BLE/NVS provisioning.
 */
#define WIFI_PASSWORD ""

/**
 * @brief  Optional development OpenAI API key.
 *
 * Leave empty for normal BLE/NVS provisioning. Do not commit real secrets here.
 */
#define OPENAI_API_KEY ""

/**
 * @brief  Set OpenAI Vector Store ID
 */
#define VECTOR_STORE_ID "vs_6a2e3c146a7081918e0a09313ee77e8c"

/**
 * @brief  Set default playback volume
 */
#define DEFAULT_PLAYBACK_VOL (100)

#ifdef __cplusplus
}
#endif
