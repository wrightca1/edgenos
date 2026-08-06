import mmap,struct,re,sys,time
f=open("/sys/bus/pci/devices/0000:02:00.0/resource0","r+b")
m=mmap.mmap(f.fileno(),32*1024*1024)
def rd(w): return struct.unpack("<I",m[w*4:w*4+4])[0]
def wr(w,v): m[w*4:w*4+4]=struct.pack("<I",v)
def sbus(cmd,data):
    wr(0xF002,data); wr(0xF001,0); wr(0xF001,cmd)
    for _ in range(200000):
        st=rd(0xF001)
        if not (st&(1<<25)): return (st>>26)&7
    return -1
def snap(tag):
    hi=rd(0xe383f)
    print("  [%-16s] PORT_STATUS=0x%04x TxRdy/RxRdy=%d/%d PIN=0x%x RXCFG=%08x TXCFG=%08x"%(
        tag,rd(0xe3800),(hi>>5)&1,(hi>>6)&1,rd(0x1c021),rd(0xe3839),rd(0xe383a)))
    sys.stdout.flush()
path=sys.argv[1]; pace=float(sys.argv[2]) if len(sys.argv)>2 else 0.0
ops=[];pend=0
for l in open(path):
    mm=re.match(r'Write:\s+0x([0-9a-f]+)\s+<-\s+0x([0-9a-f]+)',l)
    if not mm: continue
    a,v=int(mm.group(1),16),int(mm.group(2),16)
    if a==0xf002: pend=v; continue
    if a==0xf001:
        if v==0: continue
        ops.append(('S',v,pend)); continue
    ops.append(('M',a,v))
print("replaying %d ops (%d SBus) from %s"%(len(ops),sum(1 for o in ops if o[0]=='S'),path))
snap("before")
nS=0
for i,(k,a,v) in enumerate(ops):
    if k=='M': wr(a,v)
    else:
        rc=sbus(a,v); nS+=1
        if rc<0: print("   !! SBus timeout at op %d cmd=0x%08x"%(i,a)); break
    if pace: time.sleep(pace)
    if i%25==24 and rd(0x1c021)==0xffffffff:
        print("   !! CHIP OFF-BUS after op %d (0x%05x <- 0x%08x) -- ABORT"%(i,a,v)); break
    if i%100==99: snap("op %d"%(i+1))
snap("after")
for s in (1,2,4,8):
    time.sleep(s if s==1 else s-(s//2)); snap("after+%ds"%s)
