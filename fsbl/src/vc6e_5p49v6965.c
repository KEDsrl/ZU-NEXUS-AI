/******************************************************************************
* vc6e_5p49v6965.c
*
* Driver VersaClock 6E (5P49V6965) per FSBL Zynq UltraScale+.
* Usa il controller PS I2C tramite il driver Xilinx XIicPs in modalita'
* polling (no interrupt) — adatto al contesto FSBL dove gli interrupt non
* sono ancora abilitati.
******************************************************************************/

#include "vc6e_5p49v6965.h"
#include "vc6e_5p49v6965_regs.h"   /* sequenza vc6e_init_seq[] */

#include "xiicps.h"
#include "xparameters.h"
#include "sleep.h"

/* Sistema di logging:
 *   - Quando il driver gira nel contesto FSBL, definisci VC6E_FSBL_CONTEXT
 *     nei symbol di compilazione. In quel caso usiamo XFsbl_Printf, che
 *     rispetta i livelli di debug DEBUG_GENERAL/INFO/DETAILED definiti
 *     in xfsbl_config.h, e non spamma l'UART in produzione (-DFSBL_DEBUG=0).
 *   - Negli altri casi (test standalone, second-stage app, ...) usiamo
 *     direttamente xil_printf.
 */
#ifdef VC6E_FSBL_CONTEXT
#  include "xfsbl_main.h"
#  define VC6E_LOG(...)   XFsbl_Printf(DEBUG_GENERAL, __VA_ARGS__)
#  define VC6E_LOG_INFO(...)  XFsbl_Printf(DEBUG_INFO, __VA_ARGS__)
#else
#  include "xil_printf.h"
#  define VC6E_LOG(...)       xil_printf(__VA_ARGS__)
#  define VC6E_LOG_INFO(...)  xil_printf(__VA_ARGS__)
#endif

/* Istanza globale del driver I2C. In FSBL e' accettabile dato che il
 * controller I2C non viene riusato altrove durante il boot.
 */
static XIicPs IicInstance;
static int    g_iic_initialized = 0;

/* Loop count grossolano per il check "bus idle". In FSBL non abbiamo
 * ancora i timer applicativi, ci affidiamo al blocking dell'XIicPs API
 * piu' un timeout di sanita'.
 */
#define VC6E_I2C_BUS_BUSY_TIMEOUT_LOOPS  100000U

/* -------------------------------------------------------------------------- */
/* Inizializzazione del controller I2C                                        */
/* -------------------------------------------------------------------------- */
static int vc6e_iic_setup(void)
{
    XIicPs_Config *cfg;
    int status;

    if (g_iic_initialized) {
        return XST_SUCCESS;
    }

    cfg = XIicPs_LookupConfig(VC6E_I2C_DEVICE_ID);
    if (cfg == NULL) {
        VC6E_LOG("[VC6E] XIicPs_LookupConfig failed\r\n");
        return XST_FAILURE;
    }

    status = XIicPs_CfgInitialize(&IicInstance, cfg, cfg->BaseAddress);
    if (status != XST_SUCCESS) {
        VC6E_LOG("[VC6E] XIicPs_CfgInitialize failed (%d)\r\n", status);
        return status;
    }

    /* Self-test consigliato dal driver Xilinx */
    status = XIicPs_SelfTest(&IicInstance);
    if (status != XST_SUCCESS) {
        VC6E_LOG("[VC6E] XIicPs_SelfTest failed (%d)\r\n", status);
        return status;
    }

    status = XIicPs_SetSClk(&IicInstance, VC6E_I2C_SCLK_HZ);
    if (status != XST_SUCCESS) {
        VC6E_LOG("[VC6E] XIicPs_SetSClk failed (%d)\r\n", status);
        return status;
    }

    g_iic_initialized = 1;
    return XST_SUCCESS;
}

/* Aspetta che il bus si liberi (no transazioni in volo). */
static int vc6e_wait_bus_idle(void)
{
    u32 i;
    for (i = 0; i < VC6E_I2C_BUS_BUSY_TIMEOUT_LOOPS; i++) {
        if (!XIicPs_BusIsBusy(&IicInstance)) {
            return XST_SUCCESS;
        }
    }
    return XST_FAILURE;
}

/* -------------------------------------------------------------------------- */
/* Primitive di lettura/scrittura registro                                    */
/* -------------------------------------------------------------------------- */
int VC6E_WriteReg(u8 reg_addr, u8 value)
{
    u8  buf[2];
    int status;

    if (!g_iic_initialized) {
        status = vc6e_iic_setup();
        if (status != XST_SUCCESS) return status;
    }

    buf[0] = reg_addr;
    buf[1] = value;

    if (vc6e_wait_bus_idle() != XST_SUCCESS) {
        VC6E_LOG("[VC6E] bus busy on WriteReg(0x%02x)\r\n", reg_addr);
        return XST_FAILURE;
    }

    status = XIicPs_MasterSendPolled(&IicInstance, buf, 2, VC6E_I2C_ADDR);
    if (status != XST_SUCCESS) {
        VC6E_LOG("[VC6E] MasterSendPolled failed reg=0x%02x (%d)\r\n",
                   reg_addr, status);
        return status;
    }

    if (vc6e_wait_bus_idle() != XST_SUCCESS) {
        return XST_FAILURE;
    }

    return XST_SUCCESS;
}

int VC6E_ReadReg(u8 reg_addr, u8 *value)
{
    int status;

    if (value == NULL) return XST_INVALID_PARAM;

    if (!g_iic_initialized) {
        status = vc6e_iic_setup();
        if (status != XST_SUCCESS) return status;
    }

    if (vc6e_wait_bus_idle() != XST_SUCCESS) return XST_FAILURE;

    /* fase 1: write del register pointer */
    status = XIicPs_MasterSendPolled(&IicInstance, &reg_addr, 1,
                                     VC6E_I2C_ADDR);
    if (status != XST_SUCCESS) return status;

    if (vc6e_wait_bus_idle() != XST_SUCCESS) return XST_FAILURE;

    /* fase 2: read del dato */
    status = XIicPs_MasterRecvPolled(&IicInstance, value, 1, VC6E_I2C_ADDR);
    if (status != XST_SUCCESS) return status;

    if (vc6e_wait_bus_idle() != XST_SUCCESS) return XST_FAILURE;

    return XST_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* Esecuzione sequenza con WAIT integrato                                     */
/* -------------------------------------------------------------------------- */
int VC6E_RunSequence(const vc6e_step_t *seq)
{
    int status;
    u32 idx = 0;

    if (seq == NULL) return XST_INVALID_PARAM;

    while (seq[idx].op != VC6E_OP_END) {
        switch (seq[idx].op) {
        case VC6E_OP_WRITE:
            status = VC6E_WriteReg(seq[idx].reg, seq[idx].val);
            if (status != XST_SUCCESS) {
                VC6E_LOG("[VC6E] step[%lu] WRITE 0x%02x=0x%02x FAILED\r\n",
                           (unsigned long)idx, seq[idx].reg, seq[idx].val);
                return status;
            }
            break;

        case VC6E_OP_WAIT_MS:
            /* val e' espresso in millisecondi (max 255 ms per step) */
            usleep((u32)seq[idx].val * 1000U);
            break;

        case VC6E_OP_END:
        default:
            return XST_SUCCESS;
        }
        idx++;
    }
    return XST_SUCCESS;
}

int VC6E_VerifySequence(const vc6e_step_t *seq)
{
    u32 idx = 0;
    u32 mismatches = 0;
    int status;
    u8  rb;

    if (seq == NULL) return -1;

    /* Lista di registri da SKIPPARE durante la verify perche':
     *   - 0x00: contiene l'I2C device address; alla rilettura il chip
     *           potrebbe gia' rispondere su un nuovo address (vedi commento
     *           RICBox). Inoltre alcuni bit sono OTP/status.
     *   - 0x06, 0x07, 0x08: RSVD_TEMPY / RSVD_OFFSET_TBIN / RSVD_GAIN.
     *           Factory trim registers, read-only nei bit di trim. RICBox
     *           li scrive a 0x00 ma il chip mostra il valore di calibrazione
     *           reale (tipicamente 0xFF su parti non programmate dall'utente).
     *   - 0x76: VCO calibration trigger; bit auto-clearing.
     *   - 0x77, 0x75: registri di stato calibrazione, contengono flag
     *           dinamici.
     */
    static const u8 verify_skip[] = {
        0x00U, 0x06U, 0x07U, 0x08U,
        0x75U, 0x76U, 0x77U
    };

    /* Per i registri scritti PIU' VOLTE nella sequenza (es. 0x1C), il
     * confronto deve essere fatto solo contro l'ULTIMO valore scritto,
     * perche' e' quello effettivamente residente nel chip. Pre-calcoliamo
     * la "expected map" scorrendo tutta la sequenza in ordine, in modo che
     * l'ultima scrittura sovrascriva le precedenti. */
    u8 expected[256];
    u8 expected_set[256];
    u32 i;
    for (i = 0; i < 256U; i++) {
        expected[i] = 0;
        expected_set[i] = 0;
    }
    while (seq[idx].op != VC6E_OP_END) {
        if (seq[idx].op == VC6E_OP_WRITE) {
            expected[seq[idx].reg] = seq[idx].val;
            expected_set[seq[idx].reg] = 1;
        }
        idx++;
    }

    /* Scorre i registri 0x01..0x69 (range RAM user-config) e verifica solo
     * quelli effettivamente presenti nella sequenza e non nella skip-list. */
    for (u32 reg = 0x01U; reg <= 0x69U; reg++) {

        if (!expected_set[reg]) continue;

        u32 skip = 0;
        for (u32 k = 0; k < sizeof(verify_skip); k++) {
            if (verify_skip[k] == reg) { skip = 1; break; }
        }
        if (skip) continue;

        status = VC6E_ReadReg((u8)reg, &rb);
        if (status != XST_SUCCESS) {
            VC6E_LOG("[VC6E] verify: read reg=0x%02x FAILED (%d)\r\n",
                     reg, status);
            return -2;
        }
        if (rb != expected[reg]) {
            VC6E_LOG("[VC6E] verify MISMATCH reg=0x%02x "
                     "exp=0x%02x got=0x%02x\r\n",
                     reg, expected[reg], rb);
            mismatches++;
        }
    }

    if (mismatches == 0U) {
        VC6E_LOG("[VC6E] verify OK\r\n");
    } else {
        VC6E_LOG("[VC6E] verify: %lu mismatches\r\n",
                 (unsigned long)mismatches);
    }
    return (int)mismatches;
}

/* -------------------------------------------------------------------------- */
/* Entry point di alto livello chiamato dall'FSBL                             */
/* -------------------------------------------------------------------------- */
int VC6E_Init(void)
{
    int status;
    u8  probe;

    VC6E_LOG("[VC6E] init 5P49V6965 @0x%02x on I2C%d\r\n",
               VC6E_I2C_ADDR, VC6E_I2C_DEVICE_ID);

    status = vc6e_iic_setup();
    if (status != XST_SUCCESS) return status;

    /* Probe: leggiamo il registro 0x00 per verificare che il chip risponda.
     * Se questo fallisce, controlla:
     *   - alimentazione del VersaClock
     *   - pull-up SDA/SCL (tipicamente 2.2-4.7 kOhm verso VDD I2C)
     *   - pinmux MIO della Zynq sui pin I2C corretti
     *   - che il pin OUT0_SEL_I2CB sia basso a POR (modalita' I2C)
     */
    status = VC6E_ReadReg(0x00U, &probe);
    if (status != XST_SUCCESS) {
        VC6E_LOG("[VC6E] probe FAILED: chip non risponde\r\n");
        return status;
    }
    VC6E_LOG("[VC6E] probe OK, reg0x00 = 0x%02x\r\n", probe);

    /* Esegue la sequenza completa: write registri + calibrazione PLL */
    status = VC6E_RunSequence(vc6e_init_seq);
    if (status != XST_SUCCESS) {
        VC6E_LOG("[VC6E] RunSequence FAILED\r\n");
        return status;
    }

    /* Delay aggiuntivo per garantire il PLL lock prima che le periferiche
     * a valle inizino ad usare le clock di output. La sequenza include
     * gia' un WAIT 64 ms post-calibrazione, ma un margine extra non
     * costa nulla in fase di boot.
     */
    usleep(10000); /* 10 ms */

#ifdef VC6E_VERIFY_AFTER_INIT
    /* Read-back verification: utile durante il bring-up. Definisci la
     * macro in compilazione (es. nei symbol del progetto Vitis) per
     * abilitarla. Disabilitala in produzione.
     */
    (void)VC6E_VerifySequence(vc6e_init_seq);
#endif

    VC6E_LOG("[VC6E] init OK -- "
               "OUT1=26MHz OUT2=27MHz OUT3=100MHz OUT4=100MHz\r\n");
    return XST_SUCCESS;
}
