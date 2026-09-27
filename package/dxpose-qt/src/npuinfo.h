// ============================================================================
// npuinfo.h - Stato del DX-M1 letto da "dxrt-cli -s".
//
// Non esiste un'API pubblica di libdxrt per telemetria/temperature, quindi si
// analizza l'output del CLI. Il processo gira in background (QProcess async):
// il thread UI non si blocca mai anche se dxrt-cli si impunta.
// ============================================================================
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QVector>

struct NpuCore {
    int core = 0;
    int voltageMv = 0;
    int clockMhz = 0;
    int tempC = 0;
};

struct NpuStatus {
    bool ok = false;             // dxrt-cli ha risposto e il parsing e' riuscito
    QString rtVersion;           // "v3.3.2"
    QString driverVersion;       // RT Driver
    QString pcieDriverVersion;
    QString fwVersion;
    QString memory;              // "LPDDR5 5600 Mbps, 3.92GiB"
    QString board;               // "M.2, Rev 1.0"
    QString pcie;                // "Gen1 X2 [01:00:00]"
    QString deviceName;          // "M1, Accelerator type"
    QVector<NpuCore> cores;

    int maxTempC() const {
        int t = 0;
        for (const NpuCore &c : cores) t = qMax(t, c.tempC);
        return t;
    }
};
Q_DECLARE_METATYPE(NpuStatus)

class NpuInfo : public QObject {
    Q_OBJECT
public:
    explicit NpuInfo(QObject *parent = nullptr, int intervalMs = 2000);

    const NpuStatus &status() const { return m_st; }
    static bool devicePresent();          // /dev/dxrt* esiste?
    static NpuStatus parse(const QString &text);   // esposta per i test
    // "v2.5.6" >= 2.5.2 ?  (prefisso 'v' opzionale, componenti mancanti = 0)
    static bool versionAtLeast(const QString &ver, int maj, int min, int patch);
    // Lettura sincrona una tantum (avvio): esegue dxrt-cli -s con timeout.
    static NpuStatus probe(int timeoutMs = 5000);
    // Sospende il polling (durante l'aggiornamento firmware nessun altro
    // processo deve interrogare il modulo).
    void setPaused(bool p) { m_paused = p; }

signals:
    void updated(const NpuStatus &st);

private slots:
    void poll();
    void finished(int code, QProcess::ExitStatus st);

private:
    QProcess  *m_proc = nullptr;
    NpuStatus  m_st;
    bool       m_busy = false;
    bool       m_paused = false;
};
