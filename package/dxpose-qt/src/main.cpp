// ============================================================================
// dxpose-qt - Visualizzatore Qt6/GStreamer per la catena IMX219 + DX-M1.
//
//  - barra informazioni e controlli sulla DESTRA (video a sinistra)
//  - pipeline media (demosaic/CSC/gamma/VPSS) configurata NATIVAMENTE via
//    ioctl: nessuna dipendenza da setup_pipeline.sh
//  - selezione MANUALE del modello (nessun ciclo demo)
//  - fallback su file video quando la camera non e' disponibile
//  - controlli ISP live (WB, gamma, brightness, contrast, esposizione, gain)
//  - auto white balance gray-world attivabile
//  - telemetria: INA226 (correnti) + DX-M1 (tensioni/clock/temperature)
//
// Il thread UI non esegue MAI operazioni GStreamer/NPU: tutto sul worker.
// ============================================================================
#include "isp.h"
#include "npuinfo.h"
#include "pipelineview.h"
#include "fwupdate.h"

#include <QAbstractSocket>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QNetworkInterface>
#include <QFontMetrics>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QGuiApplication>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <unistd.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

// ============================ util sysfs / rami di alimentazione ============
static double readSysfs(const QString &path, bool *ok = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) { if (ok) *ok = false; return 0.0; }
    bool conv = false;
    const double v = QString::fromLatin1(f.readAll()).trimmed().toDouble(&conv);
    if (ok) *ok = conv;
    return v;
}

struct Rail {
    QString dir;              // /sys/class/hwmon/hwmonX (INA226/INA231: stessa interfaccia)
    double offsetMa = 0.0;    // perdite scheda da sottrarre
    bool read(double &volt, double &milliA, double &watt) const {
        if (dir.isEmpty()) return false;
        bool okV = false, okC = false, okP = false;
        const double mv = readSysfs(dir + "/in1_input", &okV);
        const double ma = readSysfs(dir + "/curr1_input", &okC);
        const double uw = readSysfs(dir + "/power1_input", &okP);
        if (!okV || !okC) return false;
        volt = mv / 1000.0;
        milliA = qMax(0.0, ma - offsetMa);
        watt = (offsetMa != 0.0 || !okP) ? (volt * milliA / 1000.0) : (uw / 1.0e6);
        return true;
    }
};

static QString hwmonName(const QString &d) {
    QFile f(d + "/name");
    return f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
}
static QString hwmonLink(const QString &d) { return QFileInfo(d + "/device").symLinkTarget(); }

static QStringList findPowerMonitors() {
    QStringList out;
    QDir base("/sys/class/hwmon");
    for (const QString &e : base.entryList(QStringList() << "hwmon*", QDir::Dirs | QDir::NoDotAndDotDot)) {
        const QString dir = base.absoluteFilePath(e);
        const QString n = hwmonName(dir);
        if (n == "ina231" || n == "ina226") out << dir;
    }
    std::sort(out.begin(), out.end(),
              [](const QString &a, const QString &b) { return hwmonLink(a) < hwmonLink(b); });
    return out;
}
static QString resolveRail(const QString &hint, const QStringList &found) {
    if (hint.isEmpty()) return QString();
    if (hint.startsWith('/')) return hint;
    for (const QString &d : found)
        if (hwmonLink(d).contains(hint, Qt::CaseInsensitive) || d.endsWith("/" + hint)) return d;
    return QString();
}

static bool activeIPv4(QString &ipOut) {
    for (const auto &itf : QNetworkInterface::allInterfaces()) {
        const auto fl = itf.flags();
        if (!(fl & QNetworkInterface::IsUp) || !(fl & QNetworkInterface::IsRunning)) continue;
        if (fl & QNetworkInterface::IsLoopBack) continue;
        for (const auto &e : itf.addressEntries()) {
            const QHostAddress a = e.ip();
            if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback()) continue;
            if ((a.toIPv4Address() & 0xFFFF0000u) == 0xA9FE0000u) continue;  // 169.254/16
            ipOut = a.toString();
            return true;
        }
    }
    ipOut.clear();
    return false;
}

// ============================ widget di supporto ============================
class NetworkIndicator : public QWidget {
public:
    explicit NetworkIndicator(QWidget *p = nullptr) : QWidget(p) { setFixedSize(28, 20); }
    void setConnected(bool c) { if (c != m_c) { m_c = c; update(); } }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter g(this);
        g.setRenderHint(QPainter::Antialiasing);
        const QColor on(0x5c, 0xd0, 0x8a), off(0xc2, 0x66, 0x6e);
        g.setPen(Qt::NoPen);
        g.setBrush(m_c ? on : off);
        for (int i = 0; i < 4; ++i) {
            const int h = 5 + i * 4;
            g.drawRoundedRect(QRect(i * 7, height() - h, 5, h), 1.5, 1.5);
        }
    }
private:
    bool m_c = false;
};

static QLabel *caption(const QString &t) {
    auto *l = new QLabel(t.toUpper());
    l->setObjectName("cap");
    return l;
}

// ============================ vista video ============================
class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setAutoFillBackground(true);
        QPalette p = palette();
        p.setColor(QPalette::Window, Qt::black);
        setPalette(p);
        setMinimumSize(160, 90);   // dimensioni gestite dallo StageWidget
        connect(&m_spin, &QTimer::timeout, this, [this] { m_angle = (m_angle + 12) % 360; update(); });
    }
    // Avviso centrato sopra il video (aggiornamento firmware ecc.).
    enum class Notice { None, Busy, Done, Error };
    void setNotice(Notice kind, const QString &title, const QString &text,
                   const QString &detail = QString()) {
        m_nKind = kind; m_nTitle = title; m_nText = text; m_nDetail = detail;
        if (kind == Notice::Busy) m_spin.start(40); else m_spin.stop();
        update();
    }
    void setNoticeDetail(const QString &d) { m_nDetail = d; update(); }

    // Copia dell'ultimo frame (thread UI): usata dall'AWB.
    QImage currentFrame() const { return m_img; }
    qint64 lastFrameMs() const { return m_lastFrameMs.load(); }
    void markFrame() { m_lastFrameMs.store(QDateTime::currentMSecsSinceEpoch()); }

public slots:
    void setFrame(const QImage &img) {
        m_img = img;
        m_lastFrameMs.store(QDateTime::currentMSecsSinceEpoch());
        update();
    }
protected:
    void mousePressEvent(QMouseEvent *) override {
        // l'avviso di errore si chiude con un tocco; quello di power cycle no
        if (m_nKind == Notice::Error) setNotice(Notice::None, {}, {});
    }
    void resizeEvent(QResizeEvent *) override { update(); }
    void paintEvent(QPaintEvent *) override {
        QPainter g(this);
        g.fillRect(rect(), Qt::black);
        if (m_img.isNull()) {
            if (m_nKind == Notice::None) {
                g.setPen(QColor(0x6b, 0x7a, 0x88));
                g.drawText(rect(), Qt::AlignCenter, "waiting for frames...");
            }
            paintNotice(g);
            return;
        }
        // Fit mantenendo l'aspetto: la scena e' 16:9 ma l'area puo' non esserlo.
        const QSize s = m_img.size().scaled(size(), Qt::KeepAspectRatio);
        const QRect target(QPoint((width() - s.width()) / 2, (height() - s.height()) / 2), s);
        g.setRenderHint(QPainter::SmoothPixmapTransform, s.width() < m_img.width());
        g.drawImage(target, m_img);
        paintNotice(g);
    }
    void paintNotice(QPainter &g) {
        if (m_nKind == Notice::None) return;
        g.setRenderHint(QPainter::Antialiasing);
        g.fillRect(rect(), QColor(0, 0, 0, 150));   // attenua il video sotto
        const QColor acc = m_nKind == Notice::Error ? QColor(0xe0, 0x5a, 0x5a)
                         : m_nKind == Notice::Done  ? QColor(0x5c, 0xd0, 0x8a)
                                                    : QColor(0xe0, 0xb2, 0x5a);
        const int bw = qMin(760, width() - 40), bh = qMin(300, height() - 40);
        const QRectF box((width() - bw) / 2.0, (height() - bh) / 2.0, bw, bh);
        g.setPen(QPen(acc, 2));
        g.setBrush(QColor(0x11, 0x15, 0x1a, 240));
        g.drawRoundedRect(box, 12, 12);

        qreal y = box.top() + 26;
        if (m_nKind == Notice::Busy) {           // spinner
            const QRectF sp(box.center().x() - 20, y, 40, 40);
            g.setPen(QPen(QColor(0x28, 0x30, 0x3a), 5));
            g.drawEllipse(sp);
            g.setPen(QPen(acc, 5, Qt::SolidLine, Qt::RoundCap));
            g.drawArc(sp, -m_angle * 16, 100 * 16);
            y += 56;
        } else if (m_nKind == Notice::Done) {    // simbolo di accensione
            const QRectF ic(box.center().x() - 20, y, 40, 40);
            g.setPen(QPen(acc, 4, Qt::SolidLine, Qt::RoundCap));
            g.setBrush(Qt::NoBrush);
            g.drawArc(ic, 120 * 16, 300 * 16);
            g.drawLine(QPointF(ic.center().x(), ic.top() - 3), QPointF(ic.center().x(), ic.center().y()));
            y += 56;
        } else {                                 // errore
            QFont fi = font(); fi.setPixelSize(40); fi.setBold(true);
            g.setFont(fi); g.setPen(acc);
            g.drawText(QRectF(box.left(), y, bw, 44), Qt::AlignCenter, QStringLiteral("!"));
            y += 56;
        }
        QFont ft = font(); ft.setPixelSize(24); ft.setBold(true);
        g.setFont(ft); g.setPen(QColor(0xee, 0xf4, 0xf8));
        g.drawText(QRectF(box.left() + 20, y, bw - 40, 32), Qt::AlignCenter, m_nTitle);
        y += 40;
        QFont fb = font(); fb.setPixelSize(17);
        g.setFont(fb); g.setPen(QColor(0xc9, 0xd6, 0xdf));
        const QRectF tr(box.left() + 30, y, bw - 60, box.bottom() - y - 34);
        g.drawText(tr, Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, m_nText);
        if (!m_nDetail.isEmpty()) {
            QFont fd = font(); fd.setPixelSize(12);
            g.setFont(fd); g.setPen(QColor(0x8b, 0x97, 0xa3));
            g.drawText(QRectF(box.left() + 20, box.bottom() - 28, bw - 40, 20), Qt::AlignCenter,
                       QFontMetrics(fd).elidedText(m_nDetail, Qt::ElideMiddle, bw - 40));
        }
    }
private:
    QImage m_img;
    std::atomic<qint64> m_lastFrameMs{0};
    Notice m_nKind = Notice::None;
    QString m_nTitle, m_nText, m_nDetail;
    QTimer m_spin;
    int m_angle = 0;
};

// ============================ modelli disponibili ============================
struct ModelInfo {
    QString name;       // "YoloV5S_PPU"
    QString path;       // /home/ked/models/YoloV5S_PPU.dxnn
    QString cfgDir;     // vuoto -> modalita' inline
    bool tracking = false;
    QString label() const {
        return name + (cfgDir.isEmpty() ? "  (inline)" : "  (config)") + (tracking ? " +tracker" : "");
    }
};
// Passa per connessioni queued verso il worker: va dichiarato come metatype.
Q_DECLARE_METATYPE(ModelInfo)

// Cerca i .dxnn e associa a ciascuno il modo di postprocess:
//  - esiste <cfgRoot>/<nome>/preprocess_config.json  -> catena a config-file
//  - altrimenti                                       -> inline, function-name=<nome>
// E' la stessa regola di test_pipeline.sh. Usare i config di un modello con un
// altro modello fa abortire dxpostprocess (vettore fuori range) appena rileva.
static QList<ModelInfo> scanModels(const QString &modelsDir, const QString &cfgRoot) {
    QList<ModelInfo> out;
    QDir d(modelsDir);
    for (const QString &f : d.entryList(QStringList() << "*.dxnn", QDir::Files, QDir::Name)) {
        ModelInfo m;
        m.name = QFileInfo(f).completeBaseName();
        m.path = d.absoluteFilePath(f);
        const QString c = cfgRoot + "/" + m.name;
        if (QFileInfo::exists(c + "/preprocess_config.json")) m.cfgDir = c;
        out << m;
    }
    return out;
}

// ============================ controller GStreamer ============================
static GstFlowReturn onNewSample(GstAppSink *sink, gpointer user_data);

class GstPipelineController : public QObject {
    Q_OBJECT
public:
    struct Config {
        QString device;        // /dev/videoN (vuoto -> fallback file)
        QString ppLib, trkCfg, fallbackDir;
        // Video di fallback preferito (installato dal pacchetto). Se manca si
        // ripiega sul primo video trovato in fallbackDir.
        QString fallbackFile;
        // Motivo per cui la camera non e' disponibile gia' all'avvio (mostrato
        // nel pannello SOURCE). Vuoto = camera configurata correttamente.
        QString cameraError;
        // Se non vuoto il NPU NON va usato (es. firmware troppo vecchio: dx_rt
        // lancia un'eccezione non gestita dentro dxinfer e il processo abortisce).
        QString npuBlock;
        int width = 1280, height = 720, fps = 30;
        bool rgb = false;      // il nodo cattura RGB3 (true) o BGR3 (false)
        bool exitOnFault = false;
    };
    GstPipelineController(VideoWidget *view, Config cfg) : m_view(view), m_cfg(std::move(cfg)) {
        m_npu = NpuInfo::devicePresent() && m_cfg.npuBlock.isEmpty();
        if (!m_cfg.npuBlock.isEmpty())
            qWarning().noquote() << "[dxpose] DX-M1 DISABILITATO:" << m_cfg.npuBlock << "-> passthrough";
        else
            qInfo().noquote() << "[dxpose] DX-M1:" << (m_npu ? "rilevato" : "ASSENTE -> passthrough");
    }
    ~GstPipelineController() override { destroy(); }
    GstElement *activeSink() const { return m_activeSink.load(); }
    VideoWidget *view() const { return m_view; }
    qint64 heartbeat() const { return m_heartbeat.load(); }

signals:
    void sourceChanged(bool usingCamera, const QString &info);
    void pipelineText(const QString &launch);   // stringa gst-launch in uso
    // Tempi del DX-M1 in ms (media mobile): calcolo puro NPU e latenza totale
    // (con i trasferimenti PCIe). Negativi = non disponibili.
    void npuTiming(double npuMs, double latencyMs);
    void modelChanged(const QString &name);
    void errorOccurred(const QString &msg);
    // Sorgente non riproducibile (elemento GStreamer mancante, nessun video di
    // fallback, pipeline non costruibile): mostrato come avviso sul video.
    void sourceFailed(const QString &title, const QString &msg);
    // Per il sinottico: stato della catena a ogni ricostruzione.
    void pipelineState(bool usingCamera, bool npu, const QString &fourcc,
                       int outW, int outH, const QString &model);

public slots:
    void begin() {
        m_heartbeat.store(QDateTime::currentMSecsSinceEpoch());
        m_busTimer = new QTimer(this);
        connect(m_busTimer, &QTimer::timeout, this, &GstPipelineController::pollBus);
        m_busTimer->start(50);
        m_watchdog = new QTimer(this);
        connect(m_watchdog, &QTimer::timeout, this, &GstPipelineController::watchdog);
        m_watchdog->start(750);
        m_timingTimer = new QTimer(this);
        connect(m_timingTimer, &QTimer::timeout, this, &GstPipelineController::pollTiming);
        m_timingTimer->start(500);
        if (!m_pending.name.isEmpty()) rebuild();
    }
    // Cambio modello MANUALE: distrugge la pipeline e ne costruisce una nuova.
    // Sincrono sul worker: il modello vecchio e' liberato dal NPU PRIMA di
    // caricare il nuovo (evita l'esaurimento degli slot sul dispositivo).
    void setModel(const ModelInfo &m) { m_pending = m; rebuild(); }
    void stop() { destroy(); }
    // Ritenta la camera (pulsante "Retry camera"). 'device' e' il nodo video
    // eventualmente appena riconfigurato dall'ISP nel thread UI.
    void retryCamera(const QString &device, const QString &cameraError) {
        m_cfg.device = device;
        m_cfg.cameraError = cameraError;
        m_forceFallback = false;
        m_fallbackReason.clear();
        qInfo().noquote() << "[dxpose] retry camera su" << (device.isEmpty() ? "(nessun device)" : device);
        rebuild();
    }

private:
    void destroy() {
        m_activeSink.store(nullptr);
        if (!m_pipeline) return;
        GstElement *pl = m_pipeline; GstElement *sk = m_sink; GstBus *bs = m_bus;
        m_pipeline = nullptr; m_sink = nullptr; m_bus = nullptr;
        // set_state(NULL) e' sincrono e puo' impuntarsi se il thread di streaming
        // e' dentro una chiamata al NPU: lo smontaggio va su un thread staccato.
        std::thread([pl, sk, bs] {
            gst_element_set_state(pl, GST_STATE_NULL);
            if (bs) gst_object_unref(bs);
            if (sk) gst_object_unref(sk);
            gst_object_unref(pl);
        }).detach();
    }

    QString dxChain(const ModelInfo &m, const QString &sync) const {
        if (!m_npu)
            return QString(" ! videoconvert ! video/x-raw,format=RGB"
                           " ! appsink name=sink max-buffers=1 drop=true sync=%1").arg(sync);
        if (m.cfgDir.isEmpty()) {
            // inline: function-name = nome del modello (YOLOV5Pose_PPU, SCRFD500M_PPU, ...)
            return QString(
                " ! dxpreprocess preprocess-id=1 resize-width=640 resize-height=640 ! queue max-size-buffers=1"
                " ! dxinfer name=infer preprocess-id=1 inference-id=1 model-path=%1 ! queue max-size-buffers=1"
                " ! dxpostprocess inference-id=1 library-file-path=%2 function-name=%3 ! queue max-size-buffers=1"
                " ! dxosd ! videoconvert ! video/x-raw,format=RGB"
                " ! appsink name=sink max-buffers=1 drop=true sync=%4")
                .arg(m.path, m_cfg.ppLib, m.name, sync);
        }
        const QString trk = (m.tracking && !m_cfg.trkCfg.isEmpty())
                                ? QString(" ! dxtracker config-file-path=%1 ! queue").arg(m_cfg.trkCfg)
                                : QString();
        // model-path DOPO config-file-path sovrascrive il model_path (relativo) del JSON
        return QString(
            " ! dxpreprocess config-file-path=%1/preprocess_config.json ! queue"
            " ! dxinfer name=infer config-file-path=%1/inference_config.json model-path=%2 ! queue"
            " ! dxpostprocess config-file-path=%1/postprocess_config.json ! queue"
            "%3"
            " ! dxosd ! queue ! videoconvert ! video/x-raw,format=RGB"
            " ! appsink name=sink max-buffers=1 drop=true sync=%4")
            .arg(m.cfgDir, m.path, trk, sync);
    }

    QString pickFallbackFile() const {
        if (!m_cfg.fallbackFile.isEmpty() && QFileInfo::exists(m_cfg.fallbackFile))
            return m_cfg.fallbackFile;
        QDir d(m_cfg.fallbackDir);
        const QStringList pats{"*.y4m", "*.mp4", "*.mov", "*.mkv", "*.avi", "*.webm"};
        const QStringList f = d.entryList(pats, QDir::Files, QDir::Name);
        return f.isEmpty() ? QString() : d.absoluteFilePath(f.first());
    }

    QString buildPipeline(const ModelInfo &m, bool &cam, QString &info) {
        cam = !m_forceFallback && !m_cfg.device.isEmpty() && QFileInfo::exists(m_cfg.device);
        QString src;
        if (cam) {
            info = QString("%1  %2x%3  %4").arg(m_cfg.device).arg(m_cfg.width).arg(m_cfg.height)
                       .arg(m_cfg.rgb ? "RGB3" : "BGR3");
            // io-mode=mmap: il frmbuf non fa da esportatore dmabuf per v4l2src.
            // dxpreprocess accetta solo { RGB, I420, NV12 }: con BGR3 serve lo swap
            // R<->B. Costa poco ora che i buffer V4L2 sono cacheable (dma-coherent
            // sul nodo vcap del DT); senza quello sarebbe ~3,5 fps.
            // name=camsrc: serve a riconoscere sul bus gli errori della camera
            // (e distinguerli da quelli del NPU, che hanno gestione diversa).
            src = QString("v4l2src name=camsrc device=%1 io-mode=mmap"
                          " ! video/x-raw,format=%2,width=%3,height=%4,framerate=%5/1")
                      .arg(m_cfg.device, m_cfg.rgb ? "RGB" : "BGR")
                      .arg(m_cfg.width).arg(m_cfg.height).arg(m_cfg.fps);
            // Con BGR3 (frmbuf senza HAS_RGB8) resta SOLO lo scambio dei byte R<->B:
            // resize e conversione di spazio colore li ha gia' fatti il VPSS.
            // Con --rgb (frmbuf con HAS_RGB8 -> RGB3) questo elemento sparisce.
            if (!m_cfg.rgb) src += " ! videoconvert n-threads=2 ! video/x-raw,format=RGB";
        } else {
            const QString f = pickFallbackFile();
            if (f.isEmpty()) {
                qCritical().noquote() << "[dxpose] nessuna camera e nessun video di fallback ("
                                      << m_cfg.fallbackFile << "," << m_cfg.fallbackDir << ")";
                emit sourceFailed("No video source",
                                  QString("The camera is not available and no fallback video was found.\n"
                                          "Expected: %1").arg(m_cfg.fallbackFile));
                return QString();
            }
            // Motivo del fallback: errore runtime della camera, oppure camera
            // assente/non configurabile gia' all'avvio.
            QString why = m_fallbackReason;
            if (why.isEmpty()) why = m_cfg.cameraError.isEmpty()
                                         ? QStringLiteral("camera not available")
                                         : m_cfg.cameraError;
            info = QFileInfo(f).fileName() + "\n" + why;
            qWarning().noquote() << "[dxpose] FALLBACK su file" << f << "-" << why;
            const QString dec = (QFileInfo(f).suffix().toLower() == "y4m") ? "y4mdec" : "decodebin";
            // y4mdec sta in gst-plugins-bad (plugin "y4m"): se manca, gst_parse_launch
            // costruirebbe una pipeline monca senza fotogrammi. Meglio dirlo subito.
            if (GstElementFactory *ef = gst_element_factory_find(dec.toUtf8().constData())) {
                gst_object_unref(ef);
            } else {
                qCritical().noquote() << "[dxpose] elemento GStreamer mancante:" << dec;
                emit sourceFailed("Fallback video cannot be played",
                                  QString("GStreamer element '%1' is not installed in the image "
                                          "(Buildroot: BR2_PACKAGE_GST1_PLUGINS_BAD_PLUGIN_Y4M).\n"
                                          "File: %2").arg(dec, f));
                return QString();
            }
            src = QString("filesrc location=%1 ! %2 ! videoconvert ! video/x-raw,format=RGB").arg(f, dec);
        }
        if (!m_npu) info += m_cfg.npuBlock.isEmpty() ? "  [no NPU]" : "  [NPU disabled]";
        return src + dxChain(m, cam ? "false" : "true");
    }

    void rebuild() {
        destroy();
        if (m_pending.name.isEmpty()) return;
        bool cam = false;
        QString info;
        const QString ps = buildPipeline(m_pending, cam, info);
        if (ps.isEmpty()) { m_givenUp = true; return; }
        qInfo().noquote() << "[dxpose] pipeline:\n" << ps;
        GError *err = nullptr;
        GstElement *pl = gst_parse_launch(ps.toUtf8().constData(), &err);
        // gst_parse_launch puo' restituire una pipeline NON nulla insieme a un
        // errore "recuperabile" (es. elemento inesistente): va trattata come fallita.
        if (pl && err) {
            gst_object_unref(pl);
            pl = nullptr;
        }
        if (!pl) {
            const QString msg = err ? QString::fromUtf8(err->message) : "errore sconosciuto";
            if (err) g_error_free(err);
            qCritical().noquote() << "[dxpose] gst_parse_launch:" << msg;
            emit errorOccurred(msg);
            if (cam) switchToFallback("camera pipeline error: " + msg);
            else emit sourceFailed("Fallback video cannot be played", msg);
            return;
        }
        if (err) g_error_free(err);
        m_pipeline = pl;
        m_sink = gst_bin_get_by_name(GST_BIN(pl), "sink");
        GstAppSinkCallbacks cbs;
        std::memset(&cbs, 0, sizeof(cbs));
        cbs.new_sample = onNewSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(m_sink), &cbs, this, nullptr);
        m_bus = gst_element_get_bus(pl);
        m_usingCamera = cam;
        m_loop = !cam;
        m_segmentArmed = false;
        m_activeSink.store(m_sink);
        m_startedMs = QDateTime::currentMSecsSinceEpoch();
        if (m_view) m_view->markFrame();
        if (gst_element_set_state(pl, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            // Con la camera non si sa ancora se il colpevole e' la sorgente o il
            // NPU: si prova il file. Se fallisce anche quello, allora e' il NPU
            // (o il modello) e si passa alla gestione fatale.
            if (cam) {
                qCritical().noquote() << "[dxpose] avvio pipeline camera FALLITO -> provo il file";
                switchToFallback("camera pipeline failed to start");
                return;
            }
            qCritical().noquote() << "[dxpose] avvio pipeline FALLITO (modello/NPU non inizializzato)";
            onFatalDeviceError();
            return;
        }
        m_failed = 0;
        m_givenUp = false;
        emit modelChanged(m_pending.name);
        emit sourceChanged(cam, info);
        emit pipelineText(ps);
        m_npuAvg = m_latAvg = -1.0;
        emit npuTiming(-1, -1);
        emit pipelineState(cam, m_npu, m_cfg.rgb ? "RGB3" : "BGR3",
                           m_cfg.width, m_cfg.height, m_pending.name);
    }

    // Loop del file senza EOS: il segment seek fa emettere SEGMENT_DONE, cosi' si
    // ri-aggancia il segmento da 0 senza ricostruire (il modello resta caricato).
    void armSegment() {
        gst_element_seek(m_pipeline, 1.0, GST_FORMAT_TIME,
                         (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_SEGMENT),
                         GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    }
    void continueSegment() {
        gst_element_seek(m_pipeline, 1.0, GST_FORMAT_TIME, GST_SEEK_FLAG_SEGMENT,
                         GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
    }
    void switchToFallback(const QString &reason) {
        if (m_forceFallback) return;          // gia' in fallback: niente loop
        m_forceFallback = true;
        m_fallbackReason = reason;
        qWarning().noquote() << "[dxpose] camera ->" << reason << "-> passo al video di fallback";
        rebuild();
    }
    void onFatalDeviceError() {
        m_givenUp = true;
        if (m_cfg.exitOnFault) {
            qWarning().noquote() << "[dxpose] errore fatale dx_rt -> esco (systemd riavviera')";
            ::_exit(1);
        }
    }

private slots:
    void pollBus() {
        m_heartbeat.store(QDateTime::currentMSecsSinceEpoch());
        if (!m_bus) return;
        GstMessage *msg;
        while ((msg = gst_bus_pop(m_bus)) != nullptr) {
            switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError *e = nullptr; gchar *dbg = nullptr;
                gst_message_parse_error(msg, &e, &dbg);
                const QString m = e ? QString::fromUtf8(e->message) : "?";
                const QString d = dbg ? QString::fromUtf8(dbg) : QString();
                const QString srcName = GST_MESSAGE_SRC(msg)
                                            ? QString::fromUtf8(GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)))
                                            : QString();
                qCritical().noquote() << "[dxpose] GST ERROR da" << srcName << ":" << m << "|" << d;
                if (e) g_error_free(e);
                g_free(dbg);
                emit errorOccurred(m);
                const QString blob = m + " " + d;
                const bool npuErr = blob.contains("InferenceEngine") ||
                                    blob.contains("dxrt", Qt::CaseInsensitive) ||
                                    blob.contains("dxinfer", Qt::CaseInsensitive);
                if (npuErr) {
                    onFatalDeviceError();
                } else if (!m_usingCamera) {
                    emit sourceFailed("Fallback video error", m);
                } else if (m_usingCamera) {
                    // Qualunque altro errore mentre si usa la camera (v4l2src,
                    // not-negotiated, STREAMON, buffer pool...) -> video di fallback.
                    // La ricostruzione distrugge m_bus: si esce subito dal ciclo.
                    gst_message_unref(msg);
                    switchToFallback("camera error: " + m);
                    return;
                }
                break;
            }
            case GST_MESSAGE_ASYNC_DONE:
                if (m_loop && !m_segmentArmed) {
                    m_segmentArmed = true;
                    armSegment();
                    m_startedMs = QDateTime::currentMSecsSinceEpoch();
                    if (m_view) m_view->markFrame();
                }
                break;
            case GST_MESSAGE_SEGMENT_DONE:
                if (m_loop) {
                    continueSegment();
                    m_startedMs = QDateTime::currentMSecsSinceEpoch();
                    if (m_view) m_view->markFrame();
                }
                break;
            case GST_MESSAGE_EOS:
                if (m_loop) { m_segmentArmed = true; armSegment(); }
                break;
            default: break;
            }
            gst_message_unref(msg);
        }
    }
    // Watchdog.
    //  - camera: niente ricostruzioni in loop (ricaricherebbero il modello sul
    //    NPU); se la camera non produce frame -> si passa al video di fallback.
    //  - file:   stallo -> ricostruzione, con limite di tentativi.
    void watchdog() {
        if (!m_pipeline || m_givenUp) return;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (m_usingCamera) {
            const qint64 lf = m_view ? m_view->lastFrameMs() : now;
            const bool got = lf > m_startedMs + 50;
            if (got && now - lf > kCamStallMs) {
                switchToFallback(QString("camera stalled (no frames for %1 s)").arg(kCamStallMs / 1000));
            } else if (!got && now - m_startedMs > kCamFirstFrameMs && !isAsync()) {
                switchToFallback(QString("no frames from camera after %1 s").arg(kCamFirstFrameMs / 1000));
            }
            return;
        }
        const qint64 lf = m_view ? m_view->lastFrameMs() : now;
        const bool got = lf > m_startedMs + 50;
        const bool stalled = got ? (now - lf > kStallMs)
                                 : (now - m_startedMs > kLoadTimeoutMs && !isAsync());
        if (!stalled) { if (got) m_failed = 0; return; }
        if (now - m_lastRestartMs < kCooldownMs) return;
        if (++m_failed > kMaxFailed) {
            m_givenUp = true;
            qWarning().noquote() << "[dxpose] watchdog: troppi riavvii falliti, mi fermo";
            if (m_cfg.exitOnFault) ::_exit(1);
            return;
        }
        m_lastRestartMs = now;
        qWarning().noquote() << "[dxpose] watchdog: stallo -> ricostruisco";
        rebuild();
    }
    // Legge le proprieta' esposte dalla patch 0001 di dx-stream. Se dx-stream non
    // e' patchato le proprieta' non esistono: nessun dato, nessun errore.
    void pollTiming() {
        if (!m_pipeline || !m_npu) return;
        GstElement *inf = gst_bin_get_by_name(GST_BIN(m_pipeline), "infer");
        if (!inf) return;
        GObjectClass *kl = G_OBJECT_GET_CLASS(inf);
        if (g_object_class_find_property(kl, "last-npu-time-us") &&
            g_object_class_find_property(kl, "last-latency-us")) {
            gint npu = 0, lat = 0;
            g_object_get(inf, "last-npu-time-us", &npu, "last-latency-us", &lat, nullptr);
            if (npu > 0 || lat > 0) {
                auto ema = [](double avg, double v) { return avg < 0 ? v : avg * 0.7 + v * 0.3; };
                m_npuAvg = ema(m_npuAvg, npu / 1000.0);
                m_latAvg = ema(m_latAvg, lat / 1000.0);
                emit npuTiming(m_npuAvg, m_latAvg);
            }
        }
        gst_object_unref(inf);
    }
    bool isAsync() {
        if (!m_pipeline) return false;
        GstState st, pend;
        return gst_element_get_state(m_pipeline, &st, &pend, 0) == GST_STATE_CHANGE_ASYNC;
    }

private:
    VideoWidget *m_view;
    Config m_cfg;
    ModelInfo m_pending;
    GstElement *m_pipeline = nullptr, *m_sink = nullptr;
    GstBus *m_bus = nullptr;
    std::atomic<GstElement *> m_activeSink{nullptr};
    QTimer *m_busTimer = nullptr, *m_watchdog = nullptr, *m_timingTimer = nullptr;
    double m_npuAvg = -1.0, m_latAvg = -1.0;
    bool m_npu = true, m_usingCamera = false, m_loop = false, m_segmentArmed = false, m_givenUp = false;
    bool m_forceFallback = false;   // errore camera a runtime: resta sul file fino a "Retry camera"
    QString m_fallbackReason;
    qint64 m_startedMs = 0, m_lastRestartMs = 0;
    int m_failed = 0;
    std::atomic<qint64> m_heartbeat{0};
    static constexpr qint64 kStallMs = 2500, kLoadTimeoutMs = 20000, kCooldownMs = 3000;
    // Camera: il primo frame arriva solo dopo il caricamento del modello sul
    // NPU (alcuni secondi), quindi la soglia iniziale e' ampia.
    static constexpr qint64 kCamStallMs = 4000, kCamFirstFrameMs = 20000;
    static constexpr int kMaxFailed = 4;
};

static GstFlowReturn onNewSample(GstAppSink *sink, gpointer user_data) {
    auto *self = static_cast<GstPipelineController *>(user_data);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    if (GST_ELEMENT_CAST(sink) != self->activeSink()) { gst_sample_unref(sample); return GST_FLOW_OK; }
    GstCaps *caps = gst_sample_get_caps(sample);
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstVideoInfo vi;
    if (caps && buf && gst_video_info_from_caps(&vi, caps)) {
        GstVideoFrame fr;
        if (gst_video_frame_map(&fr, &vi, buf, GST_MAP_READ)) {
            QImage img((const uchar *)GST_VIDEO_FRAME_PLANE_DATA(&fr, 0),
                       GST_VIDEO_FRAME_WIDTH(&fr), GST_VIDEO_FRAME_HEIGHT(&fr),
                       GST_VIDEO_FRAME_PLANE_STRIDE(&fr, 0), QImage::Format_RGB888);
            QImage copy = img.copy();
            gst_video_frame_unmap(&fr);
            if (VideoWidget *v = self->view())
                QMetaObject::invokeMethod(v, "setFrame", Qt::QueuedConnection, Q_ARG(QImage, copy));
        }
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// ============================ slider etichettato ============================
class CtrlSlider : public QWidget {
    Q_OBJECT
public:
    CtrlSlider(const QString &name, int min, int max, int val, QWidget *parent = nullptr)
        : QWidget(parent) {
        auto *g = new QGridLayout(this);
        g->setContentsMargins(0, 0, 0, 0);
        g->setSpacing(2);
        m_name = new QLabel(name);
        m_name->setObjectName("ctlName");
        m_val = new QLabel(QString::number(val));
        m_val->setObjectName("ctlVal");
        m_val->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_val->setMinimumWidth(38);
        m_s = new QSlider(Qt::Horizontal);
        m_s->setRange(min, max);
        m_s->setValue(val);
        g->addWidget(m_name, 0, 0);
        g->addWidget(m_val, 0, 1);
        g->addWidget(m_s, 1, 0, 1, 2);
        connect(m_s, &QSlider::valueChanged, this, [this](int v) {
            m_val->setText(QString::number(v));
            emit changed(v);
        });
    }
    int value() const { return m_s->value(); }
    // Aggiorna la posizione SENZA emettere changed (usato dall'AWB, che muove
    // gli slider da solo: altrimenti si rientrerebbe nel loop di controllo).
    void setValueSilent(int v) {
        QSignalBlocker b(m_s);
        m_s->setValue(v);
        m_val->setText(QString::number(v));
    }
    void setLocked(bool l) { m_s->setEnabled(!l); setStyleSheet(l ? "color:#5d6b78;" : ""); }
signals:
    void changed(int v);
private:
    QSlider *m_s;
    QLabel *m_name, *m_val;
};

// ============================ riga dei loghi ============================
// Disegna i loghi in una riga sola, alla STESSA altezza: 34 px se c'e' spazio,
// altrimenti l'altezza scende in modo che tutti ci stiano nella larghezza
// disponibile (dipende dal numero di loghi). Spaziatura uniforme tra i loghi.
class LogoStrip : public QWidget {
public:
    explicit LogoStrip(const QString &dir, QWidget *parent = nullptr) : QWidget(parent) {
        QDir d(dir);
        const QStringList files = d.entryList(QStringList() << "*.png" << "*.jpg" << "*.jpeg",
                                              QDir::Files, QDir::Name | QDir::IgnoreCase);
        for (const QString &f : files) {
            QPixmap pm(d.absoluteFilePath(f));
            if (pm.isNull()) { qWarning().noquote() << "[dxpose] logo non leggibile:" << f; continue; }
            m_logos << pm;
        }
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(kMaxH);
    }
    int count() const { return m_logos.size(); }
protected:
    void paintEvent(QPaintEvent *) override {
        if (m_logos.isEmpty()) return;
        const int n = m_logos.size();
        double sumAspect = 0;
        for (const QPixmap &p : m_logos) sumAspect += double(p.width()) / p.height();
        // altezza comune: la massima che fa stare tutto (con gli spazi minimi)
        const double h = qMin<double>(kMaxH, (width() - kMinGap * (n - 1)) / sumAspect);
        const double used = h * sumAspect;
        const double gap = n > 1 ? (width() - used) / (n - 1) : 0;   // spazio uniforme
        QPainter g(this);
        g.setRenderHint(QPainter::SmoothPixmapTransform);
        double x = n > 1 ? 0 : (width() - used) / 2;
        const double y = (height() - h) / 2;
        for (const QPixmap &p : m_logos) {
            const double w = h * p.width() / p.height();
            g.drawPixmap(QRectF(x, y, w, h), p, QRectF(p.rect()));
            x += w + gap;
        }
    }
private:
    static constexpr int kMaxH = 34, kMinGap = 18;
    QList<QPixmap> m_logos;
};

// ============================ pannello laterale destro ============================
class SidePanel : public QScrollArea {
    Q_OBJECT
public:
    SidePanel(const QString &mainDir, const QString &dxDir, IspPipeline *isp,
              const QList<ModelInfo> &models, const QString &logosDir,
              QWidget *parent = nullptr)
        : QScrollArea(parent), m_isp(isp) {
        m_main.dir = mainDir;
        m_main.offsetMa = 210.0;   // perdite scheda sul ramo 5V
        m_dx.dir = dxDir;

        setObjectName("side");
        setWidgetResizable(true);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setFrameShape(QFrame::NoFrame);
        setFixedWidth(400);
        setStyleSheet(
            "#side{background:#11151a;border-left:2px solid #4A7488;}"
            "QWidget#body{background:#11151a;}"
            "#logoBox{background:#0d1115;border:1px solid #28303a;border-radius:7px;}"
            "#cap{color:#9fb0bd;font-size:10px;font-weight:700;}"
            "#val{color:#d7e1e9;font-size:13px;}"
            "#big{color:#dfeaf1;font-size:16px;font-weight:700;}"
            "#ctlName{color:#9fb0bd;font-size:11px;}"
            "#ctlVal{color:#7fb2c8;font-size:11px;font-weight:700;}"
            "QGroupBox{color:#7fb2c8;font-size:10px;font-weight:700;border:1px solid #28303a;"
            "border-radius:7px;margin-top:9px;padding:10px 8px 8px 8px;}"
            "QGroupBox::title{subcontrol-origin:margin;left:9px;padding:0 4px;}"
            "QComboBox{background:#1b232d;color:#dbe9f1;border:1px solid #4A7488;border-radius:6px;"
            "padding:6px 8px;font-size:12px;}"
            "QComboBox QAbstractItemView{background:#1b232d;color:#dbe9f1;selection-background-color:#4A7488;}"
            "QPushButton{background:#1b232d;color:#dbe9f1;border:1px solid #4A7488;border-radius:6px;"
            "padding:6px 10px;font-size:11px;}"
            "QPushButton:hover{background:#23303c;}"
            "QCheckBox{color:#dbe9f1;font-size:12px;}"
            "QSlider::groove:horizontal{height:4px;background:#28303a;border-radius:2px;}"
            "QSlider::handle:horizontal{background:#7fb2c8;width:13px;margin:-5px 0;border-radius:6px;}"
            "QSlider::sub-page:horizontal{background:#4A7488;border-radius:2px;}"
            "#railBox{background:#171c22;border:1px solid #28303a;border-left:3px solid #4A7488;border-radius:7px;}"
            "#npuBox{background:#171c22;border:1px solid #28303a;border-left:3px solid #7fb2c8;border-radius:7px;}");

        auto *body = new QWidget;
        body->setObjectName("body");
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(12, 12, 12, 12);
        v->setSpacing(10);

        // --- loghi ---
        auto *logoBox = new QFrame;
        logoBox->setObjectName("logoBox");
        auto *ll = new QHBoxLayout(logoBox);
        ll->setContentsMargins(12, 10, 12, 10);
        // Loghi letti a runtime dalla cartella del rootfs (installata da
        // dxpose-qt.mk da <ked-external>/media/logos): solo quelli presenti, in
        // ordine alfabetico, tutti alla stessa altezza e su una sola riga.
        auto *strip = new LogoStrip(logosDir);
        ll->addWidget(strip);
        qInfo().noquote() << "[dxpose] loghi da" << logosDir << ":" << strip->count();
        logoBox->setVisible(strip->count() > 0);
        v->addWidget(logoBox);

        // --- modello ---
        auto *gm = new QGroupBox("MODEL");
        auto *gml = new QVBoxLayout(gm);
        gml->setSpacing(6);
        m_model = new QComboBox;
        for (const ModelInfo &m : models) m_model->addItem(m.label(), QVariant::fromValue(m.name));
        m_models = models;
        gml->addWidget(m_model);
        m_tracker = new QCheckBox("Enable tracker (config models only)");
        gml->addWidget(m_tracker);
        auto *apply = new QPushButton("Apply model");
        gml->addWidget(apply);
        if (models.isEmpty()) {
            m_model->addItem("no .dxnn found");
            m_model->setEnabled(false);
            apply->setEnabled(false);
        }
        v->addWidget(gm);
        connect(apply, &QPushButton::clicked, this, [this] { emitModel(); });

        // --- sorgente ---
        auto *gs = new QGroupBox("SOURCE");
        auto *gsl = new QVBoxLayout(gs);
        m_src = new QLabel("--");
        m_src->setObjectName("val");
        m_src->setWordWrap(true);
        gsl->addWidget(m_src);
        // Pipeline GStreamer in uso, in forma compatta e con carattere piccolo
        // (la stringa completa e' nel tooltip e nel log).
        m_pipeTxt = new QLabel;
        m_pipeTxt->setWordWrap(true);
        m_pipeTxt->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_pipeTxt->setStyleSheet("color:#7d8b97;font-size:10px;");
        m_pipeTxt->hide();
        gsl->addWidget(m_pipeTxt);
        m_retry = new QPushButton("Retry camera");
        m_retry->setEnabled(false);          // attivo solo in fallback
        gsl->addWidget(m_retry);
        connect(m_retry, &QPushButton::clicked, this, [this] {
            m_retry->setEnabled(false);
            m_src->setText("Retrying camera...");
            m_src->setStyleSheet("");
            emit retryCameraRequested();
        });
        v->addWidget(gs);

        // --- NPU ---
        auto *gn = new QGroupBox("DX-M1");
        auto *gnl = new QVBoxLayout(gn);
        gnl->setSpacing(4);
        m_npuHead = new QLabel("n/d");
        m_npuHead->setObjectName("val");
        m_npuHead->setWordWrap(true);
        m_npuCores = new QLabel("--");
        m_npuCores->setObjectName("val");
        m_npuCores->setTextFormat(Qt::RichText);
        m_npuWarn = new QLabel;
        m_npuWarn->setWordWrap(true);
        m_npuWarn->setStyleSheet("color:#e05a5a;font-size:12px;font-weight:700;");
        m_npuWarn->hide();
        gnl->addWidget(m_npuWarn);
        m_fwBtn = new QPushButton("Update DX-M1 firmware");
        m_fwBtn->hide();
        gnl->addWidget(m_fwBtn);
        connect(m_fwBtn, &QPushButton::clicked, this, [this] {
            m_fwBtn->setEnabled(false);
            emit fwUpdateRequested();
        });
        gnl->addWidget(m_npuHead);
        gnl->addWidget(m_npuCores);
        v->addWidget(gn);

        // --- alimentazione ---
        auto *gp = new QGroupBox("POWER");
        auto *gpl = new QVBoxLayout(gp);
        gpl->setSpacing(4);
        m_railMain = new QLabel("n/d");
        m_railMain->setObjectName("val");
        m_railMain->setTextFormat(Qt::RichText);
        m_railDx = new QLabel("n/d");
        m_railDx->setObjectName("val");
        m_railDx->setTextFormat(Qt::RichText);
        gpl->addWidget(caption("5V rail"));
        gpl->addWidget(m_railMain);
        gpl->addWidget(caption("DX-M1 3V3"));
        gpl->addWidget(m_railDx);
        v->addWidget(gp);

        // --- ISP ---
        auto *gi = new QGroupBox("ISP");
        auto *gil = new QVBoxLayout(gi);
        gil->setSpacing(6);
        IspPipeline::Ctrls c;
        if (isp && isp->valid()) isp->readCtrls(c);
        // Riga superiore: checkbox AWB a sinistra, Reset a destra.
        m_awb = new QCheckBox("Auto white balance (gray-world)");
        auto *reset = new QPushButton("Reset");
        reset->setToolTip("Restore the default ISP settings");
        auto *awbRow = new QHBoxLayout;
        awbRow->setContentsMargins(0, 0, 0, 0);
        awbRow->addWidget(m_awb, 1);
        awbRow->addWidget(reset, 0);
        gil->addLayout(awbRow);
        m_wbR = new CtrlSlider("WB red", IspPipeline::kGainMin, IspPipeline::kGainMax, c.wbRed);
        m_wbG = new CtrlSlider("WB green", IspPipeline::kGainMin, IspPipeline::kGainMax, c.wbGreen);
        m_wbB = new CtrlSlider("WB blue", IspPipeline::kGainMin, IspPipeline::kGainMax, c.wbBlue);
        m_bri = new CtrlSlider("Brightness", IspPipeline::kGainMin, IspPipeline::kGainMax, c.brightness);
        m_con = new CtrlSlider("Contrast", IspPipeline::kGainMin, IspPipeline::kGainMax, c.contrast);
        m_gam = new CtrlSlider("Gamma (x10)", IspPipeline::kGammaMin, IspPipeline::kGammaMax, c.gamma);
        m_exp = new CtrlSlider("Exposure", IspPipeline::kExpMin, IspPipeline::kExpMax, c.exposure);
        m_ag = new CtrlSlider("Analogue gain", IspPipeline::kAGainMin, IspPipeline::kAGainMax, c.analogueGain);
        m_dg = new CtrlSlider("Digital gain", IspPipeline::kDGainMin, IspPipeline::kDGainMax, c.digitalGain);
        for (CtrlSlider *s : {m_wbR, m_wbG, m_wbB, m_bri, m_con, m_gam, m_exp, m_ag, m_dg})
            gil->addWidget(s);
        v->addWidget(gi);
        v->addStretch(1);

        // --- rete ---
        auto *nr = new QHBoxLayout;
        // Riga in fondo: stato rete a SINISTRA, data e ora a DESTRA. Lo stretch
        // centrale li tiene separati; l'IP si accorcia (elide) prima di poter
        // toccare l'orologio, che ha larghezza fissa.
        m_net = new NetworkIndicator;
        m_ip = new QLabel("net --");
        m_ip->setObjectName("val");
        m_ip->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_ip->setMinimumWidth(60);
        m_clock = new QLabel;
        m_clock->setObjectName("val");
        m_clock->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        m_clock->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        nr->addWidget(m_net, 0);
        nr->addSpacing(6);
        nr->addWidget(m_ip, 1);
        nr->addStretch(1);
        nr->addWidget(m_clock, 0, Qt::AlignRight | Qt::AlignVCenter);
        v->addLayout(nr);
        auto *clk = new QTimer(this);
        connect(clk, &QTimer::timeout, this, &SidePanel::tickClock);
        clk->start(1000);

        setWidget(body);

        // --- collegamenti ISP: ogni slider scrive un solo controllo ---
        // Collegati SEMPRE, con verifica a runtime: la camera puo' comparire dopo
        // (pulsante "Retry camera"), e allora gli slider devono gia' funzionare.
        if (isp) {
            auto ok = [isp] { return isp->valid(); };
            connect(m_wbR, &CtrlSlider::changed, this, [isp, ok](int v) { if (ok()) isp->setWbRed(v); });
            connect(m_wbG, &CtrlSlider::changed, this, [isp, ok](int v) { if (ok()) isp->setWbGreen(v); });
            connect(m_wbB, &CtrlSlider::changed, this, [isp, ok](int v) { if (ok()) isp->setWbBlue(v); });
            connect(m_bri, &CtrlSlider::changed, this, [isp, ok](int v) { if (ok()) isp->setBrightness(v); });
            connect(m_con, &CtrlSlider::changed, this, [isp, ok](int v) { if (ok()) isp->setContrast(v); });
            connect(m_gam, &CtrlSlider::changed, this, [isp, ok](int v) { if (ok()) isp->setGamma(v); });
            connect(m_exp, &CtrlSlider::changed, this, [isp, ok](int v) { if (ok()) isp->setExposure(v); });
            connect(m_ag, &CtrlSlider::changed, this, [isp, ok](int v) { if (ok()) isp->setAnalogueGain(v); });
            connect(m_dg, &CtrlSlider::changed, this, [isp, ok](int v) { if (ok()) isp->setDigitalGain(v); });
        }
        setIspAvailable(isp && isp->valid());
        connect(m_awb, &QCheckBox::toggled, this, [this](bool on) {
            const bool avail = m_isp && m_isp->valid();
            m_wbR->setLocked(on || !avail);
            m_wbB->setLocked(on || !avail);   // il verde resta manuale: e' il riferimento del gray-world
            m_awbCtl.reset();
        });
        connect(reset, &QPushButton::clicked, this, [this] {
            IspPipeline::Ctrls d;   // i default tarati in laboratorio
            applyCtrls(d);
        });

        auto *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, &SidePanel::refresh);
        t->start(700);
        refresh();
        tickClock();   // orologio visibile subito, senza attendere il primo secondo
    }

    void setVideoWidget(VideoWidget *v) { m_view = v; }

    // Ciclo AWB: chiamato a bassa frequenza dal main. L'anello e' chiuso su un
    // frame GIA' corretto dal CSC, quindi la correzione e' parziale a ogni passo
    // (vedi GrayWorldAwb::m_speed): a piena correzione oscillerebbe.
    void awbTick() {
        // In fallback il frame viene dal file, non dal CSC: correggere i gain
        // sulla base di un video registrato non avrebbe senso.
        if (!m_awb->isChecked() || !m_isp || !m_isp->valid() || !m_view || !m_onCamera) return;
        const QImage img = m_view->currentFrame();
        if (img.isNull() || img.format() != QImage::Format_RGB888) return;
        const auto st = GrayWorldAwb::analyze(img.constBits(), img.width(), img.height(),
                                              img.bytesPerLine(), /*bgrOrder=*/false);
        int nr = 0, nb = 0;
        if (!m_awbCtl.step(st, m_wbR->value(), m_wbB->value(), nr, nb)) return;
        m_isp->setWbRed(nr);
        m_isp->setWbBlue(nb);
        m_wbR->setValueSilent(nr);
        m_wbB->setValueSilent(nb);
    }

    // Valori correnti degli slider (riapplicati dopo una riconfigurazione ISP).
    IspPipeline::Ctrls currentCtrls() const {
        IspPipeline::Ctrls c;
        c.wbRed = m_wbR->value(); c.wbGreen = m_wbG->value(); c.wbBlue = m_wbB->value();
        c.brightness = m_bri->value(); c.contrast = m_con->value(); c.gamma = m_gam->value();
        c.exposure = m_exp->value(); c.analogueGain = m_ag->value(); c.digitalGain = m_dg->value();
        return c;
    }
    // Pulsante visibile solo se il firmware del modulo non e' allineato.
    void setFwUpdateAvailable(bool on) { m_fwBtn->setVisible(on); m_fwBtn->setEnabled(on); }
    void setNpuWarning(const QString &w) {
        m_npuWarn->setText(w);
        m_npuWarn->setVisible(!w.isEmpty());
    }
    // AWB attivo di default (vedi --no-awb): R e B diventano automatici.
    void setAwbEnabled(bool on) { m_awb->setChecked(on); }
    // "v4l2src name=camsrc device=/dev/video0 io-mode=mmap ! video/x-raw,format=BGR,
    //  width=1280,height=720,framerate=30/1 ! videoconvert n-threads=2 ! ..."
    //  -> "v4l2src ▸ BGR 1280×720 30fps ▸ videoconvert ▸ RGB ▸ dxpreprocess ▸ ..."
    static QString compactPipeline(const QString &launch) {
        QStringList out;
        for (QString seg : launch.split('!')) {
            seg = seg.simplified();
            if (seg.isEmpty()) continue;
            if (seg.startsWith("video/x-raw")) {             // caps: formato e misure
                QString fmt, w, h, fr;
                for (const QString &kv : seg.split(',')) {
                    const QString k = kv.section('=', 0, 0).trimmed();
                    const QString v = kv.section('=', 1).trimmed();
                    if (k == "format") fmt = v;
                    else if (k == "width") w = v;
                    else if (k == "height") h = v;
                    else if (k == "framerate") fr = v.section('/', 0, 0) + "fps";
                }
                QString c = fmt;
                if (!w.isEmpty()) c += " " + w + "×" + h;
                if (!fr.isEmpty()) c += " " + fr;
                out << c.trimmed();
                continue;
            }
            const QString name = seg.section(' ', 0, 0);
            if (name == "queue") continue;   // buffer fra thread: nella forma compatta e' rumore
            QString extra;
            for (const QString &kv : seg.split(' ')) {       // solo i parametri utili
                if (kv.startsWith("location=") || kv.startsWith("model-path="))
                    extra = QFileInfo(kv.section('=', 1)).fileName();
                else if (kv.startsWith("function-name="))
                    extra = kv.section('=', 1);
            }
            out << (extra.isEmpty() ? name : name + " (" + extra + ")");
        }
        return out.join(QStringLiteral("  \u25B8  "));
    }
    void setIspAvailable(bool on) {
        for (CtrlSlider *s : {m_wbR, m_wbG, m_wbB, m_bri, m_con, m_gam, m_exp, m_ag, m_dg})
            s->setLocked(!on);
        m_awb->setEnabled(on);
        if (on && m_awb->isChecked()) { m_wbR->setLocked(true); m_wbB->setLocked(true); }
    }

signals:
    void modelRequested(const ModelInfo &m);
    void retryCameraRequested();
    void fwUpdateRequested();

public slots:
    // Solo lo stato (Camera / Fallback video); dettagli e motivo nel tooltip.
    void setSource(bool cam, const QString &info) {
        m_onCamera = cam;
        m_src->setText(cam ? QStringLiteral("Camera") : QStringLiteral("Fallback video"));
        m_src->setStyleSheet(cam ? "color:#5cd08a;font-weight:700;"
                                 : "color:#e0b25a;font-weight:700;");
        QString tip = info;
        tip.replace('\n', "  ·  ");
        m_src->setToolTip(tip);
        m_retry->setEnabled(!cam);
    }
    void setPipelineText(const QString &launch) {
        m_pipeTxt->setText(compactPipeline(launch));
        m_pipeTxt->setToolTip(launch);
        m_pipeTxt->setVisible(!launch.isEmpty());
    }
    void setNpu(const NpuStatus &s) {
        if (!s.ok) return;
        m_npuHead->setText(QString("%1 · FW %2<br>%3<br>%4")
                               .arg(s.deviceName.isEmpty() ? "M1" : s.deviceName,
                                    s.fwVersion, s.pcie, s.memory));
        QString h;
        for (const NpuCore &c : s.cores) {
            const QString col = c.tempC >= 85 ? "#e05a5a" : (c.tempC >= 70 ? "#e0b25a" : "#7fb2c8");
            h += QString("<div style='font-size:12px'>NPU %1 &nbsp; "
                         "<span style='color:#eaf1f6'>%2</span><span style='color:#8b97a3;font-size:10px'> mV</span> &nbsp; "
                         "<span style='color:#eaf1f6'>%3</span><span style='color:#8b97a3;font-size:10px'> MHz</span> &nbsp; "
                         "<span style='color:%5;font-weight:700'>%4</span><span style='color:#8b97a3;font-size:10px'> °C</span></div>")
                     .arg(c.core).arg(c.voltageMv).arg(c.clockMhz).arg(c.tempC).arg(col);
        }
        m_npuCores->setText(h);
    }

private:
    void applyCtrls(const IspPipeline::Ctrls &d) {
        m_wbR->setValueSilent(d.wbRed);
        m_wbG->setValueSilent(d.wbGreen);
        m_wbB->setValueSilent(d.wbBlue);
        m_bri->setValueSilent(d.brightness);
        m_con->setValueSilent(d.contrast);
        m_gam->setValueSilent(d.gamma);
        m_exp->setValueSilent(d.exposure);
        m_ag->setValueSilent(d.analogueGain);
        m_dg->setValueSilent(d.digitalGain);
        if (m_isp) m_isp->applyCtrls(d);
    }
    void emitModel() {
        const int i = m_model->currentIndex();
        if (i < 0 || i >= m_models.size()) return;
        ModelInfo m = m_models.at(i);
        m.tracking = m_tracker->isChecked() && !m.cfgDir.isEmpty();
        emit modelRequested(m);
    }
    static QString fmtRail(const Rail &r) {
        double v, ma, w;
        if (!r.read(v, ma, w)) return "<span style='color:#c2666e'>n/d</span>";
        auto n = [](double x, int d, const QString &u, const QString &c) {
            return QString("<span style='font-size:16px;font-weight:600;color:%3'>%1</span>"
                           "<span style='font-size:10px;color:#8b97a3'> %2</span>")
                .arg(x, 0, 'f', d).arg(u, c);
        };
        return n(v, 2, "V", "#eaf1f6") + "&nbsp;&nbsp;" + n(ma, 0, "mA", "#eaf1f6") +
               "&nbsp;&nbsp;" + n(w, 2, "W", "#7fb2c8");
    }
private slots:
    void tickClock() {
        // Formato in inglese e indipendente dal locale del sistema.
        const QString t = QLocale(QLocale::English).toString(QDateTime::currentDateTime(),
                                                             "ddd dd MMM yyyy   HH:mm:ss");
        if (m_clock->text() != t) {
            m_clock->setText(t);
            // larghezza fissata sul testo piu' lungo possibile: niente "saltelli"
            if (m_clock->minimumWidth() == 0)
                m_clock->setFixedWidth(m_clock->fontMetrics().horizontalAdvance("Wed 28 Sep 2026   23:59:59") + 6);
        }
    }
    void refresh() {
        QString ip;
        const bool c = activeIPv4(ip);
        m_net->setConnected(c);
        m_ip->setText(c ? ip : QStringLiteral("no network"));
        m_railMain->setText(fmtRail(m_main));
        m_railDx->setText(fmtRail(m_dx));
    }

private:
    IspPipeline *m_isp;
    VideoWidget *m_view = nullptr;
    GrayWorldAwb m_awbCtl;
    QList<ModelInfo> m_models;
    QComboBox *m_model;
    QCheckBox *m_tracker, *m_awb;
    QPushButton *m_retry = nullptr;
    QLabel *m_pipeTxt = nullptr;
    QLabel *m_npuWarn = nullptr;
    QPushButton *m_fwBtn = nullptr;
    bool m_onCamera = false;
    QLabel *m_src, *m_npuHead, *m_npuCores, *m_railMain, *m_railDx, *m_ip;
    NetworkIndicator *m_net;
    QLabel *m_clock = nullptr;
    CtrlSlider *m_wbR, *m_wbG, *m_wbB, *m_bri, *m_con, *m_gam, *m_exp, *m_ag, *m_dg;
    Rail m_main, m_dx;
};

// ============================ area video + sinottico ============================
// Impila il video (1280x720) e il sinottico (1280x200) come un blocco unico di
// 1280x920 in coordinate nominali. Su 1920x1080 con il pannello a destra da 640
// la scala e' 1:1: video a pixel nativi (la cattura e' 1280x720) e sinottico
// esattamente 1280x200 sotto. Su schermi diversi il blocco scala in proporzione
// (mai oltre 1:1, per non ingrandire il video sfocandolo).
class StageWidget : public QWidget {
public:
    StageWidget(VideoWidget *video, PipelineView *pipe, QWidget *parent = nullptr)
        : QWidget(parent), m_video(video), m_pipe(pipe) {
        video->setParent(this);
        pipe->setParent(this);
        setAutoFillBackground(true);
        QPalette p = palette();
        p.setColor(QPalette::Window, Qt::black);
        setPalette(p);
        setMinimumSize(320, 240);
    }
    static constexpr int kW = 1280, kVideoH = 720, kPipeH = PipelineView::kDesignH;
protected:
    // Sinottico ancorato in BASSO, a 8 px dal bordo (Weston senza barra: l'app
    // ha tutti i 1080 px). Video centrato nello spazio che resta sopra.
    // Scala < 1 solo se lo schermo e' troppo piccolo per 720 + 200 + 8 px.
    void resizeEvent(QResizeEvent *) override {
        const int m = 8;   // margine dal bordo inferiore
        const qreal s = qMin<qreal>(1.0, qMin(width() / qreal(kW),
                                              (height() - m) / qreal(kVideoH + kPipeH)));
        const int w = qRound(kW * s), vh = qRound(kVideoH * s), ph = qRound(kPipeH * s);
        const int x = (width() - w) / 2;
        const int yPipe = height() - m - ph;
        m_pipe->setGeometry(x, yPipe, w, ph);
        m_video->setGeometry(x, qMax(0, (yPipe - vh) / 2), w, vh);
    }
private:
    VideoWidget *m_video;
    PipelineView *m_pipe;
};

// ============================ main ============================
int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);
    QApplication app(argc, argv);
    QApplication::setApplicationName("dxpose-qt");
    qRegisterMetaType<QImage>("QImage");
    qRegisterMetaType<ModelInfo>("ModelInfo");
    qRegisterMetaType<NpuStatus>("NpuStatus");

    QCommandLineParser p;
    p.setApplicationDescription("KED - IMX219 + DEEPX DX-M1, Qt6/GStreamer on Weston");
    p.addHelpOption();
    QCommandLineOption oModels("models-dir", "Folder with the .dxnn models", "dir", "/home/ked/models");
    QCommandLineOption oCfg("configs-dir", "Root of dx_stream configs", "dir",
                            "/usr/share/gstdxstream/configs");
    QCommandLineOption oPp("pp-lib", "Postprocess library", "path",
                           "/usr/share/gstdxstream/lib/libpostprocess_ppu.so");
    QCommandLineOption oTrk("tracker-config", "tracker_config.json", "path",
                            "/usr/share/gstdxstream/configs/tracker_config.json");
    QCommandLineOption oModel("model", "Initial model (name without .dxnn)", "nome", "YoloV5S_PPU");
    QCommandLineOption oFbFile("fallback-file", "Fallback video used when the camera is missing or fails",
                               "path", "/usr/share/dxpose-qt/media/dron_720p.y4m");
    QCommandLineOption oFbDir("fallback-dir", "Secondary fallback folder (first video found)", "dir", "/home/ked/videos");
    QCommandLineOption oW("width", "Capture width (resize done by the VPSS scaler)", "px", "1280");
    QCommandLineOption oH("height", "Capture height", "px", "720");
    QCommandLineOption oSW("sensor-width", "ISP width", "px", "1920");
    QCommandLineOption oSH("sensor-height", "ISP height", "px", "1080");
    QCommandLineOption oFps("fps", "Framerate", "n", "30");
    QCommandLineOption oRgb("rgb", "Capture in RGB3 instead of BGR3 (needs a bitstream with HAS_RGB8)");
    QCommandLineOption oNoIsp("no-isp-setup", "Do not configure the media chain (already done by setup_pipeline.sh)");
    QCommandLineOption oMain("main-hwmon", "INA on the 5V rail", "v", "");
    QCommandLineOption oDx("dxm1-hwmon", "INA on the DX-M1 3V3 rail", "v", "");
    QCommandLineOption oWin("windowed", "Windowed instead of fullscreen");
    QCommandLineOption oLogos("logos-dir", "Folder with the panel logos (only the files present are shown)",
                              "dir", "/usr/share/dxpose-qt/logos");
    QCommandLineOption oNoFw("no-fw-update", "Do not update an outdated DX-M1 firmware automatically");
    QCommandLineOption oFwArgs("fw-update-args", "dxrt-cli arguments that flash a firmware file", "args", "-u");
    QCommandLineOption oNoAwb("no-awb", "Start with auto white balance disabled");
    QCommandLineOption oMinFw("min-npu-fw", "Minimum DX-M1 firmware required by the runtime", "ver", "2.5.2");
    QCommandLineOption oExit("exit-on-fault", "Exit on fatal NPU error (systemd restarts)");
    p.addOptions({oModels, oCfg, oPp, oTrk, oModel, oFbFile, oFbDir, oW, oH, oSW, oSH, oFps, oRgb, oNoIsp,
                  oMain, oDx, oWin, oExit, oMinFw, oNoAwb, oLogos, oNoFw, oFwArgs});
    p.process(app);

    // --- 1. Catena video in PL: la configura l'app (equivalente di setup_pipeline.sh) ---
    IspPipeline isp;
    IspPipeline::Config icfg;
    icfg.sensorW = p.value(oSW).toInt();
    icfg.sensorH = p.value(oSH).toInt();
    icfg.outW = p.value(oW).toInt();
    icfg.outH = p.value(oH).toInt();
    icfg.rgb = p.isSet(oRgb);

    // Rileva e configura la catena. Ritorna il nodo video, oppure vuoto con il
    // motivo in 'why'. Usata all'avvio e dal pulsante "Retry camera".
    const bool noIsp = p.isSet(oNoIsp);
    auto setupCamera = [&isp, icfg, noIsp](const IspPipeline::Ctrls &ctrls, QString &why) -> QString {
        QString err;
        if (!isp.discover(&err)) {
            why = "camera not detected: " + err;
            return QString();
        }
        if (noIsp) return isp.videoDevice();
        if (!isp.configure(icfg, &err)) {
            why = "ISP setup failed: " + err;
            return QString();
        }
        isp.applyCtrls(ctrls);   // S_FMT sulla CSC resetta i colori: si riapplicano DOPO
        return isp.videoDevice();
    };
    QString cameraError;
    const QString device = setupCamera(IspPipeline::Ctrls(), cameraError);
    if (device.isEmpty())
        qWarning().noquote() << "[dxpose]" << cameraError << "-> fallback su file";

    // --- 1b. Firmware del DX-M1 ---
    // dx_rt rifiuta firmware sotto la soglia con un'eccezione C++ lanciata DENTRO
    // dxinfer: non e' intercettabile dall'app e il processo abortisce (SIGABRT),
    // con systemd che riavvia all'infinito. Si controlla PRIMA di costruire la
    // pipeline e, se serve, si disattiva il NPU mostrando il motivo a schermo.
    // Confronto con il firmware incluso nell'immagine (/lib/firmware/deepx):
    // se il modulo e' piu' vecchio lo si aggiorna (vedi fwupdate.h). Durante
    // l'aggiornamento e fino al power cycle il NPU NON va usato.
    QString npuBlock;
    const bool autoFw = !p.isSet(oNoFw);
    auto *fw = new FirmwareUpdater(&app);
    fw->setUpdateArgs(p.value(oFwArgs).split(' ', Qt::SkipEmptyParts));
    const NpuStatus st0 = NpuInfo::probe();
    FirmwareUpdater::Decision fwDec = FirmwareUpdater::Decision::Unknown;
    if (st0.ok) {
        fwDec = fw->decide(st0.fwVersion, st0.deviceName, st0.board);
        switch (fwDec) {
        case FirmwareUpdater::Decision::NeedsUpdate:
            npuBlock = autoFw
                ? QString("Updating DX-M1 firmware %1 -> %2.").arg(st0.fwVersion, fw->bundledVersion())
                : QString("DX-M1 firmware %1 is older than the bundled %2.")
                      .arg(st0.fwVersion, fw->bundledVersion());
            break;
        case FirmwareUpdater::Decision::PowerCycleNeeded:
            npuBlock = QString("DX-M1 firmware %1 written: power cycle required.").arg(fw->bundledVersion());
            break;
        default: break;
        }
        // Soglia minima del runtime: vale anche se non c'e' un fw.bin incluso.
        const QStringList mv = p.value(oMinFw).split('.');
        if (npuBlock.isEmpty() && !st0.fwVersion.isEmpty() &&
            !NpuInfo::versionAtLeast(st0.fwVersion, mv.value(0).toInt(), mv.value(1).toInt(),
                                     mv.value(2).toInt()))
            npuBlock = QString("DX-M1 firmware %1 is too old: runtime needs >= %2.")
                           .arg(st0.fwVersion, p.value(oMinFw));
        qInfo().noquote() << "[dxpose] DX-M1 firmware" << st0.fwVersion << "- incluso"
                          << (fw->bundledVersion().isEmpty() ? "n/d" : fw->bundledVersion())
                          << (npuBlock.isEmpty() ? "-> OK" : "-> " + npuBlock);
    }

    // --- 2. Modelli ---
    const QList<ModelInfo> models = scanModels(p.value(oModels), p.value(oCfg));
    if (models.isEmpty())
        qWarning().noquote() << "[dxpose] nessun .dxnn in" << p.value(oModels)
                             << "(scaricarli da sdk.deepx.ai)";
    int initial = 0;
    for (int i = 0; i < models.size(); ++i)
        if (models.at(i).name == p.value(oModel)) { initial = i; break; }

    // --- 3. Rami di alimentazione ---
    const QStringList pm = findPowerMonitors();
    QString mainDir = resolveRail(p.value(oMain), pm);
    QString dxDir = resolveRail(p.value(oDx), pm);
    if (mainDir.isEmpty()) mainDir = pm.value(0);
    if (dxDir.isEmpty()) dxDir = pm.value(1);

    // --- 4. UI: video a SINISTRA, pannello a DESTRA ---
    QWidget win;
    win.setWindowTitle("KED - DEEPX dx_stream");
    auto *row = new QHBoxLayout(&win);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto *view = new VideoWidget;
    auto *pipe = new PipelineView;
    pipe->setProperty("sw", icfg.sensorW);
    pipe->setProperty("sh", icfg.sensorH);
    pipe->setResolution(icfg.sensorW, icfg.sensorH, icfg.outW, icfg.outH);
    pipe->setFourcc(icfg.rgb ? "RGB3" : "BGR3");
    pipe->setNpu(NpuInfo::devicePresent() && npuBlock.isEmpty());
    auto *side = new SidePanel(mainDir, dxDir, &isp, models, p.value(oLogos));
    side->setVideoWidget(view);
    side->setNpuWarning(npuBlock);
    side->setAwbEnabled(!p.isSet(oNoAwb));

    // Sinistra: video + sinottico impilati (StageWidget). Destra: pannello, largo
    // quanto resta dopo i 1280 px del video (su 1920 -> 640), minimo 400.
    auto *stage = new StageWidget(view, pipe);
    if (QScreen *scr = QGuiApplication::primaryScreen()) {
        const int sw = p.isSet(oWin) ? 1920 : scr->size().width();
        side->setFixedWidth(qMax(400, sw - StageWidget::kW));
    }
    row->addWidget(stage, 1);
    row->addWidget(side, 0);

    // --- 5. Telemetria NPU ---
    auto *npu = new NpuInfo(&win);
    QObject::connect(npu, &NpuInfo::updated, side, &SidePanel::setNpu);
    QObject::connect(npu, &NpuInfo::updated, pipe, [pipe](const NpuStatus &st) {
        QString t = "PCIe " + st.pcie.section('[', 0, 0).trimmed();
        if (!st.fwVersion.isEmpty()) t += "  ·  FW " + st.fwVersion;
        if (!st.cores.isEmpty()) t += QString("  ·  %1 cores  ·  max %2 °C").arg(st.cores.size()).arg(st.maxTempC());
        pipe->setNpuLink(t);
    });

    // --- 6. Controller su thread dedicato ---
    GstPipelineController::Config gcfg;
    gcfg.device = device;
    gcfg.ppLib = p.value(oPp);
    gcfg.trkCfg = p.value(oTrk);
    gcfg.fallbackDir = p.value(oFbDir);
    gcfg.fallbackFile = p.value(oFbFile);
    gcfg.cameraError = cameraError;
    gcfg.npuBlock = npuBlock;
    gcfg.width = icfg.outW;
    gcfg.height = icfg.outH;
    gcfg.fps = p.value(oFps).toInt();
    gcfg.rgb = icfg.rgb;
    gcfg.exitOnFault = p.isSet(oExit);

    auto *ctrl = new GstPipelineController(view, gcfg);
    auto *worker = new QThread;
    ctrl->moveToThread(worker);
    QObject::connect(side, &SidePanel::modelRequested, ctrl, &GstPipelineController::setModel);
    QObject::connect(ctrl, &GstPipelineController::sourceChanged, side, &SidePanel::setSource);
    QObject::connect(ctrl, &GstPipelineController::pipelineText, side, &SidePanel::setPipelineText);
    QObject::connect(ctrl, &GstPipelineController::npuTiming, pipe, &PipelineView::setNpuTiming);
    QObject::connect(ctrl, &GstPipelineController::sourceFailed, view,
                     [view](const QString &title, const QString &msg) {
                         view->setNotice(VideoWidget::Notice::Error, title, msg);
                     });
    // Retry camera: la riconfigurazione ISP (ioctl sui subdev) avviene nel thread
    // UI, poi il controller ricostruisce la pipeline sul suo thread.
    QObject::connect(side, &SidePanel::retryCameraRequested, side, [side, ctrl, setupCamera] {
        QString why;
        const QString dev = setupCamera(side->currentCtrls(), why);
        side->setIspAvailable(!dev.isEmpty());
        if (dev.isEmpty()) qWarning().noquote() << "[dxpose] retry camera:" << why;
        QMetaObject::invokeMethod(ctrl, "retryCamera", Qt::QueuedConnection,
                                  Q_ARG(QString, dev), Q_ARG(QString, why));
    });
    QObject::connect(ctrl, &GstPipelineController::pipelineState, pipe,
                     [pipe](bool cam, bool npu, const QString &fourcc, int ow, int oh, const QString &mdl) {
                         pipe->setUsingCamera(cam);
                         pipe->setNpu(npu);
                         pipe->setFourcc(fourcc);
                         pipe->setModelName(mdl);
                         // la risoluzione sensore non cambia a runtime, solo l'uscita
                         pipe->setResolution(pipe->property("sw").toInt(), pipe->property("sh").toInt(), ow, oh);
                     });
    QObject::connect(worker, &QThread::started, ctrl, &GstPipelineController::begin);
    worker->start();
    if (!models.isEmpty())
        QMetaObject::invokeMethod(ctrl, "setModel", Qt::QueuedConnection,
                                  Q_ARG(ModelInfo, models.at(initial)));

    // --- 7. Ciclo AWB (thread UI: legge l'ultimo frame e scrive i gain del CSC) ---
    auto *awbT = new QTimer(&win);
    QObject::connect(awbT, &QTimer::timeout, side, [side] { side->awbTick(); });
    awbT->start(400);

    // --- 8. Rete di sicurezza: se il worker si pianta dentro il NPU, esci ---
    if (p.isSet(oExit)) {
        std::thread([ctrl] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                const qint64 hb = ctrl->heartbeat();
                if (hb == 0) continue;
                if (QDateTime::currentMSecsSinceEpoch() - hb > 15000) {
                    fprintf(stderr, "[dxpose] MONITOR: worker bloccato -> esco\n");
                    fflush(stderr);
                    ::_exit(2);
                }
            }
        }).detach();
    }

    // --- 9. Aggiornamento firmware DX-M1 ---
    auto showPowerCycle = [view, fw](const QString &detail) {
        view->setNotice(VideoWidget::Notice::Done, "DX-M1 firmware updated",
                        QString("The firmware has been updated to %1.\n\n"
                                "Disconnect the board power supply, wait a few seconds and power it on again.\n"
                                "A reboot is not enough: the new firmware becomes active only after a full power cycle.")
                            .arg(fw->bundledVersion()),
                        detail);
    };
    auto startFwUpdate = [view, fw, npu, side, showPowerCycle] {
        npu->setPaused(true);   // nessun dxrt-cli -s concorrente durante il flash
        side->setFwUpdateAvailable(false);
        view->setNotice(VideoWidget::Notice::Busy, "Updating DX-M1 firmware",
                        QString("Updating the NPU firmware from %1 to %2.\n\n"
                                "Do not power off the board and do not remove the module.")
                            .arg(fw->currentVersion(), fw->bundledVersion()),
                        fw->firmwareFile());
        QTimer::singleShot(1200, fw, &FirmwareUpdater::start);   // lascia disegnare l'avviso
    };
    QObject::connect(fw, &FirmwareUpdater::progress, view, &VideoWidget::setNoticeDetail);
    QObject::connect(fw, &FirmwareUpdater::finished, view,
                     [view, npu, side, showPowerCycle](bool ok, const QString &msg) {
        npu->setPaused(false);
        if (ok) {
            showPowerCycle(msg);
            side->setNpuWarning("Firmware updated: power cycle the board.");
        } else {
            view->setNotice(VideoWidget::Notice::Error, "DX-M1 firmware update failed",
                            msg + "\n\nThe NPU stays disabled and the camera runs without inference. "
                                  "Tap to close; use \"Update DX-M1 firmware\" to retry.");
            side->setNpuWarning("Firmware update failed.");
            side->setFwUpdateAvailable(true);
        }
    });
    QObject::connect(side, &SidePanel::fwUpdateRequested, fw, [fw, startFwUpdate] {
        fw->forgetState();   // flash forzato dall'utente
        startFwUpdate();
    });
    if (fwDec == FirmwareUpdater::Decision::NeedsUpdate) {
        if (autoFw) startFwUpdate();
        else side->setFwUpdateAvailable(true);
    } else if (fwDec == FirmwareUpdater::Decision::PowerCycleNeeded) {
        showPowerCycle(QString("Firmware %1 was already written; the module still reports %2.")
                           .arg(fw->bundledVersion(), fw->currentVersion()));
        side->setFwUpdateAvailable(true);   // se il flash non fosse andato a buon fine
    }

    if (p.isSet(oWin)) { win.resize(1920, 1080); win.show(); }
    else win.showFullScreen();

    const int ret = app.exec();
    QMetaObject::invokeMethod(ctrl, "stop", Qt::QueuedConnection);
    worker->quit();
    worker->wait(3000);
    return ret;
}

#include "main.moc"
