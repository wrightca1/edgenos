import mmap, os, struct
fd = os.open('/sys/bus/pci/devices/0000:04:00.0/resource0', os.O_RDWR)
m = mmap.mmap(fd, 0x8000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
print 'before 0x7000 =', hex(struct.unpack_from('<I', m, 0x7000)[0])
struct.pack_into('<I', m, 0x7000, 0xDEAD)
print 'wrote 0xDEAD -> power-cycling'
