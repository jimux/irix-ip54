#!/bin/sh
# setup_ip54.sh - Stage IP54 drivers, install headers, compile natively, prep kernel.o
# Run on IRIX 6.5.5 (irix655-full dev instance)
# TFTP server: 10.0.2.2 (QEMU SLIRP), root = ip54_tftp_staging/
# Usage: tftp 10.0.2.2 -> binary; get setup_ip54.sh /tmp/setup_ip54.sh
#        sh /tmp/setup_ip54.sh
#
# NOTE: This script prepares the dev instance (compilation only).
#       Run lboot on the forked test instance, not here.

set -e

CC_FLAGS="-c -G 8 -DIP54 -DMAXCPU=32 -D_PAGESZ=4096 -D_KERNEL -n32 -mips3 -O2 -non_shared -TENV:kernel -DDEFAULTSEMAMETER=1 -I/usr/include"

echo "=== Step 1: Create /tmp/cc wrapper ==="
cat > /tmp/cc << 'CCEOF'
#!/bin/sh
for a in "$@"; do
  case "$a" in *master.c)
    ed "$a" << 'ED'
/^struct edt edt\[\] = {/+1i
{ 0 }
.
w
q
ED
    ;;
  esac
done
exec /usr/bin/cc "$@"
CCEOF
chmod +x /tmp/cc
echo "  /tmp/cc created"

echo "=== Step 2: Create header directories ==="
/usr/bin/cc -n32 -o /tmp/mkdirs - << 'CEOF'
#include <sys/stat.h>
int main(void) {
    mkdir("/usr/include/sys/RACER", 0755);
    mkdir("/usr/include/ksys", 0755);
    return 0;
}
CEOF
/tmp/mkdirs
echo "  /usr/include/sys/RACER and /usr/include/ksys created"

echo "=== Step 3: TFTP kernel headers ==="
tftp 10.0.2.2 << 'TEOF'
binary
get RACER/heart.h /usr/include/sys/RACER/heart.h
get RACER/heartio.h /usr/include/sys/RACER/heartio.h
get RACER/heart_vec.h /usr/include/sys/RACER/heart_vec.h
get khdrs/systm.h /usr/include/sys/systm.h
get khdrs/sbd.h /usr/include/sys/sbd.h
get khdrs/pda.h /usr/include/sys/pda.h
get khdrs/runq.h /usr/include/sys/runq.h
get khdrs/kopt.h /usr/include/sys/kopt.h
get khdrs/callo.h /usr/include/sys/callo.h
get khdrs/iograph.h /usr/include/sys/iograph.h
get khdrs/atomic_ops.h /usr/include/sys/atomic_ops.h
get khdrs/clksupport.h /usr/include/sys/clksupport.h
get khdrs/ksys/ddmap.h /usr/include/ksys/ddmap.h
get IP54addrs.h /usr/include/sys/IP54addrs.h
get IP54.c /tmp/IP54.c
get pvfb.c /tmp/pvfb.c
get pvaudio.c /tmp/pvaudio.c
get if_pvnet.c /tmp/if_pvnet.c
get IP54.sm /var/sysgen/system/IP54.sm
quit
TEOF
echo "  Headers and sources installed"

echo "=== Step 3b: TFTP master.d and csu.IP54.o ==="
tftp 10.0.2.2 << 'TEOF'
binary
get master.d/if_pvnet /var/sysgen/master.d/if_pvnet
get master.d/pvfb /var/sysgen/master.d/pvfb
get master.d/pvaudio /var/sysgen/master.d/pvaudio
get compiled_objects/csu.IP54.o /var/sysgen/boot/csu.IP54.o
quit
TEOF
echo "  master.d files and csu.IP54.o installed"

echo "=== Step 4: Backup original kernel.o ==="
cp /var/sysgen/boot/kernel.o /var/sysgen/boot/kernel.o.ip22
echo "  kernel.o backed up"

echo "=== Step 5: Compile pvfb.c natively (cred_t, vhandl_t require cred.h, ddmap.h) ==="
/usr/bin/cc $CC_FLAGS /tmp/pvfb.c -o /var/sysgen/boot/pvfb.o
echo "  pvfb.o compiled"
nm /var/sysgen/boot/pvfb.o | grep "pvfbdevflag"

echo "=== Step 6: Compile pvaudio.c natively ==="
/usr/bin/cc $CC_FLAGS /tmp/pvaudio.c -o /var/sysgen/boot/pvaudio.o
echo "  pvaudio.o compiled"
nm /var/sysgen/boot/pvaudio.o | grep "pvaudiodevflag"

echo "=== Step 7: Compile if_pvnet.c natively ==="
/usr/bin/cc $CC_FLAGS /tmp/if_pvnet.c -o /var/sysgen/boot/if_pvnet.o
echo "  if_pvnet.o compiled"
nm /var/sysgen/boot/if_pvnet.o | grep "if_pvnetdevflag"

echo "=== Step 8: Compile IP54.c natively ==="
/usr/bin/cc $CC_FLAGS -I/var/sysgen/boot /tmp/IP54.c -o /var/sysgen/boot/IP54.o
echo "  IP54.o compiled"
nm /var/sysgen/boot/IP54.o | grep "maxcpus\|cputype"

echo "=== Step 9: Merge kernel.o (IP54 first, IP22 second) ==="
ld -n32 -r -o /var/tmp/kernel_ip54.o \
  /var/sysgen/boot/IP54.o \
  /var/sysgen/boot/csu.IP54.o \
  /var/sysgen/boot/kernel.o.ip22
cp /var/tmp/kernel_ip54.o /var/sysgen/boot/kernel.o
echo "  kernel.o merged (size: `ls -l /var/sysgen/boot/kernel.o | awk '{print $5}'` bytes)"

echo "=== Step 10: Sync to disk ==="
sync
sync
echo "  Sync complete"

echo "=== Done: dev instance ready for fork ==="
echo "  Next: stop this session, vm_instance_fork(irix655-full → ip54-test),"
echo "  then start ip54-test and run lboot -s /var/sysgen/system/IP54.sm"
