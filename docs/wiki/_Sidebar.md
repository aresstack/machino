## Machino

- [[Home]]
- [[Getting Started|Getting-Started]]
- [[Current C++ Runtime|Current-Runtime]]
- [[Design Principles|Design-Principles]]
- [[OpenIPC Integration|OpenIPC-Integration]]
- [[Documentation Sources|Documentation-Sources]]

## OpenIPC & Platforms

- [[T40NN OpenIPC Enablement|T40NN-OpenIPC-Enablement]]
- [[OpenIPC Patch & Upstream Catalog|OpenIPC-Patch-Catalog]]
- [[T40NN Research & Recovery|T40NN-Research-Recovery]]
- [[Platform & SDK Support|Platform-SDK-Support]]
- [[AI & Person Detection|AI-Person-Detection]]
- [[USB & Modem Connectivity|USB-Modem-Connectivity]]

## Prototype Runtime Reference

The pages in this section document the **timps-derived C validation runtime**. They preserve tested implementation knowledge and known-good Ingenic IMP behaviour, but new Machino architecture work belongs in the active C++ runtime above.

- [[Prototype Runtime Architecture|Architecture]]
- [[Configuration Reference|Configuration-Reference]]
- [[Streaming Protocols|Streaming-Protocols]]
- [[HTTP /control API|HTTP-Control-API]]
- [[Audio]]
- [[Motion Detection|Motion-Detection]]
- [[Day / Night|Day-Night]]
  - [[Day / Night Design Notes|Day-Night-Design-Notes]]
- [[Rate Control & Bandwidth|Rate-Control-Bandwidth]]
- [[Rate Control Parameters|Rate-Control-Parameters]]
- [[Recording & Timelapse|Recording-Timelapse]]
- [[Logging]]

## Development

- [[Building the C prototype|Building]]
- [[Testing / QA (prototype)|Testing-QA]]

---

Machino extends OpenIPC through small, stable integration points. Platform workarounds preserve their evidence and should become upstream fixes when responsibility belongs in OpenIPC, Linux or U-Boot.