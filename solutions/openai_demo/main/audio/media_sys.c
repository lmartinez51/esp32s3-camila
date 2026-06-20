#include "codec_init.h"
#include "codec_board.h"
#include "esp_capture_path_simple.h"
#include "esp_capture_audio_enc.h"
#include "av_render.h"
#include "common.h"
#include "settings.h"
#include "media_lib_os.h"
#include "esp_timer.h"
#include "av_render_default.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_enc_default.h"
#include "esp_capture_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio_render.h"
#include <stdlib.h>

#define RET_ON_NULL(ptr, v)                                        \
    do                                                             \
    {                                                              \
        if (ptr == NULL)                                           \
        {                                                          \
            ESP_LOGE(TAG, "Memory allocate fail on %d", __LINE__); \
            return v;                                              \
        }                                                          \
    } while (0)

#define TAG "MEDIA_SYS"

typedef struct
{
    esp_capture_path_handle_t capture_handle;
    esp_capture_aenc_if_t *aud_enc;
    esp_capture_audio_src_if_t *aud_src;
    esp_capture_path_if_t *path_if;
    esp_capture_path_handle_t primary_path;
    volatile bool mic_muted;
} capture_system_t;

typedef struct
{
    audio_render_handle_t audio_render;
    av_render_handle_t player;
} player_system_t;

static capture_system_t capture_sys;
static player_system_t player_sys;

void media_sys_teardown(void)
{
    if (capture_sys.capture_handle == NULL && player_sys.player == NULL &&
        player_sys.audio_render == NULL && capture_sys.aud_enc == NULL &&
        capture_sys.aud_src == NULL && capture_sys.path_if == NULL)
    {
        return;
    }

    ESP_LOGI(TAG, "Tearing down media system");

    if (capture_sys.capture_handle != NULL)
    {
        int ret = esp_capture_close(capture_sys.capture_handle);
        if (ret != ESP_CAPTURE_ERR_OK)
        {
            ESP_LOGW(TAG, "esp_capture_close returned %d", ret);
        }
        capture_sys.capture_handle = NULL;
        capture_sys.primary_path = NULL;
    }
    else
    {
        if (capture_sys.path_if != NULL && capture_sys.path_if->close != NULL)
        {
            capture_sys.path_if->close(capture_sys.path_if);
        }
        if (capture_sys.aud_src != NULL && capture_sys.aud_src->close != NULL)
        {
            capture_sys.aud_src->close(capture_sys.aud_src);
        }
    }

    if (capture_sys.path_if != NULL)
    {
        free(capture_sys.path_if);
        capture_sys.path_if = NULL;
    }
    if (capture_sys.aud_src != NULL)
    {
        free(capture_sys.aud_src);
        capture_sys.aud_src = NULL;
    }
    if (capture_sys.aud_enc != NULL)
    {
        free(capture_sys.aud_enc);
        capture_sys.aud_enc = NULL;
    }
    capture_sys.primary_path = NULL;
    capture_sys.mic_muted = false;

    if (player_sys.player != NULL)
    {
        int ret = av_render_close(player_sys.player);
        if (ret != ESP_MEDIA_ERR_OK)
        {
            ESP_LOGW(TAG, "av_render_close returned %d", ret);
        }
        player_sys.player = NULL;
    }
    if (player_sys.audio_render != NULL)
    {
        audio_render_free_handle(player_sys.audio_render);
        player_sys.audio_render = NULL;
    }

    ESP_LOGI(TAG, "Media system teardown complete");
}

static int build_capture_system(void)
{
    capture_sys.aud_enc = esp_capture_new_audio_encoder();
    RET_ON_NULL(capture_sys.aud_enc, -1);

    esp_capture_audio_aec_src_cfg_t codec_cfg = {
        .record_handle = get_record_handle(),
#if CONFIG_IDF_TARGET_ESP32S3
        .channel = 4,
        .channel_mask = 1 | 2,
#endif
    };
    capture_sys.aud_src = esp_capture_new_audio_aec_src(&codec_cfg);
    RET_ON_NULL(capture_sys.aud_src, -1);

    // --- INICIO DE LA MODIFICACIÓN ---

    // 1. Crear la interfaz de ruta simple (NECESARIO para el funcionamiento)
    esp_capture_simple_path_cfg_t simple_cfg = {
        .aenc = capture_sys.aud_enc,
    };
    capture_sys.path_if = esp_capture_build_simple_path(&simple_cfg);
    RET_ON_NULL(capture_sys.path_if, -1);

    // 2. Crear el sistema de captura CON la interfaz de ruta
    esp_capture_cfg_t cfg = {
        .sync_mode = ESP_CAPTURE_SYNC_MODE_AUDIO,
        .audio_src = capture_sys.aud_src,
        .video_src = NULL,
        .capture_path = capture_sys.path_if, // <-- IMPORTANTE: Usar la interfaz creada
    };
    esp_err_t ret = esp_capture_open(&cfg, &capture_sys.capture_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open capture: %d", ret);
        return -1;
    }

    // 3. AHORA configurar la ruta primaria para obtener el handle específico
    esp_capture_sink_cfg_t sink_cfg = {
        .audio_info = {
            .codec = ESP_CAPTURE_CODEC_TYPE_OPUS,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    ret = esp_capture_setup_path(capture_sys.capture_handle, ESP_CAPTURE_PATH_PRIMARY,
                                 &sink_cfg, &capture_sys.primary_path);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to setup primary path: %d", ret);
        // Limpiar en caso de fallo
        esp_capture_close(capture_sys.capture_handle);
        capture_sys.capture_handle = NULL;
        return -1;
    }

    // --- FIN DE LA MODIFICACIÓN ---

    return 0;
}

static int build_player_system()
{
    i2s_render_cfg_t i2s_cfg = {
        .play_handle = get_playback_handle(),
    };
    player_sys.audio_render = av_render_alloc_i2s_render(&i2s_cfg);
    if (player_sys.audio_render == NULL)
    {
        ESP_LOGE(TAG, "Fail to create audio render");
        return -1;
    }
    esp_codec_dev_set_out_vol(i2s_cfg.play_handle, DEFAULT_PLAYBACK_VOL);
    av_render_cfg_t render_cfg = {
        .audio_render = player_sys.audio_render,
        .audio_raw_fifo_size = 16 * 4096,
        .audio_render_fifo_size = 200 * 1024,
        .allow_drop_data = false,
    };
    player_sys.player = av_render_open(&render_cfg);
    if (player_sys.player == NULL)
    {
        ESP_LOGE(TAG, "Fail to create player");
        return -1;
    }
    // When support AEC, reference data is from speaker right channel for ES8311 so must output 2 channel
    av_render_audio_frame_info_t aud_info = {
        .sample_rate = 16000,
        .channel = 2,
        .bits_per_sample = 16,
    };
    av_render_set_fixed_frame_info(player_sys.player, &aud_info);
    return 0;
}

int media_sys_buildup(void)
{
    int ret = 0;

    if (media_sys_is_ready())
    {
        ESP_LOGI(TAG, "Media system already built");
        return 0;
    }

    esp_audio_enc_register_default();
    esp_audio_dec_register_default();

    ret = build_capture_system();
    if (ret != 0)
    {
        ESP_LOGE(TAG, "Failed to build capture system: %d", ret);
        media_sys_teardown();
        return ret;
    }

    ret = build_player_system();
    if (ret != 0)
    {
        ESP_LOGE(TAG, "Failed to build player system: %d", ret);
        media_sys_teardown();
        return ret;
    }

    ESP_LOGI(TAG, "Media system built successfully");
    return 0;
}

bool media_sys_is_ready(void)
{
    return capture_sys.capture_handle != NULL && player_sys.player != NULL;
}

int media_sys_get_provider(esp_webrtc_media_provider_t *provide)
{
    provide->capture = capture_sys.capture_handle;
    provide->player = player_sys.player;
    return 0;
}

/**
 * @brief Turn on or off the microphone (mute/unmute).
 *
 * @param mute If `true`, mutes the microphone. If `false`, unmutes it.
 * @return int 0 if successful, -1 if there is an error.
 */
bool media_sys_mic_mute(bool mute)
{
    if (capture_sys.capture_handle == NULL)
    {
        ESP_LOGE(TAG, "Capture handle inválido");
        return false;
    }

    if (mute)
    {
        ESP_LOGI(TAG, "Silenciando micrófono (stop capture)...");
        int ret = esp_capture_stop(capture_sys.capture_handle);
        if (ret != ESP_CAPTURE_ERR_OK && ret != ESP_CAPTURE_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "Fallo esp_capture_stop: %d", ret);
            return false;
        }
        capture_sys.mic_muted = true;
    }
    else
    {
        capture_sys.mic_muted = false;
        ESP_LOGI(TAG, "Reactivando micrófono (start capture)...");
        int ret = esp_capture_start(capture_sys.capture_handle);
        if (ret != ESP_CAPTURE_ERR_OK && ret != ESP_CAPTURE_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "Fallo esp_capture_start: %d", ret);
            return false;
        }
    }
    return true;
}

bool media_sys_restart_capture(void)
{
    if (capture_sys.capture_handle == NULL)
    {
        ESP_LOGE(TAG, "Capture handle invalido; no se puede reiniciar captura");
        return false;
    }

    if (capture_sys.mic_muted)
    {
        ESP_LOGW(TAG, "Microfono muteado; se omite recovery de captura");
        return false;
    }

    ESP_LOGW(TAG, "Reiniciando captura/AEC para recuperar escucha...");
    int ret = esp_capture_stop(capture_sys.capture_handle);
    if (ret == ESP_CAPTURE_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "La captura ya estaba detenida; se omite reinicio para respetar mute");
        return false;
    }
    if (ret != ESP_CAPTURE_ERR_OK)
    {
        ESP_LOGE(TAG, "Fallo esp_capture_stop durante recovery: %d", ret);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(120));

    if (capture_sys.mic_muted)
    {
        ESP_LOGW(TAG, "Microfono muteado durante recovery; no se reactivara captura");
        return false;
    }

    ret = esp_capture_start(capture_sys.capture_handle);
    if (ret != ESP_CAPTURE_ERR_OK)
    {
        ESP_LOGE(TAG, "Fallo esp_capture_start durante recovery: %d", ret);
        return false;
    }

    ESP_LOGI(TAG, "Captura/AEC reiniciada correctamente");
    return true;
}

int test_capture_to_player(void)
{
    esp_capture_sink_cfg_t sink_cfg = {
        .audio_info = {
            .codec = ESP_CAPTURE_CODEC_TYPE_OPUS,
            .sample_rate = 16000,
            .channel = 1,
            .bits_per_sample = 16,
        },
    };
    // Create capture
    esp_capture_path_handle_t capture_path = NULL;
    esp_capture_setup_path(capture_sys.capture_handle, ESP_CAPTURE_PATH_PRIMARY, &sink_cfg, &capture_path);
    esp_capture_enable_path(capture_path, ESP_CAPTURE_RUN_TYPE_ALWAYS);
    // Create player
    av_render_audio_info_t render_aud_info = {
        .codec = AV_RENDER_AUDIO_CODEC_OPUS,
        .sample_rate = 16000,
        .channel = 1,
    };
    av_render_add_audio_stream(player_sys.player, &render_aud_info);

    uint32_t start_time = (uint32_t)(esp_timer_get_time() / 1000);
    esp_capture_start(capture_sys.capture_handle);
    while ((uint32_t)(esp_timer_get_time() / 1000) < start_time + 20000)
    {
        media_lib_thread_sleep(30);
        esp_capture_stream_frame_t frame = {
            .stream_type = ESP_CAPTURE_STREAM_TYPE_AUDIO,
        };
        while (esp_capture_acquire_path_frame(capture_path, &frame, true) == ESP_CAPTURE_ERR_OK)
        {
            av_render_audio_data_t audio_data = {
                .data = frame.data,
                .size = frame.size,
                .pts = frame.pts,
            };
            av_render_add_audio_data(player_sys.player, &audio_data);
            esp_capture_release_path_frame(capture_path, &frame);
        }
    }
    esp_capture_stop(capture_sys.capture_handle);
    av_render_reset(player_sys.player);
    return 0;
}
