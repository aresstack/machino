# nowait.ko - the causal test for the T40NN hardlock

Context: `docs/ap17-hardlock.md`. After every watchdog reset the boot log
prints where each CPU stood when the hardware fired. On every hardlock so far:

```
CPU0 RESET ERROR PC:8001AC08   = ingenic_wait_irqoff+0x24   (MIPS WAIT idle)
CPU1 RESET ERROR PC:55xxxxxx   user space, PIE process, varies
```

and after a clean `reboot` the rebooting CPU stands in `__delay` instead. So
the reading is diagnostic: CPU0 is in WAIT when the box dies, every time.

This module sets the exported `cpu_wait` pointer to NULL at runtime, which is
what the `nowait` kernel parameter does at boot. The idle loop then polls
with interrupts enabled and never enters WAIT. No bootloader write, no kernel
image, reversible with `rmmod` or `echo 0 > /sys/module/nowait/parameters/enable`.

## Test protocol (A/B/A)

```
1. insmod /tmp/nowait.ko        dmesg: "cpu_wait was ingenic_wait_irqoff, now NULL"
2. idle 35 min, first access    repeat over as many rounds as the idle runner
                                needs for the arm to count (see ap17)
3. rmmod nowait  (or reboot)    WAIT is back; the hang must return
```

| outcome | reading |
|---|---|
| no kill with the module loaded, kills return without it | the WAIT/wake-up path is the cause |
| kills continue with the module loaded | the idle path is not it; look at the interrupt controller and bus |
| `insmod` refuses | vermagic or symbol mismatch; the CI gates should have caught it |

`cpu_wait` is in `t40nn-exported-symbols.txt` (captured from the camera's
`/proc/kallsyms`), so the symbol gate passes by construction; the CI run still
proves it against the real list.

Cost while loaded: the idle CPU never sleeps. Warmer, more current, fine for a
test. Do not ship it as a fix - if it works, the fix is in the kernel's idle or
interrupt code, and this module only names the place.

Built by `.github/workflows/build-nowait-t40.yml`; the artifact is `nowait-t40`.
