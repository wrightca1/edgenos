#!/bin/sh
cd /tmp
rm -f linux-i386 initrd-i386
unzip -oq /mnt/flash/edgenos-m1.swi linux-i386 initrd-i386
{
  echo "=== kexec version ==="; /usr/bin/kexec --version 2>&1
  echo "=== file linux-i386 ==="; ls -l /tmp/linux-i386 /tmp/initrd-i386
  echo "=== kexec --load (verbose stderr) ==="
  /usr/bin/kexec -d --load /tmp/linux-i386 --initrd=/tmp/initrd-i386 --append="console=ttyS0,9600 rdinit=/init panic=10 nosmp reboot=p,force" 2>&1
  echo "load rc=$?"
  echo "=== kexec_loaded ==="; cat /sys/kernel/kexec_loaded 2>&1
} > /mnt/flash/kexec.log 2>&1
echo done
