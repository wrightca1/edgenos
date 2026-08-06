import mmap,struct,sys,time
f=open("/sys/bus/pci/devices/0000:02:00.0/resource0","r+b")
m=mmap.mmap(f.fileno(),32*1024*1024)
def rd(w): return struct.unpack("<I",m[w*4:w*4+4])[0]
def wr(w,v): m[w*4:w*4+4]=struct.pack("<I",v)
def sbrd(dev,reg):
  wr(0xF002,0); wr(0xF001,0)
  wr(0xF001,((dev&0xff)<<8)|(reg&0xff)|(0x22<<16)|(1<<24))
  for _ in range(100000):
    st=rd(0xF001)
    if not (st&(1<<25)): break
  return rd(0xF003)&0xff
tag=sys.argv[1] if len(sys.argv)>1 else "?"
CORE=[0x00,0x03,0x06,0x0d,0x17,0x1d,0x1f,0x20,0x22,0x26,0x36,0x3b]
EPL=[("PORT_STATUS",0xe3800),("SERDES_CFG_lo",0xe3834),("SERDES_CFG_hi",0xe3835),
     ("LANE_CFG",0xe3837),("SERDES_RX_CFG",0xe3839),("SERDES_TX_CFG_lo",0xe383a),
     ("SERDES_TX_CFG_hi",0xe383b),("SIGDET",0xe383c),("STATUS_lo",0xe383e),("STATUS_hi",0xe383f)]
print("### %s  t=%.1f"%(tag,time.time()))
print("core: "+" ".join("%02x=%02x"%(r,sbrd(0x49,r)) for r in CORE))
print("epl : "+" ".join("%s=%08x"%(n.split('_')[-1][:6],rd(w)) for n,w in EPL))
hi=rd(0xe383f); print("      TxRdy=%d RxRdy=%d PORT_STATUS=0x%x"%((hi>>5)&1,(hi>>6)&1,rd(0xe3800)))
