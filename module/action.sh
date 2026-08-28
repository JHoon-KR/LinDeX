#!/system/bin/sh
MODDIR=${0%/*}
"$MODDIR/bin/debian-gpu-control" status
echo
echo "WebUI: open this module with KernelSU/APatch, MMRL, or WebUI X."
echo "CLI: su -c debian-gpu-control"

