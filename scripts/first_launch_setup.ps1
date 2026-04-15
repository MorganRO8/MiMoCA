param(
    [string]$InstallRoot = $(Split-Path -Parent $PSScriptRoot),
    [switch]$SkipModelWarmup
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

$venvPath = Join-Path $InstallRoot '.mimoca_sidecar_venv'
$requirementsPath = Join-Path $InstallRoot 'python\requirements.txt'
$configPath = Join-Path $InstallRoot 'mimoca_app_config.json'

$localDataRoot = Join-Path $env:LOCALAPPDATA 'MiMoCA'
$modelCacheRoot = Join-Path $localDataRoot 'model_cache'
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
} else {
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
    } | ConvertTo-Json -Depth 6
    Set-Content -Path $configPath -Value $config -Encoding UTF8
}

if (-not $SkipModelWarmup) {
    Write-Host 'MiMoCA first-launch setup complete. Model downloads continue automatically when sidecar starts.'
}
