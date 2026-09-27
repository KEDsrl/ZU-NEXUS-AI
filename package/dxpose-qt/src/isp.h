// ============================================================================
// isp.h - Configurazione nativa della catena video IMX219 in PL.
//
// Sostituisce setup_pipeline.sh + tune_isp.sh: niente QProcess, tutto via
// ioctl su /dev/media* e /dev/v4l-subdev*.
//
//   IMX219 -> CSI2RX -> Demosaic -> CSC(WB) -> Gamma -> VPSS(scaler+CSC) -> /dev/videoN
//
// Le entity sono cercate per SUFFISSO (".csi2rx", ".demosaic", ".csc",
// ".gamma", ".vpss") e non per indirizzo, cosi' un remap della memoria in
// Vivado non rompe l'app.
// ============================================================================
#pragma once

#include <QString>
#include <QStringList>

class IspPipeline {
public:
    struct Config {
        int  sensorW = 1920, sensorH = 1080;   // risoluzione ISP (nativa del sensore)
        // Risoluzione di uscita: la fa il SCALER del VPSS (v_proc_ss_0, topology 0
        // Scaler-only con C_ENABLE_CSC). Qualunque valore <= sensore, nessuna
        // ricompilazione del bitstream.
        int  outW = 1280, outH = 720;
        // false = BGR3 (V4L2_PIX_FMT_BGR24): nome DTS "rgb888", richiede CONFIG.HAS_BGR8.
        //         E' quello che funziona sul bitstream attuale.
        // true  = RGB3 (V4L2_PIX_FMT_RGB24): nome DTS "bgr888", richiede CONFIG.HAS_RGB8.
        // ATTENZIONE: i nomi DTS sono INVERTITI rispetto ai fourcc V4L2, e il driver
        // frmbuf non valida contro l'IP: un formato dichiarato ma non implementato
        // viene enumerato lo stesso e produce dati NON VALIDI, senza errori.
        bool rgb = false;
    };

    // Controlli ISP. Scale come le espone il driver (identiche a tune_isp.sh).
    //
    // Default = taratura sul campo (lab, luce interna). DEVONO restare allineati
    // ai valori in setup_pipeline.sh: sono la stessa board, e due sorgenti di
    // verita' divergenti darebbero colori diversi a seconda di chi configura.
    //
    // In questa taratura il verde e' 50, cioe' ESATTAMENTE unitario: la CSC non
    // fa piu' da boost globale (come nella taratura precedente, dove tutti i
    // canali erano >50) e i gain sono bilanciamento puro. E' anche la condizione
    // ideale per l'AWB gray-world, che tiene il verde come riferimento fisso.
    // La luminosita' viene dal sensore: analogueGain al massimo (232) ed
    // esposizione breve (784, meno mosso) compensata dal guadagno digitale
    // (660 = circa 2,6x).
    struct Ctrls {
        int wbRed = 60, wbGreen = 50, wbBlue = 86;   // 0..100, 50 = unitario
        int brightness = 43, contrast = 49;          // 0..100
        int gamma = 7;                               // 1..40, 10 = 1.0 (esponente x10)
        int exposure = 784;                          // 4..1759
        int analogueGain = 232;                      // 0..232 (qui: massimo)
        int digitalGain = 660;                       // 256..4095 (256 = 1x -> ~2,6x)
    };

    // Limiti dei driver (xilinx-vpss-csc, xilinx-gamma, imx219)
    static constexpr int kGainMin = 0,   kGainMax = 100, kGainDef = 50;
    static constexpr int kGammaMin = 1,  kGammaMax = 40,  kGammaDef = 10;
    static constexpr int kExpMin = 4,    kExpMax = 1759;
    static constexpr int kAGainMin = 0,  kAGainMax = 232;
    static constexpr int kDGainMin = 256, kDGainMax = 4095;

    bool discover(QString *err = nullptr);              // trova media device ed entity
    bool configure(const Config &cfg, QString *err = nullptr);  // formati su tutti i pad + nodo video
    bool applyCtrls(const Ctrls &c);                    // scrive tutti i controlli
    bool readCtrls(Ctrls &c) const;                     // rilegge dai driver

    // Setter singoli (per gli slider: scrivono un solo controllo)
    bool setWbRed(int v);
    bool setWbGreen(int v);
    bool setWbBlue(int v);
    bool setBrightness(int v);
    bool setContrast(int v);
    bool setGamma(int v);          // scrive i tre canali insieme
    bool setExposure(int v);
    bool setAnalogueGain(int v);
    bool setDigitalGain(int v);

    bool    valid()        const { return !m_video.isEmpty(); }
    QString videoDevice()  const { return m_video; }
    QString mediaDevice()  const { return m_media; }
    QString sensorName()   const { return m_sensorName; }
    Config  current()      const { return m_cfg; }

private:
    bool setPadFmt(const QString &dev, int pad, unsigned code, int w, int h) const;
    bool setCtrl(const QString &dev, unsigned id, int val) const;
    bool getCtrl(const QString &dev, unsigned id, int &val) const;

    QString m_media, m_video;
    QString m_sensor, m_csi, m_demosaic, m_csc, m_gamma, m_vpss;   // path dei subdev
    QString m_sensorName;
    Config  m_cfg;
};

// ============================================================================
// Gray-world AWB.
//
// Il gain del CSC e' un intero 0..100 che il driver mappa in
//     stored = 2*val + 20      (default 50 -> 120 = moltiplicatore unitario)
// e i coefficienti della matrice vengono scalati proporzionalmente a 'stored'.
// Quindi per applicare un fattore f a un canale:
//     val_new = ((2*val_old + 20) * f - 20) / 2
//
// Il verde resta il RIFERIMENTO e non viene toccato: cosi' l'AWB corregge la
// dominante senza alterare la luminosita' impostata a mano dall'utente.
// ============================================================================
class GrayWorldAwb {
public:
    struct Stats { double meanR = 0, meanG = 0, meanB = 0; bool valid = false; };

    // Medie su griglia sottocampionata, scartando i pixel bruciati/neri (che
    // falserebbero il bilanciamento). 'stride' e' il passo in byte per riga.
    // 'bgrOrder' = i byte sono B,G,R invece che R,G,B.
    static Stats analyze(const unsigned char *data, int w, int h, int stride,
                         bool bgrOrder, int step = 8);

    void reset() { m_first = true; }
    void setSpeed(double s) { m_speed = s; }   // 0..1: quota di correzione per passo

    // Calcola i nuovi gain R e B. Ritorna false se lo scatto non e' utilizzabile
    // (immagine troppo scura/piatta: meglio non correggere che sbagliare).
    bool step(const Stats &st, int gainRIn, int gainBIn, int &gainROut, int &gainBOut);

private:
    static double toMul(int val)   { return (2.0 * val + 20.0) / 120.0; }
    static int    fromMul(double m);
    double m_speed = 0.25;
    bool   m_first = true;
};
