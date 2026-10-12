# make_iso.ps1 - empaqueta PORTMENU en una ISO autocontenida (modelo EJEMPLO)
# Uso: powershell -ExecutionPolicy Bypass -File make_iso.ps1
$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $repo "build"
$stage = Join-Path $build "iso_root"
$iso = Join-Path $build "PORTMENU.iso"
$mkiso = Join-Path $repo "tools\mkiso.exe"
$gcc = "C:\devkitPro\msys2\usr\bin\gcc.exe"
$env:PATH = "C:\devkitPro\msys2\usr\bin;$env:PATH"

# compilar mkiso si falta
if (-not (Test-Path $mkiso)) {
    & $gcc -O2 -o $mkiso (Join-Path $repo "tools\mkiso.c")
    if ($LASTEXITCODE -ne 0) { throw "no pude compilar mkiso" }
}

# staging limpio (PELIGRO: el stage contiene junctions -> borrar SOLO los links
# con [IO.Directory]::Delete, que no atraviesa el target)
if (Test-Path $stage) {
    foreach ($j in @("ART", "ELFS")) {
        $p = Join-Path $stage $j
        if (Test-Path $p) { [IO.Directory]::Delete($p) }  # borra solo el junction
    }
    Get-ChildItem $stage -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# boot + config (nombres 8.3 para ISO9660)
Copy-Item (Join-Path $build "PORTMENU.elf") (Join-Path $stage "PORTMENU.ELF") -Force
Copy-Item (Join-Path $repo "games.cfg") (Join-Path $stage "GAMES.CFG") -Force

# SYSTEM.CNF (formato escena PS2)
$cnf = "BOOT2 = cdrom0:\PORTMENU.ELF;1`r`nVER = 1.00`r`nVMODE = NTSC`r`n"
[IO.File]::WriteAllText((Join-Path $stage "SYSTEM.CNF"), $cnf, [Text.Encoding]::ASCII)

# arte y juegos (junctions: sin duplicar 270 MB)
New-Item -ItemType Junction -Path (Join-Path $stage "ART") -Value (Join-Path $build "ART") | Out-Null
New-Item -ItemType Junction -Path (Join-Path $stage "ELFS") -Value (Join-Path $repo "ELFS") | Out-Null

# generar ISO
& $mkiso $stage $iso "PORTMENU"
if ($LASTEXITCODE -ne 0) { throw "mkiso fallo" }

$sz = (Get-Item $iso).Length / 1MB
Write-Host ("=== PORTMENU.iso lista: {0:N1} MB ===" -f $sz)
Write-Host "Graba la ISO a DVD (IMG Burn) o montala en PCSX2. Todo va dentro: menu + juegos + arte."