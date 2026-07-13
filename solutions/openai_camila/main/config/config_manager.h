#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdbool.h>
#include <stddef.h> // Para size_t

#ifdef __cplusplus
extern "C"
{
#endif

    // Este enum nos ayudará a saber de dónde estamos intentando sacar la llave.
    typedef enum
    {
        KEY_SOURCE_UNINITIALIZED,
        KEY_SOURCE_HARDCODED,     // La llave del archivo settings.h
        KEY_SOURCE_NVS,           // La llave guardada en la memoria NVS
        KEY_SOURCE_NONE_AVAILABLE // Cuando ya probamos todas y ninguna funcionó
    } api_key_source_t;

    /**
     * @brief Inicializa el manejador de configuración.
     * Debe llamarse una sola vez al arrancar el programa.
     */
    void config_manager_init(void);

    /**
     * @brief Obtiene la API Key que se debe usar en el intento de conexión actual.
     * @return Un puntero a la API Key (const char*), o NULL si no hay más llaves que probar.
     */
    const char *config_manager_get_current_api_key(void);

    /**
     * @brief Se debe llamar cuando la conexión con la API Key actual falla.
     * Prepara al manejador para que en el siguiente intento use la llave de respaldo.
     */
    void config_manager_on_api_key_failure(void);

    /**
     * @brief Devuelve la fuente de la llave actual que se está intentando.
     * Útil para saber en qué paso del proceso de reintento nos encontramos.
     */
    api_key_source_t config_manager_get_current_source(void);

    /**
     * @brief Intenta leer y validar la API Key desde NVS.
     * @param out_buffer Buffer donde se copiará la API Key leída (debe ser al menos 128 bytes).
     * @param buffer_size Tamaño del buffer proporcionado.
     * @return true si se leyó y validó correctamente, false en caso de error o key inválida.
     */
    bool config_manager_try_load_api_key(char *out_buffer, size_t buffer_size);

    /**
     * @brief Obtiene la API Key pre-cargada en la caché global (RAM Interna).
     * Esto evita bloquear el bus SPI y previene deadlocks con la PSRAM.
     * @return Puntero a la caché interna de la API Key.
     */
    const char *config_manager_get_cached_api_key(void);

#ifdef __cplusplus
}
#endif

#endif // CONFIG_MANAGER_H