/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#pragma once

#include "esp_peer.h"
#include "esp_peer_signaling.h"
#include "esp_capture.h"
#include "av_render.h"
#include "esp_codec_dev.h"
#include "media_lib_os.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * @brief  ESP WebRTC handle
     */
    typedef void *esp_webrtc_handle_t;

    typedef enum
    {
        ESP_WEBRTC_CUSTOM_DATA_VIA_NONE,
        ESP_WEBRTC_CUSTOM_DATA_VIA_SIGNALING,
        ESP_WEBRTC_CUSTOM_DATA_VIA_DATA_CHANNEL,
    } esp_webrtc_custom_data_via_t;

    /**
     * @brief  ESP WebRTC peer connection configuration
     */
    typedef struct
    {
        esp_peer_ice_server_cfg_t *server_lists;      /*!< STUN/Relay server URL lists, can be NULL when get from signaling */
        uint8_t server_num;                           /*!< Number of STUN/Relay server URL */
        esp_peer_ice_trans_policy_t ice_trans_policy; /*!< ICE transport policy */
        esp_peer_audio_stream_info_t audio_info;      /*!< Audio stream information for send */
        esp_peer_video_stream_info_t video_info;      /*!< Video stream information for send */
        esp_peer_media_dir_t audio_dir;               /*!< Audio transmission direction */
        esp_peer_media_dir_t video_dir;               /*!< Video transmission direction */
        bool enable_data_channel;                     /*!< Whether enable data channel */
        bool video_over_data_channel;                 /*!< Whether send and receive data through data channel */
        bool no_auto_reconnect;                       /*!< Disable auto reconnect */
        void *extra_cfg;                              /*!< Extra configuration for peer connection */
        int extra_size;                               /*!< Size of extra configuration */
        int (*on_custom_data)(esp_webrtc_custom_data_via_t via, uint8_t *data, int size, void *ctx);
        void *ctx;
    } esp_webrtc_peer_cfg_t;

    /**
     * @brief  ESP WebRTC signaling configuration
     */
    typedef struct
    {
        char *signal_url; /*!< Signaling server URL */
        void *extra_cfg;  /*!< Extra configuration for special signaling server */
        int extra_size;   /*!< Size of extra configuration */
        void *ctx;        /*!< User context */
    } esp_webrtc_signaling_cfg_t;

    /**
     * @brief  ESP WebRTC configuration
     */
    typedef struct
    {
        const esp_peer_signaling_impl_t *signaling_impl; /*!< Signaling implementation */
        esp_webrtc_signaling_cfg_t signaling_cfg;        /*!< Signaling configuration */
        const esp_peer_ops_t *peer_impl;                 /*!< Peer connection implementation */
        esp_webrtc_peer_cfg_t peer_cfg;                  /*!< Peer connection configuration */
    } esp_webrtc_cfg_t;

    /**
     * @brief  WebRTC event type
     */
    typedef enum
    {
        ESP_WEBRTC_EVENT_NONE = 0,                      /*!< None event */
        ESP_WEBRTC_EVENT_CONNECTED = 1,                 /*!< Connected event */
        ESP_WEBRTC_EVENT_CONNECT_FAILED = 2,            /*!< Connected failed event */
        ESP_WEBRTC_EVENT_DISCONNECTED = 3,              /*!< Disconnected event */
        ESP_WEBRTC_EVENT_DATA_CHANNEL_CONNECTED = 4,    /*!< Data channel connected event */
        ESP_WEBRTC_EVENT_DATA_CHANNEL_DISCONNECTED = 5, /*!< Data channel disconnected event */
    } esp_webrtc_event_type_t;

    /**
     * @brief  WebRTC event
     */
    typedef struct
    {
        esp_webrtc_event_type_t type; /*!< Event type */
        char *body;                   /*!< Event body (maybe NULL) */
    } esp_webrtc_event_t;

    /**
     * @brief  WebRTC media provider
     *
     * @note  Media player and capture system are created from outside.
     *        WebRTC will internally use the capture and player handle to capture media data and do media playback
     */
    typedef struct
    {
        esp_capture_handle_t capture; /*!< Capture system handle */
        av_render_handle_t player;    /*!< Player handle */
    } esp_webrtc_media_provider_t;

    /**
     * @brief  WebRTC event handler
     *
     * @param[in]  event  WebRTC event
     * @param[in]  ctx    User context
     *
     * @return  Status to indicate success or failure
     */
    typedef int (*esp_webrtc_event_handler_t)(esp_webrtc_event_t *event, void *ctx);

    /**
     * @brief  WebRTC event handler
     *
     * @param[in]   event       WebRTC event
     * @param[out]  rtc_handle  WebRTC handle
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     *      - ESP_PEER_ERR_NO_MEM       Not enough memory
     */
    int esp_webrtc_open(esp_webrtc_cfg_t *cfg, esp_webrtc_handle_t *rtc_handle);

    /**
     * @brief  WebRTC set media provider
     *
     * @param[in]  rtc_handle  WebRTC handle
     * @param[in]  provider    Media player and capture provider setting
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     */
    int esp_webrtc_set_media_provider(esp_webrtc_handle_t rtc_handle, esp_webrtc_media_provider_t *provider);

    /**
     * @brief  WebRTC set event handler
     *
     * @param[in]  rtc_handle  WebRTC handle
     * @param[in]  handler     Event handler
     * @param[in]  ctx         Event user context
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     */
    int esp_webrtc_set_event_handler(esp_webrtc_handle_t rtc_handle, esp_webrtc_event_handler_t handler, void *ctx);

    /**
     * @brief  Start WebRTC
     *
     * @param[in]  rtc_handle  WebRTC handle
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     *      - ESP_PEER_ERR_NO_MEM       Not enough memory
     */
    int esp_webrtc_enable_peer_connection(esp_webrtc_handle_t rtc_handle, bool enable);

    /**
     * @brief  Start WebRTC
     *
     * @param[in]  rtc_handle  WebRTC handle
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     *      - Others                    Fail to start
     */
    int esp_webrtc_start(esp_webrtc_handle_t rtc_handle);

    /**
     * @brief  Send customized data
     *
     * @param[in]  rtc_handle  WebRTC handle
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     *      - Others                    Fail to send customized data
     */
    int esp_webrtc_send_custom_data(esp_webrtc_handle_t rtc_handle, esp_webrtc_custom_data_via_t via, uint8_t *data, int size);

    /**
     * @brief  Query status of WebRTC
     *
     * @param[in]  rtc_handle  WebRTC handle
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     */
    int esp_webrtc_query(esp_webrtc_handle_t rtc_handle);

    /**
     * @brief  Get elapsed time since the last received remote audio frame.
     *
     * @param[in]  rtc_handle   WebRTC handle
     * @param[out] age_ms       Elapsed milliseconds since last remote audio frame
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     */
    int esp_webrtc_get_audio_recv_age(esp_webrtc_handle_t rtc_handle, uint32_t *age_ms);

    /**
     * @brief  Stop WebRTC
     *
     * @param[in]  rtc_handle  WebRTC handle
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     *      - Others                    Fail to send customized data
     */
    int esp_webrtc_stop(esp_webrtc_handle_t rtc_handle);

    /**
     * @brief  Close WebRTC
     *
     * @param[in]  rtc_handle  WebRTC handle
     *
     * @return
     *      - ESP_PEER_ERR_NONE         On success
     *      - ESP_PEER_ERR_INVALID_ARG  Invalid argument
     */
    int esp_webrtc_close(esp_webrtc_handle_t rtc_handle);

    typedef struct
    {
        esp_webrtc_cfg_t rtc_cfg;
        esp_peer_handle_t pc;
        esp_peer_signaling_handle_t signaling;
        esp_peer_state_t peer_state;
        bool data_channel_open;
        bool data_channel_connected;
        bool data_channel_create_requested;
        bool data_channel_stream_id_valid;
        uint16_t data_channel_stream_id;
        bool running;
        bool pause;
        media_lib_event_grp_handle_t wait_event;
        esp_webrtc_event_handler_t event_handler;
        esp_peer_role_t ice_role;
        void *ctx;
        // These field will change to esp_media_stream in the near feature

        esp_timer_handle_t send_timer;
        bool send_going;
        esp_webrtc_media_provider_t media_provider;
        esp_capture_path_handle_t capture_path;
        esp_codec_dev_handle_t play_handle;
        esp_peer_audio_stream_info_t recv_aud_info;
        esp_peer_video_stream_info_t recv_vid_info;
        bool pending_connect;
        esp_peer_signaling_ice_info_t ice_info;
        bool ice_info_loaded;
        bool signaling_connected;

        uint8_t *aud_fifo;
        uint32_t aud_fifo_size;
        // For debug only
        uint32_t vid_send_pts;
        uint32_t aud_send_pts;
        uint32_t aud_recv_pts;
        uint64_t last_audio_recv_us;
        uint32_t vid_send_size;
        uint32_t aud_send_size;
        uint32_t aud_recv_size;
        uint32_t vid_recv_size;
        uint8_t aud_send_num;
        uint8_t vid_send_num;
        uint8_t aud_recv_num;
        uint8_t vid_recv_num;
    } webrtc_t;

    extern webrtc_t *rtc_handle;

#ifdef __cplusplus
}
#endif
