#include "config_manager.h"
#include "nvs_setup.h" // Para leer/borrar de la NVS
#include "settings.h"  // Para la llave hardcodeada
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include "nvs_setup.h" // Para la validación de la API Key

static const char *TAG = "CONFIG_MGR";

// --- Variables Internas del Módulo ---
// Guardan el estado actual de nuestro "llavero"
static api_key_source_t s_current_key_source = KEY_SOURCE_UNINITIALIZED;
static char s_nvs_api_key[256]; // Un buffer para guardar la llave que leamos de la NVS
static char g_cached_api_key[256] = {0}; // T4 FIX: RAM Interna

static bool has_hardcoded_api_key(void)
{
    return OPENAI_API_KEY[0] != '\0';
}

void config_manager_init(void)
{
    // Si no hay llave de desarrollo compilada, arrancamos directamente desde NVS.
    s_current_key_source = has_hardcoded_api_key() ? KEY_SOURCE_HARDCODED : KEY_SOURCE_NVS;
    memset(s_nvs_api_key, 0, sizeof(s_nvs_api_key)); // Limpiamos el buffer
    
    // T4 FIX: Precargar la API Key para tareas en PSRAM
    const char *initial_key = config_manager_get_current_api_key();
    if (initial_key != NULL) {
        strncpy(g_cached_api_key, initial_key, sizeof(g_cached_api_key) - 1);
    }

    ESP_LOGI(TAG, "Llavero Inteligente inicializado. Fuente inicial: %d", s_current_key_source);
}

const char *config_manager_get_cached_api_key(void)
{
    return g_cached_api_key;
}

const char *config_manager_get_current_api_key(void)
{
    esp_err_t err;
    switch (s_current_key_source)
    {
    case KEY_SOURCE_HARDCODED:
        if (!has_hardcoded_api_key())
        {
            ESP_LOGW(TAG, "No hay API Key hardcodeada. Cambiando a NVS.");
            s_current_key_source = KEY_SOURCE_NVS;
            return config_manager_get_current_api_key();
        }
        ESP_LOGI(TAG, "Proporcionando API Key HARDCODEADA.");
        return OPENAI_API_KEY; // Devuelve la llave de settings.h

    case KEY_SOURCE_NVS:
        ESP_LOGI(TAG, "Intentando leer API Key desde la NVS...");
        memset(s_nvs_api_key, 0, sizeof(s_nvs_api_key)); // Limpiamos el buffer
        err = nvs_load_api_key(s_nvs_api_key, sizeof(s_nvs_api_key));

        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "✅ API Key de NVS cargada exitosamente");
            return s_nvs_api_key;
        }
        else
        {
            ESP_LOGE(TAG, "❌ No se pudo cargar API Key de NVS: %s", esp_err_to_name(err));

            // Si no existe o es inválida, no hay más fuentes disponibles.
            if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NVS_NOT_FOUND)
            {
                ESP_LOGE(TAG, "   API Key de NVS no disponible. Cambiando a KEY_SOURCE_NONE_AVAILABLE");
                s_current_key_source = KEY_SOURCE_NONE_AVAILABLE;
            }

            return NULL;
        }

    default: // Incluye UNINITIALIZED y NONE_AVAILABLE
        ESP_LOGE(TAG, "No hay más API Keys disponibles para probar.");
        return NULL;
    }
}

/**
 * @brief Maneja el fallo de autenticación de una API Key.
 *
 * Esta función implementa una estrategia de fallback entre dos fuentes de API Keys:
 * 1. HARDCODED: Key predefinida en el código (fallback de emergencia)
 * 2. NVS: Key proporcionada por el usuario vía BLE
 *
 * Flujo de operación:
 * - Si falla HARDCODED → Intenta con NVS en el próximo ciclo
 * - Si falla NVS → Borra la key inválida y entra en modo configuración
 *
 * @note Esta función debe llamarse SOLO cuando se confirme que la API Key
 *       es inválida (no por errores de red temporales)
 */
void config_manager_on_api_key_failure(void)
{
    if (s_current_key_source == KEY_SOURCE_HARDCODED)
    {
        ESP_LOGW(TAG, "⚠️ Falló la API Key HARDCODEADA.");
        ESP_LOGI(TAG, "🔄 Cambiando a API Key de NVS para el próximo intento...");
        s_current_key_source = KEY_SOURCE_NVS;
    }
    else if (s_current_key_source == KEY_SOURCE_NVS)
    {
        ESP_LOGE(TAG, "❌ Falló la API Key de NVS (probablemente caducada, inválida o mal escrita).");
        ESP_LOGW(TAG, "🗑️ Eliminando API Key inválida de NVS...");

        esp_err_t err = nvs_delete_api_key();
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "✅ API Key eliminada. Sistema entrará en modo configuración.");
        }
        else
        {
            ESP_LOGE(TAG, "❌ No se pudo eliminar la API Key de NVS: %s", esp_err_to_name(err));
            ESP_LOGW(TAG, "   El dispositivo puede intentar usar la misma key inválida nuevamente.");
        }

        s_current_key_source = KEY_SOURCE_NONE_AVAILABLE;

        // Opcional: Notificar a la UI que se requiere nueva configuración
        // ui_show_config_required_message();
    }
    else
    {
        ESP_LOGW(TAG, "⚠️ config_manager_on_api_key_failure() llamada sin fuente válida (estado: %d)",
                 s_current_key_source);
    }
}

api_key_source_t config_manager_get_current_source(void)
{
    return s_current_key_source;
}

/**
 * @brief Lee y valida la API Key desde NVS antes de intentar usarla
 */
bool config_manager_try_load_api_key(char *out_buffer, size_t buffer_size)
{
    ESP_LOGI(TAG, "Intentando leer API Key desde la NVS...");

    esp_err_t err = nvs_load_api_key(out_buffer, buffer_size);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "No se pudo leer API Key de NVS: %s", esp_err_to_name(err));
        return false;
    }

    // ⬇️ NUEVA VALIDACIÓN ANTES DE USAR LA KEY
    ESP_LOGI(TAG, "API Key leída de NVS (%zu caracteres). Validando formato...", strlen(out_buffer));

    err = validate_openai_api_key(out_buffer); // Usar la función que creamos antes

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "❌ La API Key en NVS es INVÁLIDA. Borrándola automáticamente...");
        nvs_delete_api_key();
        return false;
    }

    ESP_LOGI(TAG, "✅ API Key de NVS validada correctamente.");
    return true;
}
