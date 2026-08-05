/**
 * @file csi_handler.c
 * @brief CSI Receiver stub. CSI receiver and ML inference are disabled on Beacon-only node (Camila).
 */

#include "csi_handler.h"

esp_err_t csi_handler_start(void)
{
    return ESP_OK;
}

void csi_handler_stop(void)
{
}
