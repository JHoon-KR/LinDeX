#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
helper=$repo_root/module/payload/debian/lindex-hardware-info
wrapper=$repo_root/module/payload/debian/lindex-neofetch
adapter=$repo_root/module/payload/debian/lindex-neofetch.conf
installer=$repo_root/module/payload/debian/android-drm-install
customize=$repo_root/module/customize.sh

sh -n "$helper"
sh -n "$wrapper"
bash -n "$adapter"
sh -n "$installer"
sh -n "$customize"

fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
helper_exec=$fixture/lindex-hardware-info
wrapper_exec=$fixture/lindex-neofetch
cp "$helper" "$helper_exec"
cp "$wrapper" "$wrapper_exec"
chmod 0755 "$helper_exec" "$wrapper_exec"
mkdir -p "$fixture/etc/lindex" \
    "$fixture/sys/class/kgsl/kgsl-3d0" \
    "$fixture/sys/devices/system/cpu/cpu0/cpufreq" \
    "$fixture/sys/devices/system/cpu/cpu6/cpufreq" \
    "$fixture/xdg/neofetch"
printf '%s\n' \
    'format=1' \
    'product_model=SM-S937N' \
    'soc_manufacturer=QTI' \
    'soc_model=SM8750' \
    'board_platform=sun' \
    'gpu_model=Adreno830v2' > "$fixture/etc/lindex/hardware.conf"
printf '%s\n' 'Architecture: aarch64' 'Model name: Oryon' > "$fixture/lscpu"
printf '%s\n' Adreno830v2 > "$fixture/sys/class/kgsl/kgsl-3d0/gpu_model"
printf '%s\n' 3532800 > \
    "$fixture/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"
printf '%s\n' 4473600 > \
    "$fixture/sys/devices/system/cpu/cpu6/cpufreq/cpuinfo_max_freq"
printf '%s\n' 0-7 > "$fixture/sys/devices/system/cpu/online"

run_helper() {
    env LINDEX_HARDWARE_FILE="$fixture/etc/lindex/hardware.conf" \
        LINDEX_SYS_ROOT="$fixture" LINDEX_LSCPU_FILE="$fixture/lscpu" \
        "$helper_exec" "$1"
}

[[ $(run_helper cpu) == 'Qualcomm SM8750 (Oryon)' ]]
[[ $(run_helper gpu) == 'Qualcomm Adreno 830 v2' ]]
[[ $(run_helper cpu-max-khz) == 4473600 ]]
[[ $(run_helper cpu-cores) == 8 ]]

adapter_output=$(
    env LINDEX_HARDWARE_FILE="$fixture/etc/lindex/hardware.conf" \
        LINDEX_SYS_ROOT="$fixture" LINDEX_LSCPU_FILE="$fixture/lscpu" \
        LINDEX_HARDWARE_INFO_BIN="$helper_exec" \
        LINDEX_NEOFETCH_EXPLICIT_CONFIG=1 XDG_CONFIG_HOME="$fixture/xdg" \
        bash -c '
            cpu_brand=on
            cpu_cores=logical
            cpu_speed=on
            speed_shorthand=off
            gpu_brand=on
            subtitle=GPU:
            gpu_name=
            prin() { printf "GPU=%s\n" "$2"; }
            source "$1"
            get_cpu
            printf "CPU=%s\n" "$cpu"
            get_gpu
        ' bash "$adapter"
)
grep -qx 'CPU=Qualcomm SM8750 (Oryon) (8) @ 4.473GHz' <<< "$adapter_output"
grep -qx 'GPU=Qualcomm Adreno 830 v2' <<< "$adapter_output"

# The wrapper must preserve arbitrary Neofetch arguments, load the adapter
# before CLI overrides, and leave the reviewed upstream executable separate.
cat > "$fixture/neofetch-real" <<'EOF'
#!/bin/sh
printf '%s\n' "$@"
EOF
chmod 0755 "$fixture/neofetch-real"
wrapper_output=$(
    LINDEX_NEOFETCH_REAL="$fixture/neofetch-real" \
    LINDEX_NEOFETCH_CONFIG="$adapter" "$wrapper_exec" --stdout --cpu_speed off
)
expected_wrapper=$(printf '%s\n' --config "$adapter" --stdout --cpu_speed off)
[[ $wrapper_output == "$expected_wrapper" ]]

grep -q 'real_file=/usr/local/libexec/android-drm/neofetch-7.1.0' "$installer"
grep -q 'getprop ro.soc.model' "$customize"
grep -q '/sys/class/kgsl/kgsl-3d0/gpu_model' "$customize"

echo 'Neofetch hardware identity tests passed'
