# ==============================================================================
# mps3_version.ps1 — Gerenciador de Builds e Flashing de Versões do MPS3 (ESP32-S3)
# PowerShell nativo para Windows
# ==============================================================================

param(
    [Parameter(Position=0)]
    [string]$Command = "help",

    [Parameter(Position=1)]
    [string]$Arg1 = "",

    [Parameter(Position=2)]
    [string]$Arg2 = ""
)

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectRoot = Resolve-Path "$ScriptDir\.."
$EspIdfDir = "$ProjectRoot\espidf"
$BinariosDir = "$ScriptDir\binarios"
$DefaultPort = "COM7"

if (-not (Test-Path $BinariosDir)) {
    New-Item -ItemType Directory -Force -Path $BinariosDir | Out-Null
}

function Ensure-IdfEnv {
    if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
        Write-Host "[*] Carregando ambiente ESP-IDF v6.0.1..." -ForegroundColor Cyan
        if (Test-Path "C:\esp\v6.0.1\esp-idf\export.ps1") {
            . "C:\esp\v6.0.1\esp-idf\export.ps1"
        } else {
            Write-Host "[!] ERRO: C:\esp\v6.0.1\esp-idf\export.ps1 nao encontrado." -ForegroundColor Red
            exit 1
        }
    }
}

function Get-EsptoolCmd {
    if (Get-Command esptool.py -ErrorAction SilentlyContinue) {
        return "esptool.py"
    } elseif (Test-Path "C:\esp\v6.0.1\esp-idf\components\esptool_py\esptool\esptool.py") {
        return "python C:\esp\v6.0.1\esp-idf\components\esptool_py\esptool\esptool.py"
    } else {
        return "python -m esptool"
    }
}

function Invoke-FlashDir($DirToFlash, $Port) {
    Ensure-IdfEnv
    if (-not $Port) { $Port = $DefaultPort }
    if (-not (Test-Path $DirToFlash)) {
        Write-Host "[!] ERRO: Pasta $DirToFlash nao existe!" -ForegroundColor Red
        return
    }

    $bootloader = "$DirToFlash\bootloader.bin"
    $partitions = "$DirToFlash\partition-table.bin"
    $app = "$DirToFlash\mps3.bin"

    if (-not (Test-Path $bootloader) -or -not (Test-Path $partitions) -or -not (Test-Path $app)) {
        Write-Host "[!] ERRO: Arquivos de firmware incompletos em $DirToFlash" -ForegroundColor Red
        return
    }

    $vname = Split-Path -Leaf $DirToFlash
    Write-Host "==================================================" -ForegroundColor Cyan
    Write-Host "  GRAVANDO NO ESP32-S3 VIA ESPTOOL" -ForegroundColor Cyan
    Write-Host "  Versao: $vname" -ForegroundColor Cyan
    Write-Host "  Porta:  $Port | Baud: 460800" -ForegroundColor Cyan
    Write-Host "==================================================" -ForegroundColor Cyan

    $esptool = if (Get-Command esptool.exe -ErrorAction SilentlyContinue) { "esptool.exe" } else { Get-EsptoolCmd }
    $cmd = "$esptool --chip esp32s3 -p $Port -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 `"$bootloader`" 0x8000 `"$partitions`" 0x10000 `"$app`""
    
    Invoke-Expression $cmd
    if ($LASTEXITCODE -ne 0) {
        Write-Host "==================================================" -ForegroundColor Red
        Write-Host "[!] ERRO: Falha na gravacao (exit code: $LASTEXITCODE)!" -ForegroundColor Red
        Write-Host "==================================================" -ForegroundColor Red
        return
    }
    Write-Host "==================================================" -ForegroundColor Green
    Write-Host "  GRAVACAO FINALIZADA COM SUCESSO!" -ForegroundColor Green
    Write-Host "==================================================" -ForegroundColor Green
}

function Build-Version {
    Ensure-IdfEnv
    Write-Host "==================================================" -ForegroundColor Cyan
    Write-Host "  COMPILANDO NOVA VERSAO DO MPS3 (ESP32-S3)" -ForegroundColor Cyan
    Write-Host "==================================================" -ForegroundColor Cyan

    Push-Location $EspIdfDir
    try {
        idf.py build
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[!] Falha na compilacao!" -ForegroundColor Red
            return
        }
    } finally {
        Pop-Location
    }

    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $vname = "mps3_$timestamp"
    $targetDir = "$BinariosDir\$vname"
    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null

    Copy-Item "$EspIdfDir\build\mps3.bin" "$targetDir\"
    Copy-Item "$EspIdfDir\build\bootloader\bootloader.bin" "$targetDir\"
    Copy-Item "$EspIdfDir\build\partition_table\partition-table.bin" "$targetDir\"
    Copy-Item "$EspIdfDir\build\flasher_args.json" "$targetDir\"

    $commit = git rev-parse --short HEAD 2>$null
    $branch = git branch --show-current 2>$null
    $sz = (Get-Item "$targetDir\mps3.bin").Length

@"
Versao: $vname
Data: $(Get-Date -Format "yyyy-MM-dd HH:mm:ss")
Branch: $branch
Commit: $commit
Tamanho mps3.bin: $sz bytes
"@ | Out-File -Encoding utf8 "$targetDir\info.txt"

    Write-Host "==================================================" -ForegroundColor Green
    Write-Host "  SUCESSO! Versao criada: $vname" -ForegroundColor Green
    Write-Host "  Salva em: versionamento\binarios\$vname" -ForegroundColor Green
    Write-Host "==================================================" -ForegroundColor Green
    return $targetDir
}

function List-Versions {
    Write-Host "==================================================" -ForegroundColor Cyan
    Write-Host "  VERSOES SALVAS DO FIRMWARE MPS3" -ForegroundColor Cyan
    Write-Host "==================================================" -ForegroundColor Cyan

    $dirs = Get-ChildItem -Path $BinariosDir -Directory | Sort-Object Name
    $idx = 1
    foreach ($d in $dirs) {
        $vname = $d.Name
        $infoFile = "$($d.FullName)\info.txt"
        $dateStr = "-"
        $commitStr = "-"
        if (Test-Path $infoFile) {
            $lines = Get-Content $infoFile
            foreach ($l in $lines) {
                if ($l -match "^Data:\s*(.+)") { $dateStr = $matches[1] }
                if ($l -match "^Commit:\s*(.+)") { $commitStr = $matches[1] }
            }
        }
        $appBin = "$($d.FullName)\mps3.bin"
        $szStr = "-"
        if (Test-Path $appBin) {
            $sz = (Get-Item $appBin).Length
            $szStr = "$([math]::Round($sz / 1024)) KB"
        }
        Write-Host ((" [{0:D2}] {1,-25} | Data: {2,-19} | Commit: {3,-8} | Tam: {4}" -f $idx, $vname, $dateStr, $commitStr, $szStr))
        $idx++
    }
    if ($dirs.Count -eq 0) {
        Write-Host "  Nenhuma versao salva encontrada."
    }
    Write-Host "==================================================" -ForegroundColor Cyan
}

switch ($Command.ToLower()) {
    "build" {
        Build-Version
    }
    { $_ -in "flash-latest", "flash" } {
        $port = if ($Arg1) { $Arg1 } else { $DefaultPort }
        $dirs = Get-ChildItem -Path $BinariosDir -Directory | Sort-Object Name
        if ($dirs.Count -gt 0) {
            $latest = $dirs[-1].FullName
            Invoke-FlashDir $latest $port
        } else {
            Write-Host "[!] Nenhuma versao encontrada para gravar." -ForegroundColor Red
        }
    }
    "build-flash" {
        $port = if ($Arg1) { $Arg1 } else { $DefaultPort }
        $dir = Build-Version
        if ($dir) {
            Invoke-FlashDir $dir $port
        }
    }
    { $_ -in "flash-prev", "flash-previous" } {
        $port = if ($Arg1) { $Arg1 } else { $DefaultPort }
        $dirs = Get-ChildItem -Path $BinariosDir -Directory | Sort-Object Name
        if ($dirs.Count -ge 2) {
            $prev = $dirs[-2].FullName
            Invoke-FlashDir $prev $port
        } else {
            Write-Host "[!] Nao ha versao anterior salva." -ForegroundColor Red
        }
    }
    { $_ -in "flash-ver", "flash-version" } {
        $vname = $Arg1
        $port = if ($Arg2) { $Arg2 } else { $DefaultPort }
        if (-not $vname) {
            Write-Host "[!] Especifique o nome da versao. Ex: .\mps3_version.ps1 flash-ver mps3_20260913_011500" -ForegroundColor Red
            return
        }
        $target = "$BinariosDir\$vname"
        Invoke-FlashDir $target $port
    }
    "list" {
        List-Versions
    }
    default {
        $helpText = @'
================================================================================
  MPS3 (ESP32-S3) — GERENCIADOR DE VERSIONAMENTO E FLASHING DE BINARIOS
================================================================================

  ESTRUTURA DE DIRETORIOS:
    versionamento/
    ├── binarios/
    │   └── mps3_AAAAMMDD_HHMMSS/    (pasta da versao gerada)
    │       ├── bootloader.bin       (offset 0x0)
    │       ├── partition-table.bin  (offset 0x8000)
    │       ├── mps3.bin             (offset 0x10000 - firmware principal)
    │       ├── flasher_args.json    (parametros do ESP-IDF)
    │       └── info.txt             (data, branch, commit e tamanho)
    ├── mps3_version.sh              (script Bash para Git Bash / Linux)
    └── mps3_version.ps1             (script PowerShell nativo para Windows)

--------------------------------------------------------------------------------
  COMANDOS DISPONIVEIS:
--------------------------------------------------------------------------------
  list
      Lista todas as versoes salvas em binarios\, exibindo nome, data, commit
      Git e tamanho do arquivo mps3.bin.

  build
      Compila o firmware via ESP-IDF v6.0.1 em espidf\, cria uma nova pasta
      em binarios\mps3_AAAAMMDD_HHMMSS e salva todos os binarios gerados.

  flash-latest [PORTA]  (ou 'flash')
      Identifica a versao mais recente salva e grava diretamente no ESP32-S3.
      Se a PORTA nao for informada, usa COM7.

  build-flash [PORTA]
      Executa 'build' (compila e salva versao) e em seguida grava no ESP32-S3.

  flash-prev [PORTA]  (ou 'flash-previous')
      Identifica a versao anterior (penultima salva) e faz o rollback.

  flash-ver <NOME_DA_VERSAO> [PORTA]
      Grava uma versao especifica informada pelo nome da pasta.
      Exemplo: .\mps3_version.ps1 flash-ver mps3_20260913_011500 COM7

  help
      Exibe este menu de ajuda detalhado.

--------------------------------------------------------------------------------
  PARAMETROS DE GRAVACAO:
    Chip:       ESP32-S3 (N16R8)
    Baud rate:  460800 bps
    Flash mode: DIO @ 80 MHz | Tamanho: 16 MB
    Offsets:    0x0 (bootloader) | 0x8000 (particoes) | 0x10000 (app mps3)
    Porta:      Padrao: COM7 (ou passe como argumento)

--------------------------------------------------------------------------------
  EXEMPLOS DE USO:
    .\mps3_version.ps1 list
    .\mps3_version.ps1 build
    .\mps3_version.ps1 build-flash COM7
    .\mps3_version.ps1 flash-latest COM7
    .\mps3_version.ps1 flash-prev COM7
    .\mps3_version.ps1 flash-ver mps3_20260913_011500 COM7
================================================================================
'@
        Write-Host $helpText -ForegroundColor Cyan
    }
}

