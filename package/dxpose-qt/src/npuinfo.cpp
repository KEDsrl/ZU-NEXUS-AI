#include "npuinfo.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>

bool NpuInfo::devicePresent() {
    if (QFileInfo::exists("/dev/dxrt0")) return true;
    const QStringList n = QDir("/dev").entryList(QStringList() << "dxrt*",
                                                 QDir::System | QDir::AllEntries | QDir::NoDotAndDotDot);
    return !n.isEmpty();
}

// Analizza l'output di dxrt-cli -s. Formato di riferimento (v3.3.2):
//
//   DXRT v3.3.2
//    * Device 0: M1, Accelerator type
//    * RT Driver version   : v2.4.1
//    * PCIe Driver version : v2.2.0
//    * FW version          : v2.5.6
//    * Memory : LPDDR5 5600 Mbps, 3.92GiB
//    * Board  : M.2, Rev 1.0
//    * PCIe   : Gen1 X2 [01:00:00]
//   NPU 0: voltage 750 mV, clock 1000 MHz, temperature 61'C
NpuStatus NpuInfo::parse(const QString &text) {
    NpuStatus s;
    if (text.trimmed().isEmpty()) return s;

    auto grab = [&text](const QString &pat) -> QString {
        QRegularExpression re(pat, QRegularExpression::CaseInsensitiveOption);
        const auto m = re.match(text);
        return m.hasMatch() ? m.captured(1).trimmed() : QString();
    };

    s.rtVersion        = grab(R"(DXRT\s+(v[\d.]+))");
    s.deviceName       = grab(R"(Device\s+\d+\s*:\s*([^\r\n]+))");
    s.driverVersion    = grab(R"(RT Driver version\s*:\s*(\S+))");
    s.pcieDriverVersion= grab(R"(PCIe Driver version\s*:\s*(\S+))");
    s.fwVersion        = grab(R"(FW version\s*:\s*(\S+))");
    s.memory           = grab(R"(Memory\s*:\s*([^\r\n]+))");
    s.board            = grab(R"(Board\s*:\s*([^\r\n]+))");
    s.pcie             = grab(R"(PCIe\s*:\s*([^\r\n]+))");

    // NPU 0: voltage 750 mV, clock 1000 MHz, temperature 61'C
    // L'apostrofo prima della C non e' garantito su tutte le versioni: reso opzionale.
    QRegularExpression re(R"(NPU\s+(\d+)\s*:\s*voltage\s+(\d+)\s*mV\s*,\s*clock\s+(\d+)\s*MHz\s*,\s*temperature\s+(\d+)\s*'?\s*C)",
                          QRegularExpression::CaseInsensitiveOption);
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        NpuCore c;
        c.core      = m.captured(1).toInt();
        c.voltageMv = m.captured(2).toInt();
        c.clockMhz  = m.captured(3).toInt();
        c.tempC     = m.captured(4).toInt();
        s.cores.append(c);
    }
    s.ok = !s.cores.isEmpty() || !s.rtVersion.isEmpty();
    return s;
}

NpuInfo::NpuInfo(QObject *parent, int intervalMs) : QObject(parent) {
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &NpuInfo::finished);

    auto *t = new QTimer(this);
    connect(t, &QTimer::timeout, this, &NpuInfo::poll);
    t->start(intervalMs);
    QTimer::singleShot(0, this, &NpuInfo::poll);
}

void NpuInfo::poll() {
    if (m_busy || m_paused || !devicePresent()) return;
    m_busy = true;
    m_proc->start("dxrt-cli", QStringList() << "-s");
}

void NpuInfo::finished(int, QProcess::ExitStatus) {
    const QString out = QString::fromUtf8(m_proc->readAll());
    m_busy = false;
    const NpuStatus s = parse(out);
    // Se un ciclo fallisce (device occupato) teniamo l'ultimo stato valido:
    // meglio un dato di 2 s fa che un pannello che lampeggia "n/d".
    if (s.ok) { m_st = s; emit updated(m_st); }
}

bool NpuInfo::versionAtLeast(const QString &ver, int maj, int min, int patch) {
    QString v = ver.trimmed();
    if (v.startsWith('v') || v.startsWith('V')) v.remove(0, 1);
    const QStringList p = v.split('.');
    const int a = p.value(0).toInt(), b = p.value(1).toInt(), c = p.value(2).toInt();
    if (a != maj) return a > maj;
    if (b != min) return b > min;
    return c >= patch;
}

NpuStatus NpuInfo::probe(int timeoutMs) {
    if (!devicePresent()) return NpuStatus();
    QProcess pr;
    pr.setProcessChannelMode(QProcess::MergedChannels);
    pr.start("dxrt-cli", QStringList() << "-s");
    if (!pr.waitForFinished(timeoutMs)) { pr.kill(); pr.waitForFinished(1000); return NpuStatus(); }
    return parse(QString::fromUtf8(pr.readAll()));
}
