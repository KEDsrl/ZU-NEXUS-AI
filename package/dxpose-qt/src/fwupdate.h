// ============================================================================
// fwupdate.h - Aggiornamento automatico del firmware del DX-M1.
//
// All'avvio si confronta il firmware del modulo (dxrt-cli -s) con quello
// incluso nell'immagine dal pacchetto dx-fw:
//     /lib/firmware/deepx/<chip>/<versione>/<formato>/fw.bin
//     es. /lib/firmware/deepx/m1/2.5.6/mdot2/fw.bin
// Se il modulo e' piu' VECCHIO, lo si aggiorna con "dxrt-cli -u <fw.bin>"
// (mai downgrade). Il nuovo firmware entra in funzione solo dopo un POWER
// CYCLE completo: fino ad allora il modulo riporta ancora la versione vecchia.
// Per non rifare il flash a ogni riavvio si registra l'esito in un file di
// stato persistente ($STATE_DIRECTORY, cioe' /var/lib/dxpose-qt con systemd).
// ============================================================================
#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class FirmwareUpdater : public QObject {
    Q_OBJECT
public:
    enum class Decision {
        UpToDate,          // firmware del modulo >= quello incluso
        NeedsUpdate,       // da aggiornare ora
        PowerCycleNeeded,  // flash gia' eseguito, manca lo spegnimento/riaccensione
        NoFirmwareFile,    // nessun fw.bin adatto nell'immagine
        Unknown            // dxrt-cli non ha risposto
    };

    explicit FirmwareUpdater(QObject *parent = nullptr);

    // Cerca il fw.bin piu' recente per chip/formato del modulo.
    //   deviceName: "M1, Accelerator type"   board: "M.2, Rev 1.5"
    static QString findFirmware(const QString &deviceName, const QString &board,
                                QString *version = nullptr,
                                const QString &root = "/lib/firmware/deepx");
    // -1 se a<b, 0 se uguali, 1 se a>b ("v2.5.6" e "2.5.6" equivalenti)
    static int compareVersions(const QString &a, const QString &b);

    Decision decide(const QString &currentFw, const QString &deviceName, const QString &board);

    QString firmwareFile() const    { return m_file; }
    QString bundledVersion() const  { return m_bundled; }
    QString currentVersion() const  { return m_current; }

    void setUpdateArgs(const QStringList &a) { m_args = a; }   // default: -u <file>
    void start();                   // asincrono: vedi segnali
    void forgetState();             // cancella il file di stato (forza un nuovo flash)

signals:
    void progress(const QString &line);
    void finished(bool ok, const QString &message);

private slots:
    void onReadyRead();
    void onFinished(int code, QProcess::ExitStatus st);

private:
    QString stateFile() const;
    void writeState(const QString &status);
    QString readStateStatus(QString *target = nullptr) const;

    QProcess   *m_proc = nullptr;
    QString     m_file, m_bundled, m_current;
    QStringList m_args{"-u"};
    QString     m_log;
};
