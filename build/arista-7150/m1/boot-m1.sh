#!/bin/sh
# kexec into the fixed EdgeNOS M1 from EOS (same as Aboot boot0, minus Aboot).
set -e
cd /tmp
rm -f linux-i386 initrd-i386
unzip -oq /mnt/flash/edgenos-m1.swi linux-i386 initrd-i386
/usr/bin/kexec --load /tmp/linux-i386 --initrd=/tmp/initrd-i386 \
  --append="console=ttyS0,9600 earlyprintk=serial,ttyS0,9600 rdinit=/init panic=10 nosmp reboot=p,force"
sync
echo "boot-m1: kexec loaded; executing into fixed M1..."
/usr/bin/kexec --exec
