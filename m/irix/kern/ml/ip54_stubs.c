/*
 * ip54_stubs.c - Null implementations for symbols referenced by IP22 code
 * in the merged kernel.o that don't have real implementations for IP54.
 *
 * These are either IP22-specific (R4000 workarounds, WD95 SCSI) or
 * modules whose .a archives don't exist on this IRIX installation
 * (capability, checkpoint, EAG).
 */
#if IP54

/* R4000/R5000 CPU bug workaround flags — not applicable to IP54 */
int R4000_jump_war_correct = 0;
int R4000_jump_war_always = 0;
int R4000_jump_war_warn = 0;
int R4000_jump_war_kill = 0;
int R4000_div_eop_correct = 0;
int R5000_cvt_war = 0;

/* WD95 SCSI controller — not present on IP54 */
void wd95_earlyinit(void) {}
void wd95intr(void) {}

/* Capability module stubs */
void cap_init(void) {}
int cap_get(void) { return 0; }
int cap_set(void) { return 0; }
void cap_empower_cred(void) {}
void cap_recalc(void) {}

/* EAG security framework */
int eag_mount(void) { return 0; }

/* Checkpoint/restart stubs */
int ckpt_sys(void) { return 0; }
int ckpt_restartreturn(void) { return 0; }
int ckpt_prioctl(void) { return 0; }
int ckpt_prioctl_attr(void) { return 0; }
int ckpt_prioctl_thread(void) { return 0; }

/* Process attribute stubs */
int proc_attr_get(void) { return 0; }
int proc_attr_set(void) { return 0; }

#endif /* IP54 */
