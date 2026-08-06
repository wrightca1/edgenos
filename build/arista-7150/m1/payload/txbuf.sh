( ping -i 0.002 -c 6000 10.101.101.25 >/dev/null 2>&1 & )
sleep 1
python - <<'PY'
import mmap,struct
bar=open("/sys/bus/pci/devices/0000:02:00.0/resource0","r+b")
M=mmap.mmap(bar.fileno(),32*1024*1024)
def rb(o): return struct.unpack("<I",M[o:o+4])[0]
tx=rb(0x5020); txe=rb(0x5028); n=(txe-tx)//32
dm=open("/dev/mem","rb"); PAGE=4096
base=tx&~(PAGE-1); off=tx-base
ring=mmap.mmap(dm.fileno(),(txe-tx)+off,offset=base,prot=mmap.PROT_READ)
print("dma_cfg=0x%08x (tag location/size selector)"%rb(0x5060))
seen=0
for it in range(400000):
    cur=rb(0x5048); i=((cur-tx)//32)%n
    for j in (i-1,i,(i+1)%n):
        d=ring[off+(j%n)*32:off+(j%n)*32+32]
        if d=='\0'*32: continue
        ln=struct.unpack("<H",d[2:4])[0]
        lo=struct.unpack("<I",d[4:8])[0]; hi=struct.unpack("<I",d[8:12])[0]
        if ln==0 or (lo==0 and hi==0): continue
        phys=(hi<<32)|lo
        try:
            pb=phys&~(PAGE-1); po=phys-pb
            bm=mmap.mmap(dm.fileno(),po+min(ln,128),offset=pb,prot=mmap.PROT_READ)
            f=bm[po:po+min(ln,64)]
        except Exception as ex:
            print("  BD[%d] buf@0x%x unreadable: %s"%(j%n,phys,ex)); continue
        print("  BD[%d] len=%d buf@0x%x"%(j%n,ln,phys))
        print("    frame[0:32]  : "+" ".join("%02x"%ord(c) for c in f[:32]))
        print("    DMAC=%s SMAC=%s ethertype/tag@12=%s"%(
              ":".join("%02x"%ord(c) for c in f[0:6]),
              ":".join("%02x"%ord(c) for c in f[6:12]),
              " ".join("%02x"%ord(c) for c in f[12:20])))
        seen+=1
        bm.close()
        if seen>=2: break
    if seen>=2: break
print("captured %d frames"%seen)
PY
