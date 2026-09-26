#include "adapters/ingenic/ingenic_platform.hpp"
#include "adapters/ingenic/detection/ivs_motion.hpp"
#include "adapters/ingenic/detection/nna_source.hpp"
#include "adapters/linux/linux_nna_process.hpp"
#include "core/detection/nna_detector.hpp"
#include "core/detection/nna_manifest.hpp"
#include "core/log.hpp"
#include "core/runtime_stats.hpp"
#include "adapters/ingenic/ingenic_encoder.hpp"
#include "adapters/ingenic/ingenic_framesource.hpp"
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace machino { namespace ingenic {
// Wo der Helfer liegt, wenn die NNA-Payload installiert ist. Eine Konstante
// statt Konfiguration: es gibt genau einen Installationsweg (install.sh).
static const char* kNnaHelperPath = "/usr/sbin/machino-nna";
}} // namespace machino::ingenic

namespace machino { namespace ingenic {

static const char* MOD = "ING_PLAT";

hw::PlatformDefaults IngenicPlatform::platform_defaults() { return hw::PlatformDefaults{}; }

IngenicPlatform::IngenicPlatform(const hw::ResolvedHardware& hw) : hw_(hw) {
    params_ok_ = sensor_params_from(hw_, params_, params_err_);
    if (!params_ok_) LOGE(MOD, "sensor parameters unusable: %s", params_err_.c_str());
}

IngenicPlatform::~IngenicPlatform() { tear_down(); }

// What has actually been established on this platform (M1-M5 hardware runs).
CapabilitySet IngenicPlatform::capabilities() const {
    CapabilitySet c;
    c.video.h264            = Cap::Supported;
    c.video.h265            = Cap::Unknown;
    c.video.max_streams     = 2;                                                                // main + sub (own IMP channels)
    c.jpeg.supported        = Cap::Supported;                                                   // hardware JPEG via a dedicated channel
    c.video.fps             = RangeCap{Cap::Supported, -1, -1, ApplyMode::PipelineRestart};   // FrameSource out rate: attr before enable
    c.video.bitrate         = RangeCap{Cap::Supported, -1, -1, ApplyMode::Live};              // IMP_Encoder_SetChnAttrRcMode
    c.video.gop             = RangeCap{Cap::Supported, 1, 1000, ApplyMode::Live};              // SetChnGopLength + read-back
    c.video.framesource_buffers = RangeCap{Cap::Supported, 1, 8, ApplyMode::PipelineRestart}; // IMPFSChnAttr::nrVBs
    c.video.encoder_buffers = RangeCap{Cap::Supported, 1, 8, ApplyMode::PipelineRestart};      // SetMaxStreamCnt before CreateChn
    c.sensor.configurable_fps = Cap::Supported;                                                // IMP_ISP_Tuning_SetSensorFPS (+GetSensorFPS read-back)
    c.sensor.fps            = RangeCap{Cap::Supported, -1, -1, ApplyMode::Live};
    c.isp.available         = Cap::Supported;
    c.encoder.hardware      = Cap::Supported;
    c.ai.available          = Cap::Supported;   // IMP_IVS analysis pipeline (motion)
    c.ai.motion             = Cap::Supported;   // IMP_IVS_CreateMoveInterface backend
    c.ai.person             = Cap::Unknown;     // venus_nna backend gebaut, aber PENDING_PHYSICAL
                                                // (AP-NNA6): Supported sagt erst, wer eine Inferenz
                                                // auf der echten NNA gesehen hat
    power_.fill_capabilities(c);
    return c;
}

Result IngenicPlatform::bring_up() {
    if (isp_) return Result::ok();
    if (!params_ok_) { LOGE(MOD, "refusing bring-up: %s", params_err_.c_str()); return Result::unsupported(); }

    memset(&info_, 0, sizeof info_);
    snprintf(info_.name, sizeof info_.name, "%s", params_.name.c_str());
    info_.cbus_type = TX_SENSOR_CONTROL_INTERFACE_I2C;
    snprintf(info_.i2c.type, sizeof info_.i2c.type, "%s", params_.name.c_str());
    info_.i2c.addr           = params_.i2c_addr;
    info_.i2c.i2c_adapter_id = params_.i2c_bus;
    info_.rst_gpio           = params_.reset_gpio;
    info_.pwdn_gpio          = params_.pwdn_gpio;
    info_.power_gpio         = params_.power_gpio;
    info_.sensor_id          = 0;
    info_.video_interface    = params_.mipi ? IMPISP_SENSOR_VI_MIPI_CSI0 : IMPISP_SENSOR_VI_DVP;
    info_.mclk               = (IMPSensorMclk)params_.mclk;
    info_.default_boot       = 0;

    // Bring-up stages, in order. Each local is RAII: an early return here
    // destroys the ones already built, in reverse, with no goto chain - and
    // each failure records WHICH stage it was, because "bring-up failed (-1)"
    // alone cost an evening of forensics once already.
    auto isp = std::make_unique<imp::IspSession>();
    if (!isp->ok()) { RuntimeStats::get().init_failed("ISP_OPEN", isp->rc()); return Result::error(isp->rc()); }
    auto sensor = std::make_unique<imp::SensorSession>(info_);
    if (!sensor->ok()) { RuntimeStats::get().init_failed("SENSOR_ENABLE", sensor->rc()); return Result::error(sensor->rc()); }
    auto system = std::make_unique<imp::SystemSession>();
    if (!system->ok()) { RuntimeStats::get().init_failed("IMP_SYSTEM_INIT", system->rc()); return Result::error(system->rc()); }
    auto tuning = std::make_unique<imp::TuningSession>();

    isp_ = std::move(isp); sensor_session_ = std::move(sensor);
    system_ = std::move(system); tuning_ = std::move(tuning);
    image_.set_active(tuning_->ok());
    char binpath[256] = {0};
    if (IMP_ISP_GetDefaultBinPath(IMPVI_MAIN, binpath) == 0 && binpath[0])
        LOGI(MOD, "ISP tuning bin (kernel default path): %s", binpath);
    else LOGW(MOD, "ISP tuning bin path not reported by the driver");
    LOGI(MOD, "up: %s i2c%d/0x%02x mclk%d rst=%d pwdn=%d %s tuning=%d", info_.name, params_.i2c_bus,
         params_.i2c_addr, params_.mclk, params_.reset_gpio, params_.pwdn_gpio,
         params_.mipi ? "mipi" : "dvp", (int)tuning_->ok());
    return Result::ok();
}

void IngenicPlatform::tear_down() {
    if (!isp_) return;
    image_.set_active(false);
    bindings_.clear();
    tuning_.reset();
    system_.reset();
    sensor_session_.reset();
    isp_.reset();
    LOGI(MOD, "down");
}

std::unique_ptr<IFrameSource> IngenicPlatform::create_framesource(int chn, const EffectiveStream& sc) {
    return IngenicFrameSource::create(chn, sc);
}

std::unique_ptr<IEncoder> IngenicPlatform::create_encoder(int chn, const EffectiveStream& sc) {
    return IngenicEncoder::create(chn, sc);
}

std::unique_ptr<IJpegEncoder> IngenicPlatform::create_jpeg(int chn, const JpegParams& p) {
    int nw = hw_.sensor.native_width  > 0 ? hw_.sensor.native_width  : hw_.mode.value.width;
    int nh = hw_.sensor.native_height > 0 ? hw_.sensor.native_height : hw_.mode.value.height;
    return IngenicJpegEncoder::create(chn, p, nw, nh);
}

namespace {

// NnaDetector nimmt seine Ports als Referenzen (die Hosttests halten die
// Attrappen auf dem Stack); hier draussen muss jemand Prozess und Quelle
// BESITZEN, solange der Detector lebt. Genau das tut dieser Umschlag --
// Reihenfolge der Member = Abbauordnung: erst der Detector (beendet den
// Helfer, stoppt die Quelle), dann Quelle und Prozess selbst.
class OwnedNnaDetector final : public IDetector {
public:
    OwnedNnaDetector(std::unique_ptr<INnaProcess> proc,
                     std::unique_ptr<IAnalysisSource> src,
                     detection::NnaDetectorConfig cfg)
        : proc_(std::move(proc)), src_(std::move(src)),
          det_(*proc_, *src_, std::move(cfg)) {}

    DetectorInput input_mode() const override { return det_.input_mode(); }
    const char*   backend()    const override { return det_.backend(); }
    Result start() override { return det_.start(); }
    Result stop()  override { return det_.stop(); }
    Result poll(detection::DetectionResult& out, int t) override { return det_.poll(out, t); }

private:
    std::unique_ptr<INnaProcess>     proc_;
    std::unique_ptr<IAnalysisSource> src_;
    detection::NnaDetector           det_;
};

} // namespace

std::unique_ptr<IDetector> IngenicPlatform::create_detector(int chn, const DetectorParams& p) {
    int nw = hw_.sensor.native_width  > 0 ? hw_.sensor.native_width  : hw_.mode.value.width;
    int nh = hw_.sensor.native_height > 0 ? hw_.sensor.native_height : hw_.mode.value.height;
    if (p.detector == "motion")
        return create_motion_detector(chn, p, nw, nh);

    if (p.detector == "person") {
        // Jede Voraussetzung einzeln und mit Grund verweigert: "unavailable"
        // ohne Warum war beim WLAN der Zeitfresser. Der Geraeteknoten zuerst --
        // ohne nmem-Bootarg und soc-nna.ko gibt es ihn nicht, und ein Helfer,
        // der dann im Backoff gegen ENODEV anrennt, waere nur Laerm.
        // Stabile Reason-Codes (AP-NNA4 §20): "unavailable" ohne Warum war der
        // verbotene Zustand. Die KI-Seite und der Log sprechen dieselben Codes.
        if (::access("/dev/soc-nna", F_OK) != 0) {
            LOGW(MOD, "person: NNA_DEVICE_MISSING - /dev/soc-nna fehlt (nmem-Bootarg gesetzt und soc-nna.ko geladen? Cam-Tool: NNA-Dialog)");
            return nullptr;
        }
        if (::access(kNnaHelperPath, X_OK) != 0) {
            LOGW(MOD, "person: NNA_RUNTIME_MISSING - %s fehlt (NNA-Payload nicht installiert)", kNnaHelperPath);
            return nullptr;
        }
        if (p.model_path.empty() || ::access(p.model_path.c_str(), R_OK) != 0) {
            LOGW(MOD, "person: AI_MODEL_MISSING - Modell '%s' nicht lesbar (ai.model_path pruefen, Seite AI)",
                 p.model_path.c_str());
            return nullptr;
        }
        // Manifest-Gate: liegt neben dem Modell ein manifest.json, MUSS es zu
        // Backend/NNA-Generation/SoC/Dateiname passen. Fehlt es, bleibt die
        // nackte .bin nutzbar (Entwicklungsmodus) -- gesagt wird es.
        {
            const size_t slash = p.model_path.find_last_of('/');
            const std::string dir =
                slash == std::string::npos ? std::string(".") : p.model_path.substr(0, slash);
            const std::string base =
                slash == std::string::npos ? p.model_path : p.model_path.substr(slash + 1);
            const std::string mpath = dir + "/manifest.json";
            if (FILE* mf = ::fopen(mpath.c_str(), "rb")) {
                std::string text;
                char buf[512];
                size_t n;
                while ((n = ::fread(buf, 1, sizeof buf, mf)) > 0 && text.size() < 65536)
                    text.append(buf, n);
                ::fclose(mf);
                const detection::NnaManifestCheck chk =
                    detection::nna_manifest_check(text, base, hw_.platform.model);
                if (!chk.ok) {
                    LOGW(MOD, "person: %s - %s (%s)", chk.reason_code.c_str(),
                         chk.detail.c_str(), mpath.c_str());
                    return nullptr;
                }
            } else {
                LOGI(MOD, "person: kein manifest.json neben dem Modell - Entwicklungsmodus, keine Kompatibilitaetspruefung");
            }
        }
        // Analysegeometrie wie beim Motion-Backend: ~640 breit, Seitenverhaeltnis
        // vom Sensor; das Letterboxing auf die Modellgeometrie macht der Helfer.
        int aw = p.source_width  > 0 ? p.source_width  : 640;
        int ah = p.source_height > 0 ? p.source_height : (nw > 0 ? (nh * 640 / nw) : 360);
        if (nw > 0 && aw > nw) aw = nw;
        if (nh > 0 && ah > nh) ah = nh;
        aw &= ~1; ah &= ~1;
        auto src = create_nna_source(chn, aw, ah, p.inference_fps, nw, nh);
        if (!src) return nullptr;
        detection::NnaDetectorConfig cfg;
        cfg.helper_path   = kNnaHelperPath;
        cfg.model_path    = p.model_path;
        cfg.frame_path    = "/tmp/machino-nna.frame";   // tmpfs; nie auf dem Overlay
        cfg.inference_fps = p.inference_fps;
        return std::make_unique<OwnedNnaDetector>(
            std::make_unique<linuxsys::LinuxNnaProcess>(), std::move(src), std::move(cfg));
    }

    LOGW(MOD, "detector backend '%s' not implemented on this platform", p.detector.c_str());
    return nullptr;
}

Result IngenicPlatform::bind(IFrameSource& fs, IEncoder& enc) {
    // One binding per encoder channel: main and sub coexist. Rebinding the same
    // channel is refused rather than leaking the previous IMP_System_Bind.
    int key = enc.channel();
    if (bindings_.count(key)) return Result::busy();
    IMPCell src = { DEV_ID_FS,  fs.channel(),  0 };
    IMPCell dst = { DEV_ID_ENC, enc.channel(), 0 };
    auto b = std::make_unique<imp::Binding>(src, dst);
    if (!b->ok()) return Result::error(b->rc());
    bindings_[key] = std::move(b);
    return Result::ok();
}

Result IngenicPlatform::unbind(IFrameSource&, IEncoder& enc) { bindings_.erase(enc.channel()); return Result::ok(); }

int64_t IngenicPlatform::timestamp_us() { return IMP_System_GetTimeStamp(); }

// Sensor frame rate via ISP tuning. Requires EnableSensor + EnableTuning
// (SDK note). The effective value is read back from the ISP, so telemetry can
// tell a real sensor-rate change from frame dropping.
Result IngenicPlatform::set_sensor_fps(int fps, int& effective) {
    effective = -1;
    if (!isp_ || !tuning_ || !tuning_->ok()) return Result::busy();
    uint32_t num = (uint32_t)fps, den = 1;
    int32_t rc = IMP_ISP_Tuning_SetSensorFPS(IMPVI_MAIN, &num, &den);
    if (rc != 0) { LOGW(MOD, "IMP_ISP_Tuning_SetSensorFPS(%d) failed (%d)", fps, (int)rc); return Result::error((int)rc); }
    uint32_t rn = 0, rd = 1;
    if (IMP_ISP_Tuning_GetSensorFPS(IMPVI_MAIN, &rn, &rd) == 0 && rd > 0) effective = (int)(rn / rd);
    LOGI(MOD, "sensor fps requested=%d effective=%d (GetSensorFPS %u/%u)", fps, effective, rn, rd);
    return Result::ok();
}

Result IngenicPlatform::get_sensor_fps(int& fps) {
    fps = -1;
    if (!isp_ || !tuning_ || !tuning_->ok()) return Result::busy();
    uint32_t rn = 0, rd = 1;
    int32_t rc = IMP_ISP_Tuning_GetSensorFPS(IMPVI_MAIN, &rn, &rd);
    if (rc != 0 || rd == 0) return Result::error((int)rc);
    fps = (int)(rn / rd);
    return Result::ok();
}

}} // namespace machino::ingenic
