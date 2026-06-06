#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "network_storage.h"

#define STORAGE_NAMESPACE "wifi_config"
#define MAX_SAVED_NETWORKS 3
#define KEY_FORMAT_SSID "wifi_%d_ssid"
#define KEY_FORMAT_PASSWORD "wifi_%d_password"
#define WIFI_STORAGE_SSID_LEN 33
#define WIFI_STORAGE_PASSWORD_LEN 65

static const char *TAG = "NVS_STORAGE";

static void build_wifi_keys(int index, char *key_ssid, size_t key_ssid_len,
                            char *key_password, size_t key_password_len)
{
    snprintf(key_ssid, key_ssid_len, KEY_FORMAT_SSID, index);
    snprintf(key_password, key_password_len, KEY_FORMAT_PASSWORD, index);
}

static esp_err_t commit_and_close(nvs_handle_t nvs_handle, const char *success_msg, const char *ssid)
{
    esp_err_t err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "%s: %s", success_msg, ssid);
    }
    else
    {
        ESP_LOGE(TAG, "Error haciendo commit para '%s': %s", ssid, esp_err_to_name(err));
    }

    return err;
}

esp_err_t network_save_wifi_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password || ssid[0] == '\0')
    {
        ESP_LOGE(TAG, "Credenciales WiFi invalidas para guardar.");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error abriendo NVS: %s", esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < MAX_SAVED_NETWORKS; i++)
    {
        char key_ssid[20];
        char key_password[20];
        char existing_ssid[WIFI_STORAGE_SSID_LEN] = {0};
        size_t ssid_size = sizeof(existing_ssid);

        build_wifi_keys(i, key_ssid, sizeof(key_ssid), key_password, sizeof(key_password));
        if (nvs_get_str(nvs_handle, key_ssid, existing_ssid, &ssid_size) == ESP_OK &&
            strcmp(existing_ssid, ssid) == 0)
        {
            err = nvs_set_str(nvs_handle, key_password, password);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error actualizando password para '%s': %s", ssid, esp_err_to_name(err));
                nvs_close(nvs_handle);
                return err;
            }

            return commit_and_close(nvs_handle, "Password actualizada para SSID existente", ssid);
        }
    }

    for (int i = 0; i < MAX_SAVED_NETWORKS; i++)
    {
        char key_ssid[20];
        char key_password[20];
        size_t ssid_size = 0;

        build_wifi_keys(i, key_ssid, sizeof(key_ssid), key_password, sizeof(key_password));
        if (nvs_get_str(nvs_handle, key_ssid, NULL, &ssid_size) != ESP_OK)
        {
            err = nvs_set_str(nvs_handle, key_ssid, ssid);
            if (err == ESP_OK)
            {
                err = nvs_set_str(nvs_handle, key_password, password);
            }
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error guardando nueva red '%s': %s", ssid, esp_err_to_name(err));
                nvs_close(nvs_handle);
                return err;
            }

            return commit_and_close(nvs_handle, "Nueva red WiFi guardada en slot vacio", ssid);
        }
    }

    ESP_LOGW(TAG, "Sin espacio. Aplicando politica FIFO: descartando red mas antigua.");

    char key_ssid[20];
    char key_password[20];
    build_wifi_keys(0, key_ssid, sizeof(key_ssid), key_password, sizeof(key_password));

    err = nvs_erase_key(nvs_handle, key_ssid);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Error borrando '%s': %s", key_ssid, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_erase_key(nvs_handle, key_password);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Error borrando '%s': %s", key_password, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    for (int i = 1; i < MAX_SAVED_NETWORKS; i++)
    {
        char key_ssid_src[20];
        char key_password_src[20];
        char key_ssid_dst[20];
        char key_password_dst[20];
        char ssid_buf[WIFI_STORAGE_SSID_LEN] = {0};
        char pass_buf[WIFI_STORAGE_PASSWORD_LEN] = {0};
        size_t ssid_size = sizeof(ssid_buf);
        size_t pass_size = sizeof(pass_buf);

        build_wifi_keys(i, key_ssid_src, sizeof(key_ssid_src), key_password_src, sizeof(key_password_src));
        build_wifi_keys(i - 1, key_ssid_dst, sizeof(key_ssid_dst), key_password_dst, sizeof(key_password_dst));

        if (nvs_get_str(nvs_handle, key_ssid_src, ssid_buf, &ssid_size) == ESP_OK &&
            nvs_get_str(nvs_handle, key_password_src, pass_buf, &pass_size) == ESP_OK)
        {
            err = nvs_set_str(nvs_handle, key_ssid_dst, ssid_buf);
            if (err == ESP_OK)
            {
                err = nvs_set_str(nvs_handle, key_password_dst, pass_buf);
            }
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error rotando credencial WiFi %d -> %d: %s",
                         i, i - 1, esp_err_to_name(err));
                nvs_close(nvs_handle);
                return err;
            }
        }
    }

    build_wifi_keys(MAX_SAVED_NETWORKS - 1, key_ssid, sizeof(key_ssid), key_password, sizeof(key_password));
    err = nvs_set_str(nvs_handle, key_ssid, ssid);
    if (err == ESP_OK)
    {
        err = nvs_set_str(nvs_handle, key_password, password);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error guardando red '%s' tras FIFO: %s", ssid, esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    return commit_and_close(nvs_handle, "Nueva red WiFi guardada reemplazando la mas antigua", ssid);
}

bool network_get_saved_credentials(int index, char *ssid, char *password)
{
    if (index < 0 || index >= MAX_SAVED_NETWORKS || !ssid || !password)
    {
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "No se pudo abrir NVS para leer SSID/Password");
        return false;
    }

    char key_ssid[20];
    char key_password[20];
    build_wifi_keys(index, key_ssid, sizeof(key_ssid), key_password, sizeof(key_password));

    size_t ssid_size = WIFI_STORAGE_SSID_LEN;
    size_t pass_size = WIFI_STORAGE_PASSWORD_LEN;

    bool ret = (nvs_get_str(nvs_handle, key_ssid, ssid, &ssid_size) == ESP_OK &&
                nvs_get_str(nvs_handle, key_password, password, &pass_size) == ESP_OK);

    nvs_close(nvs_handle);
    return ret;
}

bool network_delete_wifi_credential_by_ssid(const char *ssid_to_delete)
{
    if (!ssid_to_delete || strlen(ssid_to_delete) == 0)
    {
        ESP_LOGE(TAG, "SSID invalido para borrar.");
        return false;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error abriendo NVS para borrar WiFi: %s", esp_err_to_name(err));
        return false;
    }

    bool found_and_deleted = false;
    for (int i = 0; i < MAX_SAVED_NETWORKS; i++)
    {
        char key_ssid[20];
        char key_password[20];
        char existing_ssid[WIFI_STORAGE_SSID_LEN] = {0};
        size_t ssid_size = sizeof(existing_ssid);

        build_wifi_keys(i, key_ssid, sizeof(key_ssid), key_password, sizeof(key_password));
        if (nvs_get_str(nvs_handle, key_ssid, existing_ssid, &ssid_size) == ESP_OK &&
            strcmp(existing_ssid, ssid_to_delete) == 0)
        {
            ESP_LOGI(TAG, "Encontrado SSID '%s' en slot %d. Borrando...", ssid_to_delete, i);

            esp_err_t err_erase_ssid = nvs_erase_key(nvs_handle, key_ssid);
            esp_err_t err_erase_pass = nvs_erase_key(nvs_handle, key_password);

            if (err_erase_ssid == ESP_OK && err_erase_pass == ESP_OK)
            {
                err = nvs_commit(nvs_handle);
                if (err == ESP_OK)
                {
                    ESP_LOGI(TAG, "Credenciales para '%s' borradas exitosamente.", ssid_to_delete);
                    found_and_deleted = true;
                }
                else
                {
                    ESP_LOGE(TAG, "Error haciendo commit al borrar '%s': %s",
                             ssid_to_delete, esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGE(TAG, "Error al borrar claves para '%s' (SSID: %d, PASS: %d)",
                         ssid_to_delete, err_erase_ssid, err_erase_pass);
            }
            break;
        }
    }

    nvs_close(nvs_handle);

    if (!found_and_deleted)
    {
        ESP_LOGW(TAG, "SSID '%s' no encontrado en NVS para borrar.", ssid_to_delete);
    }

    return found_and_deleted;
}

esp_err_t network_get_all_ssids(char *buffer, size_t buffer_len)
{
    if (!buffer || buffer_len < 3)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(buffer, 0, buffer_len);
    strcpy(buffer, "[");
    bool first_ssid = true;
    size_t current_len = strlen(buffer);

    ESP_LOGI(TAG, "Leyendo SSIDs guardados...");

    for (int i = 0; i < MAX_SAVED_NETWORKS; i++)
    {
        char ssid_buf[WIFI_STORAGE_SSID_LEN] = {0};
        char pass_buf[WIFI_STORAGE_PASSWORD_LEN] = {0};

        if (network_get_saved_credentials(i, ssid_buf, pass_buf))
        {
            ESP_LOGI(TAG, "  Slot %d: '%s'", i, ssid_buf);

            int required_for_ssid = snprintf(NULL, 0, "%s\"%s\"", (first_ssid ? "" : ","), ssid_buf);
            if (required_for_ssid < 0)
            {
                return ESP_FAIL;
            }

            if (current_len + (size_t)required_for_ssid + 1 > buffer_len)
            {
                ESP_LOGE(TAG, "Buffer insuficiente para listar todos los SSIDs (%zu < %zu)",
                         buffer_len, current_len + (size_t)required_for_ssid + 1);
                strlcat(buffer, "]", buffer_len);
                return ESP_ERR_NO_MEM;
            }

            snprintf(buffer + current_len, buffer_len - current_len, "%s\"%s\"",
                     (first_ssid ? "" : ","), ssid_buf);

            current_len = strlen(buffer);
            first_ssid = false;
        }
    }

    if (current_len + 1 >= buffer_len)
    {
        ESP_LOGE(TAG, "Buffer insuficiente para cerrar JSON (%zu < %zu)", buffer_len, current_len + 2);
        return ESP_ERR_NO_MEM;
    }

    strcat(buffer, "]");

    ESP_LOGI(TAG, "Lista de SSIDs generada: %s", buffer);
    return ESP_OK;
}

esp_err_t network_delete_all_wifi_credentials(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    esp_err_t overall_err = ESP_OK;

    ESP_LOGW(TAG, "Iniciando borrado de TODAS las credenciales WiFi guardadas...");

    err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error abriendo NVS namespace '%s': %s", STORAGE_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    for (int i = 0; i < MAX_SAVED_NETWORKS; i++)
    {
        char key_ssid[20];
        char key_password[20];
        build_wifi_keys(i, key_ssid, sizeof(key_ssid), key_password, sizeof(key_password));

        err = nvs_erase_key(nvs_handle, key_ssid);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Error borrando '%s': %s", key_ssid, esp_err_to_name(err));
            overall_err = err;
        }
        else if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Clave '%s' borrada.", key_ssid);
        }

        err = nvs_erase_key(nvs_handle, key_password);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGE(TAG, "Error borrando '%s': %s", key_password, esp_err_to_name(err));
            overall_err = err;
        }
        else if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Clave '%s' borrada.", key_password);
        }
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error haciendo commit al borrar credenciales WiFi: %s", esp_err_to_name(err));
        overall_err = err;
    }
    else
    {
        ESP_LOGI(TAG, "Commit de borrado de WiFi exitoso.");
    }

    nvs_close(nvs_handle);

    if (overall_err == ESP_OK)
    {
        ESP_LOGW(TAG, "Borrado de todas las credenciales WiFi completado.");
    }
    else
    {
        ESP_LOGE(TAG, "Se encontraron errores durante el borrado de credenciales WiFi.");
    }

    return overall_err;
}
