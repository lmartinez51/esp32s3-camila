---
name: esp32-clean-build-flash
description: ALWAYS TRIGGER THIS SKILL when the user asks to compile, build, flash, upload, monitor, wipe the cache, or deploy code for the ESP32-S3-BOX3 project. Do not attempt manual compilation; use this exact strict clean build routine.
---

# ESP32-S3-BOX3 Clean Build and Flash Routine

This skill defines the exact workflow for performing a raw clean build and flashing the ESP32 board. It enforces a completely strict workflow using the provided helper script.

## Trigger Evaluation Queries

These are sample queries that should trigger this skill:
- "Do a clean build and flash for the ESP32."
- "Wipe the build folder, recompile the project, and monitor."
- "Build and flash the project to the board."
- "Rebuild the firmware from scratch and flash it."

## Instructions

When the user requests to clean build and flash the ESP32 project, you MUST execute the dedicated PowerShell helper script which wraps the entire routine. Do NOT run the individual IDF or CMake commands manually.

### Execution
Execute the script from the project root using PowerShell:
```powershell
.\.agent\skills\esp32-clean-build-flash\scripts\build_flash.ps1
```

The script will automatically:
1. Enforce the strict ESP-IDF v5.4.3 dependency.
2. Clean out `build` and `managed_components` directories.
3. Build the project.
4. Dynamically detect the correct COM port for the ESP32-S3-BOX3.
5. Flash and monitor the firmware.

Wait for the script to finish and report any success or error messages back to the user.
