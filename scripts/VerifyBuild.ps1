#Requires -Version 5.1
<#
.SYNOPSIS
    ServerCore의 빌드·시험 검증을 한 번에 돌린다.

.DESCRIPTION
    이 스크립트가 ServerCore의 검증 절차를 소유한다. 규율 몇 가지를 코드로 박아 둔다.

      - 판정은 종료 코드로 한다. 로그 문자열이나 경고 개수로 판정하지 않는다.
      - ctest는 빌드를 하지 않는다. 그래서 시험 전에 반드시 빌드를 돌리고,
        빌드가 실패하면 시험 단계로 넘어가지 않는다(오래된 실행 파일로 통과하는 것을 막는다).
      - 표방하는 성질은 표방하는 모든 구성에서 검증한다. 기본값이 Debug·Release 둘 다인 이유다.
      - 건너뛴 것은 통과가 아니다. SKIPPED는 요약에서 PASS와 구분되고 사유가 함께 남는다.
      - 결과보다 대상을 먼저 적는다. 어느 툴체인·어느 빌드 트리·어느 커밋인지를 머리말에 찍는다.

    빌드 트리는 CLion이 쓰는 cmake-build-* 와 겹치지 않는 build\<preset> 아래에 만든다.
    CMakePresets.json을 그대로 쓰므로, 이 스크립트와 IDE가 같은 구성을 본다.

.PARAMETER Configuration
    Debug, Release, All(기본값) 중 하나.

.PARAMETER VsInstallPath
    쓸 Visual Studio 설치 경로를 직접 지정한다. 비우면 vswhere로 찾고 2022를 우선한다.

.PARAMETER Clean
    빌드 트리를 지우고 처음부터 구성한다.

.PARAMETER SkipTests
    시험을 건너뛴다. 요약에는 SKIPPED로 남는다.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\VerifyBuild.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'All')]
    [string] $Configuration = 'All',

    [string] $VsInstallPath = '',

    [switch] $Clean,

    [switch] $SkipTests
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path

# --preset은 현재 작업 디렉터리에서 CMakePresets.json을 찾는다. 스크립트 경로만 절대여도
# 호출자가 다른 저장소에 서 있으면 그쪽 preset을 읽어, 머리말의 저장소와 실제 빌드 대상이
# 갈릴 수 있다. 이후의 preset 명령은 모두 이 저장소를 기준으로 해석하게 고정한다.
Set-Location -LiteralPath $RepoRoot

# --------------------------------------------------------------------------
# 결과 수집
# --------------------------------------------------------------------------
$script:Results = New-Object System.Collections.ArrayList

function Add-Result {
    param([string] $Step, [string] $Status, [string] $Detail = '')
    [void] $script:Results.Add([pscustomobject]@{ Step = $Step; Status = $Status; Detail = $Detail })
}

function Write-Section {
    param([string] $Text)
    Write-Host ''
    Write-Host ('=' * 78) -ForegroundColor DarkGray
    Write-Host $Text -ForegroundColor Cyan
    Write-Host ('=' * 78) -ForegroundColor DarkGray
}

# --------------------------------------------------------------------------
# 툴체인 탐색
# --------------------------------------------------------------------------
function Find-VisualStudio {
    if ($VsInstallPath) {
        if (-not (Test-Path $VsInstallPath)) { throw "지정한 Visual Studio 경로가 없다: $VsInstallPath" }
        return $VsInstallPath
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe를 찾을 수 없다. Visual Studio가 설치되어 있는지 확인해라.' }

    $found = & $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format value -property installationPath
    if (-not $found) { throw 'C++ 도구(VC Tools)가 있는 Visual Studio 설치를 찾지 못했다.' }

    $candidates = @($found)

    # Visual Studio 2022를 우선해 설치 후보를 고른다. 선택한 경로는 머리말에 표시한다.
    $pinned = $candidates | Where-Object { $_ -like '*\2022\*' } | Select-Object -First 1
    if ($pinned) { return $pinned }
    return $candidates[0]
}

function Import-VcVars {
    param([string] $VsPath)

    $vcvars = Join-Path $VsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat를 찾을 수 없다: $vcvars" }

    $vcVarsPath = $null
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            # Windows 환경 변수 이름은 대소문자를 가리지 않는다. 그런데 Codex가 띄운 PowerShell에는
            # Path가, vcvars64.bat 뒤 cmd에는 MSVC를 앞에 붙인 PATH가 함께 있을 수 있다. 둘을 순서대로
            # Env:에 넣으면 나중의 Path가 컴파일러 경로를 다시 지워 버린다. vcvars가 만든 정확한 PATH를
            # 따로 보관해 마지막에 Process 환경에 한 번만 넣는다.
            if ($matches[1] -ieq 'PATH') {
                if ($matches[1] -ceq 'PATH' -or $null -eq $vcVarsPath) {
                    $vcVarsPath = $matches[2]
                }
            }
            else {
                Set-Item -Path ('Env:' + $matches[1]) -Value $matches[2]
            }
        }
    }

    if ($null -eq $vcVarsPath) { throw 'vcvars64.bat 출력에서 PATH를 찾지 못했다.' }
    [System.Environment]::SetEnvironmentVariable(
        'PATH', $vcVarsPath, [System.EnvironmentVariableTarget]::Process)

    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'vcvars64.bat를 불렀는데도 cl.exe가 PATH에 없다.'
    }
}

function Find-Tool {
    param([string] $Name, [string[]] $ExtraPaths)

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    foreach ($p in $ExtraPaths) {
        if ($p -and (Test-Path $p)) { return $p }
    }
    return $null
}

function Assert-BuildTreeTargetsRepository {
    param([string] $BuildDirectory)

    # 위치를 옮기는 것만으로는 충분하지 않다. CMakeCache는 configure가 실제로 쓴 source
    # directory를 남기므로, build 전에 그것을 머리말의 저장소와 다시 대조한다.
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath)) {
        throw "configure 뒤 CMakeCache.txt를 찾지 못했다: $cachePath"
    }

    $homeEntries = @(Select-String -LiteralPath $cachePath -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$')
    if ($homeEntries.Count -ne 1) {
        throw "CMakeCache.txt에서 CMAKE_HOME_DIRECTORY를 하나로 읽지 못했다: $cachePath"
    }

    $expectedRoot = ([System.IO.Path]::GetFullPath($RepoRoot)).TrimEnd('\', '/')
    $configuredRoot =
        ([System.IO.Path]::GetFullPath($homeEntries[0].Matches[0].Groups[1].Value)).TrimEnd('\', '/')
    if (-not [string]::Equals($expectedRoot, $configuredRoot,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw ("CMake가 다른 source directory를 구성했다. expected={0}, actual={1}" -f `
                $expectedRoot, $configuredRoot)
    }
}

# 외부 의존성이 없으므로 vcpkg 탐색을 수행하지 않는다.
# vcvars64.bat은 VCPKG_ROOT를 Visual Studio 번들 경로로 설정할 수 있으므로,
# 의존성 경로가 필요하면 이 환경 변수 대신 사용할 경로를 명시적으로 정한다.

# --------------------------------------------------------------------------
# 준비 — 대상을 먼저 적는다
# --------------------------------------------------------------------------
Write-Section 'ServerCore 검증 - 대상 확인'

$vsPath = Find-VisualStudio
Import-VcVars -VsPath $vsPath

$vsName = Split-Path -Leaf (Split-Path -Parent $vsPath)
$toolset = $env:VCToolsVersion

$vsCMake = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$vsCTest = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
$clionCMake = Join-Path $env:LOCALAPPDATA 'Programs\CLion\bin\cmake\win\x64\bin\cmake.exe'
$clionCTest = Join-Path $env:LOCALAPPDATA 'Programs\CLion\bin\cmake\win\x64\bin\ctest.exe'
$clionNinja = Join-Path $env:LOCALAPPDATA 'Programs\CLion\bin\ninja\win\x64\ninja.exe'

$cmakeExe = Find-Tool -Name 'cmake.exe' -ExtraPaths @($vsCMake, $clionCMake)
$ctestExe = Find-Tool -Name 'ctest.exe' -ExtraPaths @($vsCTest, $clionCTest)
$ninjaExe = Find-Tool -Name 'ninja.exe' -ExtraPaths @($clionNinja)

if (-not $cmakeExe) { throw 'cmake.exe를 찾지 못했다.' }
if (-not $ninjaExe) { throw 'ninja.exe를 찾지 못했다.' }

# ninja가 PATH에 없으면 CMake가 생성기를 못 찾는다. 찾은 것을 PATH 앞에 붙인다.
$env:PATH = (Split-Path -Parent $ninjaExe) + ';' + $env:PATH

$cmakeVersion = (& $cmakeExe --version | Select-Object -First 1)

$gitBranch = '(git 저장소 아님)'
$gitCommit = '(git 저장소 아님)'
if (Test-Path (Join-Path $RepoRoot '.git')) {
    $gitBranch = (& git -C $RepoRoot rev-parse --abbrev-ref HEAD)
    $gitCommit = (& git -C $RepoRoot rev-parse --short HEAD)
}

Write-Host ("저장소       : {0}" -f $RepoRoot)
Write-Host ("브랜치/커밋  : {0} @ {1}" -f $gitBranch, $gitCommit)
Write-Host ("Visual Studio: {0}  ({1})" -f $vsName, $vsPath)
Write-Host ("MSVC 툴셋    : {0}" -f $toolset)
Write-Host ("CMake        : {0}  [{1}]" -f $cmakeVersion, $cmakeExe)
Write-Host ("Ninja        : {0}" -f $ninjaExe)
Write-Host  "외부 의존성  : 없음"

$configs = @()
if ($Configuration -eq 'All') { $configs = @('Debug', 'Release') } else { $configs = @($Configuration) }
Write-Host ("검증 구성    : {0}" -f ($configs -join ', '))

# --------------------------------------------------------------------------
# 구성별 검증
# --------------------------------------------------------------------------
foreach ($cfg in $configs) {
    $preset = 'msvc-' + $cfg.ToLower()
    $buildDir = Join-Path $RepoRoot ('build\' + $preset)

    Write-Section ("[{0}] 구성 - 빌드 트리 {1}" -f $cfg, $buildDir)

    if ($Clean -and (Test-Path $buildDir)) {
        Write-Host '기존 빌드 트리를 지운다.'
        Remove-Item -Recurse -Force $buildDir
    }

    # 1) configure
    # CMAKE_MAKE_PROGRAM을 명시적으로 넘긴다. PATH에 ninja가 있는지에 기대면, 없을 때
    # "생성기를 못 찾았다"라는 앞뒤 없는 오류로 나타난다.
    & $cmakeExe --preset $preset "-DCMAKE_MAKE_PROGRAM=$ninjaExe"
    if ($LASTEXITCODE -ne 0) {
        Add-Result -Step ("$cfg / configure") -Status 'FAIL' -Detail ("종료 코드 {0}" -f $LASTEXITCODE)
        Add-Result -Step ("$cfg / build")     -Status 'SKIPPED' -Detail 'configure가 실패해서 돌지 않았다'
        Add-Result -Step ("$cfg / test")      -Status 'SKIPPED' -Detail 'configure가 실패해서 돌지 않았다'
        continue
    }

    try {
        Assert-BuildTreeTargetsRepository -BuildDirectory $buildDir
    }
    catch {
        Add-Result -Step ("$cfg / configure") -Status 'FAIL' -Detail $_.Exception.Message
        Add-Result -Step ("$cfg / build")     -Status 'SKIPPED' -Detail 'configure 대상 검증이 실패해서 돌지 않았다'
        Add-Result -Step ("$cfg / test")      -Status 'SKIPPED' -Detail 'configure 대상 검증이 실패해서 돌지 않았다'
        continue
    }
    Add-Result -Step ("$cfg / configure") -Status 'PASS' -Detail $preset

    # 2) build - ctest는 빌드를 하지 않으므로 여기가 유일한 빌드 지점이다.
    Write-Section ("[{0}] 빌드" -f $cfg)
    & $cmakeExe --build --preset $preset
    if ($LASTEXITCODE -ne 0) {
        Add-Result -Step ("$cfg / build") -Status 'FAIL' -Detail ("종료 코드 {0}" -f $LASTEXITCODE)
        Add-Result -Step ("$cfg / test")  -Status 'SKIPPED' -Detail '빌드가 실패해서 돌지 않았다'
        continue
    }
    Add-Result -Step ("$cfg / build") -Status 'PASS' -Detail '종료 코드 0'

    # 3) test
    if ($SkipTests) {
        Add-Result -Step ("$cfg / test") -Status 'SKIPPED' -Detail '-SkipTests 로 건너뛰었다'
        continue
    }
    if (-not $ctestExe) {
        Add-Result -Step ("$cfg / test") -Status 'SKIPPED' -Detail 'ctest.exe를 찾지 못했다'
        continue
    }

    Write-Section ("[{0}] 시험" -f $cfg)
    & $ctestExe --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        Add-Result -Step ("$cfg / test") -Status 'FAIL' -Detail ("종료 코드 {0}" -f $LASTEXITCODE)
        continue
    }
    Add-Result -Step ("$cfg / test") -Status 'PASS' -Detail '종료 코드 0'
}

# --------------------------------------------------------------------------
# 요약
# --------------------------------------------------------------------------
Write-Section '요약'

foreach ($r in $script:Results) {
    $color = 'Gray'
    if ($r.Status -eq 'PASS') { $color = 'Green' }
    if ($r.Status -eq 'FAIL') { $color = 'Red' }
    if ($r.Status -eq 'SKIPPED') { $color = 'Yellow' }
    Write-Host ("  {0,-22} {1,-8} {2}" -f $r.Step, $r.Status, $r.Detail) -ForegroundColor $color
}

$failCount = @($script:Results | Where-Object { $_.Status -eq 'FAIL' }).Count
$skipCount = @($script:Results | Where-Object { $_.Status -eq 'SKIPPED' }).Count
$passCount = @($script:Results | Where-Object { $_.Status -eq 'PASS' }).Count

Write-Host ''
Write-Host ("PASS {0} / FAIL {1} / SKIPPED {2}" -f $passCount, $failCount, $skipCount)

if ($failCount -gt 0) {
    Write-Host '검증 실패.' -ForegroundColor Red
    exit 1
}
if ($skipCount -gt 0) {
    Write-Host '실패는 없지만 건너뛴 단계가 있다. 건너뛴 것은 통과가 아니다.' -ForegroundColor Yellow
    exit 0
}
Write-Host '검증 통과.' -ForegroundColor Green
exit 0
