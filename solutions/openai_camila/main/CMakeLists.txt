idf_component_register(
    SRCS
        "core/main.c"
        "core/orchestrator_helpers.c"
        "core/orchestrator_tasks.c"
        "core/orchestrator_vigilante.c"
        "core/orchestrator_fsm.c"
        "core/lua_benchmark.c"
        "core/esp_claw_init.c"
        "core/audio_watchdog.c"

        "config/config_manager.c"
        "config/nvs_setup.c"

        "sensing/esp_now_beacon.c"

        "hardware/board.c"
        "hardware/kill_switch.c"

        "audio/media_sys.c"
        "audio/mute_handler.c"

        "alert/alert_dispatcher.c"

        "ble/ble_common.c"
        "ble/ble_config.c"
        "ble/ble_device_callbacks.c"
        "ble/ble_device_control.c"

        "hal/robot_hal.c"
        "hal/robot_tools.c"
        "hal/robot_health.c"

        "adapters/webrtc_tool_adapter.c"

        "drivers/ble/ble_transport.c"
        "drivers/ble/ble_elegoo_bt16.c"
        "drivers/ble/ble_generic_nus.c"
        "drivers/ble/ble_hue.c"

        "drivers/wifi/wifi_tcp.c"
        "drivers/wifi/wifi_http.c"
        "drivers/wifi/wifi_arm.c"
        "drivers/wifi/wifi_pantilt.c"

        "drivers/ir/ir_rmt.c"
        "drivers/ir/ir_nec.c"
        "drivers/ir/ir_sony.c"
        "drivers/ir/ir_rc5.c"

        "registry/device_registry.c"
        "registry/registry_migrate.c"
        "registry/registry_persist.c"

        "ui/ui.c"

        "webrtc/webrtc.c"
        "webrtc/openai_signaling.c"
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
        "hal"
        "adapters"
        "drivers"
        "drivers/ble"
        "drivers/wifi"
        "drivers/ir"
        "registry"
        "ui"
        "webrtc"
        "openai"
        "sensing"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -w -Wno-error)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LUA_32BITS=1)

littlefs_create_partition_image(littlefs ../data FLASH_IN_PROJECT)
