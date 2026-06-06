/* Common header

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

#include "settings.h"
#include "media_sys.h"
#include "network.h"
#include "sys_state.h"
#include "esp_webrtc.h"

   /**
    * @brief  Initialize board
    */
   void init_board(void);

   /**
    * @brief  Get OpenAI signaling implementation
    *
    * @return
    *      - NULL    Not enough memory
    *      - Others  OpenAI signaling implementation
    */
   const esp_peer_signaling_impl_t *esp_signaling_get_openai_signaling(void);

   /**
    * @brief  Start WebRTC
    *
    * @return
    *      - 0       On success
    *      - Others  Fail to start
    */
   int start_webrtc(void);

   /**
    * @brief  Query WebRTC status
    */
   void query_webrtc(void);

   /**
    * @brief  Start WebRTC
    *
    * @param[in]  url  Signaling URL
    *
    * @return
    *      - 0       On success
    *      - Others  Fail to start
    */
   int stop_webrtc(void);

#ifdef __cplusplus
}
#endif
