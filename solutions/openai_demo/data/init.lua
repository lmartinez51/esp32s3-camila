-- IR Database (RAM Table)
ir_db = ir_db or {}

-- Target for current learning session
ir_learning_target_device = nil
ir_learning_target_button = nil

-- Task 5 (Init DB): Load and parse /littlefs/ir_db.json
local function load_ir_db()
    local content = nil
    if ir.read_db then
        content = ir.read_db()
    end
    
    if not content or content == "" then
        print("ir_db.json not found or empty. Initializing empty DB for first-run.")
        ir_db = {}
        return
    end
    
    -- Lightweight JSON parser tailored for the 2-level schema: {"device": {"button": code, ...}, ...}
    ir_db = {}
    for dev_name, buttons_str in string.gmatch(content, '"([^"]+)":%s*({[^}]+})') do
        ir_db[dev_name] = {}
        for btn, code in string.gmatch(buttons_str, '"([^"]+)":%s*(%d+)') do
            ir_db[dev_name][btn] = tonumber(code)
        end
    end
    print("ir_db.json loaded into RAM successfully.")
end

-- Initialize DB on boot
load_ir_db()

-- Task 7 (Save Mechanism): Serialize RAM table back to JSON
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
            json_str = json_str .. '    "' .. btn .. '": ' .. tostring(code)
        end
        json_str = json_str .. "\n  }"
    end
    json_str = json_str .. "\n}\n"
    
    if ir.write_db(json_str) then
        print("ir_db.json flushed to LittleFS successfully.")
    else
        print("ERROR: Failed to write ir_db.json to LittleFS.")
    end
end

-- Task 6 (IPC Handlers): Hook into the existing IPC loop
function register_rule(rule)
    -- Handle IR Learning Mode Events
    if rule.trigger == "LUA_CMD_IR_LEARNED" then
        if rule.actions and rule.actions[1] then
            local hex_code = tonumber(rule.actions[1])
            
            if ir_learning_target_device and ir_learning_target_button then
                local is_duplicate = false
                local duplicate_button = nil
                
                -- Ensure device table exists
                ir_db[ir_learning_target_device] = ir_db[ir_learning_target_device] or {}
                
                -- Duplicate Detection
                for btn, code in pairs(ir_db[ir_learning_target_device]) do
                    if code == hex_code and btn ~= ir_learning_target_button then
                        is_duplicate = true
                        duplicate_button = btn
                        break
                    end
                end
                
                if is_duplicate then
                    print("WARNING: Captured code matches existing button: " .. duplicate_button)
                    if c_inject_webrtc_message then
                        c_inject_webrtc_message("System Warning: Captured code is identical to " .. duplicate_button .. ". Ask the user if they made a mistake, want to overwrite, or retry.")
                    end
                else
                    -- Update active RAM table
                    ir_db[ir_learning_target_device][ir_learning_target_button] = hex_code
                    print("LEARNED: " .. ir_learning_target_device .. "." .. ir_learning_target_button .. " -> " .. tostring(hex_code))
                    
                    if c_inject_webrtc_message then
                        c_inject_webrtc_message("System Context: Code successfully captured. Ask the user if they want to test it by transmitting it back, or save the database.")
                    end
                end
                
                -- Reset learning targets
                ir_learning_target_device = nil
                ir_learning_target_button = nil
            end
        end
        return
        
    elseif rule.trigger == "LUA_CMD_IR_TIMEOUT" then
        print("ERROR: IR Learning timed out.")
        -- Reset learning targets
        ir_learning_target_device = nil
        ir_learning_target_button = nil
        
        if c_inject_webrtc_message then
            c_inject_webrtc_message("System Error: IR learning timed out. No code received. Inform the user.")
        end
        return

    elseif rule.trigger == "LUA_TOOL_IR_LEARN" then
        if rule.actions and rule.actions[1] and rule.actions[2] then
            ir_learning_target_device = rule.actions[1]
            ir_learning_target_button = rule.actions[2]
            if ir.start_learning then
                ir.start_learning()
                if c_send_webrtc_response then
                    c_send_webrtc_response(rule.call_id, "Armed. Ask the user to point the remote and press the button now.")
                end
            else
                if c_send_webrtc_response then
                    c_send_webrtc_response(rule.call_id, "Error: ir.start_learning C-binding not found.")
                end
            end
        end
        return

    elseif rule.trigger == "LUA_TOOL_IR_TRANSMIT" then
        if rule.actions and rule.actions[1] and rule.actions[2] then
            local device = rule.actions[1]
            local button = rule.actions[2]
            if not ir_db[device] or not ir_db[device][button] then
                if c_send_webrtc_response then
                    c_send_webrtc_response(rule.call_id, "Error: Device or button not found in database.")
                end
            else
                if ir.send then
                    ir.send(ir_db[device][button])
                    if c_send_webrtc_response then
                        c_send_webrtc_response(rule.call_id, "Success: Code transmitted.")
                    end
                else
                    if c_send_webrtc_response then
                        c_send_webrtc_response(rule.call_id, "Error: ir.send C-binding not found.")
                    end
                end
            end
        end
        return

    elseif rule.trigger == "LUA_TOOL_IR_SAVE" then
        if ir.save_db then
            ir.save_db()
        end
        if c_send_webrtc_response then
            c_send_webrtc_response(rule.call_id, "Success: Database saved to flash.")
        end
        return

    elseif rule.trigger == "LUA_TOOL_IR_GET_DEVICES" then
        local response = ""
        local has_devices = false
        if ir_db then
            for device, buttons in pairs(ir_db) do
                has_devices = true
                local btn_list = ""
                for btn, _ in pairs(buttons) do
                    if btn_list == "" then
                        btn_list = btn
                    else
                        btn_list = btn_list .. ", " .. btn
                    end
                end
                if response == "" then
                    response = "Devices saved: " .. device .. " (buttons: " .. btn_list .. ")"
                else
                    response = response .. "; " .. device .. " (buttons: " .. btn_list .. ")"
                end
            end
        end
        if not has_devices then
            response = "No devices saved yet."
        else
            response = response .. "."
        end
        if c_send_webrtc_response then
            c_send_webrtc_response(rule.call_id, response)
        end
        return
    end

    -- Process normal rules (legacy stub for existing IPC architecture)
    rules_db = rules_db or {}
    rules_db[rule.trigger] = rule
end
