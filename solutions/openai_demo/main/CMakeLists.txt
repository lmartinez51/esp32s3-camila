idf_component_register(
    SRCS
        "core/main.c"
        "core/sensor_dock.c"
        "core/orchestrator_helpers.c"
        "core/orchestrator_tasks.c"
        "core/orchestrator_vigilante.c"
        "core/orchestrator_fsm.c"
        "core/lua_benchmark.c"
        "core/esp_claw_init.c"

        "config/config_manager.c"
        "config/nvs_setup.c"

        "hardware/board.c"
        "hardware/aht30.c"
        "hardware/radar.c"
        "hardware/ir_sniffer.c"
        "hardware/ir_action_map.c"

        "audio/media_sys.c"
        "audio/mute_handler.c"

        "alert/alert_dispatcher.c"

        "ble/ble_common.c"
        "ble/ble_config.c"
        "ble/ble_device_callbacks.c"
        "ble/ble_device_control.c"

        "ui/ui.c"
        "ui/simi_canvas.c"
        "ui/simi.c"

        "webrtc/webrtc.c"
        "webrtc/openai_signaling.c"
        "webrtc/lua_ir_bindings.c"
        "webrtc/lua_mic_bindings.c"

        "openai/http_client.c"
        "openai/web_search.c"
        "openai/responses_client.c"

        "sensing/csi_handler.c"
        "sensing/csi_dsp.c"
        "sensing/ei_inference.cpp"

    INCLUDE_DIRS
        "."
        "core"
        "config"
        "hardware"
        "audio"
        "alert"
        "ble"
        "ui"
        "webrtc"
        "openai"
        "sensing"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -w -Wno-error)

littlefs_create_partition_image(littlefs ../data FLASH_IN_PROJECT)
