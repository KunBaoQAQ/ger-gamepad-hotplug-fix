# =====================================================================
#  噬神者：解放重生  手柄拔插失效修复  卸载脚本
# =====================================================================
param([string]$GameDir = "")

$ErrorActionPreference = 'Stop'
$pkgRoot = Split-Path -Parent $MyInvocation.MyCommand.Path

function Find-GameDir {
    $cand = New-Object System.Collections.Generic.List[string]
    if ($GameDir) { $cand.Add($GameDir) }
    $cand.Add($pkgRoot)
    try { $cand.Add((Get-Location).Path) } catch { }

    try {
        $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath
        if (-not $steam) {
            $steam = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' -ErrorAction SilentlyContinue).InstallPath
        }
        if ($steam) {
            $cand.Add((Join-Path $steam 'steamapps\common\GOD EATER RESURRECTION'))
            $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
            if (Test-Path -LiteralPath $vdf) {
                $raw = Get-Content -LiteralPath $vdf -Raw
                foreach ($m in [regex]::Matches($raw, '"path"\s+"([^"]+)"')) {
                    $p = $m.Groups[1].Value -replace '\\\\', '\'
                    $cand.Add((Join-Path $p 'steamapps\common\GOD EATER RESURRECTION'))
                }
            }
        }
    } catch { }

    foreach ($p in $cand) {
        if ($p -and (Test-Path -LiteralPath (Join-Path $p 'GER.exe'))) {
            return (Resolve-Path -LiteralPath $p).Path
        }
    }
    return $null
}

Write-Host ''
Write-Host '==========================================================' -ForegroundColor Cyan
Write-Host ' 噬神者：解放重生 - 手柄修复包 卸载' -ForegroundColor Cyan
Write-Host '==========================================================' -ForegroundColor Cyan
Write-Host ''

$game = Find-GameDir
if (-not $game) {
    Write-Host '没有自动找到游戏目录（含 GER.exe 的文件夹）。' -ForegroundColor Yellow
    $game = Read-Host '请把游戏目录完整路径粘贴到这里，然后回车'
    $game = $game.Trim('"')
    if (-not (Test-Path -LiteralPath (Join-Path $game 'GER.exe'))) {
        Write-Host '这个目录里没有 GER.exe，卸载已取消。' -ForegroundColor Red
        exit 1
    }
}

Write-Host ("游戏目录 : " + $game)

$dstDll = Join-Path $game 'dinput8.dll'
$gecv   = Join-Path $game 'GECV_LOADER.dll'

$ours = $false
if (Test-Path -LiteralPath $dstDll) {
    $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dstDll))
    $ours = ($ascii -match 'dinput8_fix')
    if ($ours) {
        Remove-Item -LiteralPath $dstDll -Force
        Write-Host 'dinput8.dll（修复代理）已删除' -ForegroundColor Green
    } else {
        Write-Host '当前 dinput8.dll 不是本修复，未做改动' -ForegroundColor Yellow
    }
}

foreach ($f in @('dinput8_fix.ini', 'dinput8_fix.log', 'dinput8_crash.log')) {
    $p = Join-Path $game $f
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force }
}
Write-Host 'dinput8_fix.ini / 日志 已清理' -ForegroundColor Green

if ((Test-Path -LiteralPath $gecv) -and -not (Test-Path -LiteralPath $dstDll)) {
    Move-Item -LiteralPath $gecv -Destination $dstDll -Force
    Write-Host '汉化补丁加载器已还原 : GECV_LOADER.dll -> dinput8.dll' -ForegroundColor Green
}

Write-Host ''
Write-Host '卸载完成（手柄拔插失效的问题会重新出现）。' -ForegroundColor Cyan
Write-Host ''
