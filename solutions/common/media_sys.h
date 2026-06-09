/* Media system

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include "esp_webrtc.h"

#ifdef __cplusplus
extern "C"
{
#endif

   /**
    * @brief  Build media system
    *
    * @param[in]  rtc_handle  WebRTC handle
    *
    * @return
    *      - 0       On success
    *      - Others  Fail to build
    */
   int media_sys_buildup(void);

   bool media_sys_is_ready(void);

   /**
    * @brief Tear down the media system built by media_sys_buildup.
    *
    * This function is idempotent and releases the capture/player resources
    * owned by the media system wrapper.
    */
   void media_sys_teardown(void);

   /**
    * @brief  Get media provider
    *
    * @param[out]  provider  Media provider to be returned
    *
    * @return
    *      - 0       On success
    *      - Others  Invalid argument
    */
   int media_sys_get_provider(esp_webrtc_media_provider_t *provider);

   /**
    * @brief  Play captured media directly
    *
    * @return
    *      - 0       On success
    *      - Others  Fail to capture or play
    */
   int test_capture_to_player(void);

   /**
    * @brief  Mute or unmute the microphone
    *
    * @param[in]  mute  If true, mutes the microphone; if false, unmutes it
    *
    * @return
    *      - 0       On success
    *      - Others  Fail to mute/unmute
    */
   bool media_sys_mic_mute(bool mute);

   /**
    * @brief Restart the microphone capture/AEC pipeline without rebuilding WebRTC.
    *
    * This is intended as a small recovery step when capture keeps producing
    * frames but server-side VAD stops detecting local speech after playback.
    *
    * @return
    *      - true    Capture was restarted
    *      - false   Capture was not restarted or an error occurred
    */
   bool media_sys_restart_capture(void);

   /**
    * @brief  Check if WebRTC is running
    *
    * @return
    *      - true    If WebRTC is running
    *      - false   If WebRTC is not running
    */
   // bool media_sys_webrtc_is_running(void);

   /**
    * @brief  Set WebRTC running state
    *
    * @param[in]  running  True to set WebRTC as running, false otherwise
    */
   // void media_sys_set_webrtc_running(bool running);

#ifdef __cplusplus
}
#endif
