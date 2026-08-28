#requires -Version 7.0

param(
    [Parameter(Mandatory=$true)][string]$ModuleZip,
    [ValidateSet('release','dev')][string]$ExpectedFlavor = 'release'
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$versionFile = Join-Path $root 'VERSION'
if (-not (Test-Path -LiteralPath $versionFile -PathType Leaf)) {
    throw 'VERSION is missing'
}
$releaseVersion = [IO.File]::ReadAllText($versionFile).Trim()
if ($releaseVersion -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+$') {
    throw "VERSION must contain a release version such as 3.0.0: '$releaseVersion'"
}
$expectedVersionCode = ($releaseVersion -split '\.') -join ''
$zipPath = [IO.Path]::GetFullPath($ModuleZip)
if (-not (Test-Path -LiteralPath $zipPath -PathType Leaf)) {
    throw "Module ZIP does not exist: $zipPath"
}

Add-Type -AssemblyName System.IO.Compression
$stream = [IO.File]::OpenRead($zipPath)
try {
    $zip = [IO.Compression.ZipArchive]::new(
        $stream, [IO.Compression.ZipArchiveMode]::Read, $false)
    try {
        $entries = @{}
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName.Replace('\','/').TrimStart('/')
            if (-not $name -or $name.EndsWith('/')) { continue }
            if ($entries.ContainsKey($name)) { throw "Duplicate ZIP entry: $name" }
            if ($name -match '(^|/)\.\.(/|$)') { throw "Unsafe ZIP path: $name" }
            $entries[$name] = $entry
        }

        $required = @(
            'module.prop', 'flavor.conf', 'config.conf', 'customize.sh', 'service.sh',
            'uninstall.sh', 'debianfs-arm64.tar.xz',
            'bin/auto-service', 'bin/debian-gpu-control',
            'bin/launch-stock-profile', 'bin/stock-profile-session',
            'bin/session-runner', 'bin/advc-broker-service', 'bin/advc-broker',
            'bin/advc-capability-probe',
            'advc-artifacts.sha256',
            'payload/debian/android-drm-install',
            'payload/debian/android-drm-profile-manager',
            'payload/debian/android-drm-provider-manager',
            'payload/debian/start-profile-client',
            'payload/debian/lindex-firefox',
            'payload/debian/lindex-hardware-info',
            'payload/debian/lindex-neofetch',
            'payload/debian/lindex-neofetch.conf',
            'payload/debian/codec/advc_drv_video.so',
            'payload/debian/codec/advc-repack-gateway',
            'payload/debian/codec/advc-vaapi-decode-preflight',
            'payload/debian/codec/liblindex-firefox-advc-rdd-socket.so',
            'payload/debian/codec/liblindex-firefox-egl-drm-identity.so',
            'payload/debian/codec/SHA256SUMS',
            'payload/bridge/BRIDGE_PAYLOAD.sha256',
            'payload/bridge/stock-profile-bridge-v12/BRIDGE_RUNTIME.sha256',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-drm-bridge.so.1',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-drm-preload.so',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-vulkan-drm-identity.so',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-vulkan-drm-identity-layer.so',
            'payload/bridge/stock-profile-bridge-v12/lib/libdrm_lease_seat.so',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-usb-input-grab.so.1',
            'payload/bridge/stock-profile-bridge-v12/share/vulkan/explicit_layer.d/VK_LAYER_LINDEX_android_drm_identity.json',
            'profiles/lxqt.profile', 'profiles/xfce.profile',
            'profiles/sway.profile',
            'profile-assets/archcraft-sway-free-e4d0126d.tar.gz',
            'profile-assets/archcraft-sway-free-e4d0126d.tar.gz.sha256',
            'profile-assets/lindex-archcraft-sway-public-assets-v2.tar.gz',
            'profile-assets/lindex-archcraft-sway-public-assets-v2.tar.gz.sha256',
            'profile-assets/APPEARANCE_SOURCES.lock',
            'profile-assets/SOURCES.lock',
            'webroot/index.html', 'webroot/app.js',
            'webroot/style.css', 'webroot/locales.js'
        )
        foreach ($name in $required) {
            if (-not $entries.ContainsKey($name)) { throw "Missing v3 entry: $name" }
        }

        $forbiddenPatterns = @(
            '^system/', '^runtime/', '^payload/compositors/', '^provider-assets/',
            '^payload/bridge/(?!stock-profile-bridge-v12/|BRIDGE_PAYLOAD\.sha256$|install-bridge-runtime$)',
            '(?i)(^|/)(gnome|mutter|kwin|plasma)(/|$)',
            '(?i)^profiles/(wayfire|river|newm)\.profile$',
            '(?i)archcraft-sway-appearance-v1',
            '(?i)patched-wlroots',
            '(?i)(^|/)patches?/',
            '(?i)\.patch$',
            '(?i)(^|/).*smoke[^/]*$',
            '(?i)(^|/)advc-ffmpeg-decode-eos-probe(?:\.so)?$',
            '(?i)(^|/)advc-nv12-frame-hash$',
            '(?i)(^|/)(?:sample|fixture)s?/'
        )
        foreach ($name in $entries.Keys) {
            foreach ($pattern in $forbiddenPatterns) {
                if ($name -match $pattern) { throw "Legacy/patched entry leaked into v3: $name" }
            }
        }

        function Read-ZipText([string]$Name) {
            $reader = [IO.StreamReader]::new($entries[$Name].Open(),
                [Text.Encoding]::UTF8, $true)
            try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
        }

        function Get-ZipEntrySha256([string]$Name) {
            $algorithm = [Security.Cryptography.SHA256]::Create()
            $input = $entries[$Name].Open()
            try {
                return ([BitConverter]::ToString($algorithm.ComputeHash($input))).Replace('-', '').ToLowerInvariant()
            } finally {
                $input.Dispose()
                $algorithm.Dispose()
            }
        }

        function Test-XzEntry([string]$Name) {
            $expected = [byte[]](0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00)
            $actual = New-Object byte[] $expected.Length
            $input = $entries[$Name].Open()
            try {
                if ($input.Read($actual, 0, $actual.Length) -ne $actual.Length) { return $false }
            } finally { $input.Dispose() }
            return -not (Compare-Object $expected $actual -SyncWindow 0)
        }

        function Test-Aarch64ElfEntry([string]$Name) {
            $header = New-Object byte[] 20
            $input = $entries[$Name].Open()
            try {
                if ($input.Read($header, 0, $header.Length) -ne $header.Length) {
                    return $false
                }
            } finally { $input.Dispose() }
            # ELF64, little-endian, e_machine=EM_AARCH64 (183).
            return $header[0] -eq 0x7f -and $header[1] -eq 0x45 -and
                $header[2] -eq 0x4c -and $header[3] -eq 0x46 -and
                $header[4] -eq 2 -and $header[5] -eq 1 -and
                $header[18] -eq 183 -and $header[19] -eq 0
        }

        function Test-ZipSha256Manifest([string]$ManifestName,
                [string]$EntryPrefix, [int]$ExpectedCount) {
            $lines = (Read-ZipText $ManifestName) -split "`r?`n" |
                Where-Object { $_.Length -gt 0 }
            if ($lines.Count -ne $ExpectedCount) {
                throw "Unexpected manifest entry count in ${ManifestName}: $($lines.Count)"
            }
            foreach ($line in $lines) {
                if ($line -cnotmatch '^([0-9a-f]{64})  (.+)$') {
                    throw "Malformed SHA-256 manifest line in ${ManifestName}: $line"
                }
                $expectedHash = $Matches[1]
                $relativeName = $Matches[2].Replace('\','/')
                if ($relativeName.StartsWith('/') -or
                    $relativeName -match '(^|/)\.\.(/|$)') {
                    throw "Unsafe SHA-256 manifest path in ${ManifestName}: $relativeName"
                }
                $entryName = "$EntryPrefix$relativeName"
                if (-not $entries.ContainsKey($entryName)) {
                    throw "SHA-256 manifest target is missing: $entryName"
                }
                $actualHash = Get-ZipEntrySha256 $entryName
                if ($actualHash -cne $expectedHash) {
                    throw "SHA-256 manifest mismatch: $entryName"
                }
            }
        }

        Test-ZipSha256Manifest 'payload/bridge/BRIDGE_PAYLOAD.sha256' `
            'payload/bridge/' 11
        Test-ZipSha256Manifest `
            'payload/bridge/stock-profile-bridge-v12/BRIDGE_RUNTIME.sha256' `
            'payload/bridge/stock-profile-bridge-v12/' 9

        $bridgeElfEntries = @(
            'payload/bridge/stock-profile-bridge-v12/bin/android-drm-bridge-probe',
            'payload/bridge/stock-profile-bridge-v12/bin/drm_lease_client',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-drm-bridge.so.1',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-drm-preload.so',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-usb-input-grab.so.1',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-vulkan-drm-identity-layer.so',
            'payload/bridge/stock-profile-bridge-v12/lib/libandroid-vulkan-drm-identity.so',
            'payload/bridge/stock-profile-bridge-v12/lib/libdrm_lease_seat.so'
        )
        foreach ($name in $bridgeElfEntries) {
            if (-not (Test-Aarch64ElfEntry $name)) {
                throw "Bridge payload is not a little-endian AArch64 ELF64 binary: $name"
            }
        }

        $flavor = (Read-ZipText 'flavor.conf').Trim()
        if ($flavor -ne "BUILD_FLAVOR=$ExpectedFlavor") {
            throw "Flavor mismatch: expected $ExpectedFlavor, found '$flavor'"
        }
        $moduleProp = Read-ZipText 'module.prop'
        if ($moduleProp -notmatch '(?m)^id=debian_chroot$' -or
            $moduleProp -notmatch "(?m)^versionCode=$expectedVersionCode$") {
            throw 'Unexpected module identity or versionCode'
        }
        if ($moduleProp -notmatch '(?m)^name=LinDeX$' -or
            $moduleProp -notmatch '(?m)^author=JHoon$') {
            throw 'Public module name/author must be LinDeX by JHoon'
        }
        if ($ExpectedFlavor -eq 'release') {
            $expectedVersion = "version=v$releaseVersion"
            if (-not (($moduleProp -split "`r?`n") -ccontains $expectedVersion)) {
                throw "Release ZIP does not have $expectedVersion"
            }
            if ($moduleProp -notmatch '(?m)^description=.*no persistent logs.*$') {
                throw 'Release description does not declare the no-log policy'
            }
        } else {
            $expectedVersion = "version=v$releaseVersion-dev"
            if (-not (($moduleProp -split "`r?`n") -ccontains $expectedVersion)) {
                throw "Dev ZIP does not have $expectedVersion"
            }
        }

        $rootfsName = 'debianfs-arm64.tar.xz'
        $rootfsBytes = 51724364
        $rootfsSha256 = 'c4c380ed14926d77aeb273301ccc83636ee371739a17ec52a1ba41e163dc5741'
        if ($entries[$rootfsName].Length -ne $rootfsBytes) {
            throw "Canonical rootfs size mismatch: expected $rootfsBytes, found $($entries[$rootfsName].Length)"
        }
        if (-not (Test-XzEntry $rootfsName)) { throw 'Canonical rootfs is not an XZ stream' }
        if ((Get-ZipEntrySha256 $rootfsName) -ne $rootfsSha256) {
            throw 'Canonical Debian ARM64 rootfs SHA-256 mismatch'
        }

        $customize = Read-ZipText 'customize.sh'
        foreach ($needle in @('debianfs-arm64.tar.xz',
                'tar -xJf "$MODPATH/debianfs-arm64.tar.xz" -C "$ROOTFS"',
                'profile-runtime-manager', 'profile-configurator',
                'lindex-hardware-info', 'lindex-neofetch.conf',
                'getprop ro.soc.model',
                '/sys/class/kgsl/kgsl-3d0/gpu_model')) {
            if (-not $customize.Contains($needle)) {
                throw "Fresh-install customize flow is missing: $needle"
            }
        }

        $vaDriver = 'payload/debian/codec/advc_drv_video.so'
        if (-not (Test-Aarch64ElfEntry $vaDriver)) {
            throw 'ADVC VA-API payload is not a little-endian AArch64 ELF64 binary'
        }
        $codecArtifacts = @('advc_drv_video.so', 'advc-repack-gateway',
            'advc-vaapi-decode-preflight',
            'liblindex-firefox-advc-rdd-socket.so',
            'liblindex-firefox-egl-drm-identity.so')
        foreach ($codecName in $codecArtifacts) {
            $codecEntry = "payload/debian/codec/$codecName"
            if (-not (Test-Aarch64ElfEntry $codecEntry)) {
                throw "Codec runtime payload is not AArch64 ELF64: $codecEntry"
            }
        }
        Test-ZipSha256Manifest 'payload/debian/codec/SHA256SUMS' `
            'payload/debian/codec/' 5
        Test-ZipSha256Manifest 'advc-artifacts.sha256' '' 7
        $sourceAdvcManifest = [IO.File]::ReadAllText(
            (Join-Path $root 'module/advc-artifacts.sha256')).Replace("`r`n", "`n").Trim()
        $advcManifest = (Read-ZipText 'advc-artifacts.sha256').Replace("`r`n", "`n").Trim()
        if ($advcManifest -cne $sourceAdvcManifest) {
            throw 'Packaged ADVC artifact manifest differs from the reviewed source manifest'
        }

        $vulkanLayer = 'payload/bridge/stock-profile-bridge-v12/lib/libandroid-vulkan-drm-identity-layer.so'
        if (-not (Test-Aarch64ElfEntry $vulkanLayer)) {
            throw 'Vulkan DRM identity layer is not a little-endian AArch64 ELF64 binary'
        }
        $vulkanLayerSha256 = Get-ZipEntrySha256 $vulkanLayer
        if ($vulkanLayerSha256 -ne '36569734d4f7b49f2fd260490862022e7f0957b74a6f82fe1935e5b76fe58c5d') {
            throw "Unexpected Vulkan DRM identity layer SHA-256: $vulkanLayerSha256"
        }
        $vulkanLayerJson = Read-ZipText 'payload/bridge/stock-profile-bridge-v12/share/vulkan/explicit_layer.d/VK_LAYER_LINDEX_android_drm_identity.json'
        try { $vulkanLayerManifest = $vulkanLayerJson | ConvertFrom-Json }
        catch { throw "Vulkan explicit-layer manifest is invalid JSON: $_" }
        if ($vulkanLayerManifest.file_format_version -cne '1.0.0' -or
            $vulkanLayerManifest.layer.name -cne 'VK_LAYER_LINDEX_android_drm_identity' -or
            $vulkanLayerManifest.layer.type -cne 'GLOBAL' -or
            $vulkanLayerManifest.layer.library_path -cne '../../../lib/libandroid-vulkan-drm-identity-layer.so' -or
            $vulkanLayerManifest.layer.api_version -cne '1.1.0' -or
            [string]$vulkanLayerManifest.layer.implementation_version -cne '2') {
            throw 'Vulkan explicit-layer manifest contract mismatch'
        }
        foreach ($needle in @(
                'payload/debian/codec/advc_drv_video.so',
                'payload/debian/codec/advc-repack-gateway',
                'payload/debian/codec/advc-vaapi-decode-preflight',
                'payload/debian/codec/liblindex-firefox-advc-rdd-socket.so',
                'opt/android-drm-lease-kit/codec',
                'Installed ADVC codec runtime verification failed')) {
            if (-not $customize.Contains($needle)) {
                throw "Fresh-install ADVC VA-API staging is missing: $needle"
            }
        }

        $launchProfile = Read-ZipText 'bin/launch-stock-profile'
        foreach ($needle in @('prepare_advc_codec_runtime',
                'SESSION_VIDEO_ACCELERATION=disabled',
                'ANDROID_DRM_VIDEO_ACCELERATION="$SESSION_VIDEO_ACCELERATION"')) {
            if (-not $launchProfile.Contains($needle)) {
                throw "Fail-closed ADVC launch gate is missing: $needle"
            }
        }
        $stockSession = Read-ZipText 'bin/stock-profile-session'
        foreach ($needle in @('FD_KGSL_ENABLE_DMABUF=1',
                'LIBVA_DRIVER_NAME=advc',
                'LIBVA_DRIVERS_PATH=$ADVC_VAAPI_DRIVER_DIR',
                'ADVC_VAAPI_SOCKET=$ADVC_VAAPI_SOCKET_PATH',
                 'GST_VAAPI_DRM_DEVICE=/dev/dri/renderD128',
                 'ADVC_VAAPI_ENABLE_AVC=validated-v1',
                 'ADVC_VAAPI_H264_REORDER_BOUND=validated-main-reorder1-v1',
                 'validated-main-inter-prime-eos-120of120-v1',
                 'validated-profile0-inter-prime-eos-120of120-v1',
                 'export ADVC_VAAPI_ENABLE_HEVC=$hevc_validation',
                 'export ADVC_VAAPI_ENABLE_VP9=$vp9_validation',
                 'ADVC_VAAPI_ENABLE_ENCODE=validated-avc-hevc-v1',
                'ADVC_REPACK_GATEWAY_ENABLE=validated-qcom-nv12-v1',
                'ADVC_REPACK_GATEWAY_OUTPUT=auto',
                'ADVC_VAAPI_DECODE_OUTPUT=auto',
                'ADVC_VAAPI_ENCODE_INPUT=auto',
                'LINDEX_FIREFOX_RDD_SOCKET_ACK=firefox-rdd-advc-socket-v1',
                'LINDEX_EGL_DRM_IDENTITY_ACK=kgsl-card0-renderD128-firefox-glxtest-v1',
                'export LINDEX_FIREFOX_PRELOAD=$FIREFOX_PRELOAD',
                'ADVC_VAAPI_ENABLE_GENERIC_UPLOAD=validated-nv12-v1',
                'ADVC_VAAPI_ENABLE_WRITE_EXPORT=validated-dmabuf-syncfile-v1',
                'unset ADVC_VAAPI_ENABLE_PRIVATE_DECODE_EOS',
                'unset ADVC_CODEC_DECODER_DESTROY_DRAIN',
                'unset LIBVA_DRIVER_NAME LIBVA_DRIVERS_PATH ADVC_VAAPI_SOCKET',
                'unset ADVC_VAAPI_ENABLE_WRITE_EXPORT ADVC_VAAPI_ENABLE_GENERIC_UPLOAD',
                '[ -e /proc/self/fd ]', '[ -d /sys/class/drm/renderD128 ]')) {
            if (-not $stockSession.Contains($needle)) {
                throw "ADVC application environment gate is missing: $needle"
            }
        }
        if ($stockSession.Contains('PRELOAD=$FIREFOX_PRELOAD:$PRELOAD') -or
            $stockSession.Contains('PRELOAD=$CODEC_PRELOAD:$PRELOAD')) {
            throw 'Firefox adapters leaked into the compositor-wide LD_PRELOAD'
        }
        $firefoxLauncher = Read-ZipText 'payload/debian/lindex-firefox'
        foreach ($needle in @('expected_preload=$egl_adapter:$rdd_adapter',
                'export LD_PRELOAD=$expected_preload',
                'unset DRM_LEASE_FD DRM_LEASE_LESSEE_ID DRM_LEASE_OBJECTS')) {
            if (-not $firefoxLauncher.Contains($needle)) {
                throw "Scoped Firefox launcher gate is missing: $needle"
            }
        }
        $kgslProfile = Read-ZipText 'payload/debian/99-android-kgsl.sh'
        if (-not $kgslProfile.Contains('export FD_KGSL_ENABLE_DMABUF=1')) {
            throw 'Packaged Debian KGSL profile is missing FD_KGSL_ENABLE_DMABUF=1'
        }
        foreach ($forbiddenExport in @(
                'export ADVC_VAAPI_ENABLE_PRIVATE_DECODE_EOS=',
                'export ADVC_CODEC_DECODER_DESTROY_DRAIN=',
                'export ADVC_VAAPI_QCOM_IMPORT=',
                'MOZ_DISABLE_RDD_SANDBOX=')) {
            if ($stockSession.Contains($forbiddenExport)) {
                throw "Unvalidated ADVC decode experiment leaked into the stock session: $forbiddenExport"
            }
        }
        foreach ($needle in @('exact_rdev_pair /dev/dri/card0',
                'exact_rdev_pair /dev/dri/renderD128',
                'exact_rdev_pair /dev/kgsl-3d0',
                'ANDROID_VULKAN_DRM_IDENTITY_ACK=$vk_identity_ack',
                'VK_INSTANCE_LAYERS=VK_LAYER_LINDEX_android_drm_identity',
                'export ANDROID_VULKAN_DRM_IDENTITY_OWNER_PID=$$')) {
            if (-not $stockSession.Contains($needle)) {
                throw "Dynamic Vulkan DRM identity gate is missing: $needle"
            }
        }

        $profileClient = Read-ZipText 'payload/debian/start-profile-client'
        foreach ($needle in @('unset VK_LAYER_PATH VK_INSTANCE_LAYERS',
                'unset ANDROID_VULKAN_DRM_IDENTITY_OWNER_PID ANDROID_VULKAN_DRM_IDENTITY_TRACE',
                'export XFCE4_SESSION_COMPOSITOR=/usr/bin/xfce4-session',
                'exec /bin/sh /etc/xdg/xfce4/xinitrc')) {
            if (-not $profileClient.Contains($needle)) {
                throw "Desktop Vulkan child isolation is missing: $needle"
            }
        }
        if ($profileClient.Contains('startxfce4 --wayland')) {
            throw 'XFCE client would launch a nested labwc compositor'
        }
        foreach ($needle in @('xfce_labwc_dir=${XDG_CONFIG_HOME:-$HOME/.config}/xfce4/labwc',
                '--config-dir "$xfce_labwc_dir"',
                '--config "$xfce_labwc_dir/rc.xml"')) {
            if (-not $stockSession.Contains($needle)) {
                throw "XFCE lease compositor configuration is missing: $needle"
            }
        }

        $profileManager = Read-ZipText 'payload/debian/android-drm-profile-manager'
        $providerManager = Read-ZipText 'payload/debian/android-drm-provider-manager'
        $swayProfile = Read-ZipText 'profiles/sway.profile'
        $installer = Read-ZipText 'payload/debian/android-drm-install'
        $lxqtProfile = Read-ZipText 'profiles/lxqt.profile'
        $xfceProfile = Read-ZipText 'profiles/xfce.profile'
        foreach ($noPolkitContract in @($profileManager,
                $profileClient, $swayProfile, $lxqtProfile, $xfceProfile)) {
            if ($noPolkitContract -match '(?i)(lxpolkit|xfce-polkit|lxqt-policykit)') {
                throw 'Unused chroot PolicyKit agent remains in the release package contract'
            }
        }
        foreach ($needle in @(
                'for obsolete_package in lxpolkit xfce-polkit lxqt-policykit lxqt-core; do',
                'apt_get purge -y $obsolete_profile_packages')) {
            if (-not $installer.Contains($needle)) {
                throw "Legacy PolicyKit-agent migration is missing: $needle"
            }
        }
        foreach ($needle in @('adapt_sway_waybar_for_android "$target"',
                '-e ''s/, "bluetooth"//g''',
                '-e ''s/, "backlight"//g''',
                'scripts/android_battery', '"custom/android-battery"',
                'scripts/android_volume', 'scripts/android_volume_control',
                '"custom/android-volume"', 'STREAM_MUSIC',
                '"on-click": "$HOME/.config/sway/scripts/android_volume_control toggle"',
                '"on-scroll-up": "$HOME/.config/sway/scripts/android_volume_control raise"',
                '"on-scroll-down": "$HOME/.config/sway/scripts/android_volume_control lower"',
                '-e ''s/#pulseaudio/#custom-android-volume/g''',
                'android-volume-v1',
                'fa4-lightmode-v1',
                's/󰖨//g',
                'charge_now=0', '"FontAwesome"')) {
            if (-not $profileManager.Contains($needle)) {
                throw "Android-owned Waybar module adapter is missing: $needle"
            }
        }
        if ($profileManager.Contains('run mpd')) {
            throw 'Sway startup must not launch a Linux audio daemon'
        }
        $volumeUiStart = $profileManager.IndexOf('cat > "$runtime_target/scripts/android_volume"')
        $volumeUiEnd = $profileManager.IndexOf('cat > "$runtime_target/scripts/rofi_powermenu"')
        if ($volumeUiStart -lt 0 -or $volumeUiEnd -le $volumeUiStart) {
            throw 'Android volume UI script block is malformed'
        }
        $volumeUiBlock = $profileManager.Substring(
            $volumeUiStart, $volumeUiEnd - $volumeUiStart)
        if ($volumeUiBlock -match '(?i)(^|[^a-z0-9_])(aplay|amixer|pactl|pulsemixer)([^a-z0-9_]|$)|/dev/snd') {
            throw 'Android volume UI block controls a Linux audio output path'
        }
        $autoService = Read-ZipText 'bin/auto-service'
        foreach ($volumeNeedle in @('process_android_volume_commands',
                'android_media_volume_command --get --stream 3',
                'ANDROID_MEDIA_TIMEOUT=${ANDROID_MEDIA_TIMEOUT:-/system/bin/timeout}',
                'android-volume-commands', 'android-volume.state',
                'chmod 0700 "$ANDROID_VOLUME_RUNTIME" "$ANDROID_VOLUME_COMMANDS"')) {
            if (-not $autoService.Contains($volumeNeedle)) {
                throw "Android volume bridge is missing: $volumeNeedle"
            }
        }
        $volumeBridgeStart = $autoService.IndexOf('ANDROID_VOLUME_LAST=')
        $volumeBridgeEnd = $autoService.IndexOf('# Reconcile the Android hardware-codec broker')
        if ($volumeBridgeStart -lt 0 -or $volumeBridgeEnd -le $volumeBridgeStart) {
            throw 'Android volume host bridge block is malformed'
        }
        $volumeBridgeBlock = $autoService.Substring(
            $volumeBridgeStart, $volumeBridgeEnd - $volumeBridgeStart)
        if ($volumeBridgeBlock.Contains('log_event')) {
            throw 'Android volume bridge must not create release or development logs'
        }
        foreach ($requiredSwayPackage in @('wofi|wofi', 'kanshi|kanshi', 'wlogout|wlogout')) {
            if (-not $profileManager.Contains($requiredSwayPackage)) {
                throw "Public Sway package parity is incomplete: $requiredSwayPackage"
            }
        }

        $common = Read-ZipText 'bin/common.sh'
        $brokerService = Read-ZipText 'bin/advc-broker-service'
        $sessionRunner = Read-ZipText 'bin/session-runner'
        $autoService = Read-ZipText 'bin/auto-service'
        foreach ($needle in @('/apps/uid_*/pid_*',
                '> /sys/fs/cgroup/cgroup.procs',
                'cannot detach from Android WebUI freezer cgroup')) {
            if (-not $sessionRunner.Contains($needle)) {
                throw "WebUI freezer-cgroup detachment is missing: $needle"
            }
        }
        foreach ($needle in @('LOGFILE=/dev/null', 'SETUP_LOG=/dev/null',
                'rm -f "$STATE/session.log"', 'tail -c 262144')) {
            if (-not $common.Contains($needle)) {
                throw "Release/dev bounded-log contract is missing from common.sh: $needle"
            }
        }
        foreach ($needle in @('ADVC_LOG_FILE=/dev/null', 'ADVC_EVENT_LOG=/dev/null')) {
            if (-not $brokerService.Contains($needle)) {
                throw "Release broker no-log contract is missing: $needle"
            }
        }
        if (-not $sessionRunner.Contains('[ "$BUILD_FLAVOR" = dev ] || rm -f "$DISPLAY_CMD_LOG"')) {
            throw 'Release DisplayManager temporary-log cleanup is missing'
        }
        foreach ($needle in @('owned_process_group_running',
                'LINDEX_SESSION_TOKEN=$SESSION_TOKEN')) {
            if (-not ($common.Contains($needle) -or $sessionRunner.Contains($needle))) {
                throw "Token-owned process-group cleanup is missing: $needle"
            }
        }
        foreach ($needle in @('stop_threshold=1',
                '[ "$present_samples" -ge 2 ]', 'typec_partner_present')) {
            if (-not $autoService.Contains($needle)) {
                throw "Hardened hotplug lifecycle gate is missing: $needle"
            }
        }

        $swayAsset = 'profile-assets/archcraft-sway-free-e4d0126d.tar.gz'
        $swayDigest = "$swayAsset.sha256"
        $expected = ((Read-ZipText $swayDigest) -split '\s+')[0].ToLowerInvariant()
        if ($expected -ne 'da89184c13bb68affc89b2638efb2d93736dc685e4104f6a5a497c6f9e43dadc' -or
            (Get-ZipEntrySha256 $swayAsset) -ne $expected) {
            throw 'Official Archcraft Sway asset digest mismatch'
        }
        $appearanceAsset = 'profile-assets/lindex-archcraft-sway-public-assets-v2.tar.gz'
        $appearanceDigest = "$appearanceAsset.sha256"
        $appearanceExpected = ((Read-ZipText $appearanceDigest) -split '\s+')[0].ToLowerInvariant()
        if ($appearanceExpected -ne '4b84564c692e270bb46bbc36c4e5f9b1684c5ed4f1d8bcbf053698780a0af08c' -or
            (Get-ZipEntrySha256 $appearanceAsset) -ne $appearanceExpected) {
            throw 'LinDeX Archcraft public-asset aggregate digest mismatch'
        }
        $sourceLock = Read-ZipText 'profile-assets/SOURCES.lock'
        foreach ($needle in @(
                'source=https://github.com/archcraft-os/archcraft-sway',
                'commit=e4d0126d7f236fee50a84fbb0e61498dcf5705e7',
                'license=GPL-3.0')) {
            if (-not $sourceLock.Contains($needle)) {
                throw "Archcraft Sway attribution lock is missing: $needle"
            }
        }
        $appearanceLock = Read-ZipText 'profile-assets/APPEARANCE_SOURCES.lock'
        foreach ($needle in @(
                'source=https://github.com/archcraft-os/archcraft-themes',
                'commit=7322626c48be183bfdd7c3eeb2faad1fb69da0f4',
                'source=https://github.com/archcraft-os/archcraft-icons',
                'commit=1af3af70ccb233bf26f42162f7e65e4a36803667',
                'source=https://github.com/archcraft-os/archcraft-cursors',
                'commit=8b7e4633cf8e73502f2cfd396d077edf9304c440',
                'source_path=archcraft-gtk-theme-qogir/files/Qogir-Light',
                'source_path=archcraft-icons-qogir/files/Qogir',
                'source_path=archcraft-icons-breeze/files/Archcraft',
                'source_path=archcraft-cursor-qogirr/files/Qogirr-Dark',
                'corresponding_source_path=corresponding-source/Qogirr-Dark/src/cursors')) {
            if (-not $appearanceLock.Contains($needle)) {
                throw "Archcraft appearance attribution lock is missing: $needle"
            }
        }

        foreach ($scriptName in @('customize.sh', 'bin/setup-debian-gpu',
                'payload/debian/android-drm-install',
                'payload/debian/android-drm-profile-manager')) {
            $text = Read-ZipText $scriptName
            if ($text -match '(?i)archcraft-archive') {
                throw "Obsolete appearance archive path leaked into v3: $scriptName"
            }
        }

        $profileManager = Read-ZipText 'payload/debian/android-drm-profile-manager'
        foreach ($needle in @(
                'hyprpicker|hyprpicker', 'hyprlock|hyprlock', 'light|light',
                'wofi|wofi', 'kanshi|kanshi', 'wlogout|wlogout',
                'remove_legacy_hypr_wrappers',
                '[ "$candidate" != - ] && package_installed "$candidate" &&',
                '0432b6f49bff0028a86cae6e15439c460163c21769a13c95f1892c587ee74d17',
                '03d649a8357a585369386c7016c3374aaa43d849aee0ebf1ee602dae2ae2c987',
                'package-command-and-appearance-verified',
                'Suites: trixie-backports', 'composition_manifest',
                'runtime_manifest')) {
            if (-not $profileManager.Contains($needle)) {
                throw "Profile package/command verification is missing: $needle"
            }
        }
        foreach ($obsolete in @('light|brightnessctl',
                'exec swaylock "$@"',
                'LinDeX compatibility command using public wlroots screenshot tools',
                'wf-background', 'wf-panel', 'wf-dock')) {
            if ($profileManager.Contains($obsolete)) {
                throw "Obsolete or unlisted profile substitute remains: $obsolete"
            }
        }

        $providerManager = Read-ZipText 'payload/debian/android-drm-provider-manager'
        if (-not $providerManager.Contains('PROVIDER: pastel | python-pywal')) {
            throw 'Sway public provider set is incomplete'
        }
        foreach ($removedProvider in @('river-stack', 'newm-next-stack', 'zig-0.16')) {
            if ($providerManager.Contains($removedProvider)) {
                throw "Removed provider remains in release module: $removedProvider"
            }
        }

        $common = Read-ZipText 'bin/common.sh'
        if (-not $common.Contains('package-command-and-appearance-verified')) {
            throw 'Runtime accepts profile markers without package/command verification'
        }
        foreach ($needle in @('SWAY_THEME=dark',
                'case "$SWAY_THEME" in dark|light|pywal)',
                'standard) MESA_MODE=turnip',
                'turnip|turnip-unpatched|system)')) {
            if (-not $common.Contains($needle)) {
                throw "Runtime configuration policy is missing: $needle"
            }
        }
        $moduleConfig = Read-ZipText 'config.conf'
        if ($moduleConfig -notmatch '(?m)^MESA_MODE=turnip$') {
            throw 'Recommended patched-Turnip mode is not the module default'
        }
        $mesaInstaller = Read-ZipText 'payload/debian/android-drm-install'
        foreach ($needle in @(
                'turnip|turnip-unpatched|system)',
                'installing-kgsl-patched-turnip',
                'installing-unpatched-turnip-compatibility',
                'MESA_SELECTION_SCHEMA=2',
                'MESA_UNPATCHED_TURNIP_TAG=none')) {
            if (-not $mesaInstaller.Contains($needle)) {
                throw "Mesa selection/migration policy is missing: $needle"
            }
        }
        foreach ($needle in @('apply_official_sway_theme',
                'ANDROID_DRM_SWAY_THEME', 'theme_argument=--light',
                'theme_argument=--pywal')) {
            if (-not $profileManager.Contains($needle)) {
                throw "Official Sway theme application is missing: $needle"
            }
        }
        $webApp = Read-ZipText 'webroot/app.js'
        if (-not $webApp.Contains("command('profiles')")) {
            throw 'WebUI profile list is not synchronized with packaged metadata'
        }
        $webIndex = Read-ZipText 'webroot/index.html'
        foreach ($needle in @(
                'href="https://ko-fi.com/s/10f2e87af3"',
                'target="_blank" rel="noopener noreferrer"',
                'data-i18n="supportArchcraft"',
                'id="swayThemeSetting"', 'id="swayTheme"',
                '<option value="turnip" data-i18n="mesaTurnipPatched">',
                '<option value="turnip-unpatched" data-i18n="mesaTurnipUnpatched">')) {
            if (-not $webIndex.Contains($needle)) {
                throw "Archcraft creator support link is missing or unsafe: $needle"
            }
        }
        if (-not $webApp.Contains("archcraftSupport').hidden = profile !== 'sway'")) {
            throw 'Archcraft support link is not scoped to the Sway profile'
        }
        if (-not $webApp.Contains('option.textContent = nameKey ? t(nameKey) : profile.name')) {
            throw 'Dynamic profile names are not localized by stable profile ID'
        }
        $webLocales = Read-ZipText 'webroot/locales.js'
        foreach ($needle in @(
                "profileSway:'Archcraft Sway Free · 공식 공개 dotfiles'",
                "profileSway:'Archcraft Sway Free · official public dotfiles'",
                "mesaTurnipPatched:'KGSL + 패치 Turnip · 권장'",
                "mesaTurnipPatched:'KGSL + patched Turnip · recommended'",
                "mesaTurnipUnpatched:'KGSL + 비패치 Turnip · 호환'",
                "mesaTurnipUnpatched:'KGSL + unpatched Turnip · compatibility'")) {
            if (-not $webLocales.Contains($needle)) {
                throw "Bilingual WebUI label is missing: $needle"
            }
        }

        $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
        [pscustomobject]@{
            Zip = $zipPath
            Flavor = $ExpectedFlavor
            Entries = $entries.Count
            Bytes = (Get-Item -LiteralPath $zipPath).Length
            Sha256 = $hash
            Result = 'PASS'
        }
    } finally {
        $zip.Dispose()
    }
} finally {
    $stream.Dispose()
}
