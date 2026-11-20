[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Endpoint,
    [string]$MayaRoot = 'C:\Program Files\Autodesk\Maya2024',
    [string]$OutputDirectory,
    [string]$MayaModulePath,
    [switch]$SourceAdapter,
    [switch]$ExtendedCapture
)
$ErrorActionPreference = 'Stop'
if ($SourceAdapter -and $MayaModulePath) { throw 'Choose SourceAdapter or MayaModulePath, not both.' }
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot ('test-results\maya-gui-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
if (-not (Test-Path -LiteralPath $Endpoint -PathType Leaf)) { throw 'The owned test instance endpoint does not exist.' }
if (Test-Path -LiteralPath (Join-Path $OutputDirectory 'maya-report.json')) { throw 'Choose a new output directory to preserve previous evidence.' }
$mayaExecutable = Join-Path $MayaRoot 'bin\maya.exe'
if (-not (Test-Path -LiteralPath $mayaExecutable -PathType Leaf)) { throw 'Maya executable was not found.' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$env:OPENFRAME_QA_ROOT = $projectRoot
$env:OPENFRAME_QA_ENDPOINT = $Endpoint
$env:OPENFRAME_QA_OUTPUT = $OutputDirectory
$env:OPENFRAME_QA_MODULE = if ($MayaModulePath) { [System.IO.Path]::GetFullPath($MayaModulePath) } elseif ($SourceAdapter) { Join-Path $projectRoot 'maya' } else { Join-Path $projectRoot 'dist\OpenFrame\maya' }
$env:OPENFRAME_QA_EXTENDED = if ($ExtendedCapture) { '1' } else { '0' }
$env:OPENFRAME_QA_BOOTSTRAP = Join-Path $OutputDirectory 'bootstrap.py'
@'
import os
import sys
from pathlib import Path
root = Path(os.environ['OPENFRAME_QA_ROOT'])
sys.path.insert(0, str(root / 'tools'))
import test_maya_runtime
import maya.cmds as cmds
import maya.utils

def check_openframe():
    try:
        test_maya_runtime.run(os.environ['OPENFRAME_QA_ENDPOINT'],
            str(root / 'test-results' / 'fixtures' / 'sync24-lossless.mkv'),
            str(Path(os.environ['OPENFRAME_QA_OUTPUT']) / 'maya-report.json'),
            playblast=True, maya_module_path=os.environ['OPENFRAME_QA_MODULE'],
            extended_capture=os.environ.get('OPENFRAME_QA_EXTENDED') == '1')
    finally:
        cmds.quit(force=True)

maya.utils.executeDeferred(check_openframe)
'@ | Set-Content -LiteralPath $env:OPENFRAME_QA_BOOTSTRAP -Encoding utf8
@'
python("import os; exec(compile(open(os.environ['OPENFRAME_QA_BOOTSTRAP'], encoding='utf-8-sig').read(), os.environ['OPENFRAME_QA_BOOTSTRAP'], 'exec'))");
'@ | Set-Content -LiteralPath (Join-Path $OutputDirectory 'startup.mel') -Encoding utf8
$env:MAYA_LOCATION = $MayaRoot
$env:MAYA_APP_DIR = Join-Path $OutputDirectory 'prefs'
$env:MAYA_DISABLE_CIP = '1'
$env:MAYA_DISABLE_CER = '1'
$env:MAYA_SKIP_USERSETUP_PY = '1'
$env:PATH = "$MayaRoot\bin;$env:WINDIR\System32;$env:WINDIR"
foreach ($name in @('PYTHONPATH','PYTHONHOME','MAYA_SCRIPT_PATH','MAYA_MODULE_PATH','MAYA_PLUG_IN_PATH','QT_PLUGIN_PATH','QT_QPA_PLATFORM','QT_SCALE_FACTOR')) {
    Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
}
$mayaProcess = Start-Process -FilePath $mayaExecutable -ArgumentList @('-noAutoloadPlugins', '-script', ('"' + (Join-Path $OutputDirectory 'startup.mel') + '"'), '-log', ('"' + (Join-Path $OutputDirectory 'maya.log') + '"')) -WindowStyle Hidden -PassThru
$result = [pscustomobject]@{
    processId = $mayaProcess.Id
    executable = $mayaExecutable
    report = Join-Path $OutputDirectory 'maya-report.json'
    scope = 'Separate empty Maya with synthetic geometry; endpoint must belong to the test.'
}
$result | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'process.json') -Encoding utf8
$result | ConvertTo-Json
