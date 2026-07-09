-- =============================================================================
-- ESP-Camila Automation Engine
-- File: /littlefs/init.lua  (flashed from data/init.lua)
--
-- Architecture:
--   - ir_db  : RAM table for IR device/button codes. Persisted to ir_db.json.
--   - rules_db: RAM table for automation rules. Persisted to rules.json via C.
--   - register_rule(rule): IPC entry point called by the C worker on every
--     incoming esp_claw_rule_t message. Routes by trigger string.
-- =============================================================================

-- -----------------------------------------------------------------------------
-- 1. IR Database Initialization
-- -----------------------------------------------------------------------------
ir_db = ir_db or {}
ir_learning_target_device = nil
ir_learning_target_button  = nil

-- Stateful two-pass line-by-line deserializer.
-- Replaces the brittle {[^}]+} block-capture regex that failed on multi-button
-- devices and was vulnerable to scientific notation and } in device names.
local function load_ir_db()
    local content = nil
    if ir.read_db then
        content = ir.read_db()
    end

    if not content or content == "" then
        print("ir_db: file not found or empty. Initializing clean DB.")
        ir_db = {}
        return
    end

    ir_db = {}
    local current_device = nil

    -- Append a sentinel newline so the last line is always captured.
    for line in string.gmatch(content .. "\n", "([^\n]*)\n") do

        -- Pattern 1: Device header line  →  "TV Sala": {
        -- The line must end with { (after optional whitespace).
        -- Device names containing } are safely ignored because the }
        -- is inside the quoted string, not at the start of the line.
        local dev = line:match('^%s*"([^"]+)"%s*:%s*{%s*$')
        if dev then
            current_device = dev
            ir_db[current_device] = {}

        else
            -- Pattern 2: Button entry  →  "power": "0x00FF00FF"
            -- Only fires when inside a device block (current_device ~= nil).
            local btn, hex = line:match('^%s*"([^"]+)"%s*:%s*"0x(%x+)"%s*,?%s*$')
            if btn and hex and current_device then
                -- Canonicalize to uppercase 0x prefix for consistent comparison.
                ir_db[current_device][btn] = "0x" .. string.upper(hex)
            end

            -- Pattern 3: Closing brace  →  }  or  },
            -- Resets the device context. Only matches a line that is solely a
            -- closing brace (with optional comma and whitespace), so it cannot
            -- be confused with a device name that happens to contain }.
            if line:match('^%s*}%s*,?%s*$') then
                current_device = nil
            end
        end
    end

    print("ir_db: loaded successfully.")
end

load_ir_db()

-- -----------------------------------------------------------------------------
-- 2. IR Database Save (Serializer)
-- -----------------------------------------------------------------------------
-- All codes in ir_db are stored as canonical "0x%08X" strings.
-- The serializer wraps them in JSON quotes directly — no tostring() conversion,
-- no risk of scientific notation corruption.
function ir.save_db()
    if not ir.write_db then
        print("ERROR: ir.write_db C-binding not found.")
        return
    end

    local json_str = "{\n"
    local first_dev = true
    for dev, buttons in pairs(ir_db) do
        if not first_dev then json_str = json_str .. ",\n" end
        first_dev = false
        json_str = json_str .. '  "' .. dev .. '": {\n'

        local first_btn = true
        for btn, code in pairs(buttons) do
            if not first_btn then json_str = json_str .. ",\n" end
            first_btn = false
            -- code is already a "0x%08X" string — wrap in JSON quotes directly.
            json_str = json_str .. '    "' .. btn .. '": "' .. code .. '"'
        end
        json_str = json_str .. "\n  }"
    end
    json_str = json_str .. "\n}\n"

    if ir.write_db(json_str) then
        print("ir_db: flushed to LittleFS.")
    else
        print("ERROR: ir.write_db failed to write ir_db.json.")
    end
end

-- -----------------------------------------------------------------------------
-- 3. Rules DB Initialization
-- -----------------------------------------------------------------------------
rules_db = rules_db or {}

-- -----------------------------------------------------------------------------
-- 4. Rules DB Sanitization (one-time boot cleanup)
-- -----------------------------------------------------------------------------
-- Removes phantom IR entries that were incorrectly written into rules.json
-- during the conflation period (when the broken routing fell through to the
-- standard rule-creation path). Runs once at boot before any tool call arrives.
local function sanitize_rules_db()
    if type(rules_db) ~= "table" then return end

    local ir_phantom_keys = {
        -- IPC trigger names written during the conflation period
        ["LUA_TOOL_IR_LEARN"]       = true,
        ["LUA_TOOL_IR_TRANSMIT"]    = true,
        ["LUA_TOOL_IR_SAVE"]        = true,
        ["LUA_TOOL_IR_GET_DEVICES"] = true,
        ["LUA_TOOL_IR_DELETE"]      = true,
        ["LUA_CMD_IR_LEARNED"]      = true,
        ["LUA_CMD_IR_TIMEOUT"]      = true,
        -- Raw WebRTC JSON tool names that fell through the broken minimal_lua stub
        ["ir_learn_button"]         = true,
        ["ir_transmit_command"]     = true,
        ["ir_get_devices"]          = true,
        ["ir_save_database"]        = true,
        ["ir_delete_device"]        = true,
    }

    local removed = 0
    for key in pairs(rules_db) do
        if ir_phantom_keys[key] then
            rules_db[key] = nil
            removed = removed + 1
            print("SANITIZE: removed phantom IR rule key: " .. key)
        end
    end

    if removed > 0 then
        if c_save_rules then
            c_save_rules()
            print("SANITIZE: rules.json flushed. " .. removed .. " phantom(s) removed.")
        end
    else
        print("SANITIZE: rules.json is clean.")
    end
end

sanitize_rules_db()

-- -----------------------------------------------------------------------------
-- 5. IPC Entry Point: register_rule(rule)
-- -----------------------------------------------------------------------------
-- Called by the C worker (esp_claw_init.c) for every incoming IPC message.
-- rule.trigger  : string — the IPC command name
-- rule.actions  : array  — flat strings (from claw_push_rule_to_lua)
-- rule.call_id  : string — OpenAI function call ID for response routing
-- rule.conditions: array — sensor condition tables (for automation rules)
function register_rule(rule)
    if type(rule) ~= "table" then return end
    local trigger = rule.trigger or ""

    -- -------------------------------------------------------------------------
    -- IR Hardware Callback: code successfully captured by RMT receiver
    -- -------------------------------------------------------------------------
    if trigger == "LUA_CMD_IR_LEARNED" then
        if rule.actions and rule.actions[1] then
            -- The C layer sends the code as a hex string: "0x00FF00FF"
            local hex_code = tonumber(rule.actions[1])  -- number for arithmetic

            if ir_learning_target_device and ir_learning_target_button then
                -- Ensure the device table exists in RAM
                ir_db[ir_learning_target_device] = ir_db[ir_learning_target_device] or {}

                -- Duplicate Detection
                -- ir_db stores strings; hex_code is a number.
                -- Re-parse each stored string to number before comparing.
                local is_duplicate = false
                local duplicate_button = nil
                for btn, stored_str in pairs(ir_db[ir_learning_target_device]) do
                    local stored_num = tonumber(stored_str)
                    if stored_num == hex_code and btn ~= ir_learning_target_button then
                        is_duplicate = true
                        duplicate_button = btn
                        break
                    end
                end

                if is_duplicate then
                    print("WARNING: Captured code matches existing button: " .. duplicate_button)
                    if c_inject_webrtc_message then
                        c_inject_webrtc_message(
                            "System Warning: Captured IR code is identical to the existing button '" ..
                            duplicate_button .. "'. Ask the user if they made a mistake, " ..
                            "want to overwrite it, or want to retry.")
                    end
                else
                    -- Store as canonical hex string — eliminates tostring() scientific notation.
                    local hex_str = string.format("0x%08X", hex_code)
                    ir_db[ir_learning_target_device][ir_learning_target_button] = hex_str
                    print("LEARNED: " .. ir_learning_target_device .. "." ..
                          ir_learning_target_button .. " -> " .. hex_str)

                    -- Auto-save: persist to LittleFS immediately on successful capture.
                    if ir.save_db then ir.save_db() end

                    if c_inject_webrtc_message then
                        c_inject_webrtc_message(
                            "System Context: IR code captured and saved successfully. " ..
                            "Ask the user if they want to test it by transmitting it back.")
                    end
                end

                -- Reset learning targets regardless of outcome
                ir_learning_target_device = nil
                ir_learning_target_button  = nil
            end
        end
        return

    -- -------------------------------------------------------------------------
    -- IR Hardware Callback: 15-second learning window expired
    -- -------------------------------------------------------------------------
    elseif trigger == "LUA_CMD_IR_TIMEOUT" then
        print("IR: Learning timed out.")
        ir_learning_target_device = nil
        ir_learning_target_button  = nil
        if c_inject_webrtc_message then
            c_inject_webrtc_message(
                "System Error: IR learning timed out. No code was received. " ..
                "Inform the user and ask if they want to retry.")
        end
        return

    -- -------------------------------------------------------------------------
    -- LLM Tool: Arm IR learning mode for a specific device/button
    -- -------------------------------------------------------------------------
    elseif trigger == "LUA_TOOL_IR_LEARN" then
        if rule.actions and rule.actions[1] and rule.actions[2] then
            ir_learning_target_device = rule.actions[1]
            ir_learning_target_button  = rule.actions[2]
            if ir.start_learning then
                ir.start_learning()
                if c_send_webrtc_response then
                    c_send_webrtc_response(rule.call_id,
                        "Armed. Ask the user to point the remote at the device and press the button now.")
                end
            else
                if c_send_webrtc_response then
                    c_send_webrtc_response(rule.call_id,
                        "Error: ir.start_learning C-binding not found.")
                end
            end
        else
            if c_send_webrtc_response then
                c_send_webrtc_response(rule.call_id,
                    "Error: ir_learn_button requires both device_name and button_name arguments.")
            end
        end
        return

    -- -------------------------------------------------------------------------
    -- LLM Tool: Transmit a stored IR code
    -- -------------------------------------------------------------------------
    elseif trigger == "LUA_TOOL_IR_TRANSMIT" then
        if rule.actions and rule.actions[1] and rule.actions[2] then
            local device = rule.actions[1]
            local button = rule.actions[2]
            if not ir_db[device] or not ir_db[device][button] then
                if c_send_webrtc_response then
                    c_send_webrtc_response(rule.call_id,
                        "Error: Device '" .. device .. "' or button '" .. button ..
                        "' not found in the database.")
                end
            else
                if ir.send then
                    -- ir.send() C-binding calls strtoul(hex_str, NULL, 16).
                    -- Passing the "0x%08X" string directly is correct and safe.
                    ir.send(ir_db[device][button])
                    if c_send_webrtc_response then
                        c_send_webrtc_response(rule.call_id, "Success: IR code transmitted.")
                    end
                else
                    if c_send_webrtc_response then
                        c_send_webrtc_response(rule.call_id,
                            "Error: ir.send C-binding not found.")
                    end
                end
            end
        else
            if c_send_webrtc_response then
                c_send_webrtc_response(rule.call_id,
                    "Error: ir_transmit_command requires both device_name and button_name arguments.")
            end
        end
        return

    -- -------------------------------------------------------------------------
    -- LLM Tool: Explicit user-commanded save (optional; auto-save handles most cases)
    -- -------------------------------------------------------------------------
    elseif trigger == "LUA_TOOL_IR_SAVE" then
        if ir.save_db then ir.save_db() end
        if c_send_webrtc_response then
            c_send_webrtc_response(rule.call_id, "Success: IR database saved to flash.")
        end
        return

    -- -------------------------------------------------------------------------
    -- LLM Tool: Delete an IR device or a specific button (granular, nil-safe)
    -- -------------------------------------------------------------------------
    elseif trigger == "LUA_TOOL_IR_DELETE" then
        -- rule.actions elements are flat strings (confirmed from claw_push_rule_to_lua).
        -- actions[1] = device_name (required)
        -- actions[2] = button_name (optional — if absent, deletes entire device)
        local device = rule.actions and rule.actions[1]
        local button = rule.actions and rule.actions[2]

        -- Guard: device argument is mandatory
        if not device or device == "" then
            if c_send_webrtc_response then
                c_send_webrtc_response(rule.call_id,
                    "Error: device_name argument is missing.")
            end
            return
        end

        -- Guard: device must exist in DB
        if not ir_db or not ir_db[device] then
            if c_send_webrtc_response then
                c_send_webrtc_response(rule.call_id,
                    "Error: Device '" .. device .. "' not found in the database.")
            end
            return
        end

        if button and button ~= "" then
            -- Button-level deletion
            if not ir_db[device][button] then
                if c_send_webrtc_response then
                    c_send_webrtc_response(rule.call_id,
                        "Error: Button '" .. button .. "' not found on device '" .. device .. "'.")
                end
                return
            end

            ir_db[device][button] = nil

            -- Garbage-collect the device table if it is now empty
            if next(ir_db[device]) == nil then
                ir_db[device] = nil
                print("GC: removed empty device '" .. device .. "' from ir_db.")
            end

            if c_send_webrtc_response then
                c_send_webrtc_response(rule.call_id,
                    "Button '" .. button .. "' deleted from device '" .. device .. "'.")
            end
        else
            -- Device-level deletion (all buttons removed)
            ir_db[device] = nil
            if c_send_webrtc_response then
                c_send_webrtc_response(rule.call_id,
                    "Device '" .. device .. "' and all its buttons have been deleted.")
            end
        end

        if ir.save_db then ir.save_db() end
        return

    -- -------------------------------------------------------------------------
    -- LLM Tool: List all known IR devices and their buttons
    -- -------------------------------------------------------------------------
    elseif trigger == "LUA_TOOL_IR_GET_DEVICES" then
        local response = ""
        local has_devices = false
        if ir_db then
            for device, buttons in pairs(ir_db) do
                has_devices = true
                local btn_list = ""
                for btn, _ in pairs(buttons) do
                    btn_list = btn_list == "" and btn or (btn_list .. ", " .. btn)
                end
                local entry = device .. " (buttons: " .. btn_list .. ")"
                response = response == "" and entry or (response .. "; " .. entry)
            end
        end
        if not has_devices then
            response = "No IR devices saved yet."
        else
            response = "Devices: " .. response .. "."
        end
        if c_send_webrtc_response then
            c_send_webrtc_response(rule.call_id, response)
        end
        return

    -- -------------------------------------------------------------------------
    -- System Command: List all automation rules
    -- -------------------------------------------------------------------------
    elseif trigger == "SYS_CMD:LIST" then
        local list_json = "["
        local first = true
        for k in pairs(rules_db) do
            if not first then list_json = list_json .. ", " end
            list_json = list_json .. '{"trigger": "' .. tostring(k) .. '"}'
            first = false
        end
        list_json = list_json .. "]"
        if c_send_webrtc_response then
            c_send_webrtc_response(rule.call_id, list_json)
        end
        return

    -- -------------------------------------------------------------------------
    -- System Command: Delete an automation rule
    -- -------------------------------------------------------------------------
    elseif trigger == "SYS_CMD:DELETE" then
        local target = rule.actions and rule.actions[1]
        if target and rules_db[target] then
            rules_db[target] = nil
            if c_save_rules then c_save_rules() end
            if c_send_webrtc_response then
                c_send_webrtc_response(rule.call_id, '{"status": "deleted"}')
            end
        else
            if c_send_webrtc_response then
                c_send_webrtc_response(rule.call_id, '{"error": "rule not found"}')
            end
        end
        return

    -- -------------------------------------------------------------------------
    -- System Commands: Execute / IR Direct (stub responses)
    -- -------------------------------------------------------------------------
    elseif trigger == "SYS_CMD:EXECUTE" or trigger == "SYS_CMD:IR_DIRECT" then
        if c_send_webrtc_response then
            c_send_webrtc_response(rule.call_id, '{"status": "executed"}')
        end
        return
    end

    -- -------------------------------------------------------------------------
    -- Standard Automation Rule Creation (fallback for unrecognized triggers)
    -- -------------------------------------------------------------------------
    rules_db = rules_db or {}
    rules_db[rule.trigger] = rule
    if c_save_rules then c_save_rules() end
    if c_send_webrtc_response and rule.call_id then
        c_send_webrtc_response(rule.call_id, "Automation rule created and saved.")
    end
end
