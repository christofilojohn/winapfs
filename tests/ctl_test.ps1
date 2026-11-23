# winapfs - the service's control pipe through `winapfs ctl` (no administrator needed)
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Runs `winapfs service debug` on image copies (settings in HKCU, control pipe
# \\.\pipe\winapfs-debug) and drives it with winapfs ctl: list, unmount and
# mount, set ReadWrite (then write a file) and Disabled, eject, unlocking an
# encrypted volume (wrong password, then the right one), unknown volumes.
# Afterwards winapfs-fsck -c on the written image.

param(
	[Parameter(Mandatory)] [string]$Winapfs,
	[string]$Fsck = '',
	[string]$Testdata = '',
	[string]$WorkDir = '',
	[string]$Phase = ''
)
$ErrorActionPreference = 'Continue'

function Ctl([string[]]$a) {
	$out = & $Winapfs ctl @a 2>&1 | ForEach-Object { "$_" }
	return [pscustomobject]@{ Code = $LASTEXITCODE; Out = ($out -join "`n") }
}
function Vols {
	$r = Ctl @('list')
	$v = @{}
	foreach ($l in ($r.Out -split "`n")) {
		# name letter state mode uuid disk
		if ($l -match '^(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+([0-9A-F-]{36})') { $v[$Matches[1]] = [pscustomobject]@{ Letter = $Matches[2]; State = $Matches[3]; Mode = $Matches[4] } }
	}
	return $v
}
function RawVols {
	$pipeName = 'winapfs-debug'
	if ($env:WINAPFS_PIPE -match '\\\\\.\\pipe\\(.+)$') { $pipeName = $Matches[1] }
	$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $pipeName, [System.IO.Pipes.PipeDirection]::InOut)
	$pipe.Connect(5000)
	try {
		$pipe.ReadMode = [System.IO.Pipes.PipeTransmissionMode]::Message
		$req = [Text.Encoding]::UTF8.GetBytes("list`n")
		$pipe.Write($req, 0, $req.Length)
		$pipe.Flush()
		$ms = New-Object System.IO.MemoryStream
		$buf = New-Object byte[] 4096
		do {
			$n = $pipe.Read($buf, 0, $buf.Length)
			if ($n -gt 0) { $ms.Write($buf, 0, $n) }
		} while ($n -gt 0 -and -not $pipe.IsMessageComplete)
		$text = [Text.Encoding]::UTF8.GetString($ms.ToArray())
	} finally {
		$pipe.Dispose()
	}
	$map = @{}
	foreach ($line in ($text -split "`n")) {
		$parts = $line.TrimEnd("`r") -split "`t"
		if ($parts.Length -ge 11 -and $parts[0] -eq 'vol') {
			$map[$parts[2]] = [pscustomobject]@{
				Uuid = $parts[1]; State = $parts[3]; Letter = $parts[4]; Mode = $parts[5];
				HideDotfiles = $parts[8]; ReadBytes = [uint64]$parts[9]; WriteBytes = [uint64]$parts[10]
			}
		}
	}
	return $map
}
function Check([bool]$ok, [string]$what) { if (-not $ok) { Write-Output "FAIL: $what"; exit 1 }; Write-Output "   ok: $what" }

if ($Phase -eq 'inner') {
	$v = Vols
	foreach ($n in 'Basic', 'VolA', 'VolB', 'VolC', 'Public') { Check ($v[$n] -and $v[$n].State -eq 'mounted') "$n mounted" }
	Check ($v['Secret'] -and $v['Secret'].State -eq 'locked') "Secret is locked (encrypted)"

	# HideDotfiles live toggle
	Check (((Get-Item -Force "$($v['Basic'].Letter)\.hidden_dotfile").Attributes -band [IO.FileAttributes]::Hidden) -ne 0) "Basic .hidden_dotfile is hidden by default"
	Check ((Ctl @('set', 'Basic', 'HideDotfiles', '0')).Code -eq 0) "set Basic HideDotfiles 0"
	$v = Vols; $rv = RawVols
	Check ($rv['Basic'].HideDotfiles -eq '0' -and (((Get-Item -Force "$($v['Basic'].Letter)\.hidden_dotfile").Attributes -band [IO.FileAttributes]::Hidden) -eq 0)) "Basic .hidden_dotfile is shown after HideDotfiles=0"
	Check ((Ctl @('set', 'Basic', 'HideDotfiles', '1')).Code -eq 0) "set Basic HideDotfiles 1"
	$v = Vols; $rv = RawVols
	Check ($rv['Basic'].HideDotfiles -eq '1' -and (((Get-Item -Force "$($v['Basic'].Letter)\.hidden_dotfile").Attributes -band [IO.FileAttributes]::Hidden) -ne 0)) "Basic .hidden_dotfile is hidden again after HideDotfiles=1"

	# Pin / change DriveLetter live + invalid rejections
	$used = @([IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0, 1).ToUpperInvariant() })
	foreach ($k in $v.Keys) { if ($v[$k].Letter -match '^([A-Z]):') { $used += $Matches[1] } }
	$free = [char[]](68..90) | ForEach-Object { [string]$_ } | Where-Object { $used -notcontains $_ -and -not (Test-Path "${_}:\") } | Select-Object -First 1
	Check ($null -ne $free) "a free drive letter is available"
	Check ((Ctl @('set', 'Basic', 'DriveLetter', "${free}:")).Code -eq 0) "set Basic DriveLetter ${free}:"
	$v = Vols
	Check ($v['Basic'].State -eq 'mounted' -and $v['Basic'].Letter -eq "${free}:" -and (Test-Path "${free}:\hello.txt")) "Basic moved to ${free}:"
	Check ((Ctl @('set', 'Basic', 'DriveLetter', 'C:')).Code -ne 0) "C: is rejected for DriveLetter"
	Check ((Ctl @('set', 'Basic', 'DriveLetter', $v['VolA'].Letter)).Code -ne 0) "in-use DriveLetter is rejected"
	Check ((Ctl @('set', 'Basic', 'DriveLetter', 'ZZ')).Code -ne 0) "malformed DriveLetter is rejected"

	Check ((Ctl @('unmount', 'VolC')).Code -eq 0) "unmount VolC"
	$v = Vols
	Check ($v['VolC'].State -eq 'unmounted' -and $v['VolC'].Letter -eq '-') "VolC shows as unmounted"
	Check ((Ctl @('rescan')).Code -eq 0 -and (Vols)['VolC'].State -eq 'unmounted') "a rescan leaves it unmounted"
	Check ((Ctl @('mount', 'VolC')).Code -eq 0 -and (Vols)['VolC'].State -eq 'mounted') "mount VolC again"

	Check ((Ctl @('set', 'VolA', 'ReadWrite', '1')).Code -eq 0) "set VolA ReadWrite 1"
	$v = Vols
	Check ($v['VolA'].State -eq 'mounted' -and $v['VolA'].Mode -eq 'rw') "VolA remounted read-write"
	Check ($v['VolB'].Mode -eq 'ro') "VolB stays read-only"
	try { [IO.File]::WriteAllText("$($v['VolA'].Letter)\from-ctl.txt", 'written via winapfs ctl'); $w = $true } catch { $w = $false }
	Check $w "a file written to VolA"

	# 12 MiB bounce-pool write/read + live I/O counters across unmount/remount
	$chunk = New-Object byte[] 65536
	(New-Object Random 42).NextBytes($chunk)
	$big = New-Object byte[] 12582912
	for ($off = 0; $off -lt $big.Length; $off += $chunk.Length) { [Buffer]::BlockCopy($chunk, 0, $big, $off, $chunk.Length) }
	$sha = [Security.Cryptography.SHA256]::Create()
	$wantHash = [BitConverter]::ToString($sha.ComputeHash($big))
	[IO.File]::WriteAllBytes("$($v['VolA'].Letter)\large-12m.bin", $big)
	$rv = RawVols
	Check ($rv['VolA'].WriteBytes -ge 12582912) "VolA live WriteBytes recorded 12 MiB ($($rv['VolA'].WriteBytes))"
	Check ((Ctl @('unmount', 'VolA')).Code -eq 0) "unmount VolA before cold read"
	Check ((Ctl @('mount', 'VolA')).Code -eq 0) "remount VolA"
	$v = Vols
	Check ($v['VolA'].State -eq 'mounted' -and $v['VolA'].Mode -eq 'rw') "VolA stays rw after remount"
	$got = [IO.File]::ReadAllBytes("$($v['VolA'].Letter)\large-12m.bin")
	$gotHash = [BitConverter]::ToString($sha.ComputeHash($got))
	Check ($got.Length -eq 12582912 -and $gotHash -eq $wantHash) "12 MiB file SHA-256 matches after remount"
	$rv = RawVols
	Check ($rv['VolA'].ReadBytes -ge 12582912) "VolA live ReadBytes recorded 12 MiB ($($rv['VolA'].ReadBytes))"

	# Verify Disk through control pipe (ro + rw mounted)
	Check ((Ctl @('verify', 'Basic')).Code -eq 0) "ctl verify Basic"
	Check ((Ctl @('verify', 'VolA')).Code -eq 0) "ctl verify VolA (while mounted rw)"

	Check ((Ctl @('set', 'VolB', 'Disabled', '1')).Code -eq 0 -and (Vols)['VolB'].State -eq 'disabled') "VolB disabled (unmounted)"
	Check ((Ctl @('set', 'VolB', 'Disabled', '0')).Code -eq 0 -and (Vols)['VolB'].State -eq 'mounted') "VolB enabled (mounted)"

	$r = Ctl @('mount', 'Secret', '--password', 'wrong-password')
	Check ($r.Code -ne 0 -and (Vols)['Secret'].State -eq 'locked') "a wrong password is refused"
	$r = Ctl @('mount', 'Secret', '--password', 'winapfs-test')
	$v = Vols
	Check ($r.Code -eq 0 -and $v['Secret'].State -eq 'mounted' -and $v['Secret'].Mode -eq 'ro') "Secret unlocked and mounted (read-only)"
	Check (Test-Path "$($v['Secret'].Letter)\") "Secret is reachable at $($v['Secret'].Letter)"

	Check ((Ctl @('eject', 'Basic')).Code -eq 0) "eject Basic"
	Check (-not (Vols).ContainsKey('Basic')) "Basic is gone from the list"
	Ctl @('rescan') | Out-Null
	Check (-not (Vols).ContainsKey('Basic')) "and stays gone after a rescan"

	Check ((Ctl @('mount', 'NoSuchVolume')).Code -ne 0) "an unknown volume is an error"
	Check ((Ctl @('set', 'VolA', 'Bogus', '1')).Code -ne 0) "an unknown setting is an error"
	exit 0
}

# driver
$self = $MyInvocation.MyCommand.Path
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$imgs = @()
foreach ($i in 'basic', 'multivol', 'encrypted') {
	Copy-Item "$Testdata\$i.img" "$WorkDir\ctl-$i.img" -Force
	$imgs += @('--image', "$WorkDir\ctl-$i.img")
}
$key = 'HKCU:\Software\winapfs-test'
Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue
$env:WINAPFS_CONFIG_HKCU = '1'
$inner = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$self`" -Winapfs `"$Winapfs`" -Phase inner"
& $Winapfs service debug @imgs --run $inner 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch 'space manager' }
$code = $LASTEXITCODE
Remove-Item $key -Recurse -Force -ErrorAction SilentlyContinue
if ($code -ne 0) { Write-Output "FAIL: control test (exit $code)"; exit 1 }
& $Fsck -c "$WorkDir\ctl-multivol.img"
if ($LASTEXITCODE -ne 0) { Write-Output "FAIL: fsck after writing through the service"; exit 1 }
Write-Output "OK: control pipe (list, mount, unmount, set, eject, unlock); fsck clean"
exit 0
