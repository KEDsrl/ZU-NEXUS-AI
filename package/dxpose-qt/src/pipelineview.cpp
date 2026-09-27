#include "pipelineview.h"

#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

namespace {
// Palette per corsia
const QColor kPl(0x4A, 0x74, 0x88);    // PL / FPGA
const QColor kDdr(0x5b, 0x67, 0x70);   // memoria condivisa
const QColor kSw(0x5d, 0x72, 0xb0);    // A53 / GStreamer
const QColor kNpu(0xd0, 0x9a, 0x4a);   // DX-M1
const QColor kArrow(0x9c, 0xc2, 0xd2);
const QColor kFmt(0x9f, 0xe0, 0xc0);   // formato dati sulle frecce
const QColor kBg(0x0d, 0x11, 0x15);

QColor withAlpha(QColor c, qreal a) { c.setAlphaF(c.alphaF() * a); return c; }

QFont px(int size, bool bold = false) {
    QFont f;
    f.setPixelSize(size);
    f.setBold(bold);
    return f;
}
} // namespace

PipelineView::PipelineView(QWidget *parent) : QWidget(parent) {
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void PipelineView::setResolution(int sw, int sh, int ow, int oh) {
    m_sensorW = sw; m_sensorH = sh; m_outW = ow; m_outH = oh; update();
}
void PipelineView::setFourcc(const QString &f)    { m_fourcc = f; update(); }
void PipelineView::setModelName(const QString &n) { m_model = n; update(); }
void PipelineView::setNpu(bool e)                 { m_npu = e; update(); }
void PipelineView::setUsingCamera(bool c)         { m_cam = c; update(); }
void PipelineView::setNpuLink(const QString &i)   { m_npuLink = i; update(); }
void PipelineView::setNpuTiming(double npu, double lat) { m_npuMs = npu; m_latMs = lat; update(); }

// ---------------------------------------------------------------------------
void PipelineView::drawBlock(QPainter &g, const QRectF &r, const Node &n,
                             const QColor &accent, qreal a) {
    QLinearGradient grad(r.topLeft(), r.bottomLeft());
    grad.setColorAt(0, withAlpha(accent.lighter(118), a));
    grad.setColorAt(1, withAlpha(accent.darker(140), a));
    QPainterPath path;
    path.addRoundedRect(r, 6, 6);
    g.fillPath(path, grad);
    QPen pen(withAlpha(accent.lighter(160), 0.9 * a), n.dashed ? 1.3 : 1.0);
    if (n.dashed) pen.setStyle(Qt::DashLine);
    g.strokePath(path, pen);

    const QFont ft = px(10, true), fs = px(9);
    const QFontMetricsF mt(ft), ms(fs);
    const qreal w = r.width() - 8;
    g.setFont(ft);
    g.setPen(withAlpha(QColor(0xf2, 0xf7, 0xfa), a));
    g.drawText(QRectF(r.left() + 4, r.top() + 3, w, mt.height()), Qt::AlignCenter,
               mt.elidedText(n.title, Qt::ElideRight, w));
    if (!n.sub.isEmpty()) {
        g.setFont(fs);
        g.setPen(withAlpha(QColor(0xe0, 0xea, 0xf0), 0.78 * a));
        g.drawText(QRectF(r.left() + 4, r.bottom() - ms.height() - 3, w, ms.height()),
                   Qt::AlignCenter, ms.elidedText(n.sub, Qt::ElideRight, w));
    }
}

// Freccia orizzontale; il formato va SOPRA (prima riga) e SOTTO (seconda riga)
// la linea, cosi' resta nello spazio tra i blocchi senza sovrapporsi.
void PipelineView::drawHArrow(QPainter &g, qreal x0, qreal x1, qreal y,
                              const QString &label, qreal a) {
    const QColor c = withAlpha(kArrow, a);
    g.setPen(QPen(c, 1.4));
    g.drawLine(QPointF(x0 + 2, y), QPointF(x1 - 6, y));
    QPainterPath head;
    head.moveTo(x1 - 1, y);
    head.lineTo(x1 - 7, y - 3.5);
    head.lineTo(x1 - 7, y + 3.5);
    head.closeSubpath();
    g.fillPath(head, c);

    if (label.isEmpty()) return;
    const QFont fl = px(9, true);
    const QFontMetricsF ml(fl);
    const QStringList lines = label.split('\n');
    const qreal w = x1 - x0;
    g.setFont(fl);
    g.setPen(withAlpha(kFmt, a));
    g.drawText(QRectF(x0, y - ml.height() - 1, w, ml.height()), Qt::AlignCenter,
               ml.elidedText(lines.value(0), Qt::ElideRight, w));
    if (lines.size() > 1)
        g.drawText(QRectF(x0, y + 2, w, ml.height()), Qt::AlignCenter,
                   ml.elidedText(lines.value(1), Qt::ElideRight, w));
}

// Freccia verticale (y0 -> y1, verso qualsiasi); etichetta su una riga a lato.
void PipelineView::drawVArrow(QPainter &g, qreal x, qreal y0, qreal y1,
                              const QString &label, bool labelOnLeft, qreal a) {
    const QColor c = withAlpha(kArrow, a);
    const qreal dir = (y1 > y0) ? 1.0 : -1.0;
    g.setPen(QPen(c, 1.4));
    g.drawLine(QPointF(x, y0), QPointF(x, y1 - dir * 6));
    QPainterPath head;
    head.moveTo(x, y1);
    head.lineTo(x - 3.5, y1 - dir * 7);
    head.lineTo(x + 3.5, y1 - dir * 7);
    head.closeSubpath();
    g.fillPath(head, c);

    if (label.isEmpty()) return;
    const QFont fl = px(9, true);
    const QFontMetricsF ml(fl);
    const qreal tw = ml.horizontalAdvance(label) + 2;
    const qreal ty = (y0 + y1) / 2 - ml.height() / 2;
    g.setFont(fl);
    g.setPen(withAlpha(kFmt, a));
    const QRectF tr = labelOnLeft ? QRectF(x - 6 - tw, ty, tw, ml.height())
                                  : QRectF(x + 6, ty, tw, ml.height());
    g.drawText(tr, labelOnLeft ? (Qt::AlignRight | Qt::AlignVCenter)
                               : (Qt::AlignLeft | Qt::AlignVCenter), label);
}

// ---------------------------------------------------------------------------
void PipelineView::paintEvent(QPaintEvent *) {
    QPainter g(this);
    g.setRenderHint(QPainter::Antialiasing);
    g.setRenderHint(QPainter::TextAntialiasing);
    g.fillRect(rect(), kBg);
    // Cornice sottile, stesso azzurro dei titoli del pannello (#7fb2c8). Disegnata
    // in pixel reali (prima della scala) cosi' resta di 1 px a ogni dimensione.
    g.save();
    g.setPen(QPen(QColor(0x7f, 0xb2, 0xc8), 1.0));
    g.setBrush(Qt::NoBrush);
    g.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    g.restore();
    // Scala uniforme dal sistema logico 1280x200 all'area reale, centrato.
    const qreal sc = qMin(width() / qreal(kDesignW), height() / qreal(kDesignH));
    g.translate((width() - kDesignW * sc) / 2, (height() - kDesignH * sc) / 2);
    g.scale(sc, sc);
    const qreal W = kDesignW;

    // ---- geometria: 7 colonne allineate tra le corsie ----
    const qreal pad = 6, laneW = 76;
    const qreal x0 = pad + laneW;
    const qreal availW = W - x0 - pad;
    const int cols = 7;
    const qreal colW = availW / cols;
    // spazio fra i blocchi ampio e fisso, cosi' i formati sulle frecce si leggono
    const qreal gap = qBound<qreal>(64.0, colW * 0.42, 90.0);
    const qreal bw = colW - gap;
    auto bx = [&](int i) { return x0 + i * colW; };

    // Nel sistema 1280x200: colonne da ~170, blocchi ~99, frecce ~71.
    const qreal hB = 36;                       // altezza blocchi
    const qreal yPL = 6;
    const qreal yDDR = yPL + hB + 20, hDDR = 16;
    const qreal ySW = yDDR + hDDR + 20;
    const qreal yNPU = ySW + hB + 26, hN = 30;   // termina a 190 < 200

    const qreal aPL = m_cam ? 1.0 : 0.35;      // in fallback la camera non e' in uso
    const qreal aNPU = m_npu ? 1.0 : 0.35;

    // ---- fasce di corsia + etichette ----
    auto lane = [&](qreal y, qreal h, const QColor &c, const QString &t1, const QString &t2, qreal a) {
        QPainterPath band;
        band.addRoundedRect(QRectF(pad, y - 4, W - 2 * pad, h + 8), 6, 6);
        g.fillPath(band, withAlpha(QColor(c.red(), c.green(), c.blue(), 26), a));
        g.setPen(withAlpha(c.lighter(150), a));
        g.setFont(px(11, true));
        g.drawText(QRectF(pad + 6, y, laneW - 12, h / 2 + 2), Qt::AlignLeft | Qt::AlignBottom, t1);
        g.setFont(px(9));
        g.setPen(withAlpha(QColor(0x9f, 0xb0, 0xbd), a));
        g.drawText(QRectF(pad + 6, y + h / 2 + 1, laneW - 12, h / 2), Qt::AlignLeft | Qt::AlignTop, t2);
    };
    lane(yPL, hB, kPl, "PL", "FPGA ISP", aPL);
    lane(ySW, hB, kSw, "A53", "GStreamer", 1.0);
    lane(yNPU, hN, kNpu, "DX-M1", "PCIe NPU", aNPU);

    const QString sres = QString("%1×%2").arg(m_sensorW).arg(m_sensorH);
    const QString ores = QString("%1×%2").arg(m_outW).arg(m_outH);

    // ================= corsia 1: PL =================
    const Node pl[cols] = {
        { "IMX219", "sensor · 2 lanes", false },
        { "CSI-2 RX", "0x8002_0000", false },
        { "Demosaic", "0x8003_0000", false },
        { "CSC", "WB · 0x8030_0000", false },
        { "Gamma LUT", "0x8008_0000", false },
        { "VPSS", "scaler · 0x8010", true },
        { "frmbuf WR", "S2MM · 0x8004", false },
    };
    const QString plFmt[cols - 1] = {
        "MIPI\nRAW10", "RAW10\n" + sres, "RGB888\n" + sres, "RGB888\n" + sres,
        "RGB888\n" + sres, m_fourcc + "\n" + ores,
    };
    for (int i = 0; i < cols; ++i)
        drawBlock(g, QRectF(bx(i), yPL, bw, hB), pl[i], kPl, aPL);
    for (int i = 0; i < cols - 1; ++i)
        drawHArrow(g, bx(i) + bw, bx(i + 1), yPL + hB / 2, plFmt[i], aPL);

    // ================= DDR: bus condiviso =================
    {
        const QRectF r(x0, yDDR, W - x0 - pad, hDDR);
        QPainterPath p;
        p.addRoundedRect(r, 4, 4);
        g.fillPath(p, kDdr.darker(150));
        g.strokePath(p, QPen(kDdr.lighter(150), 1.0));
        g.setFont(px(10, true));
        g.setPen(QColor(0xdf, 0xe8, 0xee));
        g.drawText(r, Qt::AlignCenter,
                   "PS DDR  ·  shared system memory  ·  CMA buffers, cache-coherent (HPC0 / CCI-400)");
        g.setFont(px(11, true));
        g.setPen(kDdr.lighter(170));
        g.drawText(QRectF(pad + 6, yDDR, laneW - 12, hDDR), Qt::AlignLeft | Qt::AlignVCenter, "DDR");
    }
    // PL -> DDR (scrittura del frmbuf) e DDR -> A53 (lettura v4l2src)
    const qreal xFb = bx(cols - 1) + bw / 2;
    drawVArrow(g, xFb, yPL + hB, yDDR, "AXI write", true, aPL);
    // DDR -> v4l2src solo con la camera: in fallback il video viene letto dal
    // file sul rootfs (SD), non dal buffer che il frmbuf scrive in DDR.
    if (m_cam) {
        const qreal xSrc = bx(0) + bw / 2;
        drawVArrow(g, xSrc, yDDR + hDDR, ySW, "mmap", false, 1.0);
    }

    // ================= corsia 2: A53 / GStreamer =================
    const bool rgbCap = m_cam && m_fourcc == "RGB3";
    Node sw[cols] = {
        m_cam ? Node{ "v4l2src", "io-mode=mmap", false } : Node{ "filesrc", "y4mdec · SD card", false },
        Node{ "videoconvert", rgbCap ? "not used" : (m_cam ? "R↔B swap" : "I420 → RGB"), rgbCap },
        Node{ "dxpreprocess", "letterbox 640²", !m_npu },
        Node{ "dxinfer", m_npu ? "runtime dx_rt" : "disabled", !m_npu },
        Node{ "dxpostprocess", "decode + NMS", !m_npu },
        Node{ "dxosd", "draw results", !m_npu },
        Node{ "appsink", "Qt · Weston", false },
    };
    const QString srcFmt = m_cam ? m_fourcc : QStringLiteral("I420");
    const QString swFmt[cols - 1] = {
        srcFmt + "\n" + ores, "RGB\n" + ores, "RGB\n+ tensor", "RGB\n+ output",
        "RGB\n+ objects", "RGB\n" + ores,
    };
    for (int i = 0; i < cols; ++i)
        drawBlock(g, QRectF(bx(i), ySW, bw, hB), sw[i], kSw, (i >= 2 && i <= 5 && !m_npu) ? 0.45 : 1.0);
    for (int i = 0; i < cols - 1; ++i)
        drawHArrow(g, bx(i) + bw, bx(i + 1), ySW + hB / 2, swFmt[i],
                   (i >= 1 && i <= 4 && !m_npu) ? 0.45 : 1.0);

    // ================= corsia 3: DX-M1 su PCIe =================
    const int iInf = 3;   // colonna di dxinfer
    const QRectF rn(bx(iInf), yNPU, bw, hN);
    drawBlock(g, rn, Node{ "DX-M1 NPU", m_npu ? (m_model.isEmpty() ? "model" : m_model)
                                              : QStringLiteral("not in use"),
                           !m_npu }, kNpu, aNPU);
    const qreal xc = bx(iInf) + bw / 2;
    drawVArrow(g, xc - bw * 0.22, ySW + hB, yNPU, "tensor 640×640 · PCIe DMA", true, aNPU);
    drawVArrow(g, xc + bw * 0.22, yNPU, ySW + hB, "results · PCIe DMA", false, aNPU);
    // Stato del link/firmware/temperatura seguito, sulla stessa riga e con lo
    // stesso carattere, dai tempi del DX-M1 (NPU = calcolo puro, latency =
    // inferenza completa con i trasferimenti PCIe).
    {
        const QFont fi = px(10);     // stesso carattere per stato e tempi
        const QFontMetricsF mi(fi);
        qreal x = rn.right() + gap;
        const qreal xmax = W - pad - 6;
        if (!m_npuLink.isEmpty()) {
            g.setFont(fi);
            g.setPen(withAlpha(QColor(0xc9, 0xb0, 0x86), aNPU));
            const QString t = mi.elidedText(m_npuLink, Qt::ElideRight, xmax - x);
            g.drawText(QRectF(x, yNPU, xmax - x, hN), Qt::AlignLeft | Qt::AlignVCenter, t);
            x += mi.horizontalAdvance(t);
        }
        if (m_npu && m_npuMs > 0 && x < xmax) {
            const QString t = QString("%1NPU %2 ms  ·  latency %3 ms")
                                  .arg(m_npuLink.isEmpty() ? QString() : QStringLiteral("  ·  "))
                                  .arg(m_npuMs, 0, 'f', 1).arg(m_latMs, 0, 'f', 1);
            g.setFont(fi);
            g.setPen(withAlpha(QColor(0xc9, 0xb0, 0x86), aNPU));
            g.drawText(QRectF(x, yNPU, xmax - x, hN), Qt::AlignLeft | Qt::AlignVCenter,
                       mi.elidedText(t, Qt::ElideRight, xmax - x));
        }
    }
}
