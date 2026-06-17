#!/bin/sh
for a in "$@"; do
  case "$a" in *master.c)
    /usr/bin/ed "$a" << 'EDEOF'
1i
/* cc_wrapper patches for IP54 lboot */
#include <sys/immu.h>
typedef void *ddv_handle_t;
typedef void *vhandl_t;
#define CPU_NONE (-1)
.
/^struct edt edt\[\] = {/+1i
{ 0 }
.
w
q
EDEOF
    ;;
  esac
done
exec /usr/bin/cc "$@"
