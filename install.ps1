# =====================================================================
#  噬神者：解放重生  手柄拔插失效修复  安装脚本
#  (DirectInput8 代理, 32 位)
# =====================================================================
param([string]$GameDir = "")

$ErrorActionPreference = 'Stop'
$pkgRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcDir  = Join-Path $pkgRoot 'files'

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
Write-Host ' 噬神者：解放重生 - 手柄拔插失效 修复包 v1.0' -ForegroundColor Cyan
Write-Host '==========================================================' -ForegroundColor Cyan
Write-Host ''

$game = Find-GameDir
if (-not $game) {
    Write-Host '没有自动找到游戏目录（含 GER.exe 的文件夹）。' -ForegroundColor Yellow
    $game = Read-Host '请把游戏目录完整路径粘贴到这里，然后回车'
    $game = $game.Trim('"')
    if (-not (Test-Path -LiteralPath (Join-Path $game 'GER.exe'))) {
        Write-Host '这个目录里没有 GER.exe，安装已取消。' -ForegroundColor Red
        exit 1
    }
}

Write-Host ("游戏目录 : " + $game)

if (-not (Test-Path -LiteralPath (Join-Path $srcDir 'dinput8.dll'))) {
    Write-Host 'files\dinput8.dll 不存在 —— 请把压缩包完整解压后再运行。' -ForegroundColor Red
    exit 1
}

$dstDll = Join-Path $game 'dinput8.dll'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'

if (Test-Path -LiteralPath $dstDll) {
    $ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($dstDll))
    if ($ascii -notmatch 'dinput8_fix' -and $ascii -match 'GECV') {
        # 汉化补丁的加载器：改名保留，不能覆盖，否则汉化失效
        Move-Item -LiteralPath $dstDll -Destination (Join-Path $game 'GECV_LOADER.dll') -Force
        Write-Host '检测到汉化补丁加载器 : dinput8.dll -> GECV_LOADER.dll  （已保留，汉化不受影响）' -ForegroundColor Green
    }
    elseif ($ascii -notmatch 'dinput8_fix') {
        $bak = Join-Path $game ("dinput8.dll.backup-" + $stamp)
        Copy-Item -LiteralPath $dstDll -Destination $bak -Force
        Write-Host ("发现其它 dinput8.dll，已备份为 : " + (Split-Path -Leaf $bak)) -ForegroundColor Yellow
    }
}

Copy-Item -LiteralPath (Join-Path $srcDir 'dinput8.dll')     -Destination $dstDll -Force
Copy-Item -LiteralPath (Join-Path $srcDir 'dinput8_fix.ini') -Destination (Join-Path $game 'dinput8_fix.ini') -Force

Write-Host ''
Write-Host 'dinput8.dll      : 已安装（DirectInput8 修复代理）' -ForegroundColor Green
Write-Host 'dinput8_fix.ini  : 已安装' -ForegroundColor Green
Write-Host ''
Write-Host '安装完成。' -ForegroundColor Cyan
Write-Host '重要：请先插好手柄，再从 Steam 启动游戏。' -ForegroundColor Yellow
Write-Host '      启动后手柄可以随意拔插，会自动恢复。' -ForegroundColor Yellow
Write-Host ''
Write-Host '（想用无边框全屏：把 dinput8_fix.ini 里 [Borderless] Enabled 改成 1，' -ForegroundColor Gray
Write-Host '  并在游戏里把显示模式设为窗口化，游戏内按 F11 可切换。）' -ForegroundColor Gray
Write-Host ''
