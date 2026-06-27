import sys

path = sys.argv[1]
with open(path, 'r', encoding='utf-8') as f:
    content = f.read()

# The block to extract
start_marker = '        else if (strcmp(iter->name, "create_automation_rule") == 0)\n        {'
end_marker = '        // --- MANEJO DE FUNCIONES CON PARÁMETROS ---'

start_idx = content.find(start_marker)
end_idx = content.find(end_marker, start_idx)

if start_idx == -1 or end_idx == -1:
    print("Block not found!")
    sys.exit(1)

block = content[start_idx:end_idx]

# Remove the block from content
content = content[:start_idx] + content[end_idx:]

# Modify the block for its new location
block = block.replace('else if (strcmp(iter->name, "create_automation_rule") == 0)', 'if (strcmp(name->valuestring, "create_automation_rule") == 0)')
block = block.replace('        {\n            ESP_LOGI', '        {\n            class_found = true;\n            ESP_LOGI')
block = block.replace('            break;\n        }\n', '        }\n')

# Find injection point
inject_marker = '    for (class_t *iter = classes; iter; iter = iter->next)'
inject_idx = content.find(inject_marker)

if inject_idx == -1:
    print("Injection point not found!")
    sys.exit(1)

# Wrap the loop in if (!class_found)
# Find the end of the loop
loop_end_marker = '    } // Fin del for loop'
loop_end_idx = content.find(loop_end_marker, inject_idx)
loop_end_idx += len(loop_end_marker)

loop_block = content[inject_idx:loop_end_idx]
loop_block = '    if (!class_found)\n    {\n    ' + loop_block.replace('\n', '\n    ') + '\n    }\n'
# Note: the replace adds 4 spaces, so the closing bracket of loop_block also gets indented, we should fix it manually but it's valid C.

content = content[:inject_idx] + block + '\n' + loop_block + content[loop_end_idx:]

with open(path, 'w', encoding='utf-8') as f:
    f.write(content)

print("Patch applied successfully.")
