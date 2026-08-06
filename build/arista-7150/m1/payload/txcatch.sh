# drive sustained CPU->Et1 traffic, then tight-poll the TX ring to catch a live descriptor
( ping -i 0.002 -c 4000 10.101.101.25 >/dev/null 2>&1 & ) 
sleep 1
python - <<'PY'
import mmap,struct
bar=open("/sys/bus/pci/devices/0000:02:00.0/resource0","r+b")
M=mmap.mmap(bar.fileno(),32*1024*1024)
def rb(o): return struct.unpack("<I",M[o:o+4])[0]
tx=rb(0x5020); txe=rb(0x5028); n=(txe-tx)//32
dm=open("/dev/mem","rb"); PAGE=4096; base=tx&~(PAGE-1); off=tx-base
mm=mmap.mmap(dm.fileno(),(txe-tx)+off,offset=base,prot=mmap.PROT_READ)
print("ring 0x%08x n=%d ; tight-polling for a live descriptor..."%(tx,n))
hits=[]
for it in range(400000):
    cur=rb(0x5048)                      # PCI_CURRENT_TX_BD_PTR
    i=((cur-tx)//32) % n
    for j in (i-1, i, (i+1)%n):
        d=mm[off+(j%n)*32:off+(j%n)*32+32]
        if d != '\0'*32:
            key=d
            if key not in hits:
                hits.append(key)
                ln=struct.unpack("<H",d[2:4])[0]
                print("  BD[%d] status=0x%02x len=%d"%(j%n,ord(d[0]),ln))
                print("    full: "+" ".join("%02x"%ord(c) for c in d))
                print("    0x0C: "+" ".join("%02x"%ord(c) for c in d[0x0C:0x14]))
                print("    0x14: "+" ".join("%02x"%ord(c) for c in d[0x14:0x1C]))
    if len(hits)>=3: break
print("caught %d live descriptors"%len(hits))
PY
