# Documentation Sources

Machino knowledge is spread across several repositories for good reasons: production
runtime code, hardware reverse engineering and modem research have different owners and
different update cycles.

The Machino Wiki is the **curated system-level view**. It should not become a copy of
every experiment, dump or vendor PDF.

## Ownership model

| Source | Owns | What the Machino Wiki should do |
| --- | --- | --- |
| [`aresstack/machino`](https://github.com/aresstack/machino) | Machino runtime, OpenIPC integration, reproducible target builds, canonical wiki | Describe supported architecture, interfaces, capabilities and verified operational behaviour |
| [`Miguel0888/ipcam-lan-discovery`](https://github.com/Miguel0888/ipcam-lan-discovery) | Camera discovery, firmware/boot analysis, recovery research, executable OpenIPC/T40NN patches | Promote stable hardware findings and upstream candidates; link back to detailed evidence and executable patches |
| [`Miguel0888/quectel-ec200a-eu`](https://github.com/Miguel0888/quectel-ec200a-eu) | EC200A-EU research, vendor documents, USB/network-mode experiments, host implementations | Promote the modem facts required by the camera integration; keep vendor material and broad modem research in the modem repository |

## What belongs in the Wiki

A fact belongs here when it is stable enough to guide implementation, operation or
future upstream work. Typical examples:

- why Machino exists and how it relates to OpenIPC/Majestic;
- architectural constraints and extension points;
- verified T40NN/YT8512B register state;
- known cold-boot/warm-reboot differences that affect platform support;
- capability/API contracts;
- the supported USB/modem integration model;
- reproducible target build requirements;
- links to the experiment or code that proves a hardware conclusion.

The wiki should answer **"what do we know and how does it fit into the system?"**

## What should remain in engineering repositories

Keep raw or rapidly changing evidence close to the code/research that produced it:

- complete UART/boot logs;
- firmware extraction notes;
- flash dumps/backups;
- temporary hypotheses;
- one-off register experiments;
- screenshots used during analysis;
- test scripts and executable patches;
- incident/debugging timelines;
- large binary reference files.

Those sources answer **"how did we prove this?"**

## `ipcam-lan-discovery` source map

The camera tool is the main evidence repository for board- and firmware-level findings.
Its current documentation includes:

| Source document | Primary value to Machino/OpenIPC | Wiki destination |
| --- | --- | --- |
| `docs/advanced-hardware-audit.md` | board/SoC/peripheral evidence | platform enablement pages as findings become stable |
| `docs/firmware-analyse.md` | stock firmware structure and behaviour | platform/recovery references, not copied wholesale |
| `docs/flash-backup.md` | backup/recovery procedure and evidence | recovery prerequisite for risky hardware work |
| `docs/openipc-flashen.md` | OpenIPC migration/flash research | installation/recovery documentation where Machino actually depends on it |
| `docs/root-zugang.md` | stock-firmware access research | evidence/reference only unless required by a supported workflow |
| `docs/kamera-verhalten.md` | observed stock-camera behaviour | comparison evidence for runtime/platform work |
| `docs/nna-boot-environment.md` | NNA-related boot/environment findings | [AI & Person Detection](AI-Person-Detection.md) / platform support |
| `docs/t40n-ethernet-analysis.md` | verified T40NN/YT8512B Ethernet root-cause work | [T40NN OpenIPC Enablement](T40NN-OpenIPC-Enablement.md) |
| `tool/src/core/known_patches.cpp` | executable, gated OpenIPC repair patches | referenced from the corresponding platform pages |
| screenshots / camera subfolders | visual/raw evidence | link selectively; do not mirror by default |

The Cam Tool remains the owner of the executable patch logic. Machino should document
the stable contract and the upstream relevance rather than duplicating the same patch
implementation in prose and code.

## `quectel-ec200a-eu` source map

The modem repository is broader than the camera use case. It contains both vendor
reference documents and original engineering work.

Relevant categories include:

| Source | Value | Wiki treatment |
| --- | --- | --- |
| `doc/linux.md` | Linux USB serial/usbnet modes and driver mapping | curate supported embedded-camera path in [USB & Modem Connectivity](USB-Modem-Connectivity.md) |
| `doc/PLATTFORM-RUNTIME.md` | runtime/platform separation research | use as architecture input where applicable |
| `doc/ecm-host-plan.md` | ECM host design work | reference when implementing/validating USB-network mode |
| `doc/STAND-2026-09-09.md` and research notes | dated project evidence | keep as research history, promote only verified/current conclusions |
| Quectel hardware/USB-driver PDFs | manufacturer primary sources | link/reference; do not duplicate into Machino wiki |
| host/ESP experiments | cross-platform proof and limitations | import only conclusions relevant to the OpenIPC camera |

A dated research note is evidence, not automatically the current specification. Wiki
pages should identify the supported state and link to dated evidence when history
matters.

## Machino repository source map

The Machino repository owns the integration contract and the reproducible embedded
payloads.

Important sources include:

- `README.md` — public project direction and production architecture blueprint;
- `docs/wiki/` — canonical curated GitHub Wiki source;
- `.github/workflows/build-modem-modules-t40.yml` — reproducible Linux 4.4.94/OpenIPC
  T40 USB/modem kernel-module build and ABI validation;
- `.github/workflows/build-ppp-t40.yml` — minimal optional `pppd`/`chat` target payload;
- runtime code and tests — executable truth for supported media/API behaviour.

When wiki prose disagrees with a verified test or current implementation, fix the
prose or explicitly document the transition. Do not silently preserve an obsolete
architecture merely because it existed in the inherited timps documentation.

## Promotion workflow: evidence -> curated knowledge -> upstream

A useful lifecycle for platform findings is:

```text
experiment / dump / patch
          |
          v
verified finding in owning repository
          |
          v
curated Machino wiki statement
          |
          +--> Machino implementation, when it is a Machino responsibility
          |
          +--> OpenIPC / kernel / U-Boot upstream candidate, when responsibility lies there
```

This avoids two failure modes:

1. **Wiki as notebook:** hundreds of transient observations make it impossible to know
   what is actually supported.
2. **Patch without knowledge:** a magic register/script remains forever because nobody
   preserved why it exists.

## Vendor documents and licensing

Vendor PDFs remain in the repository where they were collected and retain their
original copyright/licensing terms. Machino documentation should summarize the small
parts necessary for interoperability or implementation and link to the original source
rather than republishing a second copy as wiki content.

The same principle applies to third-party SDK documentation and binary libraries.
Machino's MIT lineage does not relicense third-party material.

## Canonical Wiki workflow

`docs/wiki/` in `aresstack/machino` is the source of truth. The GitHub Action mirrors
that directory to the GitHub Wiki repository, including deletions.

The sync must remain **deterministic**. CI may validate structure, links or generated
indexes, but it should not ask an LLM to rewrite documentation during a build. Curated
prose changes belong in normal commits and code review.

This provides the same architectural property we want from Machino itself: automation
at a narrow, predictable boundary instead of hidden behaviour spread across the
system.
