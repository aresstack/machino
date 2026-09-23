# The camera's exported kernel symbols

`t40nn-exported-symbols.txt` is every symbol the **running** kernel on the
T40NN exports, one per line, sorted.

```
source      /proc/kallsyms on the camera, __ksymtab_* entries
captured    2026-09-23 from 192.168.1.10, with cfg80211 loaded
kernel      4.4.94, vermagic "4.4.94 SMP preempt mod_unload MIPS32_R2 32BIT"
count       5815
md5         2a692756b6b52fd2d1e6ab7f0cc756f0
```

## Why this file exists instead of a kernel build

The AIC8800 CI job has to answer one question before a `.ko` is handed to
anyone: *will every symbol this module needs be there when it is loaded?*

The obvious way is to build the kernel in CI and read `Module.symvers`. Two
things are wrong with that:

* **It answers the wrong question.** `Module.symvers` describes the kernel CI
  just built. The module is loaded into the kernel on the camera. Those are
  supposed to be the same, and the vermagic gate checks that they are — but
  the symbol list is exactly where "supposed to be" should not be relied on.

* **It does not work here.** Kernel 4.4 does not build with GCC 15: the
  `vmlinux` link fails on `-Werror=attribute-alias` in the MIPS syscall
  aliases (`sys_cacheflush`, `sys_sigaction`). Getting past that means
  switching off warnings in a kernel nobody will ever run, to obtain a list we
  already have a better copy of.

An earlier version did build `make modules` without `vmlinux`, got 249 symbols
instead of 5708, and reported `printk`, `kfree` and `memcpy` as "not exported
by this kernel". That false alarm is the reason this file exists: a symbol
gate comparing against the wrong list is worse than no gate, because it is
believable.

## Keeping it honest

The file is a snapshot and can go stale. Two guards:

* The vermagic gate in the workflow compares the module against
  `WANT_VERMAGIC`, which is the same kernel this list came from. A kernel
  change that would invalidate this list also changes vermagic, and the run
  fails there first.
* Re-capture after any kernel change:

```sh
# on the camera -- load the wireless stack FIRST, see below
modprobe cfg80211
awk '/ __ksymtab_/ { sub(/.*__ksymtab_/,""); sub(/[ \t].*/,""); print }' \
    /proc/kallsyms | sort -u > /tmp/exp.txt
wc -l /tmp/exp.txt; md5sum /tmp/exp.txt
```

and update the header above with the new count and md5.

Two traps in that one command, both hit on the first attempt:

* **`modprobe cfg80211` has to come first.** `aic8800.ko` needs 44 `cfg80211_*`
  symbols. They are not built into this kernel -- `cfg80211.ko` and
  `mac80211.ko` ship as modules under `/lib/modules/4.4.94/kernel/net/` and
  are not loaded by default. A capture taken before loading it makes the gate
  report those 44 as missing, which reads like "this board cannot do WiFi" and
  is really just load order.

* **Module symbols carry a trailing `[module]` field** in `/proc/kallsyms`,
  built-in ones do not. A `sed 's/ __ksymtab_//'` leaves it attached, so the
  entries come out as `cfg80211_connect_result\t[cfg80211]` and match nothing.
  The line count still goes up, which makes it look like it worked. Hence the
  `awk` above, which cuts at the first whitespace.

The gate still prefers a real `Module.symvers` when one is present, so a CI
setup that can build the kernel does not lose anything by this file existing.
