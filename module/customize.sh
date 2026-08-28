SKIPUNZIP=0
ROOTFS=/data/local/debian
PERSIST=/data/adb/debian-drm-lease-kit

ui_print "- Checking Adreno/KGSL and MSM/SDE"
[ "$ARCH" = arm64 ] || abort "! This module requires arm64"
if [ ! -c /dev/kgsl-3d0 ] && [ ! -d /sys/class/kgsl/kgsl-3d0 ]; then
    abort "! Qualcomm Adreno KGSL is required"
fi
[ -c /dev/dri/card0 ] || abort "! /dev/dri/card0 is missing"

ui_print "- Verifying the stock compositor bridge"
(cd "$MODPATH/payload/bridge" && sha256sum -c BRIDGE_PAYLOAD.sha256) ||
    abort "! Stock bridge payload verification failed"
chmod 0755 "$MODPATH/payload/bridge/install-bridge-runtime"

[ -r "$MODPATH/advc-artifacts.sha256" ] ||
    abort "! Hardware video runtime manifest is missing"
ui_print "- Verifying hardware video runtime"
(cd "$MODPATH" && sha256sum -c advc-artifacts.sha256) ||
    abort "! Hardware video runtime verification failed"
[ -f "$MODPATH/payload/debian/codec/advc_drv_video.so" ] ||
    abort "! ADVC VA-API driver is missing"
[ -f "$MODPATH/payload/debian/codec/advc-repack-gateway" ] ||
    abort "! ADVC repack gateway is missing"
[ -f "$MODPATH/payload/debian/codec/advc-vaapi-decode-preflight" ] ||
    abort "! ADVC decode capability preflight is missing"
[ -f "$MODPATH/payload/debian/codec/liblindex-firefox-advc-rdd-socket.so" ] ||
    abort "! Firefox RDD socket adapter is missing"
[ -f "$MODPATH/payload/debian/codec/liblindex-firefox-egl-drm-identity.so" ] ||
    abort "! Firefox EGL identity adapter is missing"
[ -f "$MODPATH/payload/debian/codec/SHA256SUMS" ] ||
    abort "! ADVC codec runtime manifest is missing"
[ -f "$MODPATH/payload/debian/lindex-firefox" ] ||
    abort "! Scoped Firefox launcher is missing"
(cd "$MODPATH/payload/debian/codec" && sha256sum -c SHA256SUMS) ||
    abort "! ADVC VA-API driver verification failed"

mkdir -p "$ROOTFS" "$PERSIST/state"
if [ -r "$ROOTFS/etc/os-release" ]; then
    grep -q 'VERSION_CODENAME=trixie' "$ROOTFS/etc/os-release" ||
        abort "! Existing /data/local/debian is not Debian trixie; no data was changed"
    ui_print "- Existing Debian rootfs found; preserving user data"
else
    [ -f "$MODPATH/debianfs-arm64.tar.xz" ] ||
        abort "! debianfs-arm64.tar.xz is required for a new installation"
    ui_print "- Extracting Debian trixie rootfs"
    xz -t "$MODPATH/debianfs-arm64.tar.xz" || abort "! Rootfs archive is corrupt"
    tar -xJf "$MODPATH/debianfs-arm64.tar.xz" -C "$ROOTFS" ||
        abort "! Rootfs extraction failed"
fi

ui_print "- Installing immutable stock bridge v12"
"$MODPATH/payload/bridge/install-bridge-runtime" \
    "$ROOTFS" "$MODPATH/payload/bridge" ||
    abort "! Stock bridge staging failed"

ui_print "- Installing immutable ADVC codec and Firefox gateway runtime"
ADVC_VAAPI_ROOT="$ROOTFS/opt/android-drm-lease-kit/codec"
mkdir -p "$ADVC_VAAPI_ROOT" || abort "! Cannot create ADVC VA-API directory"
for codec_file in advc_drv_video.so advc-repack-gateway \
    advc-vaapi-decode-preflight \
    liblindex-firefox-advc-rdd-socket.so \
    liblindex-firefox-egl-drm-identity.so SHA256SUMS; do
    cp -f "$MODPATH/payload/debian/codec/$codec_file" \
        "$ADVC_VAAPI_ROOT/$codec_file" || abort "! Cannot stage ADVC codec runtime"
done
chown 0:0 "$ADVC_VAAPI_ROOT"/advc_drv_video.so \
    "$ADVC_VAAPI_ROOT"/advc-repack-gateway \
    "$ADVC_VAAPI_ROOT"/advc-vaapi-decode-preflight \
    "$ADVC_VAAPI_ROOT"/liblindex-firefox-advc-rdd-socket.so \
    "$ADVC_VAAPI_ROOT"/liblindex-firefox-egl-drm-identity.so \
    "$ADVC_VAAPI_ROOT"/SHA256SUMS
chmod 0755 "$ADVC_VAAPI_ROOT/advc_drv_video.so" \
    "$ADVC_VAAPI_ROOT/advc-repack-gateway" \
    "$ADVC_VAAPI_ROOT/advc-vaapi-decode-preflight"
chmod 0644 "$ADVC_VAAPI_ROOT/liblindex-firefox-advc-rdd-socket.so" \
    "$ADVC_VAAPI_ROOT/liblindex-firefox-egl-drm-identity.so" \
    "$ADVC_VAAPI_ROOT/SHA256SUMS"
(cd "$ADVC_VAAPI_ROOT" && sha256sum -c SHA256SUMS) ||
    abort "! Installed ADVC codec runtime verification failed"

ui_print "- Staging profile, Mesa and session installers"
mkdir -p "$ROOTFS/usr/local/bin" "$ROOTFS/usr/local/sbin" \
    "$ROOTFS/usr/local/libexec/android-drm" \
    "$ROOTFS/usr/local/share/android-drm/profiles" "$ROOTFS/etc/profile.d" \
    "$ROOTFS/etc/lindex" \
    "$ROOTFS/var/cache/android-drm-lease-kit/providers" \
    "$ROOTFS/var/cache/android-drm-lease-kit/profiles"
[ -f "$MODPATH/payload/debian/android-drm-install" ] || abort "! Missing Debian installer"
cp -f "$MODPATH/payload/debian/android-drm-install" \
    "$ROOTFS/usr/local/sbin/android-drm-install"
chmod 0755 "$ROOTFS/usr/local/sbin/android-drm-install"
[ -f "$MODPATH/payload/debian/android-drm-profile-manager" ] || \
    abort "! Missing LinDeX profile configurator"
cp -f "$MODPATH/payload/debian/android-drm-profile-manager" \
    "$ROOTFS/usr/local/libexec/android-drm/profile-configurator"
chmod 0755 "$ROOTFS/usr/local/libexec/android-drm/profile-configurator"
provider_manager=android-drm-provider-manager
[ -f "$MODPATH/payload/debian/$provider_manager" ] || \
    abort "! Missing internal profile runtime installer"
cp -f "$MODPATH/payload/debian/$provider_manager" \
    "$ROOTFS/usr/local/libexec/android-drm/profile-runtime-manager"
chmod 0755 "$ROOTFS/usr/local/libexec/android-drm/profile-runtime-manager"

# Archcraft Sway Free is an official public GPL-3.0 dotfile package.  Pin and
# verify the exact upstream archive shipped with the release; no Ko-fi asset or
# user-supplied download is involved.
for archive in "$MODPATH"/profile-assets/*.tar.gz; do
    [ -f "$archive" ] || continue
    archive_name=${archive##*/}
    case "$archive_name" in
        archcraft-sway-free-e4d0126d.tar.gz|lindex-archcraft-sway-public-assets-v2.tar.gz)
            [ -r "$archive.sha256" ] || abort "! Missing Archcraft Sway asset digest"
            (cd "$MODPATH/profile-assets" && sha256sum -c "$archive_name.sha256") ||
                abort "! Archcraft Sway asset verification failed"
            cp -f "$archive" "$ROOTFS/var/cache/android-drm-lease-kit/profiles/$archive_name"
            cp -f "$archive.sha256" \
                "$ROOTFS/var/cache/android-drm-lease-kit/profiles/$archive_name.sha256"
            ;;
    esac
done
cp -f "$MODPATH/payload/debian/android-drm-rollback" \
    "$ROOTFS/usr/local/sbin/android-drm-rollback"
cp -f "$MODPATH/payload/debian/start-profile-client" \
    "$ROOTFS/usr/local/libexec/android-drm/start-profile-client"
cp -f "$MODPATH/payload/debian/lindex-firefox" \
    "$ROOTFS/usr/local/libexec/android-drm/lindex-firefox"
for neofetch_payload in lindex-hardware-info lindex-neofetch lindex-neofetch.conf; do
    [ -f "$MODPATH/payload/debian/$neofetch_payload" ] || \
        abort "! Missing LinDeX Neofetch hardware adapter: $neofetch_payload"
done
neofetch_real="$ROOTFS/usr/local/libexec/android-drm/neofetch-7.1.0"
if [ -x "$ROOTFS/usr/local/bin/neofetch" ] &&
   ! grep -q '^# LinDeX Neofetch wrapper\.' "$ROOTFS/usr/local/bin/neofetch"; then
    # Preserve an existing pinned or user-provided executable behind the
    # wrapper. android-drm-install later refreshes this path to the reviewed
    # upstream 7.1.0 hash.
    cp -f "$ROOTFS/usr/local/bin/neofetch" "$neofetch_real"
    chmod 0755 "$neofetch_real"
fi
cp -f "$MODPATH/payload/debian/lindex-hardware-info" \
    "$ROOTFS/usr/local/libexec/android-drm/lindex-hardware-info"
cp -f "$MODPATH/payload/debian/lindex-neofetch.conf" \
    "$ROOTFS/usr/local/share/android-drm/neofetch.conf"
cp -f "$MODPATH/payload/debian/lindex-neofetch" \
    "$ROOTFS/usr/local/bin/neofetch"

# Android deliberately omits the consumer SoC name from ARM64 /proc/cpuinfo.
# Snapshot only public, read-only identity properties into the writable chroot;
# the helper never needs /system mounted and never sources this data as shell.
hardware_tmp="$ROOTFS/etc/lindex/hardware.conf.tmp.$$"
{
    echo 'format=1'
    printf 'product_model=%s\n' "$(getprop ro.product.model 2>/dev/null)"
    printf 'soc_manufacturer=%s\n' "$(getprop ro.soc.manufacturer 2>/dev/null)"
    printf 'soc_model=%s\n' "$(getprop ro.soc.model 2>/dev/null)"
    printf 'board_platform=%s\n' "$(getprop ro.board.platform 2>/dev/null)"
    printf 'gpu_model=%s\n' "$(cat /sys/class/kgsl/kgsl-3d0/gpu_model 2>/dev/null)"
} > "$hardware_tmp" || abort "! Cannot snapshot Android hardware identity"
chmod 0644 "$hardware_tmp"
mv -f "$hardware_tmp" "$ROOTFS/etc/lindex/hardware.conf"
ln -sfn ../libexec/android-drm/lindex-firefox \
    "$ROOTFS/usr/local/bin/firefox-esr"
ln -sfn ../libexec/android-drm/lindex-firefox \
    "$ROOTFS/usr/local/bin/firefox"
cp -f "$MODPATH/payload/debian/99-android-kgsl.sh" \
    "$ROOTFS/etc/profile.d/99-android-kgsl.sh"
cp -f "$MODPATH"/profiles/*.profile \
    "$ROOTFS/usr/local/share/android-drm/profiles/"
chmod 0755 "$ROOTFS/usr/local/sbin/android-drm-rollback" \
    "$ROOTFS/usr/local/libexec/android-drm/start-profile-client" \
    "$ROOTFS/usr/local/libexec/android-drm/lindex-firefox" \
    "$ROOTFS/usr/local/libexec/android-drm/lindex-hardware-info" \
    "$ROOTFS/usr/local/bin/neofetch"
chmod 0644 "$ROOTFS/etc/profile.d/99-android-kgsl.sh" \
    "$ROOTFS/usr/local/share/android-drm/neofetch.conf" \
    "$ROOTFS/usr/local/share/android-drm/profiles"/*.profile

if [ ! -f "$PERSIST/config.conf" ]; then
    cp "$MODPATH/config.conf" "$PERSIST/config.conf"
    chmod 0600 "$PERSIST/config.conf"
fi

set_perm_recursive "$MODPATH/bin" 0 0 0755 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/boot-completed.sh" 0 0 0755
set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

ui_print " "
ui_print "- Module staged. Reboot the device normally, then open WebUI."
ui_print "- Start automatically installs the selected stock profile and Mesa."
ui_print "- Official public Archcraft Sway dotfiles are bundled and installed automatically."
