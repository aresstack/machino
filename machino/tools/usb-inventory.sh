#!/bin/sh
# Read-only USB inventory for the OpenIPC T40 (AP35).
#
# Purpose: produce the SAME output before and after a device is plugged in, so
# the PENDING_PHYSICAL steps yield comparable evidence instead of prose. It
# writes nothing, loads nothing and touches no register.
#
# Usage:  sh usb-inventory.sh            (on the camera)
#         ssh root@cam 'sh -s' < usb-inventory.sh

say() { printf '%s\n' "$*"; }
kv()  { printf '  %-26s %s\n' "$1" "$2"; }

say "=== machino usb-inventory  $(date -u '+%Y-%m-%dT%H:%M:%SZ') ==="
kv "kernel" "$(uname -r)"
kv "uptime_s" "$(cut -d. -f1 /proc/uptime)"

say "--- controller ---"
kv "otg platform dev" "$(ls -d /sys/bus/platform/devices/*otg* 2>/dev/null | tr '\n' ' ')"
kv "bound drivers" "$(ls /sys/bus/platform/drivers 2>/dev/null | grep -iE 'dwc|usb_phy' | tr '\n' ' ')"
kv "gadget udc" "$([ -d /sys/class/udc ] && ls /sys/class/udc || echo 'none -> not in device mode')"
kv "irq" "$(grep -iE 'otg|dwc' /proc/interrupts | tr -s ' ' | sed 's/^ //')"

say "--- role / port (dwc2 registers, read only) ---"
if [ -r /sys/kernel/debug/13500000.otg/regdump ]; then
    for r in GOTGCTL GUSBCFG GINTSTS HPRT0; do
        v=$(awk -v k="$r" '$1==k {print $3}' /sys/kernel/debug/13500000.otg/regdump)
        kv "$r" "${v:-n/a}"
    done
    g=$(awk '$1=="GUSBCFG"{print $3}' /sys/kernel/debug/13500000.otg/regdump)
    # GUSBCFG bit29 = FORCEHSTMODE, bit30 = FORCEDEVMODE
    if [ -n "$g" ]; then
        d=$(printf '%d' "$g" 2>/dev/null)
        [ -n "$d" ] && kv "forced host mode" "$(( (d >> 29) & 1 ))"
        [ -n "$d" ] && kv "forced device mode" "$(( (d >> 30) & 1 ))"
    fi
    h=$(awk '$1=="HPRT0"{print $3}' /sys/kernel/debug/13500000.otg/regdump)
    if [ -n "$h" ]; then
        d=$(printf '%d' "$h" 2>/dev/null)
        # HPRT0 bit0 PRTCONNSTS, bit2 PRTENA, bit4 PRTOVRCURRACT, bit12 PRTPWR
        [ -n "$d" ] && kv "port power (PRTPWR)"    "$(( (d >> 12) & 1 ))"
        [ -n "$d" ] && kv "device connected"       "$(( d & 1 ))"
        [ -n "$d" ] && kv "port enabled"           "$(( (d >> 2) & 1 ))"
        [ -n "$d" ] && kv "overcurrent"            "$(( (d >> 4) & 1 ))"
    fi
else
    kv "regdump" "not available (debugfs not mounted?)"
fi

say "--- vbus gpio ---"
if [ -r /sys/kernel/debug/gpio ]; then
    grep -i drvvbus /sys/kernel/debug/gpio | sed 's/^/  /' || kv "drvvbus" "not claimed by any driver"
else
    kv "gpio debugfs" "not readable"
fi
kv "dt drvvbus prop" "$([ -e /proc/device-tree/apb/otg_phy/ingenic,drvvbus-gpio ] && echo present || echo absent)"

say "--- enumerated devices ---"
for d in /sys/bus/usb/devices/*; do
    [ -e "$d/idVendor" ] || continue
    printf '  %-10s %s:%s  %s %s\n' "$(basename "$d")" \
        "$(cat "$d/idVendor")" "$(cat "$d/idProduct")" \
        "$(cat "$d/manufacturer" 2>/dev/null)" "$(cat "$d/product" 2>/dev/null)"
done
[ -n "$(ls /sys/bus/usb/devices 2>/dev/null)" ] || kv "(none)" ""
kv "root hub ports" "$(cat /sys/bus/usb/devices/usb1/maxchild 2>/dev/null)"
kv "interfaces bound" "$(ls -d /sys/bus/usb/devices/*:* 2>/dev/null | wc -l)"

say "--- class drivers available ---"
kv "built-in (usb bus)" "$(ls /sys/bus/usb/drivers 2>/dev/null | tr '\n' ' ')"
kv "usb-serial bus" "$([ -d /sys/bus/usb-serial/drivers ] && ls /sys/bus/usb-serial/drivers | tr '\n' ' ' || echo 'absent')"
for m in usbnet cdc_ether cdc_acm rndis_host cdc_ncm option usb_wwan qcserial usb-storage; do
    f=$(find /lib/modules/"$(uname -r)"/kernel -name "$m.ko" 2>/dev/null | head -1)
    kv "module $m" "${f:-MISSING}"
done

say "--- network ---"
kv "interfaces" "$(ls /sys/class/net | tr '\n' ' ')"
kv "80211 stack" "$(find /lib/modules/"$(uname -r)"/kernel/net -name 'mac80211.ko' -o -name 'cfg80211.ko' 2>/dev/null | wc -l) of 2 modules"
kv "wireless device drivers" "$(find /lib/modules/"$(uname -r)"/kernel/drivers/net/wireless -name '*.ko' 2>/dev/null | wc -l)"
say "=== end ==="
