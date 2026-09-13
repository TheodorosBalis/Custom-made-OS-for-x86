#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$FasmPath = $env:FASM,
    [string]$LlvmBin = $env:LLVM_BIN
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
. (Join-Path $root 'build-tools.ps1')
$buildTools = Get-OsBuildTools -FasmPath $FasmPath -LlvmBin $LlvmBin
$layout = Get-OsBootLayout (Join-Path $root 'Development/Boot/BootLayout.inc')

Push-Location $root
try {
    Write-Host 'Building interrupt handlers...'
    & $buildTools.Fasm 'Development/Arch/x86/InterruptHandlers.asm' 'Development/Arch/x86/InterruptHandlers.bin'
    if ($LASTEXITCODE -ne 0) { throw 'Interrupt handler assembly failed' }

    Write-Host 'Building kernel, drivers and desktop...'
    & (Join-Path $root 'build-kernel-c.ps1') -FasmPath $buildTools.Fasm -LlvmBin $LlvmBin

    # The bootstrap embeds CRCs of the finished, sector-padded kernel payloads.
    Write-Host 'Building bootstrap and payload manifest...'
    & (Join-Path $root 'build-bootstrap.ps1') -FasmPath $buildTools.Fasm -LlvmBin $LlvmBin

    $image = New-Object byte[] ($layout.IMAGE_SECTORS * 512)
    $payloads = @(
        @{ Path = 'Development/Boot/bl.bin'; Lba = 0; Sectors = 1; Exact = $true },
        @{ Path = 'Development/Boot/kernel.bin'; Lba = $layout.SETUP_START_LBA; Sectors = $layout.SETUP_SECTORS; Exact = $true },
        @{ Path = 'Development/Arch/x86/VirtualKernel.bin'; Lba = $layout.HIGH_START_LBA; Sectors = $layout.HIGH_SECTORS; Exact = $true },
        @{ Path = 'Development/Arch/x86/InterruptHandlers.bin'; Lba = $layout.INT_START_LBA; Sectors = $layout.INT_SECTORS; Exact = $false }
    )
    foreach ($payload in $payloads) {
        $bytes = [IO.File]::ReadAllBytes((Join-Path $root $payload.Path))
        $capacity = $payload.Sectors * 512
        if ($bytes.Length -eq 0 -or $bytes.Length -gt $capacity -or
            ($payload.Exact -and $bytes.Length -ne $capacity)) {
            throw "Invalid size for $($payload.Path): $($bytes.Length) bytes, reservation $capacity bytes"
        }
        [Array]::Copy($bytes, 0, $image, $payload.Lba * 512, $bytes.Length)
    }
    if ($image[510] -ne 0x55 -or $image[511] -ne 0xAA) {
        throw 'Boot-sector signature is missing'
    }

    $imagePath = Join-Path $root 'myos.img'
    [IO.File]::WriteAllBytes($imagePath, $image)
    Write-Host "Created: $imagePath ($($image.Length) bytes)"
} finally {
    Pop-Location
}
