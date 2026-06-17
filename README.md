# irix-ip54 — IRIX 6.5.5 paravirtual port for QEMU's `sgi-ip54` machine

A self-contained IRIX kernel port targeting the paravirtual IP54 workstation
emulated by [qemu-sgi](https://github.com/jimsmyth/qemu-sgi) (the `sgi-ip54`
machine type). This adds:

- **5 paravirtual device drivers** — `pvfb`, `pvuart_cn`, `pvdisk`, `pvaudio`,
  `if_pvnet` — that talk to QEMU's matching virtual hardware.
- **An IP54 machine module** — `IP54.c` + `csu.IP54.s` + `ip54_stubs.c` —
  providing the kernel's IP54 boot/init plumbing and the stubs needed to link
  against stock IRIX kernel libraries.
- **Sysgen config** — `IP54.sm` (the master config for `lboot`) and the
  per-driver `master.d/` entries.

The source files follow SGI's IRIX tree layout (`m/irix/kern/io/`,
`m/irix/kern/bsd/mips/`, `m/irix/kern/ml/`, `m/irix/kern/master.d/`,
`sysgen/system/`) so they can be dropped into a stock IRIX 6.5.5 source tree
for building.

## Layout

```
m/irix/kern/
  io/pvfb.c            — paravirtual framebuffer + gfx_gfx board ("NEWPORT")
  io/pvuart_cn.c       — paravirtual UART console driver
  io/pvdisk.c          — paravirtual disk driver (PIO with dcache flush)
  io/pvaudio.c         — paravirtual audio sink driver
  bsd/mips/if_pvnet.c  — paravirtual ethernet
  ml/IP54.c            — IP54 machine support
  ml/csu.IP54.s        — IP54 startup assembly
  ml/ip54_stubs.c      — link-time stubs for IRIX symbols not present on IP54
  sys/IP54addrs.h      — IP54 physical address map
  master.d/{pvfb,pvuart_cn,pvdisk,pvaudio,if_pvnet,ip54_stubs}  — driver descs
sysgen/system/IP54.sm  — kernel master config (lboot -s)
scripts/
  setup_ip54.sh        — stage sources + cc wrapper in the build guest
  cc_wrapper.sh        — wraps cc to patch master.c during sysgen
  Makefile.ip54        — kernel-driver build rules
  khdrs.tar            — IRIX 6.5.5 kernel headers needed for if_pvnet build
```

## Build pipeline (driven from qemu-sgi)

`qemu-sgi`'s `run_m1_kernel_rebuild.py` boots an IRIX 6.5.5 build guest
(machine=indy, MIPSpro 7.2.1 onboard), TFTPs these sources in, compiles them,
copies the .o files into `/var/sysgen/boot/`, then runs `lboot -s IP54.sm -u
/unix.new` to relink the kernel. The resulting `/unix.new` boots on
`qemu-system-mips64 -M sgi-ip54`.

## Pinned state

The `pvfb.c` checked in is the **800-line version** recovered from the working
qemu-sgi golden disk (`disk.qcow2.golden.desktop`'s `/tmp/pvfb.c`); this is
the source that built the working `pvfb.o` shipping a working `gf_Initialize`
/ `gf_MapGfx` / `GfxRegisterBoard("NEWPORT")` graphics-board path that Xsgi
binds to. An experimental 877-line version with additional development is
preserved alongside as `pvfb.c.dev_877_experimental` for future merging.

## License

These files are derived from / target IRIX 6.5.5, which remains SGI's
property. This port is provided for emulation research and historical
preservation. See the IRIX source tree's own license terms for the underlying
kernel headers/types these drivers use.
