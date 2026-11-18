# Muxella Virtual Audio Endpoint Configuration Script
# Uses targeted SetValue permissions on LocalMachine

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Muxella Virtual Audio Endpoint Setup" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host ""

$propKey = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'

# 1. Clean up any unneeded / failed ROOT\VIRTUALAUDIODRIVER nodes if present
Write-Host "[1/3] Checking and removing failed unsigned driver nodes..." -ForegroundColor Yellow
$bad_nodes = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'ROOT\\VIRTUALAUDIODRIVER' }
if ($bad_nodes) {
    foreach ($node in $bad_nodes) {
        Write-Host "  -> Removing $($node.InstanceId)..."
        pnputil /remove-device $node.InstanceId | Out-Null
    }
} else {
    Write-Host "  -> No unverified nodes found."
}

# Helper to set value with targeted SetValue rights
function Set-EndpointLabel($regRelPath, $newName) {
    try {
        $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
            $regRelPath,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [System.Security.AccessControl.RegistryRights]::SetValue
        )
        if ($key) {
            $key.SetValue($propKey, $newName, [Microsoft.Win32.RegistryValueKind]::String)
            $key.Close()
            return $true
        }
    } catch {
        Write-Warning "Failed to set $newName on $regRelPath : $_"
    }
    return $false
}

function Set-EndpointState($regRelPath, $stateVal) {
    try {
        $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey(
            $regRelPath,
            [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,
            [System.Security.AccessControl.RegistryRights]::SetValue
        )
        if ($key) {
            $key.SetValue('DeviceState', $stateVal, [Microsoft.Win32.RegistryValueKind]::DWord)
            $key.Close()
            return $true
        }
    } catch {}
    return $false
}

function Get-EndpointLabel($regRelPath) {
    try {
        $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($regRelPath)
        if ($key) {
            $val = $key.GetValue($propKey)
            $key.Close()
            return $val
        }
    } catch {}
    return $null
}

# 2. Configure Friendly Names and Activate Endpoints in Windows MMDevices
Write-Host "`n[2/3] Configuring Muxella Endpoints in Windows Sound System..." -ForegroundColor Yellow

$renderMap = @{
    "Voicemeeter Input"       = "Muxella Main VAIO"
    "Voicemeeter AUX Input"   = "Muxella Spotify VAIO"
    "Voicemeeter VAIO3 Input" = "Muxella Discord VAIO"
    "Voicemeeter In 1"        = "Muxella Out B1 Link"
    "Voicemeeter In 2"        = "Muxella Out B2 Link"
}

$captureMap = @{
    "Voicemeeter Out B1"      = "Muxella Out B1"
    "Voicemeeter Out B2"      = "Muxella Out B2"
}

# Process Playback (Render)
$renderBase = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
$renderKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($renderBase)
if ($renderKey) {
    foreach ($guid in $renderKey.GetSubKeyNames()) {
        $nodePath = "$renderBase\$guid"
        $propsPath = "$nodePath\Properties"
        $currentName = Get-EndpointLabel $propsPath
        if ($currentName) {
            foreach ($old in $renderMap.Keys) {
                if ($currentName -eq $old -or $currentName -eq $renderMap[$old]) {
                    $target = $renderMap[$old]
                    Set-EndpointState $nodePath 1 | Out-Null
                    if (Set-EndpointLabel $propsPath $target) {
                        Write-Host "  [Playback]  Mapped '$target'" -ForegroundColor Green
                    }
                    break
                }
            }
        }
    }
    $renderKey.Close()
}

# Process Recording (Capture)
$captureBase = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture"
$captureKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($captureBase)
if ($captureKey) {
    foreach ($guid in $captureKey.GetSubKeyNames()) {
        $nodePath = "$captureBase\$guid"
        $propsPath = "$nodePath\Properties"
        $currentName = Get-EndpointLabel $propsPath
        if ($currentName) {
            foreach ($old in $captureMap.Keys) {
                if ($currentName -eq $old -or $currentName -eq $captureMap[$old]) {
                    $target = $captureMap[$old]
                    Set-EndpointState $nodePath 1 | Out-Null
                    if (Set-EndpointLabel $propsPath $target) {
                        Write-Host "  [Recording] Mapped '$target'" -ForegroundColor Green
                    }
                    break
                }
            }
        }
    }
    $captureKey.Close()
}

# 3. Refresh Windows Audio Service UI caches
Write-Host "`n[3/3] Refreshing Windows Audio Service UI caches..." -ForegroundColor Yellow
try {
    Restart-Service -Name "Audiosrv" -Force -ErrorAction Stop
    Write-Host "  -> Windows Audio service refreshed successfully." -ForegroundColor Green
} catch {
    Write-Host "  -> (Service restart skipped or managed by OS)" -ForegroundColor Gray
}

Write-Host "`n==========================================================" -ForegroundColor Cyan
Write-Host "  Muxella Virtual Devices are now active in Windows!" -ForegroundColor Cyan
Write-Host "  Playback:  Muxella Main VAIO, Muxella Spotify VAIO, Muxella Discord VAIO"
Write-Host "  Recording: Muxella Out B1, Muxella Out B2"
Write-Host "==========================================================" -ForegroundColor Cyan
