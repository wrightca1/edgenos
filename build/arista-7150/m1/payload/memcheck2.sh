python - <<'PY'
import mmap,struct,ctypes
PAGE=4096
buf=mmap.mmap(-1,PAGE)
buf.write(b'\xDE\xAD\xBE\xEF'*8); buf.seek(0)
libc=ctypes.CDLL("libc.so.6",use_errno=True)
addr=ctypes.addressof(ctypes.c_char.from_buffer(buf))
libc.mlock(ctypes.c_void_p(addr), ctypes.c_size_t(PAGE))
pm=open("/proc/self/pagemap","rb"); pm.seek((addr//PAGE)*8)
e=struct.unpack("<Q",pm.read(8))[0]
pfn=e&((1<<55)-1); phys=pfn*PAGE
print("sentinel phys=0x%x present=%d"%(phys,(e>>63)&1))
dm=open("/dev/mem","rb")
m2=mmap.mmap(dm.fileno(),PAGE,offset=phys,prot=mmap.PROT_READ)
got=m2[:8]
hexs=" ".join("%02x"%ord(c) for c in got)
print("  via /dev/mem: "+hexs)
ok = got[:4]=='\xDE\xAD\xBE\xEF'
print("  CALIBRATION: "+("PASS - /dev/mem reads real RAM" if ok else "FAIL - /dev/mem is NOT returning real RAM; all /dev/mem descriptor reads are VOID"))
PY
