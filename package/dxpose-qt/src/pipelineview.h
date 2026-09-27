// ============================================================================
// pipelineview.h - Synoptic of the processing architecture, drawn as three
// parallel lanes that only meet through the PS DDR:
//
//   PL (FPGA)       IMX219 -> CSI-2 RX -> Demosaic -> CSC -> Gamma -> VPSS -> frmbuf WR
//                                                                               | AXI HPC0
//   PS DDR          ===================== shared system memory =====================
//                     | mmap
//   A53 GStreamer   v4l2src -> videoconvert -> dxpreprocess -> dxinfer -> dxpostprocess -> dxosd -> appsink
//                                                              | PCIe DMA  ^
//   PCIe DX-M1                                               DX-M1 NPU ----+
//
// Every arrow carries the pixel/data format on the link. Resolution, capture
// fourcc, model, source (camera/file) and NPU state are updated live.
// ============================================================================
#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

class QPainter;

class PipelineView : public QWidget {
    Q_OBJECT
public:
    explicit PipelineView(QWidget *parent = nullptr);

    void setResolution(int sensorW, int sensorH, int outW, int outH);
    void setFourcc(const QString &fourcc);      // "BGR3" / "RGB3"
    void setModelName(const QString &name);
    void setNpu(bool enabled);                  // false: NPU absent or disabled
    void setUsingCamera(bool cam);              // false: file fallback
    void setNpuLink(const QString &info);       // e.g. "PCIe Gen1 x2 · FW v2.5.6"
    // Tempi DX-M1 in ms (negativi = non disponibili): mostrati accanto alla
    // temperatura massima, nella corsia DX-M1.
    void setNpuTiming(double npuMs, double latencyMs);

    // Disegno VETTORIALE in un sistema di coordinate logico 1280x200 (la misura
    // nominale su schermo 1920x1080), scalato uniformemente sull'area reale:
    // resta nitido a qualunque dimensione.
    static constexpr int kDesignW = 1280;
    static constexpr int kDesignH = 200;
    QSize sizeHint() const override { return QSize(kDesignW, kDesignH); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w * kDesignH / kDesignW; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    struct Node {
        QString title;
        QString sub;
        bool dashed = false;
    };
    void drawBlock(QPainter &g, const QRectF &r, const Node &n, const QColor &accent, qreal alpha);
    void drawHArrow(QPainter &g, qreal x0, qreal x1, qreal y, const QString &label, qreal alpha);
    void drawVArrow(QPainter &g, qreal x, qreal y0, qreal y1, const QString &label,
                    bool labelOnLeft, qreal alpha);

    int m_sensorW = 1920, m_sensorH = 1080, m_outW = 1280, m_outH = 720;
    QString m_fourcc = "BGR3";
    QString m_model;
    QString m_npuLink;
    double m_npuMs = -1.0, m_latMs = -1.0;
    bool m_npu = true;
    bool m_cam = true;
};
