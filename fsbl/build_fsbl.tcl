# ============================================================================
# build_fsbl.tcl - Build automatica FSBL con init VersaClock 5P49V6965
#
# Uso (da shell, con l'ambiente Vitis 2023.2 caricato):
#     xsct build_fsbl.tcl [percorso XSA]
#
# Default XSA: ../system_wrapper.xsa (rispetto a questa cartella).
# Output: ./fsbl_ws/ked_fsbl/Debug/ked_fsbl.elf
#
# Cosa fa:
#   1. crea workspace + platform dall'XSA (dominio standalone su A53-0)
#   2. crea l'app FSBL dal template "Zynq MP FSBL"
#   3. importa i sorgenti da ./src sovrascrivendo xfsbl_board.c stock e
#      aggiungendo il driver vc6e_*
#   4. definisce VC6E_FSBL_CONTEXT (logging via XFsbl_Printf)
#   5. compila
# ============================================================================

set script_dir [file dirname [file normalize [info script]]]

set xsa [lindex $argv 0]
if { $xsa eq "" } { set xsa [file normalize "$script_dir/../system_wrapper.xsa"] }
if { ![file exists $xsa] } { error "XSA non trovato: $xsa" }

set ws "$script_dir/fsbl_ws"
file delete -force $ws
setws $ws

puts ">>> platform create da $xsa"
platform create -name ked_plat -hw $xsa -proc psu_cortexa53_0 -os standalone
platform write

# ----------------------------------------------------------------------------
# I driver standalone degli IP video (v_demosaic, v_proc_ss, v_frmbuf_wr,
# v_gamma_lut) e dell'IP custom AXI_DNA non compilano nei BSP di FSBL e
# PMUFW (e non servono in quel contesto). Li assegniamo a "none" in tutti i
# domini della platform PRIMA di platform generate, replicando in automatico
# il workaround manuale "driver = null" delle BSP Settings.
# ----------------------------------------------------------------------------
set null_driver_ips [list \
    AXI_DNA_0 \
    v_demosaic_0 \
    v_gamma_lut_0 \
    v_proc_ss_0 v_proc_ss_1 v_proc_ss_2 \
    v_frmbuf_wr_0 v_frmbuf_wr_1 \
]

proc vc6e_disable_drivers { domain_name ips } {
    if { [catch { domain active $domain_name } msg] } {
        puts "    dominio $domain_name non presente, skip ($msg)"
        return
    }
    foreach ip $ips {
        if { [catch { bsp setdriver -ip $ip -driver none } msg] } {
            puts "    ($domain_name) $ip: $msg"
        } else {
            puts "    ($domain_name) $ip -> driver none"
        }
    }
    catch { bsp regenerate }
}

puts ">>> assegnazione driver none agli IP video/DNA nei domini boot"
# I nomi standard dei domini boot creati da platform create sono
# zynqmp_fsbl e zynqmp_pmufw; standalone_domain e' quello dell'app.
# Se la tua versione usa nomi diversi, verifica con: domain list
foreach d [list zynqmp_fsbl zynqmp_pmufw standalone_domain standalone_psu_cortexa53_0] {
    vc6e_disable_drivers $d $null_driver_ips
}

platform generate

puts ">>> app create (template Zynq MP FSBL)"
app create -name ked_fsbl -platform ked_plat -proc psu_cortexa53_0 \
    -os standalone -lang c -template {Zynq MP FSBL}

# il dominio dell'app potrebbe essere stato creato ora da app create:
# riapplico l'assegnazione driver anche li' prima della build
foreach d [list standalone_domain standalone_psu_cortexa53_0] {
    vc6e_disable_drivers $d $null_driver_ips
}

puts ">>> import sorgenti custom (overwrite xfsbl_board.c + driver VC6E)"
importsources -name ked_fsbl -path "$script_dir/src"

puts ">>> compiler symbols"
app config -name ked_fsbl define-compiler-symbols {VC6E_FSBL_CONTEXT}
# Bring-up: decommenta per log FSBL dettagliati e verify post-init del VC6E
# app config -name ked_fsbl define-compiler-symbols {FSBL_DEBUG_INFO}
# app config -name ked_fsbl define-compiler-symbols {VC6E_VERIFY_AFTER_INIT}

puts ">>> build"
app build -name ked_fsbl

puts ">>> ELF: $ws/ked_fsbl/Debug/ked_fsbl.elf"
