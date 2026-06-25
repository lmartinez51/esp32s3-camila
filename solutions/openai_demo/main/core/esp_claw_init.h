#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char device[16];
    char action[16];
} esp_claw_msg_t;

esp_err_t esp_claw_init(void);
esp_err_t esp_claw_execute_script(const char* script);
esp_err_t esp_claw_send_command(const char* device, const char* action);

#ifdef __cplusplus
}
#endif
