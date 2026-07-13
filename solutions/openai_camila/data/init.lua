-- =============================================================================
-- ESP-Camila Automation Engine
-- File: /littlefs/init.lua  (flashed from data/init.lua)
--
-- Architecture:
--   - rules_db: RAM table for automation rules. Persisted to rules.json via C.
--   - register_rule(rule): IPC entry point called by the C worker on every
--     incoming esp_claw_rule_t message. Routes by trigger string.
-- =============================================================================

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
    -- System Command: List all automation rules
    -- -------------------------------------------------------------------------
    if trigger == "SYS_CMD:LIST" then
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
    elseif trigger == "SYS_CMD:EXECUTE" then
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
