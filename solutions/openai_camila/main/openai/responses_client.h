#ifndef RESPONSES_CLIENT_H
#define RESPONSES_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initializes the persistent PSRAM Queue Worker for HTTP queries.
 * Must be called once during system boot, after NVS API Key fetch.
 */
void http_worker_init(void);

/**
 * @brief Dispatches a query to the persistent Queue Worker.
 * 
 * @param query The exact product query string from the user.
 * @param call_id The WebRTC function call ID to respond to.
 */
void start_lookup_product_task(const char *query, const char *call_id);

#ifdef __cplusplus
}
#endif

#endif // RESPONSES_CLIENT_H
