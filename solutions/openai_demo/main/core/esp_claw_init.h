#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char device[16];
    char action[16];
} esp_claw_msg_t;

#define MAX_CONDITIONS 8
#define MAX_ACTIONS 16

typedef enum {
    VAL_TYPE_NUMBER,
    VAL_TYPE_STRING,
    VAL_TYPE_BOOL
} rule_val_type_t;

typedef struct {
    char sensor[24];
    char op[4];            // e.g., ">", "<", "=="
    
    rule_val_type_t val_type;
    union {
        float f_val;
        bool b_val;
        char s_val[24];
    };
} rule_condition_t;

typedef struct {
    char target[256];       // e.g., "tv.power"
} rule_action_t;

typedef struct esp_claw_rule_t {
    char call_id[128];
    char trigger[32];
    
    rule_condition_t conditions[MAX_CONDITIONS];
    uint8_t num_conditions;
    
    rule_action_t actions[MAX_ACTIONS];
    uint8_t num_actions;
    
    struct esp_claw_rule_t* next; // Support for batching multiple rules in one IPC message
} esp_claw_rule_t;

typedef struct {
    bool success;
    char payload[512];
} esp_claw_response_t;

esp_err_t esp_claw_init(void);
esp_err_t esp_claw_execute_script(const char* script);
esp_err_t esp_claw_send_command(const char* device, const char* action);
esp_err_t esp_claw_send_rule(esp_claw_rule_t* rule);
esp_err_t esp_claw_request_list(char* out_buffer, size_t max_len);
esp_err_t esp_claw_request_delete(const char* trigger, char* out_buffer, size_t max_len);
void esp_claw_signal_safe_to_start(void);
bool esp_claw_is_automation_ready(void);
bool esp_claw_is_fs_corrupted(void);

#ifdef __cplusplus
}
#endif
