# Builds SOTModel with the MSVC toolchain directly (no CMake / vcvars needed).
#   .\build.ps1            release build  -> bin\SOTModel.exe
#   .\build.ps1 -Debug     debug build
#   .\build.ps1 -Clean     remove build outputs first
param([switch]$Debug, [switch]$Clean)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Find-Msvc {
    $roots = @("${env:ProgramFiles(x86)}\Microsoft Visual Studio", "$env:ProgramFiles\Microsoft Visual Studio")
    $cands = foreach ($r in $roots) { if (Test-Path $r) { Get-Item "$r\*\*\VC\Tools\MSVC" -ErrorAction SilentlyContinue } }
    foreach ($m in $cands) {
        foreach ($v in (Get-ChildItem $m.FullName -Directory | Sort-Object Name -Descending)) {
            foreach ($h in 'Hostx64', 'Hostx86') {
                $bin = Join-Path $v.FullName "bin\$h\x64"
                if ((Test-Path "$bin\cl.exe") -and (Test-Path (Join-Path $v.FullName 'lib\x64\msvcrt.lib'))) {
                    return @{ Root = $v.FullName; Bin = $bin; HostBin = (Join-Path $v.FullName "bin\$h\$($h.Substring(4))") }
                }
            }
        }
    }
    throw 'MSVC x64 toolset not found'
}

function Find-Sdk {
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
    $ver = Get-ChildItem "$kits\Lib" -Directory | Where-Object { Test-Path "$($_.FullName)\um\x64\d3d11.lib" } | Sort-Object Name -Descending | Select-Object -First 1
    if (-not $ver) { throw 'Windows 10/11 SDK not found' }
    return @{ Root = $kits; Ver = $ver.Name }
}

$msvc = Find-Msvc
$sdk = Find-Sdk
$si = "$($sdk.Root)\Include\$($sdk.Ver)"
$sl = "$($sdk.Root)\Lib\$($sdk.Ver)"
$env:INCLUDE = "$($msvc.Root)\include;$si\ucrt;$si\um;$si\shared;$si\winrt"
$env:LIB = "$($msvc.Root)\lib\x64;$sl\ucrt\x64;$sl\um\x64"
$env:VSLANG = '1033'
$env:PATH = "$($msvc.Bin);$($msvc.HostBin);$($sdk.Root)\bin\$($sdk.Ver)\x64;$env:PATH"

$cfg = if ($Debug) { 'debug' } else { 'release' }
$obj = Join-Path $root "build\$cfg"
$bin = Join-Path $root 'bin'
if ($Clean) { Remove-Item -Recurse -Force (Join-Path $root 'build') -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force $obj, $bin | Out-Null

$sources = @(Get-ChildItem "$root\src" -Recurse -Filter *.cpp) +
           @(Get-ChildItem "$root\third_party\imgui" -Recurse -Filter *.cpp)
$headers = @(Get-ChildItem "$root\src", "$root\third_party" -Recurse -Include *.h, *.hpp)
$newestHeader = ($headers | Measure-Object -Property LastWriteTime -Maximum).Maximum

$common = @('/nologo', '/std:c++20', '/EHsc', '/utf-8', '/W3', '/MP', '/permissive-', '/Zc:__cplusplus',
            '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/DUNICODE', '/D_UNICODE', '/D_CRT_SECURE_NO_WARNINGS',
            "/I$root\src", "/I$root\third_party\imgui", "/I$root\third_party\imgui\backends")
if ($Debug) { $common += @('/Od', '/Zi', '/MDd', '/D_DEBUG', "/Fd$obj\vc.pdb") }
else { $common += @('/O2', '/Oi', '/GL', '/Gy', '/MD', '/DNDEBUG', '/Zi', "/Fd$obj\vc.pdb") }

$stale = @()
$objs = @()
foreach ($s in $sources) {
    $rel = $s.FullName.Substring($root.Length + 1) -replace '[\\/:]', '_'
    $o = Join-Path $obj ($rel -replace '\.cpp$', '.obj')
    $objs += $o
    if (-not (Test-Path $o) -or (Get-Item $o).LastWriteTime -lt $s.LastWriteTime -or (Get-Item $o).LastWriteTime -lt $newestHeader) {
        $stale += @{ Src = $s.FullName; Obj = $o }
    }
}

if ($stale.Count) {
    Write-Host "Compiling $($stale.Count) file(s) [$cfg]"
    # cl /MP compiles in parallel only when all sources share one /Fo directory; group per output name instead
    $tmp = Join-Path $obj 'stale'
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $args = $common + @('/c', "/Fo$tmp\\") + ($stale | ForEach-Object { $_.Src })
    & cl.exe @args
    if ($LASTEXITCODE) { throw "compile failed ($LASTEXITCODE)" }
    foreach ($st in $stale) {
        $produced = Join-Path $tmp ([IO.Path]::GetFileNameWithoutExtension($st.Src) + '.obj')
        Move-Item -Force $produced $st.Obj
    }
}

# embedded resources (icon fonts)
$rcSrc = Join-Path $root 'src\ui\Resources.rc'
$res = Join-Path $obj 'Resources.res'
$rcInputs = @(Get-Item $rcSrc) + @(Get-ChildItem "$root\third_party\phosphor" -Filter *.ttf)
$resStale = -not (Test-Path $res) -or ($rcInputs | Where-Object { $_.LastWriteTime -gt (Get-Item $res).LastWriteTime })
if ($resStale) {
    Write-Host 'Compiling resources'
    & rc.exe /nologo /fo $res $rcSrc
    if ($LASTEXITCODE) { throw "rc failed ($LASTEXITCODE)" }
}
$objs += $res

$exe = Join-Path $bin 'SOTModel.exe'
$needLink = -not (Test-Path $exe) -or $stale.Count -gt 0 -or $resStale
if ($needLink) {
    Write-Host 'Linking'
    $libs = @('d3d11.lib', 'dxgi.lib', 'd3dcompiler.lib', 'bcrypt.lib', 'user32.lib', 'gdi32.lib', 'shell32.lib', 'ole32.lib', 'comdlg32.lib', 'dwmapi.lib', 'imm32.lib', 'shlwapi.lib', 'winmm.lib')
    $largs = @('/nologo', "/OUT:$exe", '/SUBSYSTEM:WINDOWS', '/DEBUG', "/PDB:$bin\SOTModel.pdb") + $objs + $libs
    if (-not $Debug) { $largs += @('/LTCG', '/OPT:REF', '/OPT:ICF') }
    & link.exe @largs
    if ($LASTEXITCODE) { throw "link failed ($LASTEXITCODE)" }
}
Write-Host "OK -> $exe"
