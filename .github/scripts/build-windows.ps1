# SPDX-FileCopyrightText: 2026 Kdenlive contributors
# SPDX-License-Identifier: BSD-2-Clause

param(
    [Parameter(Mandatory)]
    [ValidateSet('Setup', 'Dependencies', 'Build', 'Package')]
    [string]$Stage
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$craftRoot = $env:KDENLIVE_CRAFT_ROOT
if (-not $craftRoot) { throw 'KDENLIVE_CRAFT_ROOT must point to a dedicated Craft installation.' }

function Get-CraftPath {
    param([string]$SearchPath, [string]$Root)
    $prefix = $Root.Replace('/', '\').TrimEnd('\') + '\'
    ($SearchPath -split ';' | Where-Object {
        $entry = $_.Trim().Trim('"').Replace('/', '\').TrimEnd('\') + '\'
        # Hosted runners expose MinGW's native make, which cannot read MSYS /c/... paths.
        # Keep Craft's own MSYS installation; its shell adds the correct tools as needed.
        $entry.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
            $entry -notmatch '(?i)\\(mingw\d*|msys\d*|cygwin\d*|strawberry)\\'
    }) -join ';'
}

# Apply before bootstrap as well as in each subsequent, fresh Actions step.
$env:PATH = Get-CraftPath $env:PATH $craftRoot
# GitHub's Windows images export the Android SDK/NDK. CraftBootstrap treats any host with ANDROID_SDK_ROOT and
# ANDROID_NDK as Android and selects BuildType=MinSizeRel, for which KDE publishes no Windows binary cache:
# Craft then builds its whole toolchain from source and fails on gettext.
foreach ($name in 'ANDROID_SDK_ROOT', 'ANDROID_NDK', 'ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT', 'ANDROID_NDK_LATEST_HOME', 'ANDROID_HOME') {
    Remove-Item "Env:$name" -ErrorAction SilentlyContinue
}
$logDir = Join-Path $repoRoot 'build-release'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$stageLog = Join-Path $logDir "$($Stage.ToLowerInvariant()).log"

function Write-CacheDiagnostics {
    param([string]$Level = 'warning')
    # Job logs are only visible to signed-in users, check-run annotations are public. Surface why Craft
    # did (not) restore packages from the KDE binary cache: a miss turns the bootstrap into a from-source
    # build of its whole toolchain.
    if (-not (Test-Path -LiteralPath $stageLog)) { return }
    $pattern = 'from cache|Could not find|missmatch|not compatible|Cached config|Local config|Failed to fetch|Fetch Json|manifest\.json'
    $lines = @(Select-String -LiteralPath $stageLog -Pattern $pattern | ForEach-Object { $_.Line.Trim() })
    if (-not $lines) { return }
    $interesting = @($lines | Where-Object { $_ -notmatch 'Trying to restore' })
    $summary = "matched $($lines.Count) cache lines; non-restore lines: $($interesting.Count)"
    $detail = ((@($summary) + ($interesting | Select-Object -First 60) + ($lines | Select-Object -First 20)) -join "`n") `
        -replace '\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\))', ''
    if ($detail.Length -gt 12000) { $detail = $detail.Substring(0, 12000) }
    $detail = $detail.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
    Write-Host "::$Level title=Craft binary cache ($Stage)::$detail"
}

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)
    $tail = [Collections.Generic.Queue[string]]::new()
    & $Command @Arguments 2>&1 | Tee-Object -FilePath $stageLog -Append | ForEach-Object {
        Write-Host $_
        $tail.Enqueue([string]$_)
        if ($tail.Count -gt 30) { $null = $tail.Dequeue() }
    }
    if ($LASTEXITCODE -ne 0) {
        $exitCode = $LASTEXITCODE
        # Preserve the actual dependency error in the check annotations, even if bootstrap fails.
        $detail = ($tail.ToArray() -join "`n") -replace '\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\))', ''
        if ($detail.Length -gt 6000) { $detail = $detail.Substring($detail.Length - 6000) }
        $detail = $detail.Replace('%', '%25').Replace("`r", '%0D').Replace("`n", '%0A')
        Write-Host "::error title=Craft $Stage failed::$detail"
        Write-CacheDiagnostics -Level warning
        throw "$Command failed with exit code $exitCode; see $stageLog"
    }
}

if ($Stage -eq 'Setup') {
    foreach ($revision in @($env:CRAFT_REVISION, $env:BLUEPRINTS_REVISION)) {
        if ($revision -notmatch '^[a-f0-9]{40}$') { throw 'Craft revisions must be full commit IDs.' }
    }
    Invoke-Checked git @('config', '--global', 'core.longpaths', 'true')
    $bootstrap = Join-Path $env:RUNNER_TEMP 'CraftBootstrap.py'
    Invoke-WebRequest "https://raw.githubusercontent.com/KDE/craft/$env:CRAFT_REVISION/setup/CraftBootstrap.py" -OutFile $bootstrap
    Invoke-Checked python @($bootstrap, '--prefix', $craftRoot, '--branch', $env:CRAFT_REVISION, '--use-defaults')
    Write-CacheDiagnostics -Level notice
    # The binary cache only exists for Release/RelWithDebInfo; any other build type rebuilds everything.
    $buildType = Select-String -LiteralPath (Join-Path $craftRoot 'etc/CraftSettings.ini') -Pattern '^\s*BuildType\s*=\s*(\S+)' |
        ForEach-Object { $_.Matches[0].Groups[1].Value } | Select-Object -First 1
    if ($buildType -notin @('RelWithDebInfo', 'Release')) { throw "Unexpected Craft BuildType '$buildType'" }

    # Bootstrap installs Craft and its blueprint repository. Pin both before building.
    $repositories = @{
        (Join-Path $craftRoot 'craft') = $env:CRAFT_REVISION
        (Join-Path $craftRoot 'etc/blueprints/locations/craft-blueprints-kde') = $env:BLUEPRINTS_REVISION
    }
    foreach ($repository in $repositories.Keys) {
        Invoke-Checked git @('-C', $repository, 'fetch', '--depth', '1', 'origin', $repositories[$repository])
        Invoke-Checked git @('-C', $repository, 'checkout', '--detach', $repositories[$repository])
    }
    return
}

# Craft sets up MSVC, Qt, MLT and KDE paths, and changes the current directory.
. (Join-Path $craftRoot 'craft/craftenv.ps1')
Set-Location -LiteralPath $repoRoot
$common = @('--ci-mode', '--options', '[CodeSigning]Enabled=False', '--options', '[Compile]Jobs=4')
# KDE's msvc2022 binary cache (CacheVersion 26.05) carries KDE Gear only up to 26.04.x for these runtime
# dependencies of Kdenlive; pin them to the cached release instead of building 26.08 from source.
$common += @('--options', 'kde/kdenetwork/kio-extras.version=26.04.2', '--options', 'kde/applications/libkexiv2.version=26.04.2')
$application = @('--options', 'kdenlive.version=master', '--options', "kdenlive.srcDir=$repoRoot",
    '--options', 'kdenlive.buildTests=False', '--options', 'kdenlive.packageAppx=False')

switch ($Stage) {
    'Dependencies' {
        Invoke-Checked craft ($common + $application + @('--install-deps', 'kdenlive'))
        Invoke-Checked craft ($common + @('nsis'))
        Invoke-Checked craft ($common + @('ninja'))
    }
    'Build' {
        # Never satisfy the application itself with an upstream binary cache entry.
        Invoke-Checked craft ($common + $application + @('--no-cache', 'kdenlive'))
        $testBuild = Join-Path $repoRoot 'build-mcp-windows'
        Invoke-Checked craft ($common + @('--run', 'cmake', '-S', (Join-Path $repoRoot 'tests/mcp'),
            '-B', $testBuild, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$craftRoot"))
        Invoke-Checked craft ($common + @('--run', 'cmake', '--build', $testBuild, '--parallel', '2'))
        Invoke-Checked craft ($common + @('--run', 'ctest', '--test-dir', $testBuild, '--output-on-failure'))
    }
    'Package' {
        # NullsoftInstallerPackager creates both an NSIS installer and a portable 7z.
        Invoke-Checked craft ($common + $application + @('--options', '[Packager]PackageType=NullsoftInstallerPackager',
            '--package', 'kdenlive'))
        $packageOutput = & craft @common @application -q --get 'packageDestinationDir()' kdenlive
        if ($LASTEXITCODE -ne 0) { throw 'Could not locate Craft packages.' }
        $packageDir = ($packageOutput | Out-String).Trim()
        $installers = @(Get-ChildItem -LiteralPath $packageDir -File -Filter 'kdenlive*.exe')
        $archives = @(Get-ChildItem -LiteralPath $packageDir -File -Filter 'kdenlive*.7z' |
            Where-Object Name -NotMatch '-dbg\.')
        if ($installers.Count -ne 1 -or $archives.Count -ne 1) {
            throw 'Expected exactly one Kdenlive installer and one portable archive.'
        }

        $output = Join-Path $repoRoot 'build-release/packages'
        New-Item -ItemType Directory -Force -Path $output | Out-Null
        $revision = (& git -C $repoRoot rev-parse HEAD).Trim()
        if ($LASTEXITCODE -ne 0) { throw 'Could not determine the source revision.' }
        $name = "kdenlive-mcp-$($revision.Substring(0, 12))-windows-x64"
        Copy-Item -LiteralPath $installers[0].FullName -Destination (Join-Path $output "$name-setup.exe")
        Copy-Item -LiteralPath $archives[0].FullName -Destination (Join-Path $output "$name-portable.7z")

        $unpacked = Join-Path $repoRoot 'build-release/portable'
        Invoke-Checked craft ($common + @('--run', '7za', 'x', $archives[0].FullName, "-o$unpacked", '-y'))
        $executables = @(Get-ChildItem -LiteralPath $unpacked -Recurse -File -Filter 'kdenlive.exe')
        if ($executables.Count -ne 1) { throw 'The portable archive must contain exactly one kdenlive.exe.' }
        # Do not inherit Craft's DLL/plugin paths: test the distributed package itself.
        Invoke-Checked python @((Join-Path $PSScriptRoot 'smoke-mcp.py'), $executables[0].FullName,
            '--log', (Join-Path $repoRoot 'build-release/mcp-smoke.log'), '--craft-root', $craftRoot)

        @{
            source_revision = $revision
            craft_revision = $env:CRAFT_REVISION
            blueprints_revision = $env:BLUEPRINTS_REVISION
            architecture = 'windows-x64'
            compiler = 'msvc2022'
            signed = $false
        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'build-info.json') -Encoding utf8NoBOM
        $checksums = Get-ChildItem -LiteralPath $output -File | Where-Object Name -NE 'SHA256SUMS' |
            Sort-Object Name | ForEach-Object {
                $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
                "$hash  $($_.Name)"
            }
        $checksums | Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS') -Encoding ascii
    }
}
