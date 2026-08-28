param(
    [Parameter(Mandatory=$true)][string]$BaseModuleZip,
    [ValidateSet('release','dev')][string]$Flavor = 'release',
    [string]$OutputZip
)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$module = [IO.Path]::GetFullPath((Join-Path $root 'module'))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $root 'build'))
$versionFile = Join-Path $root 'VERSION'
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
    throw 'VERSION is missing'
}
$releaseVersion = [IO.File]::ReadAllText($versionFile).Trim()
if ($releaseVersion -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "VERSION must contain a release version such as 3.0.0: '$releaseVersion'"
}
if (-not $OutputZip) {
    $suffix = if ($Flavor -eq 'release') { '' } else { '-dev' }
    $OutputZip = "dist/LinDeX-v$releaseVersion$suffix.zip"
}
$target = [IO.Path]::GetFullPath((Join-Path $root $OutputZip))
if (-not $target.StartsWith($root + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Output must remain inside the repository: $target"
}
$stageLeaf = ([IO.Path]::GetFileNameWithoutExtension($target) -replace
    '[^A-Za-z0-9._-]', '_')
$stage = [IO.Path]::GetFullPath((Join-Path $buildRoot "v3-$Flavor-$stageLeaf-stage"))
if (-not $stage.StartsWith($buildRoot + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Unsafe staging directory: $stage"
}
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

# The earlier ZIP is only a carrier for the canonical Debian rootfs.
$baseStage = Join-Path $stage '.base'
Expand-Archive -LiteralPath $BaseModuleZip -DestinationPath $baseStage
$rootfs = Join-Path $baseStage 'debianfs-arm64.tar.xz'
if (-not (Test-Path -LiteralPath $rootfs -PathType Leaf)) {
    throw 'The base ZIP has no debianfs-arm64.tar.xz'
}
Move-Item -LiteralPath $rootfs -Destination (Join-Path $stage 'debianfs-arm64.tar.xz')
Remove-Item -LiteralPath $baseStage -Recurse -Force
Copy-Item -Path (Join-Path $module '*') -Destination $stage -Recurse -Force

# No external provider archive is part of the three-profile release.
$obsoleteProviders = Join-Path $stage 'provider-assets'
if (Test-Path -LiteralPath $obsoleteProviders) {
    Remove-Item -LiteralPath $obsoleteProviders -Recurse -Force
}

if (Test-Path -LiteralPath (Join-Path $stage 'system')) {
    throw 'v3 must not contain a system overlay'
}
$forbidden = @('runtime/gnome','runtime/kde','payload/compositors',
    'payload/bridge/3b3b391e7350-stock-compositor-bridge-v7',
    'payload/bridge/6ca987e79ab06542-stock-kwin-o2-v6')
foreach ($relative in $forbidden) {
    if (Test-Path -LiteralPath (Join-Path $stage $relative)) {
        throw "Legacy compositor payload leaked into v3: $relative"
    }
}

$flavorFile = Join-Path $stage 'flavor.conf'
[IO.File]::WriteAllText($flavorFile, "BUILD_FLAVOR=$Flavor`n",
    [Text.UTF8Encoding]::new($false))
$moduleProp = Join-Path $stage 'module.prop'
$properties = [IO.File]::ReadAllText($moduleProp)
if ($Flavor -eq 'release') {
    foreach ($name in @(
            'archcraft-sway-free-e4d0126d.tar.gz',
            'lindex-archcraft-sway-public-assets-v2.tar.gz')) {
        $asset = Join-Path $stage "profile-assets/$name"
        if (-not (Test-Path -LiteralPath $asset -PathType Leaf) -or
            -not (Test-Path -LiteralPath "$asset.sha256" -PathType Leaf)) {
            throw "Release requires a pinned Sway source/public-asset archive and digest: $name"
        }
    }
    $properties = $properties -replace '(?m)^version=.*$', "version=v$releaseVersion"
    $properties = $properties -replace '(?m)^description=.*$',
        'description=LinDeX stock Wayland profiles, official Archcraft Sway dotfiles, no persistent logs, and no system overlay'
} else {
    $properties = $properties -replace '(?m)^version=.*$', "version=v$releaseVersion-dev"
    $properties = $properties -replace '(?m)^description=.*$',
        'description=LinDeX development build with bounded diagnostics, three stock Wayland profiles, and no system overlay'
}
[IO.File]::WriteAllText($moduleProp, $properties, [Text.UTF8Encoding]::new($false))

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
Add-Type -AssemblyName System.IO.Compression
$stream = [IO.File]::Open($target, [IO.FileMode]::CreateNew)
try {
    $archive = [IO.Compression.ZipArchive]::new($stream,
        [IO.Compression.ZipArchiveMode]::Create, $false)
    try {
        $timestamp = [DateTimeOffset]::Parse('2000-01-01T00:00:00Z')
        Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName |
            ForEach-Object {
                $relative = $_.FullName.Substring($stage.Length).TrimStart('\','/')
                $entry = $archive.CreateEntry($relative.Replace('\','/'),
                    [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = $timestamp
                $input = [IO.File]::OpenRead($_.FullName)
                $output = $entry.Open()
                try { $input.CopyTo($output) }
                finally { $output.Dispose(); $input.Dispose() }
            }
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }

$result = Get-FileHash -LiteralPath $target -Algorithm SHA256
[pscustomobject]@{ Zip=$target; Flavor=$Flavor; Bytes=(Get-Item $target).Length;
    Sha256=$result.Hash.ToLowerInvariant() }
