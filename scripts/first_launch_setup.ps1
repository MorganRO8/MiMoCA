param(
    [string]$InstallRoot = $(Split-Path -Parent $PSScriptRoot),
    [switch]$SkipModelWarmup,
    [switch]$PrewarmModels,
    [int]$WarmupTimeoutSec = 240,
    [switch]$LaunchUi
)

$ErrorActionPreference = 'Stop'

function Resolve-PythonExecutable {
    $packaged = Join-Path $InstallRoot 'runtime\python\python.exe'
    if (Test-Path $packaged) {
        return $packaged
    }

    $fallback = Get-Command py -ErrorAction SilentlyContinue
    if ($fallback) {
        return 'py -3.11'
    }

    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        return 'python'
    }

    throw 'No Python interpreter found. Install Python 3.11+ or repair MiMoCA runtime payload.'
}

function Save-WarmupState {
    param(
        [string]$Status,
        [string]$Message
    )
    $payload = @{
        status = $Status
        message = $Message
        updated_at = (Get-Date).ToUniversalTime().ToString('o')
    }
    $payload | ConvertTo-Json -Depth 4 | Set-Content -Path $warmupStatePath -Encoding UTF8

    if (Test-Path $configPath) {
        $raw = Get-Content -Path $configPath -Raw
        $cfg = if ($raw.Trim().Length -gt 0) { $raw | ConvertFrom-Json } else { [pscustomobject]@{} }
        $cfg | Add-Member -NotePropertyName warmup -NotePropertyValue ([pscustomobject]@{}) -Force
        $cfg.warmup | Add-Member -NotePropertyName status -NotePropertyValue $Status -Force
        $cfg.warmup | Add-Member -NotePropertyName message -NotePropertyValue $Message -Force
        $cfg.warmup | Add-Member -NotePropertyName updated_at -NotePropertyValue $payload.updated_at -Force
        $cfg | ConvertTo-Json -Depth 8 | Set-Content -Path $configPath -Encoding UTF8
    }
}

$venvPath = Join-Path $InstallRoot '.mimoca_sidecar_venv'
$requirementsPath = Join-Path $InstallRoot 'python\requirements.txt'
$configPath = Join-Path $InstallRoot 'mimoca_app_config.json'

$localDataRoot = Join-Path $env:LOCALAPPDATA 'MiMoCA'
$modelCacheRoot = Join-Path $localDataRoot 'model_cache'
$warmupStatePath = Join-Path $modelCacheRoot 'warmup_state.json'
New-Item -Path $localDataRoot -ItemType Directory -Force | Out-Null
New-Item -Path $modelCacheRoot -ItemType Directory -Force | Out-Null

$pythonExe = Resolve-PythonExecutable
$bootstrapArgs = @('python/bootstrap_sidecar_env.py', '--venv-path', $venvPath, '--requirements', $requirementsPath)

if ($pythonExe -like 'py*') {
    Push-Location $InstallRoot
    try {
        & py -3.11 @bootstrapArgs | Out-Null
    }
    finally {
        Pop-Location
    }
}
else {
    Push-Location $InstallRoot
    try {
        & $pythonExe @bootstrapArgs | Out-Null
    }
    finally {
        Pop-Location
    }
}

if (-not (Test-Path $configPath)) {
    $config = @{
        sidecar_env_path = $venvPath
        planner_mode = 'llm'
        planner = @{
            mode = 'llm'
            provider = 'openai_compatible'
            base_url = 'https://api.openai.com/v1'
            model = 'gpt-4o-mini'
        }
        model_paths = @{
            stt_model = 'distil-large-v3'
            vision_model = 'yolov8s-worldv2.pt'
            gesture_model_path = 'python/models/hand_landmarker.task'
        }
        cache = @{
            model_cache_root = $modelCacheRoot
            stt_cache_root = (Join-Path $modelCacheRoot 'stt')
            vision_cache_root = (Join-Path $modelCacheRoot 'vision')
            gesture_cache_root = (Join-Path $modelCacheRoot 'gesture')
        }
        warmup = @{
            status = 'pending'
            message = 'warmup not started'
        }
    } | ConvertTo-Json -Depth 8
    Set-Content -Path $configPath -Value $config -Encoding UTF8
}

if ($SkipModelWarmup) {
    Save-WarmupState -Status 'skipped' -Message 'Warmup skipped by operator. Models will download in background.'
} elseif ($PrewarmModels) {
    $servicePy = Join-Path $InstallRoot 'python\service.py'
    $venvPython = Join-Path $venvPath 'Scripts\python.exe'
    if (-not (Test-Path $venvPython)) {
        Save-WarmupState -Status 'failed' -Message 'Warmup failed: sidecar venv python missing.'
    } else {
        $process = $null
        $previousConfigEnv = $env:MIMOCA_APP_CONFIG_PATH
        try {
            $env:MIMOCA_APP_CONFIG_PATH = $configPath
            Save-WarmupState -Status 'downloading' -Message 'Warmup started; checking sidecar readiness.'
            $process = Start-Process -FilePath $venvPython -ArgumentList @($servicePy) -PassThru -WindowStyle Hidden
            $deadline = (Get-Date).AddSeconds($WarmupTimeoutSec)
            $isReady = $false
            while ((Get-Date) -lt $deadline) {
                Start-Sleep -Seconds 2
                try {
                    $health = Invoke-RestMethod -Uri 'http://127.0.0.1:8080/health' -Method Get -TimeoutSec 6
                    $sttReady = ($health.startup.modalities.stt.status -eq 'ready' -or $health.startup.modalities.stt.status -eq 'disabled')
                    $visionReady = ($health.startup.modalities.vision.status -eq 'ready' -or $health.startup.modalities.vision.status -eq 'disabled')
                    if ($sttReady -and $visionReady) {
                        $isReady = $true
                        break
                    }
                } catch {
                    # keep polling until timeout
                }
            }
            if ($isReady) {
                Save-WarmupState -Status 'completed' -Message 'Model warmup completed before UI launch.'
            } else {
                Save-WarmupState -Status 'downloading' -Message 'Warmup timed out; models still downloading in background.'
            }
        } catch {
            Save-WarmupState -Status 'failed' -Message ("Warmup failed: " + $_.Exception.Message)
        } finally {
            if ($process -and -not $process.HasExited) {
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
            $env:MIMOCA_APP_CONFIG_PATH = $previousConfigEnv
        }
    }
} else {
    if (-not (Test-Path $warmupStatePath)) {
        Save-WarmupState -Status 'pending' -Message 'Warmup not pre-run; models will initialize on first launch.'
    }
}

if ($LaunchUi) {
    $uiBinary = Join-Path $InstallRoot 'mimoca.exe'
    if (Test-Path $uiBinary) {
        Start-Process -FilePath $uiBinary | Out-Null
    } else {
        Write-Host "LaunchUi requested, but $uiBinary was not found."
    }
}

if (-not $SkipModelWarmup -and -not $PrewarmModels) {
    Write-Host 'MiMoCA first-launch setup complete. Use -PrewarmModels to pre-initialize sidecar models before launching UI.'
}
