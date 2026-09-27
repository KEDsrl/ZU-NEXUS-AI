#include "fwupdate.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTimer>

FirmwareUpdater::FirmwareUpdater(QObject *parent) : QObject(parent) {}

int FirmwareUpdater::compareVersions(const QString &a, const QString &b) {
    auto parts = [](QString v) {
        v = v.trimmed();
        if (v.startsWith('v') || v.startsWith('V')) v.remove(0, 1);
        return v.split('.');
    };
    const QStringList pa = parts(a), pb = parts(b);
    for (int i = 0; i < qMax(pa.size(), pb.size()); ++i) {
        const int x = pa.value(i).toInt(), y = pb.value(i).toInt();
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

QString FirmwareUpdater::findFirmware(const QString &deviceName, const QString &board,
                                      QString *version, const QString &root) {
    // chip: primo token del nome del device ("M1, Accelerator type" -> "m1")
    const QString chip = deviceName.section(',', 0, 0).trimmed().toLower();
    // formato: "M.2, Rev 1.5" -> "mdot2" (convenzione delle cartelle di dx_fw)
    QString form = board.section(',', 0, 0).trimmed().toLower();
    form.replace('.', "dot");

    QString best, bestVer;
    QDirIterator it(root, QStringList() << "fw.bin", QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString f = it.next();
        // atteso: <root>/<chip>/<versione>/<formato>/fw.bin
        const QStringList rel = QDir(root).relativeFilePath(f).split('/');
        if (rel.size() != 4) continue;
        if (!chip.isEmpty() && rel[0].toLower() != chip) continue;
        if (!form.isEmpty() && rel[2].toLower() != form) continue;
        if (best.isEmpty() || compareVersions(rel[1], bestVer) > 0) { best = f; bestVer = rel[1]; }
    }
    if (version) *version = bestVer;
    return best;
}

QString FirmwareUpdater::stateFile() const {
    QString dir = qEnvironmentVariable("STATE_DIRECTORY");   // systemd StateDirectory=
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/dxpose-qt";
    QDir().mkpath(dir);
    return dir + "/dxm1-fw-update.json";
}

void FirmwareUpdater::writeState(const QString &status) {
    QJsonObject o;
    o["status"] = status;
    o["from"] = m_current;
    o["target"] = m_bundled;
    o["file"] = m_file;
    o["time"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    QFile f(stateFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(QJsonDocument(o).toJson());
        f.flush();
        f.close();
    }
}

QString FirmwareUpdater::readStateStatus(QString *target) const {
    QFile f(stateFile());
    if (!f.open(QIODevice::ReadOnly)) return QString();
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (target) *target = o.value("target").toString();
    return o.value("status").toString();
}

void FirmwareUpdater::forgetState() { QFile::remove(stateFile()); }

FirmwareUpdater::Decision FirmwareUpdater::decide(const QString &currentFw,
                                                  const QString &deviceName,
                                                  const QString &board) {
    m_current = currentFw;
    if (currentFw.isEmpty()) return Decision::Unknown;
    m_file = findFirmware(deviceName, board, &m_bundled);
    if (m_file.isEmpty()) {
        qWarning().noquote() << "[fw] nessun fw.bin per" << deviceName << "/" << board;
        return Decision::NoFirmwareFile;
    }
    if (compareVersions(currentFw, m_bundled) >= 0) {
        forgetState();   // allineato (anche dopo un power cycle riuscito)
        return Decision::UpToDate;
    }
    // Modulo vecchio: se il flash di QUESTA versione e' gia' stato fatto,
    // manca solo il power cycle -> non riflashare a ogni avvio.
    QString target;
    if (readStateStatus(&target) == "flashed" && compareVersions(target, m_bundled) == 0)
        return Decision::PowerCycleNeeded;
    return Decision::NeedsUpdate;
}

void FirmwareUpdater::start() {
    if (m_proc) return;
    m_log.clear();
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(m_proc, &QProcess::readyRead, this, &FirmwareUpdater::onReadyRead);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &FirmwareUpdater::onFinished);
    writeState("in-progress");
    QStringList args = m_args;
    args << m_file;
    qWarning().noquote() << "[fw] aggiornamento DX-M1:" << m_current << "->" << m_bundled
                         << ": dxrt-cli" << args.join(' ');
    emit progress(QString("dxrt-cli %1").arg(args.join(' ')));
    m_proc->start("dxrt-cli", args);
    // Se dxrt-cli chiedesse conferma (y/n), la diamo; altrimenti e' innocuo.
    m_proc->write("y\n");
    m_proc->closeWriteChannel();
    // Limite di sicurezza: un flash non dura minuti. Oltre, si segnala l'errore
    // (senza uccidere il processo: interrompere un flash e' peggio che attendere).
    QTimer::singleShot(10 * 60 * 1000, this, [this] {
        if (m_proc && m_proc->state() != QProcess::NotRunning)
            emit progress("firmware update is taking unusually long...");
    });
}

void FirmwareUpdater::onReadyRead() {
    const QString chunk = QString::fromUtf8(m_proc->readAll());
    m_log += chunk;
    for (const QString &l : chunk.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts)) {
        qInfo().noquote() << "[fw]" << l.trimmed();
        emit progress(l.trimmed());
    }
}

void FirmwareUpdater::onFinished(int code, QProcess::ExitStatus st) {
    m_log += QString::fromUtf8(m_proc->readAll());
    m_proc->deleteLater();
    m_proc = nullptr;
    const bool failWord = m_log.contains("fail", Qt::CaseInsensitive) ||
                          m_log.contains("error", Qt::CaseInsensitive) ||
                          m_log.contains("exception", Qt::CaseInsensitive);
    const bool ok = st == QProcess::NormalExit && code == 0 && !failWord;
    if (ok) {
        writeState("flashed");
        emit finished(true, QString("Firmware %1 written to the DX-M1.").arg(m_bundled));
    } else {
        writeState("failed");
        const QString last = m_log.trimmed().section('\n', -1).trimmed();
        emit finished(false, QString("dxrt-cli exit code %1%2")
                                 .arg(code).arg(last.isEmpty() ? "" : ": " + last));
    }
}
