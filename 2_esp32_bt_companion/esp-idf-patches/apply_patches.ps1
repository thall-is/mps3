# Script para aplicar patches necessarios no ESP-IDF para suporte a LDAC e Multi-SEP
param(
    [string]$IdfPath = $env:IDF_PATH
)

if (-not $IdfPath) {
    Write-Error "IDF_PATH nao definido. Carregue o ambiente ESP-IDF primeiro ou especifique -IdfPath <caminho>"
    exit 1
}

Write-Host "Aplicando patches em: $IdfPath" -ForegroundColor Cyan

$patches = Get-ChildItem -Path $PSScriptRoot -Filter "*.patch" | Sort-Object Name
foreach ($p in $patches) {
    Write-Host "Aplicando $($p.Name)..." -ForegroundColor Yellow
    git -C "$IdfPath" apply "$($p.FullName)"
    if ($LASTEXITCODE -eq 0) {
        Write-Host "  -> Sucesso!" -ForegroundColor Green
    } else {
        Write-Host "  -> Aviso: Pode ja ter sido aplicado ou houve conflito." -ForegroundColor Magenta
    }
}
Write-Host "Processo concluido!" -ForegroundColor Green
