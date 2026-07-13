#ifndef PTT_HANDLER_H
#define PTT_HANDLER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif
    extern TimerHandle_t g_idle_timer;

    /**
     * @brief Inicializa el botón de muteo usando BSP.
     * Registra el callback para el evento de presionar el botón MUTE.
     */
    void mute_handler_init(void);
    /**
     * @brief Inicia o resetea el timer de inactividad.
     * Cuando el timer expire, se enviará un prompt al sistema.
     */
    void mute_handler_start_idle_timer(void);
    /**
     * @brief Detiene el timer de inactividad.
     * Se debe llamar cuando el usuario desmutea el micrófono.
     */
    void mute_handler_stop_idle_timer(void);

#ifdef __cplusplus
}
#endif

#endif /* PTT_HANDLER_H */