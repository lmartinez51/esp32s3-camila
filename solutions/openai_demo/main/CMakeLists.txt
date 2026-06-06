idf_component_register(
    SRCS
        "core/main.c"

        "config/config_manager.c"
        "config/nvs_setup.c"

        "hardware/board.c"

        "audio/media_sys.c"
        "audio/mute_handler.c"

        "ble/ble_common.c"
        "ble/ble_config.c"
        "ble/ble_device_callbacks.c"
        "ble/ble_device_control.c"

        "ui/ui.c"

        "webrtc/webrtc.c"
        "webrtc/openai_signaling.c"

        "openai/assistants.c"
        "openai/http_client.c"
        "openai/web_search.c"

        "sensing/csi_handler.c"
        "sensing/ei_inference.cpp"

    INCLUDE_DIRS
        "."
        "core"
        "config"
        "hardware"
        "audio"
        "ble"
        "ui"
        "webrtc"
        "openai"
        "sensing"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -w -Wno-error)
