/******************************************************************************
* vc6e_5p49v6965.h
*
* Driver per Renesas/IDT VersaClock 6E 5P49V6965 via I2C.
* Pensato per essere chiamato dall'FSBL della Zynq UltraScale+ MPSoC.
*
* Configurazione applicata (vedi vc6e_5p49v6965_regs.h):
*   - Reference clock: 25 MHz
*   - OUT1 = 26 MHz
*   - OUT2 = 27 MHz
*   - OUT3 = 100 MHz
*   - OUT4 = 100 MHz
*
* Target: Xilinx Vitis SDK / standalone BSP (XIicPs driver in polled mode).
*
* NOTE FONDAMENTALI:
*  - L'indirizzo I2C a 7 bit di default e' 0x6A (con I2C_ADDR pin = 0).
*    Diventa 0x6B se il pin I2C_ADDR e' alto. Verifica sul tuo schematico.
*  - Il flusso di init segue ESATTAMENTE l'export RICBox (versaclock6e
*    1.2.1): scrittura registri 0x01..0x69, poi sequenza di calibrazione
*    PLL, infine scrittura del registro 0x00.
*  - Il registro 0x00 va scritto per ULTIMO perche' i suoi bit alti
*    contengono l'I2C device address: scriverlo prima cambierebbe
*    l'indirizzo del chip a meta' sequenza.
******************************************************************************/

#ifndef VC6E_5P49V6965_H
#define VC6E_5P49V6965_H

#include "xil_types.h"
#include "xstatus.h"

/* Indirizzo I2C a 7 bit del 5P49V6965.
 * Modificalo se sul tuo PCB hai pull-up sul pin I2C_ADDR (-> 0x6B).
 */
#ifndef VC6E_I2C_ADDR
#define VC6E_I2C_ADDR        0x6AU
#endif

/* Clock I2C in Hz. Tipicamente 100 kHz (standard) o 400 kHz (fast).
 * Il VersaClock 6E supporta fino a 400 kHz.
 */
#ifndef VC6E_I2C_SCLK_HZ
#define VC6E_I2C_SCLK_HZ     100000U
#endif

/* Quale controller I2C usare. Default: I2C0.
 * Sovrascrivilo da fuori per usare I2C1.
 */
#ifndef VC6E_I2C_DEVICE_ID
#define VC6E_I2C_DEVICE_ID   XPAR_XIICPS_0_DEVICE_ID
#endif

/* Tipo dello step in una sequenza di inizializzazione. */
typedef enum {
    VC6E_OP_WRITE = 0,   /* scrittura di un registro */
    VC6E_OP_WAIT_MS,     /* delay in millisecondi (val = ms)        */
    VC6E_OP_END          /* terminatore della sequenza              */
} vc6e_op_t;

/* Singolo step della sequenza di inizializzazione. */
typedef struct {
    vc6e_op_t op;
    u8        reg;   /* significativo solo se op == VC6E_OP_WRITE  */
    u8        val;   /* valore (WRITE) oppure ms (WAIT_MS, in u8)  */
} vc6e_step_t;

/* API */

/**
 * Inizializza il controller I2C e programma il VersaClock con la sequenza
 * di default (vc6e_init_seq[]).
 *
 * @return XST_SUCCESS se tutto OK, altrimenti codice di errore Xilinx.
 */
int VC6E_Init(void);

/**
 * Scrive un singolo registro del VersaClock.
 */
int VC6E_WriteReg(u8 reg_addr, u8 value);

/**
 * Legge un singolo registro del VersaClock.
 */
int VC6E_ReadReg(u8 reg_addr, u8 *value);

/**
 * Esegue una sequenza di step (write + wait). Si ferma al primo VC6E_OP_END.
 *
 * @return XST_SUCCESS se tutto OK, altrimenti codice di errore.
 */
int VC6E_RunSequence(const vc6e_step_t *seq);

/**
 * Rilegge i registri di sola scrittura della sequenza e confronta con il
 * valore atteso. Utile per debug del primo bring-up.
 *
 * @return numero di mismatch (0 = OK), oppure < 0 in caso di errore I2C.
 */
int VC6E_VerifySequence(const vc6e_step_t *seq);

#endif /* VC6E_5P49V6965_H */
