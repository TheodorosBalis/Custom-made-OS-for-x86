# Shared by the image, kernel and bootstrap builders. Requires no local SDK paths.
function Resolve-OsBuildExecutable {
    param([string]$Name, [string]$ExplicitPath, [string]$Hint)

    $candidate = if ($ExplicitPath) { $ExplicitPath } else { $Name }
    $command = Get-Command -Name $candidate -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $command) {
        throw "Required build tool '$candidate' was not found. $Hint"
    }
    return $command.Source
}

function Get-OsBuildTools {
    param([string]$FasmPath, [string]$LlvmBin)

    $llvmHint = 'Install LLVM with clang.exe, ld.lld.exe and llvm-objcopy.exe; set -LlvmBin or LLVM_BIN to its bin directory, or add the tools to PATH.'
    $resolved = @{}
    foreach ($tool in @('clang.exe', 'ld.lld.exe', 'llvm-objcopy.exe')) {
        $explicit = if ($LlvmBin) { Join-Path $LlvmBin $tool } else { $null }
        $resolved[$tool] = Resolve-OsBuildExecutable $tool $explicit $llvmHint
    }
    return [PSCustomObject]@{
        Fasm = Resolve-OsBuildExecutable 'fasm.exe' $FasmPath 'Install FASM 1.x; set -FasmPath or FASM to fasm.exe, or add its directory to PATH.'
        Clang = $resolved['clang.exe']
        Linker = $resolved['ld.lld.exe']
        Objcopy = $resolved['llvm-objcopy.exe']
    }
}

function Get-OsBootLayout {
    param([string]$Path)

    $layout = @{}
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^([A-Z_]+)\s*=\s*(\d+)\s*$') {
            $layout[$matches[1]] = [int]$matches[2]
        }
    }
    if ($layout.SETUP_SECTORS -ne 64 -or $layout.SETUP_START_LBA -ne 1 -or
        $layout.HIGH_START_LBA -ne 65 -or $layout.HIGH_SECTORS -ne 256 -or
        $layout.INT_START_LBA -ne 321 -or $layout.INT_SECTORS -ne 2049 -or
        $layout.IMAGE_SECTORS -ne 4096) {
        throw 'Update the native loader manifest ABI before changing BootLayout.inc'
    }
    return $layout
}
