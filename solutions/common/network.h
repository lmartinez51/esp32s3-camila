/* Network

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <stdbool.h>
#include "esp_err.h" // AÑADIDO: Para el tipo de retorno esp_err_t

#ifdef __cplusplus
extern "C"
{
#endif

   /**
    * @brief  Network connect callback
    */
   typedef int (*network_connect_cb)(bool connected);

   /**
    * @brief  Initialize low-level network stack (run once)
    *
    * @param[in]  cb      Network connect callback (can be NULL)
    *
    * @return
    * - ESP_OK   On success
    * - Others   Fail to initialize
    */
   esp_err_t network_wifi_init(network_connect_cb cb); // MODIFICADO: Nueva función de inicialización

   /**
    * @brief  Connect to WiFi, trying default and then NVS credentials.
    * This is a blocking function.
    *
    * @param[in]  default_ssid      Default Wifi ssid
    * @param[in]  default_password  Default Wifi password
    *
    * @return
    * - true   Network connected
    * - false  Network connection failed
    */
   bool network_wifi_connect_main(const char *default_ssid, const char *default_password); // AÑADIDO: Nueva función principal de conexión

   /**
    * @brief  Check network connected or not
    *
    * @return
    * - true   Network connected
    * - false  Network disconnected
    */
   bool network_is_connected(void);

   // /**
   //  * @brief  Connect wifi manually (asynchronous)
   //  *
   //  * @param[in]  ssid      Wifi ssid
   //  * @param[in]  password  Wifi password
   //  *
   //  * @return
   //  * - 0      On success
   //  * - Others Fail to connect
   //  */
   // int network_connect_wifi(const char *ssid, const char *password);

   /*
    * La función original network_init() ha sido eliminada y reemplazada
    * por network_wifi_init() y network_wifi_connect_main().
    */

#ifdef __cplusplus
}
#endif