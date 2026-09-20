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

function Invoke-Checked {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE" }
}

if ($Stage -eq 'Setup') {
    foreach ($revision in @($env:CRAFT_REVISION, $env:BLUEPRINTS_REVISION)) {
        if ($revision -notmatch '^[a-f0-9]{40}$') { throw 'Craft revisions must be full commit IDs.' }
    }
    Invoke-Checked git @('config', '--global', 'core.longpaths', 'true')
    $bootstrap = Join-Path $env:RUNNER_TEMP 'CraftBootstrap.py'
    Invoke-WebRequest "https://raw.githubusercontent.com/KDE/craft/$env:CRAFT_REVISION/setup/CraftBootstrap.py" -OutFile $bootstrap
    Invoke-Checked python @($bootstrap, '--prefix', $craftRoot, '--branch', $env:CRAFT_REVISION, '--use-defaults')

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
$common = @('--ci-mode', '--options', '[CodeSigning]Enabled=False', '--options', '[Compile]Jobs=2')
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
