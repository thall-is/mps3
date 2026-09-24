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
    $availPorts = [System.IO.Ports.SerialPort]::GetPortNames()
    if ($Port -and ($availPorts -notcontains $Port)) {
        if ($availPorts -contains $DefaultPort) {
            Write-Host "[*] Porta $Port nao disponivel. Usando porta conectada: $DefaultPort" -ForegroundColor Yellow
            $Port = $DefaultPort
        } elseif ($availPorts.Count -gt 0) {
            Write-Host "[*] Porta $Port nao disponivel. Usando primeira porta conectada: $($availPorts[0])" -ForegroundColor Yellow
            $Port = $availPorts[0]
        }
    }
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

    $appOffset = "0x20000"
    $otadataArg = ""
    if (Test-Path "$DirToFlash\flasher_args.json") {
        try {
            $json = Get-Content "$DirToFlash\flasher_args.json" -Raw | ConvertFrom-Json
            if ($json.app.offset) { $appOffset = $json.app.offset }
        } catch {}
    }
    if (Test-Path "$DirToFlash\ota_data_initial.bin") {
        $otadataArg = "0x10000 `"$DirToFlash\ota_data_initial.bin`""
    } elseif (Test-Path "$EspIdfDir\build\ota_data_initial.bin") {
        $otadataArg = "0x10000 `"$EspIdfDir\build\ota_data_initial.bin`""
    }

    $esptool = if (Get-Command esptool.exe -ErrorAction SilentlyContinue) { "esptool.exe" } else { Get-EsptoolCmd }
    $cmd = "$esptool --chip esp32s3 -p $Port -b 460800 --before default-reset --after hard-reset write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 `"$bootloader`" 0x8000 `"$partitions`" $otadataArg $appOffset `"$app`""
    
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

function Invoke-OtaUpload($BinaryPath, $TargetIp) {
    if (-not (Test-Path $BinaryPath)) {
        Write-Host "[!] ERRO: Arquivo de firmware nao encontrado: $BinaryPath" -ForegroundColor Red
        return
    }

    $otaPy = "$ScriptDir\ota_upload.py"
    if (-not (Test-Path $otaPy)) {
        Write-Host "[!] ERRO: Script $otaPy nao encontrado!" -ForegroundColor Red
        return
    }

    $pyCmd = if (Get-Command python -ErrorAction SilentlyContinue) { "python" } elseif (Get-Command python3 -ErrorAction SilentlyContinue) { "python3" } else { "py" }

    Write-Host "==================================================" -ForegroundColor Cyan
    Write-Host "  TRANSMISSAO OTA DO FIRMWARE (ESP32-S3)" -ForegroundColor Cyan
    Write-Host "  Arquivo: $(Split-Path -Leaf $BinaryPath)" -ForegroundColor Cyan
    if ($TargetIp) {
        Write-Host "  Destino: $TargetIp" -ForegroundColor Cyan
    } else {
        Write-Host "  Destino: Auto-descoberta por MAC (28:84:85:52:35:84)" -ForegroundColor Cyan
    }
    Write-Host "==================================================" -ForegroundColor Cyan

    if ($TargetIp) {
        & $pyCmd $otaPy $BinaryPath $TargetIp
    } else {
        & $pyCmd $otaPy $BinaryPath
    }

    if ($LASTEXITCODE -ne 0) {
        Write-Host "==================================================" -ForegroundColor Red
        Write-Host "[!] ERRO: Upload OTA falhou (exit code: $LASTEXITCODE)!" -ForegroundColor Red
        Write-Host "==================================================" -ForegroundColor Red
    }
}

function Build-Version {
    Ensure-IdfEnv
    Write-Host "==================================================" -ForegroundColor Cyan
    Write-Host "  COMPILANDO NOVA VERSAO DO MPS3 (ESP32-S3)" -ForegroundColor Cyan
    Write-Host "==================================================" -ForegroundColor Cyan

    Push-Location $EspIdfDir
    try {
        idf.py build | Out-Host
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[!] Falha na compilacao!" -ForegroundColor Red
            return $null
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
    if (Test-Path "$EspIdfDir\build\ota_data_initial.bin") {
        Copy-Item "$EspIdfDir\build\ota_data_initial.bin" "$targetDir\"
    }

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
        if ($dir -is [array]) { $dir = $dir[-1] }
        if ($dir -and (Test-Path $dir)) {
            Invoke-FlashDir $dir $port
        }
    }
    { $_ -in "flash-prev", "flash-previous" } {
        $port = if ($Arg1) { $Arg1 } else { $DefaultPort }
        $dirs = @(Get-ChildItem -Path $BinariosDir -Directory | Sort-Object Name)
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
    { $_ -in "ota-latest", "ota" } {
        $targetIp = if ($Arg1) { $Arg1 } else { "" }
        $dirs = @(Get-ChildItem -Path $BinariosDir -Directory | Sort-Object Name)
        if ($dirs.Count -gt 0) {
            $latest = $dirs[-1].FullName
            $bin = "$latest\mps3.bin"
            Invoke-OtaUpload $bin $targetIp
        } else {
            Write-Host "[!] Nenhuma versao encontrada para upload OTA." -ForegroundColor Red
        }
    }
    "build-ota" {
        $targetIp = if ($Arg1) { $Arg1 } else { "" }
        $dir = Build-Version
        if ($dir -is [array]) { $dir = $dir[-1] }
        if ($dir -and (Test-Path $dir)) {
            $bin = "$dir\mps3.bin"
            Invoke-OtaUpload $bin $targetIp
        }
    }
    { $_ -in "ota-ver", "ota-version" } {
        $vname = $Arg1
        $targetIp = if ($Arg2) { $Arg2 } else { "" }
        if (-not $vname) {
            Write-Host "[!] Especifique o nome da versao. Ex: .\mps3_version.ps1 ota-ver mps3_20260924_140150 192.168.1.100" -ForegroundColor Red
            return
        }
        $bin = "$BinariosDir\$vname\mps3.bin"
        Invoke-OtaUpload $bin $targetIp
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
    │       ├── mps3.bin             (offset 0x10000 / 0x20000 - firmware principal)
    │       ├── flasher_args.json    (parametros do ESP-IDF)
    │       └── info.txt             (data, branch, commit e tamanho)
    ├── ota_upload.py                (script Python para transmissao OTA Wi-Fi)
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
      Identifica a versao mais recente salva e grava diretamente no ESP32-S3 via cabo serial.
      Se a PORTA nao for informada, usa COM7.

  build-flash [PORTA]
      Executa 'build' (compila e salva versao) e em seguida grava no ESP32-S3 via serial.

  flash-prev [PORTA]  (ou 'flash-previous')
      Identifica a versao anterior (penultima salva) e faz o rollback via serial.

  flash-ver <NOME_DA_VERSAO> [PORTA]
      Grava uma versao especifica informada pelo nome da pasta via serial.
      Exemplo: .\mps3_version.ps1 flash-ver mps3_20260913_011500 COM7

  ota-latest [IP]  (ou 'ota')
      Envia a versao mais recente de mps3.bin para o ESP32-S3 via Wi-Fi (OTA).
      Se o IP nao for fornecido, localiza automaticamente o dispositivo na rede
      local atraves do endereco MAC de hardware (28:84:85:52:35:84).

  build-ota [IP]
      Compila nova versao com ESP-IDF e realiza imediatamente o upload OTA via Wi-Fi
      com auto-descoberta por MAC (se IP omitido).

  ota-ver <NOME_DA_VERSAO> [IP]
      Envia uma versao salva especifica via OTA para o IP indicado (ou busca por MAC se omitido).
      Exemplo: .\mps3_version.ps1 ota-ver mps3_20260924_140150 192.168.1.100

  help
      Exibe este menu de ajuda detalhado.

--------------------------------------------------------------------------------
  PARAMETROS DE GRAVACAO SERIAL:
    Chip:       ESP32-S3 (N16R8)
    Baud rate:  460800 bps
    Flash mode: DIO @ 80 MHz | Tamanho: 16 MB
    Offsets:    0x0 (bootloader) | 0x8000 (particoes) | 0x10000 (otadata) | 0x20000 (app mps3)
    Porta:      Padrao: COM7 (ou passe como argumento)

--------------------------------------------------------------------------------
  PARAMETROS DE GRAVACAO OTA (WI-FI):
    Protocolo:  HTTP POST /api/ota (application/octet-stream)
    Endpoints:  GET /api/ota (status/bateria/mac) | POST /api/ota (upload firmware)
    MAC Alvo:   28:84:85:52:35:84 (STA) / 28:84:85:52:35:85 (AP)
    Destinos:   Auto-descoberta inteligente por MAC (ARP + ping sweep) ou IP direto

--------------------------------------------------------------------------------
  EXEMPLOS DE USO:
    # Listar versoes salvas
    .\mps3_version.ps1 list

    # Compilar nova versao
    .\mps3_version.ps1 build

    # Gravar via cabo serial (USB)
    .\mps3_version.ps1 flash-latest COM7
    .\mps3_version.ps1 build-flash COM7
    .\mps3_version.ps1 flash-ver mps3_20260924_140150 COM7

    # Atualizar via Wi-Fi (OTA)
    .\mps3_version.ps1 ota
    .\mps3_version.ps1 ota 192.168.1.100
    .\mps3_version.ps1 ota-latest 192.168.1.100
    .\mps3_version.ps1 build-ota 192.168.1.100
    .\mps3_version.ps1 ota-ver mps3_20260924_140150 192.168.1.100
================================================================================
'@
        Write-Host $helpText -ForegroundColor Cyan
    }
}

