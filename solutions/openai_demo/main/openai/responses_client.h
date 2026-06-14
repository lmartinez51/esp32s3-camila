#ifndef RESPONSES_CLIENT_H
#define RESPONSES_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *call_id;
    char *query;
} lookup_task_args_t;

/**
 * @brief Launches the FreeRTOS task to query the OpenAI Responses API.
 * 
 * @param query The exact product query string from the user.
 * @param call_id The WebRTC function call ID to respond to.
 */
void start_lookup_product_task(const char *query, const char *call_id);

#ifdef __cplusplus
}
#endif

#endif // RESPONSES_CLIENT_H
