# Restore Original Virtual Audio Endpoint Names

Write-Host "==========================================================" -ForegroundColor Cyan
Write-Host "  Restoring Original Endpoint Names" -ForegroundColor Cyan
Write-Host "==========================================================" -ForegroundColor Cyan

$propKey = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'

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

$renderRestore = @{
    "Muxella Main VAIO"    = "Voicemeeter Input"
    "Muxella Spotify VAIO" = "Voicemeeter AUX Input"
    "Muxella Discord VAIO" = "Voicemeeter VAIO3 Input"
}

$captureRestore = @{
    "Muxella Out B1" = "Voicemeeter Out B1"
    "Muxella Out B2" = "Voicemeeter Out B2"
}

$renderBase = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
$renderKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($renderBase)
if ($renderKey) {
    foreach ($guid in $renderKey.GetSubKeyNames()) {
        $propsPath = "$renderBase\$guid\Properties"
        $currentName = Get-EndpointLabel $propsPath
        if ($currentName -and $renderRestore.ContainsKey($currentName)) {
            $orig = $renderRestore[$currentName]
            if (Set-EndpointLabel $propsPath $orig) {
                Write-Host "  [Playback]  Restored '$orig'" -ForegroundColor Green
            }
        }
    }
    $renderKey.Close()
}

$captureBase = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture"
$captureKey = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($captureBase)
if ($captureKey) {
    foreach ($guid in $captureKey.GetSubKeyNames()) {
        $propsPath = "$captureBase\$guid\Properties"
        $currentName = Get-EndpointLabel $propsPath
        if ($currentName -and $captureRestore.ContainsKey($currentName)) {
            $orig = $captureRestore[$currentName]
            if (Set-EndpointLabel $propsPath $orig) {
                Write-Host "  [Recording] Restored '$orig'" -ForegroundColor Green
            }
        }
    }
    $captureKey.Close()
}

try {
    Restart-Service -Name "Audiosrv" -Force -ErrorAction Stop
} catch {}

Write-Host "`nRestoration complete." -ForegroundColor Cyan

