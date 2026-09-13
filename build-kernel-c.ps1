#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$FasmPath = $env:FASM,
    [string]$LlvmBin = $env:LLVM_BIN
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$development = Join-Path $root 'Development'
. (Join-Path $root 'build-tools.ps1')
$buildTools = Get-OsBuildTools -FasmPath $FasmPath -LlvmBin $LlvmBin
$clang = $buildTools.Clang
$linker = $buildTools.Linker
$objcopy = $buildTools.Objcopy
$fasm = $buildTools.Fasm

$sourceNames = @(
    'Kernel\KernelMain.c',
    'Runtime\Memory.c',
    'Runtime\String.c',
    'Kernel\Panic.c',
    'Arch\x86\Exceptions.c',
    'Arch\x86\LapicInterrupts.c',
    'Drivers\Keyboard\Ps2Keyboard.c',
    'Drivers\Keyboard\KeyboardDriverHost.c',
    'Drivers\Vga\VgaTextMode.c',
    'Drivers\Intel915\Intel915.c',
    'Drivers\Storage\Storage.c',
    'Drivers\Usb\UsbHost.c',
    'Drivers\Usb\UsbMouse.c',
    'Drivers\Usb\MouseDriver.c',
    'Drivers\Audio\Azalia.c',
    'Desktop\Desktop.c'
)
$objects = @()
$elf = Join-Path $development 'Kernel\KernelMain.elf'
$binary = Join-Path $development 'Kernel\KernelMain.bin'
$linkerScript = Join-Path $development 'Kernel\kernel-c.ld'
$assembly = Join-Path $development 'Arch\x86\VirtualKernel.asm'
$kernelBinary = Join-Path $development 'Arch\x86\VirtualKernel.bin'

foreach ($tool in @($clang, $linker, $objcopy, $fasm)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required build tool was not found: $tool"
    }
}

foreach ($sourceName in $sourceNames) {
    $source = Join-Path $development $sourceName
    $object = Join-Path $development ([IO.Path]::ChangeExtension($sourceName, '.o'))
    $objects += $object

    & $clang `
        --target=i386-unknown-none-elf `
        -march=pentium-m `
        -std=c11 `
        -O1 `
        -Wall `
        -Wextra `
        -Werror `
        -ffreestanding `
        -fno-builtin `
        -fno-pic `
        -fno-pie `
        -fno-stack-protector `
        -fno-unwind-tables `
        -fno-asynchronous-unwind-tables `
        -fno-omit-frame-pointer `
        -ffunction-sections `
        -fdata-sections `
        -mno-sse `
        -mno-sse2 `
        -mno-mmx `
        -msoft-float `
        -I $development `
        -c $source `
        -o $object
    if ($LASTEXITCODE -ne 0) {
        throw "Clang failed for $sourceName with exit code $LASTEXITCODE"
    }
}

$graphics = Join-Path $development 'Drivers\Intel915'
$desktop = Join-Path $development 'Desktop'
$usb = Join-Path $development 'Drivers\Usb'

& $linker `
    -m elf_i386 `
    -T $linkerScript `
    --gc-sections `
    -nostdlib `
    -o $elf `
    $objects
if ($LASTEXITCODE -ne 0) {
    throw "LLD failed with exit code $LASTEXITCODE"
}

& $objcopy -O binary $elf $binary
if ($LASTEXITCODE -ne 0) {
    throw "llvm-objcopy failed with exit code $LASTEXITCODE"
}

# The driver uses offsets from its nonzero CS/DS/SS base, so link it separately.
& $clang --target=i386-unknown-none-elf -march=pentium-m -std=c11 -O1 `
    -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-pic -fno-pie `
    -fno-stack-protector -fno-unwind-tables -fno-asynchronous-unwind-tables `
    -fno-omit-frame-pointer -fno-jump-tables -ffunction-sections -fdata-sections `
    -mno-sse -mno-sse2 -mno-mmx -msoft-float -I $development `
    -c (Join-Path $development 'Drivers\Keyboard\KeyboardDriver.c') `
    -o (Join-Path $development 'Drivers\Keyboard\KeyboardDriver.o')
if ($LASTEXITCODE -ne 0) { throw 'Keyboard driver compilation failed' }

& $linker -m elf_i386 -T (Join-Path $development 'Drivers\Keyboard\keyboard-driver.ld') `
    --gc-sections -nostdlib -o (Join-Path $development 'Drivers\Keyboard\KeyboardDriver.elf') `
    (Join-Path $development 'Drivers\Keyboard\KeyboardDriver.o')
if ($LASTEXITCODE -ne 0) { throw 'Keyboard driver link failed' }

& $objcopy -O binary (Join-Path $development 'Drivers\Keyboard\KeyboardDriver.elf') `
    (Join-Path $development 'Drivers\Keyboard\KeyboardDriverC.bin')
if ($LASTEXITCODE -ne 0) { throw 'Keyboard driver binary conversion failed' }

& $clang --target=i386-unknown-none-elf -march=pentium-m -std=c11 -O1 `
    -DMOUSE_RING2 -Wall -Wextra -Werror -ffreestanding -fno-builtin `
    -fno-pic -fno-pie -fno-stack-protector -fno-unwind-tables `
    -fno-asynchronous-unwind-tables -fno-omit-frame-pointer -fno-jump-tables `
    -ffunction-sections -fdata-sections -mno-sse -mno-sse2 -mno-mmx `
    -msoft-float -I $development -c (Join-Path $usb 'MouseDriver.c') `
    -o (Join-Path $usb 'MouseDriverRing2.o')
if ($LASTEXITCODE -ne 0) { throw 'Mouse ring-2 compilation failed' }
& $linker -m elf_i386 -T (Join-Path $usb 'mouse-driver.ld') --gc-sections `
    -nostdlib -o (Join-Path $usb 'MouseDriverRing2.elf') (Join-Path $usb 'MouseDriverRing2.o')
if ($LASTEXITCODE -ne 0) { throw 'Mouse ring-2 link failed' }
& $objcopy -O binary (Join-Path $usb 'MouseDriverRing2.elf') (Join-Path $usb 'MouseDriverC.bin')
if ($LASTEXITCODE -ne 0) { throw 'Mouse binary conversion failed' }

# Compile the same readable driver source for its nonzero ring-1 segments.
& $clang --target=i386-unknown-none-elf -march=pentium-m -std=c11 -O1 `
    -DINTEL915_RING1 -Wall -Wextra -Werror -ffreestanding -fno-builtin `
    -fno-pic -fno-pie -fno-stack-protector -fno-unwind-tables `
    -fno-asynchronous-unwind-tables -fno-omit-frame-pointer -fno-jump-tables `
    -ffunction-sections -fdata-sections -mno-sse -mno-sse2 -mno-mmx `
    -msoft-float -I $development -c (Join-Path $graphics 'Intel915.c') `
    -o (Join-Path $graphics 'Intel915Ring1.o')
if ($LASTEXITCODE -ne 0) { throw 'Intel915 ring-1 compilation failed' }
& $linker -m elf_i386 -T (Join-Path $graphics 'intel915-driver.ld') --gc-sections `
    -nostdlib -o (Join-Path $graphics 'Intel915Ring1.elf') (Join-Path $graphics 'Intel915Ring1.o')
if ($LASTEXITCODE -ne 0) { throw 'Intel915 ring-1 link failed' }
& $objcopy -O binary (Join-Path $graphics 'Intel915Ring1.elf') (Join-Path $graphics 'Intel915Ring1.bin')
if ($LASTEXITCODE -ne 0) { throw 'Intel915 binary conversion failed' }

Push-Location $root
try {
    & $fasm (Join-Path $graphics 'Intel915DriverEntry.asm') (Join-Path $graphics 'Intel915Driver.bin')
    if ($LASTEXITCODE -ne 0) { throw 'Intel915 driver assembly failed' }
    & $fasm (Join-Path $development 'Drivers\Keyboard\KeyboardDriver.asm') (Join-Path $development 'Drivers\Keyboard\KeyboardDriver.bin')
    if ($LASTEXITCODE -ne 0) { throw 'Keyboard driver assembly failed' }
    & $fasm (Join-Path $usb 'MouseDriverEntry.asm') (Join-Path $usb 'MouseDriver.bin')
    if ($LASTEXITCODE -ne 0) { throw 'Mouse driver assembly failed' }
    & $fasm (Join-Path $development 'User\UserServices.asm') (Join-Path $development 'User\UserServices.o')
    if ($LASTEXITCODE -ne 0) { throw 'User service wrappers assembly failed' }
    & $clang --target=i386-unknown-none-elf -march=pentium-m -std=c11 -O1 `
        -DDESKTOP_USER -Wall -Wextra -Werror -ffreestanding -fno-builtin `
        -fno-pic -fno-pie -fno-stack-protector -fno-unwind-tables `
        -fno-asynchronous-unwind-tables -fno-omit-frame-pointer -fno-jump-tables `
        -ffunction-sections -fdata-sections -mno-sse -mno-sse2 -mno-mmx `
        -msoft-float -I $development -c (Join-Path $desktop 'Desktop.c') `
        -o (Join-Path $desktop 'DesktopUser.o')
    if ($LASTEXITCODE -ne 0) { throw 'Desktop user compilation failed' }
    & $linker -m elf_i386 -T (Join-Path $desktop 'desktop-user.ld') --gc-sections `
        -nostdlib -o (Join-Path $desktop 'DesktopUser.elf') `
        (Join-Path $desktop 'DesktopUser.o') (Join-Path $development 'User\UserServices.o')
    if ($LASTEXITCODE -ne 0) { throw 'Desktop user link failed' }
    & $objcopy -O binary (Join-Path $desktop 'DesktopUser.elf') (Join-Path $desktop 'DesktopUser.bin')
    if ($LASTEXITCODE -ne 0) { throw 'Desktop binary conversion failed' }
    & $fasm (Join-Path $desktop 'DesktopProcess.asm') (Join-Path $desktop 'DesktopProcess.bin')
    if ($LASTEXITCODE -ne 0) { throw 'Desktop process assembly failed' }
    & $fasm $assembly $kernelBinary
    if ($LASTEXITCODE -ne 0) {
        throw "FASM failed with exit code $LASTEXITCODE"
    }
}
finally {
    Pop-Location
}

$cSize = (Get-Item -LiteralPath $binary).Length
$kernelSize = (Get-Item -LiteralPath $kernelBinary).Length
Write-Host "C payload:      $cSize bytes"
Write-Host "Intel915 driver: $((Get-Item -LiteralPath (Join-Path $graphics 'Intel915Driver.bin')).Length) bytes"
Write-Host "Virtual kernel: $kernelSize bytes"
