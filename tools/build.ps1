[CmdletBinding()]
param(
    [string]$QtRoot = $env:OPENFRAME_QT_ROOT,
    [string]$FfmpegRoot = $env:OPENFRAME_FFMPEG_ROOT,
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')]
    [string]$Configuration = 'RelWithDebInfo',
    [string]$BuildDirectory,
    [string]$PackageDirectory,
    [switch]$Package,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectRoot 'build' }
# Reuse the locally configured SDKs when launched from Explorer. Explicit
# parameters and OPENFRAME_* environment variables take precedence.
$localConfigPath = Join-Path $projectRoot 'build.local.json'
if (Test-Path -LiteralPath $localConfigPath -PathType Leaf) {
    $localConfig = Get-Content -LiteralPath $localConfigPath -Raw | ConvertFrom-Json
    if (-not $QtRoot) { $QtRoot = $localConfig.QtRoot }
    if (-not $FfmpegRoot) { $FfmpegRoot = $localConfig.FfmpegRoot }
}
$cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
if ((-not $QtRoot -or -not $FfmpegRoot) -and (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    foreach ($line in (Get-Content -LiteralPath $cachePath)) {
        if (-not $QtRoot -and $line -match '^Qt6_DIR:[^=]+=(.+)$') {
            $QtRoot = [System.IO.Path]::GetFullPath((Join-Path $Matches[1] '..\..\..'))
        } elseif (-not $FfmpegRoot -and $line -match '^OPENFRAME_FFMPEG_ROOT:[^=]+=(.+)$') {
            $FfmpegRoot = $Matches[1]
        }
    }
}
if (-not $QtRoot -or -not (Test-Path -LiteralPath (Join-Path $QtRoot 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw 'Set OPENFRAME_QT_ROOT or pass -QtRoot with a Qt 6.8 MSVC x64 SDK.'
}
if (-not $FfmpegRoot -or -not (Test-Path -LiteralPath (Join-Path $FfmpegRoot 'include\libavformat\avformat.h'))) {
    throw 'Set OPENFRAME_FFMPEG_ROOT or pass -FfmpegRoot with the network-disabled FFmpeg SDK.'
}
# Every build starts with a clean distribution, including previous ZIP archives.
$resolvedProjectRoot = [System.IO.Path]::GetFullPath($projectRoot).TrimEnd([char[]]'\/')
$distPath = [System.IO.Path]::GetFullPath((Join-Path $resolvedProjectRoot 'dist'))
if (-not [string]::Equals([System.IO.Path]::GetDirectoryName($distPath), $resolvedProjectRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to clean a distribution outside the project root.'
}
if (Test-Path -LiteralPath $distPath) {
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($distPath)
    while ($pending.Count -gt 0) {
        $item = Get-Item -LiteralPath $pending.Pop() -Force
        if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) {
            throw "Refusing to clean a linked distribution path: $($item.FullName)"
        }
        if ($item.PSIsContainer) {
            foreach ($child in (Get-ChildItem -LiteralPath $item.FullName -Force)) {
                $pending.Push($child.FullName)
            }
        }
    }
    Write-Host "Cleaning distribution: $distPath"
    Remove-Item -LiteralPath $distPath -Recurse -Force
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio 2022 C++ tools are required.' }
    $vsRoot = & $vswhere -latest -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'Visual Studio 2022 C++ tools were not found.' }
    & (Join-Path $vsRoot 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
}
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
$env:VSLANG = '1033'
$env:PATH = (Join-Path $FfmpegRoot 'bin') + ';' + (Join-Path $QtRoot 'bin') + ';' + $env:PATH
$env:QT_PLUGIN_PATH = Join-Path $QtRoot 'plugins'
if (-not $env:QT_QPA_FONTDIR) { $env:QT_QPA_FONTDIR = Join-Path $env:WINDIR 'Fonts' }
& $cmake -S $projectRoot -B $BuildDirectory -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" "-DCMAKE_PREFIX_PATH=$QtRoot" "-DOPENFRAME_FFMPEG_ROOT=$FfmpegRoot" -DOPENFRAME_BUILD_TESTS=ON -DOPENFRAME_BUILD_UI=ON
if ($LASTEXITCODE) { throw "CMake configure failed: $LASTEXITCODE" }
# Some Windows CMake/MSVC combinations decode a UTF-8 localized /showIncludes
# prefix as OEM text. Repair that generated metadata so Ninja tracks headers.
$prefixRepaired = $false
if ($PSVersionTable.PSEdition -eq 'Core') {
    [System.Text.Encoding]::RegisterProvider([System.Text.CodePagesEncodingProvider]::Instance)
}
foreach ($metadata in (Get-ChildItem -LiteralPath (Join-Path $BuildDirectory 'CMakeFiles') -Recurse -File -Filter 'CMakeCXXCompiler.cmake')) {
    $content = Get-Content -LiteralPath $metadata.FullName -Raw
    $match = [regex]::Match($content, 'set\(CMAKE_CL_SHOWINCLUDES_PREFIX "([^"]*)"\)')
    if ($match.Success -and $match.Groups[1].Value -match '[\u2568\u2564]') {
        $decoded = [System.Text.UTF8Encoding]::new($false, $true).GetString([System.Text.Encoding]::GetEncoding(866).GetBytes($match.Groups[1].Value))
        $replacement = 'set(CMAKE_CL_SHOWINCLUDES_PREFIX "' + $decoded.TrimEnd() + ' ")'
        $content = $content.Replace($match.Value, $replacement)
        [System.IO.File]::WriteAllText($metadata.FullName, $content, [System.Text.UTF8Encoding]::new($false))
        $prefixRepaired = $true
    }
}
if ($prefixRepaired) {
    & $cmake -S $projectRoot -B $BuildDirectory
    if ($LASTEXITCODE) { throw "CMake dependency-prefix repair failed: $LASTEXITCODE" }
}
& $cmake --build $BuildDirectory --parallel
if ($LASTEXITCODE) { throw "Compilation failed: $LASTEXITCODE" }
if (-not $SkipTests) {
    & (Join-Path $PSScriptRoot 'prepare-test-fixtures.ps1') -BuildDirectory $BuildDirectory
    $env:QT_QPA_PLATFORM = 'offscreen'
    & ctest --test-dir $BuildDirectory --output-on-failure -C $Configuration
    $testExit = $LASTEXITCODE
    Remove-Item Env:QT_QPA_PLATFORM -ErrorAction SilentlyContinue
    if ($testExit) { throw "Tests failed: $testExit" }
}
if ($Package) {
    $executable = @(Get-ChildItem -LiteralPath $BuildDirectory -Recurse -File -Filter 'OpenFrame.exe' | Where-Object { $_.FullName -notmatch 'CMakeFiles' })
    if ($executable.Count -ne 1) { throw 'Expected exactly one built OpenFrame.exe.' }
    $destination = if ($PackageDirectory) { [System.IO.Path]::GetFullPath($PackageDirectory) } else { Join-Path $projectRoot 'dist\OpenFrame-local' }
    $destinationPrefix = $destination.TrimEnd([char[]]'\/') + [System.IO.Path]::DirectorySeparatorChar
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath $executable[0].FullName -Destination $destination -Force
    & (Join-Path $QtRoot 'bin\windeployqt.exe') --release --no-translations --compiler-runtime --skip-plugin-types 'networkinformation,tls' (Join-Path $destination 'OpenFrame.exe')
    if ($LASTEXITCODE) { throw "Qt deployment failed: $LASTEXITCODE" }
    # The bundled preview/QA mode must also work without the development SDK.
    Copy-Item -LiteralPath (Join-Path $QtRoot 'plugins\platforms\qoffscreen.dll') -Destination (Join-Path $destination 'platforms') -Force
    # App-local redistributable CRT keeps the portable build independent of an
    # existing Visual C++ installation; use only Microsoft's redist directory.
    if (-not $env:VCToolsRedistDir) { throw 'VCToolsRedistDir is required for portable CRT deployment.' }
    $crtDirectory = Join-Path $env:VCToolsRedistDir 'x64\Microsoft.VC143.CRT'
    foreach ($dll in (Get-ChildItem -LiteralPath $crtDirectory -File -Filter '*.dll')) {
        Copy-Item -LiteralPath $dll.FullName -Destination $destination -Force
    }
    # Qt's multimedia plugin may bundle FFmpeg. Always install our no-network DLLs last.
    foreach ($dll in (Get-ChildItem -LiteralPath (Join-Path $FfmpegRoot 'bin') -File -Filter '*.dll')) {
        Copy-Item -LiteralPath $dll.FullName -Destination $destination -Force
    }
    [System.IO.File]::WriteAllText((Join-Path $destination 'qt.conf'), "[Paths]`nPrefix=.`nPlugins=.`n", [System.Text.UTF8Encoding]::new($false))
    foreach ($name in @('maya', 'docs', 'licenses')) {
        if (Test-Path -LiteralPath (Join-Path $projectRoot $name)) {
            Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $destination -Recurse -Force
        }
    }
    foreach ($name in @('README.md', 'LICENSE', 'THIRD_PARTY.md', 'Install.mel')) {
        if (Test-Path -LiteralPath (Join-Path $projectRoot $name)) {
            Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $destination -Force
        }
    }
    $binaryManifest = @(Get-ChildItem -LiteralPath $destination -Recurse -File | Where-Object { $_.Extension -in @('.exe', '.dll') } | ForEach-Object {
        [pscustomobject]@{
            path = $_.FullName.Substring($destinationPrefix.Length).Replace('\', '/')
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    $pythonManifest = @(Get-ChildItem -LiteralPath (Join-Path $destination 'maya') -Recurse -File -Filter '*.py' | ForEach-Object {
        [pscustomobject]@{
            path = $_.FullName.Substring($destinationPrefix.Length).Replace('\', '/')
            bytes = $_.Length
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    [pscustomobject]@{
        schemaVersion = 1
        application = 'OpenFrame'
        version = '0.4.0'
        builtAtUtc = [datetime]::UtcNow.ToString('o')
        configuration = $Configuration
        architecture = 'windows-x64'
        dependencies = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'dependencies.json') -Raw | ConvertFrom-Json
        binaries = $binaryManifest
        pythonModules = $pythonManifest
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $destination 'build-info.json') -Encoding utf8
    Write-Host "Portable application: $destination"
    & (Join-Path $PSScriptRoot 'package-zip.ps1') -PackageDirectory $destination
}
