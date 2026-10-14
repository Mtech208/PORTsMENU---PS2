# make_iso.ps1 - empaqueta PORTMENU en una ISO autocontenida para PS2
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

# staging limpio (borrar reparse points/junctions con Delete sin atravesar el target)
if (Test-Path $stage) {
    foreach ($item in (Get-ChildItem $stage -Force)) {
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            [IO.Directory]::Delete($item.FullName)
        }
    }
    Get-ChildItem $stage -Force -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# boot + config (nombres 8.3 para ISO9660, SLUS_200.01 para compatibilidad OPL)
Copy-Item (Join-Path $build "PORTMENU.elf") (Join-Path $stage "PORTMENU.ELF") -Force
Copy-Item (Join-Path $build "PORTMENU.elf") (Join-Path $stage "SLUS_200.01") -Force

# Auto-indexar juegos en GAMES.CFG:
# Lee games.cfg y además escanea ELFS/ por si hay carpetas nuevas sin registrar
$gamesCfgPath = Join-Path $repo "games.cfg"
$stageCfgPath = Join-Path $stage "GAMES.CFG"

$knownMeta = @{
    "MK64" = @{ Title="Mario Kart 64"; Genre="Carreras"; Year="1996"; Desc="Mario Kart 64 Port PS2" }
    "OPTICRAF" = @{ Title="OptiCraft"; Genre="Sandbox"; Year="2024"; Desc="OptiCraft Minecraft Port PS2" }
    "OPTICRAFT" = @{ Title="OptiCraft"; Genre="Sandbox"; Year="2024"; Desc="OptiCraft Minecraft Port PS2" }
    "POU" = @{ Title="Pou PS2"; Genre="Casual"; Year="2023"; Desc="Pou Port PS2" }
    "PVZPS2" = @{ Title="Plants vs Zombies"; Genre="Estrategia"; Year="2009"; Desc="Plants vs Zombies Port PS2" }
    "SM64" = @{ Title="Super Mario 64"; Genre="Plataformas"; Year="1996"; Desc="Super Mario 64 Port PS2" }
    "SSB64" = @{ Title="Super Smash Bros 64"; Genre="Lucha"; Year="1999"; Desc="Super Smash Bros 64 Port PS2" }
}

$lines = @()
$existingFolders = @()
if (Test-Path $gamesCfgPath) {
    foreach ($l in (Get-Content $gamesCfgPath)) {
        $trimmed = $l.Trim()
        if ($trimmed -and -not $trimmed.StartsWith("#")) {
            $lines += $trimmed
            $parts = $trimmed.Split("|")
            if ($parts.Count -ge 2) {
                $elfp = $parts[1]
                if ($elfp -match "ELFS[/\\]([^/\\]+)") {
                    $existingFolders += $matches[1].ToUpper()
                }
            }
        }
    }
}

$elfsDir = Join-Path $repo "ELFS"
if (Test-Path $elfsDir) {
    foreach ($dir in (Get-ChildItem $elfsDir -Directory)) {
        $folderName = $dir.Name.ToUpper()
        if ($existingFolders -contains $folderName) { continue }

        $elfFile = Get-ChildItem $dir.FullName -Filter "*.elf" | Select-Object -First 1
        if (-not $elfFile) { continue }

        $elfRel = "ELFS/" + $dir.Name + "/" + $elfFile.Name
        $covRel = ""
        $covCand = Get-ChildItem (Join-Path $repo "ART\COV") -Filter "$($dir.Name)*" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($covCand) { $covRel = "ART/COV/" + $covCand.Name }

        $logoRel = ""
        $logoCand = Get-ChildItem (Join-Path $repo "ART\LOGO") -Filter "$($dir.Name)*" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($logoCand) { $logoRel = "ART/LOGO/" + $logoCand.Name }

        $meta = $knownMeta[$folderName]
        $title = if ($meta) { $meta.Title } else { $dir.Name }
        $genre = if ($meta) { $meta.Genre } else { "Port PS2" }
        $year  = if ($meta) { $meta.Year } else { "2026" }
        $desc  = if ($meta) { $meta.Desc } else { "$title para PS2" }

        $lines += "$title|$elfRel|$covRel||$logoRel||$genre|$year|$desc"
    }
}

$cfgContent = "# PORTMENU games.cfg`r`n# Formato: Titulo|Ruta_ELF|Cover|Fondo|Logo|Icono|Genero|Anio|Descripcion`r`n`r`n" + ($lines -join "`r`n") + "`r`n"
[IO.File]::WriteAllText($stageCfgPath, $cfgContent, [Text.Encoding]::ASCII)
[IO.File]::WriteAllText($gamesCfgPath, $cfgContent, [Text.Encoding]::ASCII)

# SYSTEM.CNF (Game ID estándar requerido por OPL y la BIOS de PS2)
$cnf = "BOOT2 = cdrom0:\SLUS_200.01;1`r`nVER = 1.00`r`nVMODE = NTSC`r`n"
[IO.File]::WriteAllText((Join-Path $stage "SYSTEM.CNF"), $cnf, [Text.Encoding]::ASCII)

# arte y juegos (junctions: sin duplicar 270 MB)
$artSource = if (Test-Path (Join-Path $repo "ART")) { Join-Path $repo "ART" } else { Join-Path $build "ART" }
New-Item -ItemType Junction -Path (Join-Path $stage "ART") -Value $artSource | Out-Null
New-Item -ItemType Junction -Path (Join-Path $stage "ELFS") -Value (Join-Path $repo "ELFS") | Out-Null
# carpeta N64/ (ROMs con rutas hardcodeadas de ports de la escena)
if (Test-Path (Join-Path $repo "N64")) {
    New-Item -ItemType Junction -Path (Join-Path $stage "N64") -Value (Join-Path $repo "N64") | Out-Null
}

# Assets en raíz para ports de la escena que buscan rutas hardcodeadas (ej. cdfs:/ o cdrom0:\):
# mkiso.c normaliza a UPPERCASE 8.3 y deduplica extents automáticamente,
# por lo que estos archivos no ocupan espacio duplicado en el ISO físico.
if (Test-Path $elfsDir) {
    foreach ($dir in (Get-ChildItem $elfsDir -Directory)) {
        # Copiar/enlazar cualquier archivo .pak a la raíz
        foreach ($pak in (Get-ChildItem $dir.FullName -Filter "*.pak")) {
            $destPak = Join-Path $stage $pak.Name.ToUpper()
            if (-not (Test-Path $destPak)) {
                Copy-Item $pak.FullName $destPak -Force
                Write-Host "  [$($dir.Name)] $($pak.Name.ToUpper()) vinculado a la raiz del ISO"
            }
        }
        # Enlazar subcarpetas de datos (data, skins, images, userdata, etc.) a la raíz si no colisionan
        foreach ($sub in (Get-ChildItem $dir.FullName -Directory)) {
            $destSub = Join-Path $stage $sub.Name
            if (-not (Test-Path $destSub) -and $sub.Name -notin @("ART", "ELFS", "N64")) {
                New-Item -ItemType Junction -Path $destSub -Value $sub.FullName -ErrorAction SilentlyContinue | Out-Null
                Write-Host "  [$($dir.Name)] $($sub.Name)/ enlazado a la raiz del ISO"
            }
        }
    }
}

# generar ISO
& $mkiso $stage $iso "PORTMENU"
if ($LASTEXITCODE -ne 0) { throw "mkiso fallo" }

# Copia para OPL con Game ID en el nombre
$oplIso = Join-Path $build "SLUS_200.01.PORTMENU.iso"
Copy-Item $iso $oplIso -Force

$sz = (Get-Item $iso).Length / 1MB
Write-Host ("=== ISOs listas ({0:N1} MB) ===" -f $sz)
Write-Host "1. Para CD/DVD fisico o PCSX2: build\PORTMENU.iso"
Write-Host "2. Para OPL en USB: copia build\SLUS_200.01.PORTMENU.iso a la carpeta CD\ de tu USB"
Write-Host "   (¡Asegurate de que la USB NO este fragmentada! Usa Defraggler si sale pantalla blanca)"