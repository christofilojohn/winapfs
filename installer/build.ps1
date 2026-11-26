# winapfs - build the MSI and optional Burn setup bundle (WiX 5: winget install WiXToolset.WiXCLI --version 5.0.2.0,
# then: wix extension add -g WixToolset.UI.wixext/5.0.2 WixToolset.Util.wixext/5.0.2 WixToolset.BootstrapperApplications.wixext/5.0.2)
# SPDX-License-Identifier: GPL-2.0-or-later
#
#   powershell -ExecutionPolicy Bypass -File installer\build.ps1 [-Build build] [-Out dist] [-WinFspMsi path\to\winfsp.msi]
#
# Needs a Release build. Writes dist\winapfs-<version>-x64.msi, and when
# -WinFspMsi is passed, also writes dist\winapfs-<version>-x64-setup.exe.

param(
	[string]$Build = "$PSScriptRoot\..\build",
	[string]$Out = "$PSScriptRoot\..\dist",
	[string]$Version = '',   # default: the CMakeLists.txt project version
	[string]$WinFspMsi = ''  # optional: path to WinFsp MSI to build the Burn setup.exe bundle
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$Build = (Resolve-Path $Build).Path
$wix = (Get-Command wix -ErrorAction SilentlyContinue).Source
if (-not $wix) { $wix = 'C:\Program Files\WiX Toolset v5.0\bin\wix.exe' }
if (-not (Test-Path $wix)) { throw "WiX 5 not found (winget install WiXToolset.WiXCLI --version 5.0.2.0)" }

$m = Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(winapfs VERSION ([0-9.]+)'
$version = if ($Version) { $Version } else { $m.Matches[0].Groups[1].Value }
foreach ($exe in 'src\mount\Release\winapfs.exe', 'src\mount\Release\winapfs-tray.exe', 'src\cli\Release\winapfs-cli.exe', 'src\fsck\Release\winapfs-fsck.exe') {
	if (-not (Test-Path "$Build\$exe")) { throw "missing $(Join-Path $Build $exe) (build Release first)" }
}

$obj = "$Build\installer"
New-Item -ItemType Directory -Force $obj, $Out | Out-Null

$winfsp = "WinFsp - Windows File System Proxy, Copyright (C) Bill Zissimopoulos`r`nhttps://github.com/winfsp/winfsp"

# THIRD-PARTY-NOTICES.txt: everything compiled into the binaries
$parts = @(
	@('linux-apfs-rw (the APFS engine)', 'third_party\linux-apfs-rw\LICENSE'),
	@('apfs-fuse / ApfsLib', 'third_party\apfs-fuse\LICENSE'),
	@('apfsprogs (apfsck, as winapfs-fsck)', 'third_party\apfsprogs\LICENSE'),
	@('LZFSE', 'third_party\apfs-fuse\3rdparty\lzfse\LICENSE'),
	@('zlib', 'third_party\zlib\LICENSE'),
	@('bzip2', 'third_party\bzip2\LICENSE')
)
$n = New-Object Text.StringBuilder
[void]$n.AppendLine("winapfs $version - third-party notices")
[void]$n.AppendLine("")
[void]$n.AppendLine("winapfs uses WinFsp (installed separately) under its FLOSS exception:")
[void]$n.AppendLine($winfsp)
foreach ($p in $parts) {
	[void]$n.AppendLine("")
	[void]$n.AppendLine("=" * 78)
	[void]$n.AppendLine($p[0])
	[void]$n.AppendLine("=" * 78)
	[void]$n.AppendLine((Get-Content "$root\$($p[1])" -Raw))
}
Set-Content "$obj\THIRD-PARTY-NOTICES.txt" $n.ToString() -Encoding UTF8

# license.rtf for the installer's licence page: WinFsp notice, then the GPL
function RtfEscape([string]$s) {
	$s = $s.Replace('\', '\\').Replace('{', '\{').Replace('}', '\}')
	$sb = New-Object Text.StringBuilder
	foreach ($ch in $s.ToCharArray()) {
		if ([int]$ch -gt 127) { [void]$sb.Append("\u$([int]$ch)?") } else { [void]$sb.Append($ch) }
	}
	return $sb.ToString() -replace "`r?`n", "\par`r`n"
}
$intro = "winapfs $version - APFS (Mac) disks on Windows. Free software under the GNU General Public License, version 2 (below).`n`n" +
	"winapfs uses $winfsp`n`nIt includes linux-apfs-rw, apfs-fuse and apfsprogs (GPL), and LZFSE, zlib and bzip2 (see THIRD-PARTY-NOTICES.txt in the install folder).`n`n" +
	"Writing to APFS disks is experimental: keep a backup.`n`n"
$rtf = "{\rtf1\ansi\ansicpg1252\deff0{\fonttbl{\f0 Segoe UI;}{\f1 Consolas;}}\f0\fs18 " +
	(RtfEscape $intro) + "\f1\fs16 " + (RtfEscape (Get-Content "$root\LICENSE" -Raw)) + "}"
Set-Content "$obj\license.rtf" $rtf -Encoding ASCII

$msi = "$Out\winapfs-$version-x64.msi"
& $wix build "$PSScriptRoot\winapfs.wxs" -arch x64 -ext WixToolset.UI.wixext -ext WixToolset.Util.wixext `
	-d "Version=$version" -d "BuildDir=$Build" -d "ObjDir=$obj" -o $msi
if ($LASTEXITCODE -ne 0) { throw "wix build failed" }
Write-Output "built $msi ($([int]((Get-Item $msi).Length / 1KB)) KiB)"

if ($WinFspMsi) {
	if (-not (Test-Path $WinFspMsi)) { throw "WinFsp MSI not found: $WinFspMsi" }
	$winfspResolved = (Resolve-Path $WinFspMsi).Path
	$msiResolved = (Resolve-Path $msi).Path
	$setup = "$Out\winapfs-$version-x64-setup.exe"
	& $wix build "$PSScriptRoot\bundle.wxs" -arch x64 -ext WixToolset.BootstrapperApplications.wixext `
		-d "Version=$version" -d "WinFspMsi=$winfspResolved" -d "WinapfsMsi=$msiResolved" -d "ObjDir=$obj" -o $setup
	if ($LASTEXITCODE -ne 0) { throw "wix bundle build failed" }
	Write-Output "built $setup ($([int]((Get-Item $setup).Length / 1KB)) KiB)"
}
