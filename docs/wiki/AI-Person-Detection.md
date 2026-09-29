# AI & Person Detection (T40 NNA)

Machino runs person/object detection on the Ingenic **NNA** (neural network
accelerator, generation `nna1` on the T40/T40N/T40NN), not on the MIPS CPU.
The motion detector (`motion`) is the ISP's IVS engine and needs none of this.

This page is the operator's view. The engineering record lives in the repo:
[nna.md](https://github.com/aresstack/machino/blob/main/machino/docs/architecture/nna.md)
(architecture decision), [nna-evidence.md](https://github.com/aresstack/machino/blob/main/machino/docs/architecture/nna-evidence.md)
(what was measured on stock firmware), [nna-model.md](https://github.com/aresstack/machino/blob/main/machino/docs/architecture/nna-model.md)
(model build and packaging).

## What has to be in place

| Layer | What | Where it comes from | Page shows it as |
| --- | --- | --- | --- |
| Boot | `nmem=8M@0x7800000` on the kernel command line | Cam-Tool, NNA dialog (writes U-Boot `bootargs`, reboot required) | *NNA memory reserved (nmem)* |
| Kernel | `soc-nna.ko` loaded, `/dev/soc-nna` present | Ships with the OpenIPC image; `/etc/init.d/machino` loads it at boot **when** nmem is set | *Driver loaded*, *Device node* |
| Helper | `/usr/sbin/machino-nna` (static uClibc binary, Venus runtime) | `install.sh --with-nna-payload` (Cam-Tool checkbox) or the CI artifact `machino-nna-t40` | *Inference helper installed* |
| Model | `/etc/machino/models/<model>.bin` + `manifest.json` | Upload on the AI page, Cam-Tool, or scp | *Models on camera* |
| Config | `ai.model_path`, `ai.detector=person`, `ai.enabled` | AI page buttons (they call `PATCH /api/v1/config`) | *Detectors* card |

`GET /api/v1/ai/detectors` lists every detector with `available` and the
complete reason list when it is not. The codes are stable:

```
NNA_BOOT_MEMORY_MISSING        no nmem= on this boot
NNA_DEVICE_MISSING             /dev/soc-nna absent (driver not loaded)
NNA_RUNTIME_MISSING            /usr/sbin/machino-nna missing or not executable
AI_MODEL_MISSING               ai.model_path unset or file unreadable
AI_MANIFEST_INVALID            manifest.json unreadable / wrong schemaVersion
AI_MODEL_INCOMPATIBLE_BACKEND  manifest backend != venus-nna
AI_MODEL_INCOMPATIBLE_NNA      manifest nnaGeneration != nna1
AI_MODEL_INCOMPATIBLE_SOC      manifest soc != this camera's SoC
AI_MODEL_FILE_MISMATCH         manifest modelFile != the file ai.model_path names
```

## Getting a model onto the camera

1. Open **AI** in the camera's web UI.
2. Check the *Storage* card: a model needs free overlay space. The stock
   `majestic` backup (~2.9 MB) can be deleted there if the board's majestic is
   the broken one (T40NN).
3. **Upload model bundle**: a `.tgz` holding the model `.bin` and its
   `manifest.json` (optionally `provenance.txt`). The upload is refused when
   the manifest names another backend/NNA generation, when paths inside the
   archive are unsafe, or when the overlay is full (HTTP 507).
4. Press **Use** next to the file. This sets `ai.model_path` through
   machino's API; a running person detector restarts with the new file.
5. In the *Detectors* card press **Select + enable** on *Person (NNA)*.
   The card shows `state`, backend and the last error live.

The CI workflow [build-nna-t40](https://github.com/aresstack/machino/actions/workflows/build-nna-t40.yml)
publishes an upload-ready bundle as the artifact `machino-nna-model-bundle`
(a `.tgz`), next to the helper `machino-nna-t40` and the raw model
directory `machino-nna-model`.

## Building your own model with the Ingenic SDK

The NNA executes **Magik** models: a network exported to ONNX (or
TensorFlow/TFLite), quantized and serialised for the T40 by Ingenic's
**TransformKit**. The runtime is Ingenic's **Venus** library from the
same toolkit (`InferenceKit/nna1`), statically linked into `machino-nna`.
Model and runtime must come from the same toolkit revision; Venus checks
the model version at load time.

```sh
git clone https://github.com/wispytrace/magik-toolkit
git -C magik-toolkit checkout e511d370dd7ff84664c9140e0590c354947c7eac   # pinned, same as CI
cd magik-toolkit/Models/post/yolov5s
../../../TransformKit/magik-transform-tools \
    --inputpath  yolov5s.onnx \
    --outputpath ./yolov5s_t40_magik.mk.h \
    --config     cfg/magik_t40.cfg \
    --save_quantize_model true
# -> yolov5s_t40_magik.bin  (SOC=T40, input 1x3x640x640 RGB, NORMAL 255, INT8)
```

TransformKit is an x86-64 Linux binary. `cfg/magik_t40.cfg` names the SoC,
the input shape and colour layout, the normalisation and the calibration
image set (`yolov5-20`, in the toolkit); a different network needs its own
config and calibration images.

Then write `manifest.json` next to the `.bin`:

```json
{
  "schemaVersion": 1,
  "id": "my-person-model",
  "task": "object-detection",
  "backend": "venus-nna",
  "nnaGeneration": "nna1",
  "soc": "t40nn",
  "modelFile": "yolov5s_t40_magik.bin",
  "input": { "width": 640, "height": 640, "format": "RGB", "layout": "NCHW", "range": "0..1 (NORMAL 255)" },
  "classes": [ { "id": 0, "label": "person" } ],
  "confidenceThreshold": 0.30,
  "nmsThreshold": 0.60
}
```

`soc` may be left out (then any SoC), everything else that is set must match.
Bundle and upload:

```sh
tar czf my-person-model-t40.tgz yolov5s_t40_magik.bin manifest.json
```

The helper decodes the three YOLOv5 heads and runs NMS itself (Venus
`generate_box`), so the model has to be YOLOv5-shaped; it reads the input
geometry from the model, so other resolutions need no code change.

### Which network

| Model | Size on flash | Licence | Status |
| --- | --- | --- | --- |
| `yolov5s` from the toolkit | 7.6 MB (7.2M INT8 weights) | Ultralytics YOLOv5 weights, AGPL-3.0 | development only; larger than the whole overlay, keep it in `/tmp` for tests |
| `persondet` trained with `Models/training/pytorch/Txx_Xs2/persondet` | 1–2M parameters | your own weights | the shippable path, fits the overlay |

The toolkit itself carries no LICENSE file; Machino ships nothing taken
from the stock firmware (`ivs_detect.bin`, `libants_*`, stock `libvenus.so`
are reference only).

## Not yet verified on hardware

The whole chain above is built and host-tested, but the first real
inference on a camera (nmem set, driver loaded, helper running, Venus
accepting the stock driver version `20190724a`, RAM headroom under
WebRTC load) is still pending. Until then `ai.person` stays *unknown* in
`/api/v1/capabilities`; the person detector can still be selected and
reports its state and reason codes honestly.
