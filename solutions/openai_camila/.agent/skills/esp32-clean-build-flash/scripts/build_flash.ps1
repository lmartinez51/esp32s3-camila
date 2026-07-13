$ErrorActionPreference = "Stop"

$ExportScript = "C:\Espressif\frameworks\esp-idf-v5.4.3\export.ps1"

Write-Host "Verifying ESP-IDF framework..." -ForegroundColor Cyan
if (-Not (Test-Path -Path $ExportScript)) {
    Write-Host "ERROR: The required ESP-IDF v5.4.3 framework could not be located at $ExportScript." -ForegroundColor Red
    Write-Host "Please ensure the framework is installed correctly. Execution aborted." -ForegroundColor Red
    exit 1
}

# Dot-sourcing export.ps1 is omitted here because it fails in non-interactive sessions
# Instead, export.bat is explicitly called in the cmd.exe child process during compilation/flashing.

Write-Host "Performing strict pre-build cleanup..." -ForegroundColor Cyan
$foldersToRemove = @("build", "managed_components")
foreach ($folder in $foldersToRemove) {
    if (Test-Path -Path ".\$folder") {
        Write-Host "Removing .\$folder..."
        Remove-Item -Recurse -Force -Path ".\$folder" -ErrorAction SilentlyContinue
    }
}
Write-Host "Cleanup complete." -ForegroundColor Green

Write-Host "Compiling the project..." -ForegroundColor Cyan
Start-Process -FilePath "powershell.exe" -ArgumentList "-Command", ".\.agent\skills\esp32-clean-build-flash\scripts\executor.bat build" -Wait
$buildExitCode = $LASTEXITCODE

if ($buildExitCode -ne 0) {
    Write-Host "ERROR: Build failed with exit code $buildExitCode. Aborting flash process." -ForegroundColor Red
    exit $buildExitCode
}
Write-Host "Build succeeded!" -ForegroundColor Green
$ErrorActionPreference = "Stop"

Write-Host "Dynamically detecting COM port for ESP32-S3-BOX3..." -ForegroundColor Cyan
$comPort = $null
$ports = Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match "\((COM\d+)\)" }

foreach ($p in $ports) {
    if ($p.Name -match "(?i)USB Serial|JTAG|CH340|CP210|UART") {
        if ($p.Name -match "\((COM\d+)\)") {
            $comPort = $matches[1]
            break
        }
    }
}

if ([string]::IsNullOrWhiteSpace($comPort)) {
    Write-Host "Warning: Unable to dynamically identify the exact ESP32 USB type." -ForegroundColor Yellow
    if ($ports) {
        # Fallback to the first found COM port
        if ($ports[0].Name -match "\((COM\d+)\)") {
            $comPort = $matches[1]
            Write-Host "Falling back to the first available port: $comPort" -ForegroundColor DarkCyan
        }
    }
    
    if ([string]::IsNullOrWhiteSpace($comPort)) {
        Write-Host "ERROR: No COM ports detected! Please check your USB connection." -ForegroundColor Red
        exit 1
    }
}

Write-Host "Target COM Port identified as: $comPort" -ForegroundColor Green
Write-Host "Flashing and Monitoring..." -ForegroundColor Cyan

$ErrorActionPreference = "Continue"
Start-Process -FilePath "powershell.exe" -ArgumentList "-NoExit", "-Command", ".\.agent\skills\esp32-clean-build-flash\scripts\executor.bat -p $comPort flash monitor"
exit $LASTEXITCODE
