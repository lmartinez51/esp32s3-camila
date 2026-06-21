#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "media_sys.h"
#include "esp_rom_sys.h"
#include <stdint.h>
#include "ui.h"
#include "simi.h"
#include "freertos/timers.h"
#include "mute_handler.h"
#include "webrtc.h"
#include "app_events.h"

#define TAG "MUTE_HANDLER"
#define IDLE_TIMEOUT_MS (14 * 60 * 1000)
#define MUTE_BUTTON_GPIO GPIO_NUM_1

typedef enum
{
    MUTE_STATE_UNMUTED = 0,
    MUTE_STATE_MUTED
} mute_state_t;

typedef struct
{
    mute_state_t state;
} mute_event_t;

static QueueHandle_t mute_evt_queue = NULL;
static volatile bool mic_muted = false;
static bool s_idle_warning_sent = false;
TimerHandle_t g_idle_timer = NULL;
static void vIdleTimerCallback(TimerHandle_t xTimer);

/**
 * @brief ISR handler for the mute button.
 *
 * This function is called when the mute button is pressed or released.
 * It debounces the button press and sends a mute event to the queue.
 *
 * @param arg Unused parameter
 */
static void IRAM_ATTR mute_isr_handler(void *arg)
{
    static uint32_t last_isr_time = 0;
    uint32_t now = xTaskGetTickCountFromISR();
    if ((now - last_isr_time) < pdMS_TO_TICKS(200))
        return; // debounce
    last_isr_time = now;

    mute_event_t evt = {
        .state = gpio_get_level(MUTE_BUTTON_GPIO) == 0 ? MUTE_STATE_MUTED : MUTE_STATE_UNMUTED};
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(mute_evt_queue, &evt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Task to handle mute events.
 *
 * This task listens for mute events from the queue and updates the microphone state accordingly.
 * It will mute or unmute the microphone based on the received event.
 *
 * @param arg Unused parameter
 */
static void mute_evt_task(void *arg)
{
    mute_event_t evt;
    while (1)
    {
        if (xQueueReceive(mute_evt_queue, &evt, portMAX_DELAY))
        {
            bool new_state = (evt.state == MUTE_STATE_MUTED);

            if (mic_muted != new_state)
            {
                mic_muted = new_state;
                
                if (mic_muted)
                {
                    ESP_LOGI(TAG, "Physical mute button pressed (MUTED)");
                }
                else
                {
                    ESP_LOGI(TAG, "Physical mute button pressed (UNMUTED)");
                }

                // Delegate media control and UI to the central orchestrator
                orchestrator_post_mute_state(mic_muted);
            }
        }
    }
}

/**
 * @brief Initializes the mute handler.
 *
 * Sets up the GPIO for the mute button, creates a queue for mute events,
 * and starts the mute event task. Also registers the ISR handler for the mute button.
 */
void mute_handler_init(void)
{
    ESP_LOGI(TAG, "Inicializando boton fisico de mute (GPIO)");

    gpio_config_t btn_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << MUTE_BUTTON_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 1};
    gpio_config(&btn_conf);

    mute_evt_queue = xQueueCreate(4, sizeof(mute_event_t));
    if (!mute_evt_queue)
    {
        ESP_LOGE(TAG, "No se pudo crear la cola de eventos de mute");
        return;
    }

    xTaskCreate(mute_evt_task, "mute_evt_task", 4096, NULL, 10, NULL);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(MUTE_BUTTON_GPIO, mute_isr_handler, NULL);

    ESP_LOGI(TAG, "Callback de mute registrado en GPIO %d", MUTE_BUTTON_GPIO);
    // Crear el timer de inactividad (one-shot)
    ESP_LOGI(TAG, "Creando timer de inactividad (%d minutos)...", IDLE_TIMEOUT_MS / 60000);
    g_idle_timer = xTimerCreate(
        "IdleTimer",                    // Nombre (para debug)
        pdMS_TO_TICKS(IDLE_TIMEOUT_MS), // Periodo (14 min)
        pdFALSE,                        // pdFALSE = No es auto-reload, es "one-shot"
        (void *)0,                      // ID del timer (no lo usamos)
        vIdleTimerCallback              // La función a llamar
    );

    if (g_idle_timer == NULL)
    {
        ESP_LOGE(TAG, "¡Fallo al crear el timer de inactividad!");
    }
}

static void vIdleTimerCallback(TimerHandle_t xTimer)
{
    if (!s_idle_warning_sent)
    {
        ESP_LOGW(TAG, "¡Timer de inactividad expiró después de 14 minutos de mute! Avisando al usuario...");
        s_idle_warning_sent = true;

        // Enviar el prompt al sistema de IA para resetear el timer de OpenAI
        webrtc_post_action(WEBRTC_ACTION_PLAY_IDLE_ALERT);

        // Cambiar el periodo del timer a 2 minutos para la etapa de gracia
        xTimerChangePeriod(g_idle_timer, pdMS_TO_TICKS(2 * 60 * 1000), 100);
    }
    else
    {
        ESP_LOGE(TAG, "¡Pasaron 2 minutos desde el aviso y no hubo respuesta! A dormir...");
        // Disparar el evento de auto-sleep forzado
        orchestrator_post_event(ORCH_EVENT_AUTO_SLEEP_TIMEOUT);
    }
}

/**
 * @brief Inicia o reinicia el timer de inactividad de 14 minutos.
 * Se debe llamar CADA VEZ que el dispositivo entra en estado MUTE.
 */
void mute_handler_start_idle_timer(void)
{
    if (g_idle_timer != NULL)
    {
        s_idle_warning_sent = false;
        // Reiniciar el timer con su duración original de 14 minutos
        xTimerChangePeriod(g_idle_timer, pdMS_TO_TICKS(IDLE_TIMEOUT_MS), 100);
    }
}

/**
 * @brief Detiene el timer de inactividad.
 * Se debe llamar CADA VEZ que el dispositivo sale del estado MUTE.
 */
void mute_handler_stop_idle_timer(void)
{
    if (g_idle_timer != NULL && xTimerIsTimerActive(g_idle_timer))
    {
        ESP_LOGI(TAG, "Deteniendo timer de inactividad (usuario hizo unmute).");
        xTimerStop(g_idle_timer, pdMS_TO_TICKS(100));
    }
}
