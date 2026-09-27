#include "isp.h"

#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHash>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <linux/media.h>
#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>

// I controlli custom Xilinx stanno in <linux/xilinx-v4l2-controls.h>, header
// dell'albero kernel Xilinx che NON e' installato dai kernel-headers sanitizzati
// del toolchain. Li ridefiniamo qui con gli stessi valori.
#ifndef V4L2_CID_XILINX_CSC
#define V4L2_CID_XILINX_CSC              (V4L2_CID_USER_BASE + 0xc0a0)
#define V4L2_CID_XILINX_CSC_BRIGHTNESS   (V4L2_CID_XILINX_CSC + 1)
#define V4L2_CID_XILINX_CSC_CONTRAST     (V4L2_CID_XILINX_CSC + 2)
#define V4L2_CID_XILINX_CSC_RED_GAIN     (V4L2_CID_XILINX_CSC + 3)
#define V4L2_CID_XILINX_CSC_GREEN_GAIN   (V4L2_CID_XILINX_CSC + 4)
#define V4L2_CID_XILINX_CSC_BLUE_GAIN    (V4L2_CID_XILINX_CSC + 5)
#endif
#ifndef V4L2_CID_XILINX_GAMMA_CORR
#define V4L2_CID_XILINX_GAMMA_CORR             (V4L2_CID_USER_BASE + 0xc0c0)
#define V4L2_CID_XILINX_GAMMA_CORR_RED_GAMMA   (V4L2_CID_XILINX_GAMMA_CORR + 1)
#define V4L2_CID_XILINX_GAMMA_CORR_BLUE_GAMMA  (V4L2_CID_XILINX_GAMMA_CORR + 2)
#define V4L2_CID_XILINX_GAMMA_CORR_GREEN_GAMMA (V4L2_CID_XILINX_GAMMA_CORR + 3)
#endif

// Codici media bus (uscita demosaic = RGB 24 bit su un lane)
#ifndef MEDIA_BUS_FMT_RBG888_1X24
#define MEDIA_BUS_FMT_RBG888_1X24 0x100e
#endif
#ifndef MEDIA_BUS_FMT_SRGGB10_1X10
#define MEDIA_BUS_FMT_SRGGB10_1X10 0x300f
#endif

static const unsigned kRawFmt = MEDIA_BUS_FMT_SRGGB10_1X10;
static const unsigned kRgbFmt = MEDIA_BUS_FMT_RBG888_1X24;

// ---------------------------------------------------------------------------
static int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ::ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

// major/minor -> path del device node (/dev/v4l-subdevN, /dev/videoN)
static QString devNodeFor(unsigned major, unsigned minor) {
    const dev_t want = makedev(major, minor);
    QDir dev("/dev");
    const QStringList pats{"v4l-subdev*", "video*", "media*"};
    for (const QString &f : dev.entryList(pats, QDir::System | QDir::NoDotAndDotDot)) {
        const QString p = "/dev/" + f;
        struct stat st {};
        if (::stat(p.toLocal8Bit().constData(), &st) == 0 && S_ISCHR(st.st_mode) && st.st_rdev == want)
            return p;
    }
    return QString();
}

// ---------------------------------------------------------------------------
// Topologia: MEDIA_IOC_G_TOPOLOGY (API v2). Restituisce entity name -> devnode.
// Il legame entity<->devnode passa dai link di tipo INTERFACE_LINK:
//   interface.id --(INTERFACE_LINK)--> entity.id
static bool readTopology(const QString &mediaPath, QHash<QString, QString> &entToNode) {
    const int fd = ::open(mediaPath.toLocal8Bit().constData(), O_RDWR);
    if (fd < 0) return false;

    struct media_v2_topology topo {};
    if (xioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topo) < 0) { ::close(fd); return false; }

    std::vector<struct media_v2_entity>    ents(topo.num_entities);
    std::vector<struct media_v2_interface> ifaces(topo.num_interfaces);
    std::vector<struct media_v2_pad>       pads(topo.num_pads);
    std::vector<struct media_v2_link>      links(topo.num_links);
    if (ents.empty()) { ::close(fd); return false; }

    topo.ptr_entities   = (__u64)(uintptr_t)ents.data();
    topo.ptr_interfaces = ifaces.empty() ? 0 : (__u64)(uintptr_t)ifaces.data();
    topo.ptr_pads       = pads.empty()   ? 0 : (__u64)(uintptr_t)pads.data();
    topo.ptr_links      = links.empty()  ? 0 : (__u64)(uintptr_t)links.data();
    if (xioctl(fd, MEDIA_IOC_G_TOPOLOGY, &topo) < 0) { ::close(fd); return false; }
    ::close(fd);

    QHash<quint32, QString> idToName;
    for (const auto &e : ents) idToName.insert(e.id, QString::fromUtf8(e.name));

    QHash<quint32, const struct media_v2_interface *> idToIface;
    for (const auto &i : ifaces) idToIface.insert(i.id, &i);

    for (const auto &l : links) {
        if ((l.flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_INTERFACE_LINK) continue;
        auto it = idToIface.find(l.source_id);
        if (it == idToIface.end()) continue;
        const QString name = idToName.value(l.sink_id);
        if (name.isEmpty()) continue;
        const QString node = devNodeFor((*it)->devnode.major, (*it)->devnode.minor);
        if (!node.isEmpty()) entToNode.insert(name, node);
    }
    // entity senza interfaccia (es. i subdev interni hanno sempre un nodo, ma
    // per sicurezza registriamo comunque il nome cosi' la ricerca non fallisce)
    for (const auto &e : ents)
        if (!entToNode.contains(QString::fromUtf8(e.name)))
            entToNode.insert(QString::fromUtf8(e.name), QString());
    return true;
}

// ---------------------------------------------------------------------------
bool IspPipeline::discover(QString *err) {
    m_media.clear(); m_video.clear();
    m_sensor.clear(); m_csi.clear(); m_demosaic.clear();
    m_csc.clear(); m_gamma.clear(); m_vpss.clear(); m_sensorName.clear();

    QDir dev("/dev");
    const QStringList medias = dev.entryList(QStringList() << "media*", QDir::System | QDir::NoDotAndDotDot);
    for (const QString &m : medias) {
        const QString path = "/dev/" + m;
        QHash<QString, QString> map;
        if (!readTopology(path, map)) continue;

        bool hasVcap = false;
        for (auto it = map.begin(); it != map.end(); ++it)
            if (it.key().contains("vcap-imx219")) { hasVcap = true; break; }
        if (!hasVcap) continue;

        m_media = path;
        for (auto it = map.begin(); it != map.end(); ++it) {
            const QString &name = it.key();
            const QString &node = it.value();
            if (name.startsWith("imx219"))            { m_sensor = node; m_sensorName = name; }
            else if (name.endsWith(".csi2rx"))        m_csi = node;
            else if (name.endsWith(".demosaic"))      m_demosaic = node;
            else if (name.endsWith(".csc"))           m_csc = node;
            else if (name.endsWith(".gamma"))         m_gamma = node;
            else if (name.endsWith(".vpss"))          m_vpss = node;
            else if (name.contains("vcap-imx219") && node.startsWith("/dev/video")) m_video = node;
        }
        break;
    }

    QStringList missing;
    if (m_media.isEmpty())    missing << "media device with vcap-imx219";
    if (m_sensor.isEmpty())   missing << "imx219";
    if (m_csi.isEmpty())      missing << "*.csi2rx";
    if (m_demosaic.isEmpty()) missing << "*.demosaic";
    if (m_csc.isEmpty())      missing << "*.csc";
    if (m_gamma.isEmpty())    missing << "*.gamma";
    if (m_vpss.isEmpty())     missing << "*.vpss";
    if (m_video.isEmpty())    missing << "/dev/video* node";
    if (!missing.isEmpty()) {
        const QString msg = "incomplete video chain, missing: " + missing.join(", ");
        if (err) *err = msg;
        qWarning().noquote() << "[isp]" << msg;
        return false;
    }
    qInfo().noquote() << "[isp] media=" << m_media << "sensore=" << m_sensorName
                      << "video=" << m_video;
    return true;
}

bool IspPipeline::setPadFmt(const QString &dev, int pad, unsigned code, int w, int h) const {
    const int fd = ::open(dev.toLocal8Bit().constData(), O_RDWR);
    if (fd < 0) { qWarning() << "[isp] open" << dev << strerror(errno); return false; }
    struct v4l2_subdev_format f {};
    f.pad = pad;
    f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    f.format.width  = w;
    f.format.height = h;
    f.format.code   = code;
    f.format.field  = V4L2_FIELD_NONE;
    const bool ok = xioctl(fd, VIDIOC_SUBDEV_S_FMT, &f) == 0;
    if (!ok)
        qWarning().noquote() << "[isp] S_FMT" << dev << "pad" << pad
                             << QString::asprintf("code=0x%04x %dx%d:", code, w, h) << strerror(errno);
    ::close(fd);
    return ok;
}

bool IspPipeline::configure(const Config &cfg, QString *err) {
    if (!valid() && !discover(err)) return false;
    m_cfg = cfg;
    const int sw = cfg.sensorW, sh = cfg.sensorH;

    // Tratto comune, a risoluzione sensore. L'ordine e' quello di setup_pipeline.sh:
    // ogni link vuole formati identici sui due pad che collega.
    struct { QString dev; int pad; unsigned code; int w, h; } steps[] = {
        { m_sensor,   0, kRawFmt, sw, sh },
        { m_csi,      0, kRawFmt, sw, sh },
        { m_csi,      1, kRawFmt, sw, sh },
        { m_demosaic, 0, kRawFmt, sw, sh },
        { m_demosaic, 1, kRgbFmt, sw, sh },
        { m_csc,      0, kRgbFmt, sw, sh },
        { m_csc,      1, kRgbFmt, sw, sh },
        { m_gamma,    0, kRgbFmt, sw, sh },
        { m_gamma,    1, kRgbFmt, sw, sh },
        { m_vpss,     0, kRgbFmt, sw, sh },
        // pad1 del VPSS: qui avviene il RESIZE hardware (scaler polyphase).
        // La risoluzione di uscita e' libera, nessuna ricompilazione del bitstream.
        { m_vpss,     1, kRgbFmt, cfg.outW, cfg.outH },
    };
    for (const auto &s : steps)
        if (!setPadFmt(s.dev, s.pad, s.code, s.w, s.h)) {
            if (err) *err = QString("S_FMT failed on %1 pad %2").arg(s.dev).arg(s.pad);
            return false;
        }

    // Nodo video: e' MULTIPLANAR (Video Capture Multiplanar).
    const int fd = ::open(m_video.toLocal8Bit().constData(), O_RDWR);
    if (fd < 0) { if (err) *err = "open " + m_video; return false; }
    struct v4l2_format f {};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width       = cfg.outW;
    f.fmt.pix_mp.height      = cfg.outH;
    f.fmt.pix_mp.pixelformat = cfg.rgb ? V4L2_PIX_FMT_RGB24 : V4L2_PIX_FMT_BGR24;
    f.fmt.pix_mp.field       = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes  = 1;
    bool ok = xioctl(fd, VIDIOC_S_FMT, &f) == 0;
    if (!ok) {
        // Ripiego: alcuni kernel espongono il nodo come single-planar.
        struct v4l2_format g {};
        g.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        g.fmt.pix.width       = cfg.outW;
        g.fmt.pix.height      = cfg.outH;
        g.fmt.pix.pixelformat = cfg.rgb ? V4L2_PIX_FMT_RGB24 : V4L2_PIX_FMT_BGR24;
        g.fmt.pix.field       = V4L2_FIELD_NONE;
        ok = xioctl(fd, VIDIOC_S_FMT, &g) == 0;
    }
    if (!ok && err)
        *err = QString("S_FMT on %1 failed (%2). The %3 fourcc is listed but may "
                       "not be implemented by the bitstream.")
                   .arg(m_video, strerror(errno), cfg.rgb ? "RGB3" : "BGR3");
    ::close(fd);
    if (ok)
        qInfo().noquote() << "[isp] configurata:" << m_video
                          << (cfg.rgb ? "RGB3" : "BGR3")
                          << QString("%1x%2 (scaler VPSS da %3x%4)")
                                 .arg(cfg.outW).arg(cfg.outH).arg(sw).arg(sh);
    return ok;
}

// ---------------------------------------------------------------------------
bool IspPipeline::setCtrl(const QString &dev, unsigned id, int val) const {
    if (dev.isEmpty()) return false;
    const int fd = ::open(dev.toLocal8Bit().constData(), O_RDWR);
    if (fd < 0) return false;
    struct v4l2_control c {};
    c.id = id;
    c.value = val;
    const bool ok = xioctl(fd, VIDIOC_S_CTRL, &c) == 0;
    ::close(fd);
    return ok;
}

bool IspPipeline::getCtrl(const QString &dev, unsigned id, int &val) const {
    if (dev.isEmpty()) return false;
    const int fd = ::open(dev.toLocal8Bit().constData(), O_RDWR);
    if (fd < 0) return false;
    struct v4l2_control c {};
    c.id = id;
    const bool ok = xioctl(fd, VIDIOC_G_CTRL, &c) == 0;
    if (ok) val = c.value;
    ::close(fd);
    return ok;
}

bool IspPipeline::setWbRed(int v)      { return setCtrl(m_csc, V4L2_CID_XILINX_CSC_RED_GAIN, v); }
bool IspPipeline::setWbGreen(int v)    { return setCtrl(m_csc, V4L2_CID_XILINX_CSC_GREEN_GAIN, v); }
bool IspPipeline::setWbBlue(int v)     { return setCtrl(m_csc, V4L2_CID_XILINX_CSC_BLUE_GAIN, v); }
bool IspPipeline::setBrightness(int v) { return setCtrl(m_csc, V4L2_CID_XILINX_CSC_BRIGHTNESS, v); }
bool IspPipeline::setContrast(int v)   { return setCtrl(m_csc, V4L2_CID_XILINX_CSC_CONTRAST, v); }
bool IspPipeline::setGamma(int v) {
    bool ok = setCtrl(m_gamma, V4L2_CID_XILINX_GAMMA_CORR_RED_GAMMA, v);
    ok = setCtrl(m_gamma, V4L2_CID_XILINX_GAMMA_CORR_GREEN_GAMMA, v) && ok;
    ok = setCtrl(m_gamma, V4L2_CID_XILINX_GAMMA_CORR_BLUE_GAMMA, v) && ok;
    return ok;
}
bool IspPipeline::setExposure(int v)     { return setCtrl(m_sensor, V4L2_CID_EXPOSURE, v); }
bool IspPipeline::setAnalogueGain(int v) { return setCtrl(m_sensor, V4L2_CID_ANALOGUE_GAIN, v); }
bool IspPipeline::setDigitalGain(int v)  { return setCtrl(m_sensor, V4L2_CID_DIGITAL_GAIN, v); }

bool IspPipeline::applyCtrls(const Ctrls &c) {
    // NB: la S_FMT sulla CSC resetta i controlli colore ai default -> questa va
    // chiamata SEMPRE dopo configure(), mai prima.
    bool ok = setWbRed(c.wbRed);
    ok = setWbGreen(c.wbGreen) && ok;
    ok = setWbBlue(c.wbBlue)   && ok;
    ok = setBrightness(c.brightness) && ok;
    ok = setContrast(c.contrast)     && ok;
    ok = setGamma(c.gamma)           && ok;
    ok = setExposure(c.exposure)     && ok;
    ok = setAnalogueGain(c.analogueGain) && ok;
    ok = setDigitalGain(c.digitalGain)   && ok;
    return ok;
}

bool IspPipeline::readCtrls(Ctrls &c) const {
    bool any = false;
    any |= getCtrl(m_csc, V4L2_CID_XILINX_CSC_RED_GAIN,   c.wbRed);
    any |= getCtrl(m_csc, V4L2_CID_XILINX_CSC_GREEN_GAIN, c.wbGreen);
    any |= getCtrl(m_csc, V4L2_CID_XILINX_CSC_BLUE_GAIN,  c.wbBlue);
    any |= getCtrl(m_csc, V4L2_CID_XILINX_CSC_BRIGHTNESS, c.brightness);
    any |= getCtrl(m_csc, V4L2_CID_XILINX_CSC_CONTRAST,   c.contrast);
    any |= getCtrl(m_gamma, V4L2_CID_XILINX_GAMMA_CORR_RED_GAMMA, c.gamma);
    any |= getCtrl(m_sensor, V4L2_CID_EXPOSURE,       c.exposure);
    any |= getCtrl(m_sensor, V4L2_CID_ANALOGUE_GAIN,  c.analogueGain);
    any |= getCtrl(m_sensor, V4L2_CID_DIGITAL_GAIN,   c.digitalGain);
    return any;
}

// ============================================================================
// Gray-world AWB
// ============================================================================
GrayWorldAwb::Stats GrayWorldAwb::analyze(const unsigned char *data, int w, int h,
                                          int stride, bool bgrOrder, int step) {
    Stats s;
    if (!data || w <= 0 || h <= 0 || step <= 0) return s;
    double sr = 0, sg = 0, sb = 0;
    quint64 n = 0;
    const int iR = bgrOrder ? 2 : 0;
    const int iB = bgrOrder ? 0 : 2;
    for (int y = 0; y < h; y += step) {
        const unsigned char *row = data + (qsizetype)y * stride;
        for (int x = 0; x < w; x += step) {
            const unsigned char *p = row + (qsizetype)x * 3;
            const int r = p[iR], g = p[1], b = p[iB];
            // Scarta bruciati e neri: sono i pixel che falsano di piu' il gray-world
            // (un bianco clippato non porta informazione di colore).
            const int mx = qMax(r, qMax(g, b));
            const int mn = qMin(r, qMin(g, b));
            if (mx >= 250 || mn <= 8) continue;
            sr += r; sg += g; sb += b; ++n;
        }
    }
    if (n < 200) return s;   // troppo pochi pixel utili: scatto inutilizzabile
    s.meanR = sr / n; s.meanG = sg / n; s.meanB = sb / n;
    s.valid = s.meanR > 1.0 && s.meanG > 1.0 && s.meanB > 1.0;
    return s;
}

int GrayWorldAwb::fromMul(double m) {
    // inversa di toMul(): val = ((m*120) - 20) / 2
    const double v = ((m * 120.0) - 20.0) / 2.0;
    return qBound(IspPipeline::kGainMin, (int)std::lround(v), IspPipeline::kGainMax);
}

bool GrayWorldAwb::step(const Stats &st, int gainRIn, int gainBIn, int &gainROut, int &gainBOut) {
    if (!st.valid) return false;
    // Obiettivo gray-world: media R = media G = media B. Il verde e' il
    // riferimento (non lo tocchiamo: cosi' la luminosita' resta quella scelta
    // dall'utente e correggiamo solo la dominante).
    const double wantR = st.meanG / st.meanR;
    const double wantB = st.meanG / st.meanB;

    // Correzione parziale per passo: evita oscillazioni e pompaggio quando la
    // scena cambia (il loop e' chiuso sul frame gia' corretto dal CSC).
    const double aR = 1.0 + m_speed * (wantR - 1.0);
    const double aB = 1.0 + m_speed * (wantB - 1.0);

    gainROut = fromMul(toMul(gainRIn) * aR);
    gainBOut = fromMul(toMul(gainBIn) * aB);
    m_first = false;
    return gainROut != gainRIn || gainBOut != gainBIn;
}
