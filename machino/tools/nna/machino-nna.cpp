// machino-nna: der Venus/NNA-Inferenzhelfer.
//
// Ein EIGENES uClibc-Binary, kein Teil von machinod -- die Begruendung steht
// in docs/architecture/nna.md (musl/uClibc-Grenze, Crash-Isolation,
// Optionalitaet). machinod spricht das Zeilenprotokoll aus
// core/detection/nna_detector.hpp: "frame ..." rein, "result/det ..." raus,
// "ready" nach dem Modell-Laden, "error <text>" wenn etwas nicht geht.
//
// Der Inferenzpfad (Letterbox-Resize NV12->RGBA, generate_box, Klassen-NMS,
// Koordinatenruecktransformation) ist aus Ingenics venus_sample_yolov5s
// (magik-toolkit, inference_nv12.cpp) uebernommen und auf das Protokoll
// umgebaut: das Sample ist Ingenics dokumentierter Weg, YOLOv5-Ausgaben der
// nna1 auszuwerten, und eine "eigene" Nachbildung davon waere dieselben
// Zahlen mit neuen Fehlern. Anchors/Strides sind YOLOv5 -- das Modell unter
// --model MUSS ein TransformKit-serialisiertes YOLOv5-artiges sein (das
// persondet-Training im selben Toolkit erzeugt genau das).
//
// Build: tools/nna/Makefile (Ingenic mips-linux-gnu-g++ -muclibc, statisch
// gegen libvenus.a aus InferenceKit/nna1). NICHT mit der musl-Toolchain
// bauen -- genau die Grenze, um die es hier geht.
#include "venus.h"

#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory>
#include <string>
#include <vector>

using namespace magik::venus;

// COCO-Namen fuer die Klassen, die auf einer Kamera etwas bedeuten; alles
// andere bleibt "class<N>", ehrlich statt geraten.
static const char* class_label(int id) {
    switch (id) {
        case 0:  return "person";
        case 1:  return "bicycle";
        case 2:  return "car";
        case 3:  return "motorcycle";
        case 5:  return "bus";
        case 7:  return "truck";
        case 14: return "bird";
        case 15: return "cat";
        case 16: return "dog";
        default: return nullptr;
    }
}

struct PixelOffset { int top, bottom, left, right; };

static void manyclass_nms(std::vector<ObjBbox_t>& input, std::vector<ObjBbox_t>& output,
                          int classnums, float nms_threshold) {
    const int box_num = (int)input.size();
    std::vector<int> merged(box_num, 0);
    std::vector<ObjBbox_t> classbuf;
    for (int clsid = 0; clsid < classnums; ++clsid) {
        classbuf.clear();
        for (int i = 0; i < box_num; ++i) {
            if (merged[i] || clsid != input[i].class_id) continue;
            classbuf.push_back(input[i]);
            merged[i] = 1;
        }
        nms(classbuf, output, nms_threshold, NmsType::HARD_NMS);
    }
}

int main(int argc, char** argv) {
    // Die NNA-Vorlast gehoert nicht auf den Kern, der das Video traegt --
    // dieselbe Affinitaet wie im Ingenic-Sample.
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(0, &mask);
    sched_setaffinity(0, sizeof(mask), &mask);

    std::string model;
    float score_threshold = 0.30f, nms_threshold = 0.60f;
    for (int i = 1; i + 1 < argc; i += 2) {
        if      (!strcmp(argv[i], "--model"))  model = argv[i + 1];
        else if (!strcmp(argv[i], "--score"))  score_threshold = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--nms"))    nms_threshold   = (float)atof(argv[i + 1]);
        // --width/--height kommen von machinod, gebraucht wird beides nicht:
        // jede frame-Zeile traegt ihre Geometrie selbst.
    }
    if (model.empty()) { printf("error no --model given\n"); fflush(stdout); return 1; }

    if (venus_init() != 0) { printf("error venus_init failed (nmem? /dev/soc-nna?)\n"); fflush(stdout); return 1; }

    std::unique_ptr<BaseNet> net = net_create(TensorFormat::NHWC);
    if (!net || net->load_model(model.c_str()) != 0) {
        printf("error load_model failed: %s\n", model.c_str()); fflush(stdout);
        venus_deinit();
        return 1;
    }

    // Modellgeometrie aus dem Modell selbst; das Sample tippte sie ab.
    int in_w = 640, in_h = 384;
    {
        std::unique_ptr<Tensor> in0 = net->get_input(0);
        if (in0) {
            shape_t s = in0->shape();
            if (s.size() >= 3 && s[1] > 0 && s[2] > 0) { in_h = s[1]; in_w = s[2]; }
        }
    }

    printf("ready venus in=%dx%d model=%s\n", in_w, in_h, model.c_str());
    fflush(stdout);

    std::vector<unsigned char> nv12;
    char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        if (!strncmp(line, "quit", 4)) break;
        long long pts = 0; int w = 0, h = 0, stride = 0; long sz = 0; char path[512] = {0};
        if (sscanf(line, "frame %lld %d %d %d %ld %511s", &pts, &w, &h, &stride, &sz, path) != 6 ||
            w <= 0 || h <= 0 || sz <= 0) {
            printf("error bad frame line\n"); fflush(stdout);
            continue;
        }
        nv12.resize((size_t)sz);
        FILE* f = fopen(path, "rb");
        if (!f || fread(nv12.data(), 1, (size_t)sz, f) != (size_t)sz) {
            if (f) fclose(f);
            printf("error frame file unreadable: %s\n", path); fflush(stdout);
            continue;
        }
        fclose(f);

        // Letterbox NV12 -> RGBA in den Eingabetensor (Ingenics common_resize).
        std::unique_ptr<Tensor> input = net->get_input(0);
        input->reshape({1, in_h, in_w, 4});
        Tensor ori({1, h, w, 1}, TensorFormat::NV12);
        memcopy((void*)ori.mudata<uint8_t>(), (void*)nv12.data(),
                (int)((size_t)w * (size_t)h * 3 / 2) * (int)sizeof(uint8_t));

        const float sx = (float)in_w / (float)w, sy = (float)in_h / (float)h;
        const float scale = sx < sy ? sx : sy;
        int vw = (int)(scale * w); if (vw & 1) ++vw;
        int vh = (int)(scale * h); if (vh & 1) ++vh;
        PixelOffset off;
        off.top    = (int)lround((double)(in_h - vh) / 2 - 0.1);
        off.bottom = (int)lround((double)(in_h - vh) / 2 + 0.1);
        off.left   = (int)lround((double)(in_w - vw) / 2 - 0.1);
        off.right  = (int)lround((double)(in_w - vw) / 2 + 0.1);

        BsCommonParam prm;
        prm.pad_val = 0;
        prm.pad_type = BsPadType::SYMMETRY;
        prm.input_height = h;
        prm.input_width = w;
        prm.input_line_stride = stride > 0 ? stride : w;
        prm.in_layout  = ChannelLayout::NV12;
        prm.out_layout = ChannelLayout::RGBA;
        common_resize((const void*)ori.mudata<uint8_t>(), *input.get(),
                      AddressLocate::NMEM_VIRTUAL, &prm);

        if (net->run() != 0) {
            printf("error net run failed\n"); fflush(stdout);
            continue;
        }

        // Drei YOLO-Koepfe einsammeln (Kopien: die Ausgabetensoren gehoeren dem Netz).
        std::vector<Tensor> heads;
        for (int oi = 0; oi < 3; ++oi) {
            std::unique_ptr<const Tensor> o = net->get_output(oi);
            if (!o) break;
            shape_t s = o->shape();
            int n = 1; for (size_t k = 0; k < s.size(); ++k) n *= s[k];
            Tensor t(s);
            memcopy((void*)t.mudata<float>(), (void*)o->data<float>(), n * (int)sizeof(float));
            heads.push_back(t);
        }

        std::vector<ObjBbox_t> raw, boxes;
        std::vector<float> strides_v = {8.f, 16.f, 32.f};
        std::vector<float> anchors = {10,13, 16,30, 33,23, 30,61, 62,45, 59,119, 116,90, 156,198, 373,326};
        const int classes = 80;
        generate_box(heads, strides_v, anchors, raw, in_w, in_h, classes, 3,
                     score_threshold, DetectorType::YOLOV5);
        manyclass_nms(raw, boxes, classes, nms_threshold);

        // Zurueck in Originalkoordinaten, dann normalisiert [0,1] --
        // machinod kennt weder Modell- noch Analysegeometrie.
        printf("result %lld %d\n", pts, (int)boxes.size());
        for (size_t i = 0; i < boxes.size(); ++i) {
            ObjBbox_t& b = boxes[i];
            float x0 = (b.box.x0 - off.left) / scale, x1 = (b.box.x1 - off.left) / scale;
            float y0 = (b.box.y0 - off.top)  / scale, y1 = (b.box.y1 - off.top)  / scale;
            if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
            if (x1 > (float)w) x1 = (float)w; if (y1 > (float)h) y1 = (float)h;
            float nx = x0 / (float)w, ny = y0 / (float)h;
            float nw = (x1 - x0) / (float)w, nh = (y1 - y0) / (float)h;
            if (nw <= 0.f || nh <= 0.f) { nx = ny = 0.f; nw = nh = 0.001f; }
            int conf = (int)lround((double)b.score * 100.0);
            if (conf < 0) conf = 0; if (conf > 100) conf = 100;
            const char* lbl = class_label(b.class_id);
            char lblbuf[32];
            if (!lbl) { snprintf(lblbuf, sizeof lblbuf, "class%d", b.class_id); lbl = lblbuf; }
            printf("det %d %d %.4f %.4f %.4f %.4f %s\n", b.class_id, conf, nx, ny, nw, nh, lbl);
        }
        fflush(stdout);
    }

    venus_deinit();
    return 0;
}
