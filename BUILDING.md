# Building irix-ip54

The build produces `/unix.new` — a re-linked IRIX 6.5.5 kernel that includes
the IP54 paravirtual drivers and machine support — by compiling our PV driver
sources on an **IRIX 6.5.5 build host** with **MIPSpro 7.2.1**, dropping the
`.o` files into `/var/sysgen/boot/`, and running `lboot -s IP54.sm -u
/unix.new`. The build host can be real SGI hardware, but in practice it's a
qemu-sgi `machine=indy` VM that already has the build tree installed
(`irix655-dev` / `ip54-test`).

There are two ways to run the build.

## (1) Driven by qemu-sgi (the normal path)

`run_m1_kernel_rebuild.py` in the qemu-sgi project orchestrates the whole
thing. It boots an IRIX guest on `machine=indy`, drives the serial console
to fetch sources via TFTP from `ip54_tftp_staging/` (a symlink farm pointing
into this repo), compiles + installs the .o files, runs `lboot`, then
cleanly shuts down. Producing `/unix.new` ends up on the guest's disk
in-place.

```
cd /path/to/qemu-sgi
python3 run_m1_kernel_rebuild.py
```

The TFTP-side symlinks make the SGI-style source tree appear as flat
filenames the build expects:

| Build sees | Resolves to (in this repo) |
|---|---|
| `pvfb.c`, `pvuart_cn.c`, `pvdisk.c`, `pvaudio.c` | `m/irix/kern/io/*.c` |
| `if_pvnet.c` | `m/irix/kern/bsd/mips/if_pvnet.c` |
| `IP54.c`, `csu.IP54.s`, `ip54_stubs.c`, `IP54addrs.h` | `m/irix/kern/{ml,sys}/*` |
| `master.d/{pvfb,pvuart_cn,pvdisk,pvaudio,if_pvnet,ip54_stubs}` | `m/irix/kern/master.d/*` |
| `IP54.sm` | `sysgen/system/IP54.sm` |
| `setup_ip54.sh`, `cc_wrapper.sh`, `Makefile.ip54`, `khdrs.tar` | `scripts/*` |

## (2) Standalone on any IRIX 6.5.5 + MIPSpro 7.2.1 host

If you have a real SGI Indy/Indigo or a different IRIX build VM, you can
build directly. The pattern (mirrors what `setup_ip54.sh` automates):

1. Get the sources onto the build host (TFTP from this repo, an NFS mount,
   or just a tar of the files).
2. Run `scripts/setup_ip54.sh` — it lays out `/tmp/cc` (the master.c
   patcher), unpacks `khdrs.tar`, and creates the build dir.
3. Compile each driver with MIPSpro:
   ```
   /tmp/cc -c -n32 -mips3 -O2 -G 8 -non_shared -TENV:kernel \
       -DIP54 -D_KERNEL -I/usr/include <name>.c -o <name>.o
   ```
   (`if_pvnet` additionally needs `-D_PAGESZ=16384 -I/tmp/khdrs`.)
4. `cp` the .o files to `/var/sysgen/boot/`.
5. `cp m/irix/kern/master.d/*` to `/var/sysgen/master.d/`.
6. `cp sysgen/system/IP54.sm` to `/var/sysgen/system/IP54.sm`.
7. Run `lboot -s /var/sysgen/system/IP54.sm -u /unix.new`.

The freshly linked `/unix.new` is the boot target for `qemu-system-mips64 -M
sgi-ip54`. The QEMU side reads the kernel's `cause_ip5_count` symbol address
from this binary (via `nm /unix.new | grep cause_ip5_count`) and passes it
to QEMU via `IP54_CAUSE_IP5_COUNT_PA=<phys>` (see qemu-sgi's pvclock fix in
`qemu-sgi-repo/hw/mips/sgi_ip54pv.c`).

## Pinning kernel-symbol drift

The QEMU pvclock device hardcodes the physical address of `cause_ip5_count`
as its compile-time default. Every kernel rebuild MOVES the symbol — so the
QEMU compile-time default rots, and the **launcher must always pass
`IP54_CAUSE_IP5_COUNT_PA=$(nm /unix.new | grep -w cause_ip5_count | awk
'{print "0x" toupper($1)}' | python3 -c 'import sys; print(hex(int(sys.stdin.read().strip(),16) & 0x1FFFFFFF))')`**
as an env var when launching QEMU. Skipping this corrupts kernel memory.

See qemu-sgi's `progress_notes/ip54/dt_desktop_zone_corruption.md` for the
full history of this bug.

## Caveats

- The 877-line `pvfb.c.dev_877_experimental` is preserved as a reference of
  newer additions on top of the canonical 800-line `pvfb.c`. It builds but
  has caused early-boot wedges in testing; merging its additions
  incrementally into the canonical source is future work.
- `khdrs.tar` is a snapshot of the IRIX 6.5.5 kernel headers needed by
  `if_pvnet.c` (it expects an older `net/raw.h` layout). If you're building
  on a real IRIX system that already has these headers, you don't need it.
