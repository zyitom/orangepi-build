import mmap,os,struct,time
fd=os.open("/dev/mem",os.O_RDWR|os.O_SYNC)
cpus=mmap.mmap(fd,4096,offset=0x07094000)   # ARM -> E902
cpux=mmap.mmap(fd,4096,offset=0x03004000)   # E902 -> ARM
V={}
def mv(m):
    if id(m) not in V: V[id(m)]=memoryview(m).cast("I")
    return V[id(m)]
r=lambda m,o: mv(m)[o//4]
def w(m,o,v): mv(m)[o//4]=v
assert r(cpux,0x6C)&0xF==0 and r(cpus,0x6C)&0xF==0, "ch3 not idle"
rtts=[]; bad=0
for i in range(200):
    hdr=0x00010202 | ((i&0x7F)<<24)      # attr 2, flags 2 (sync), type 0x01, result=junk
    t0=time.perf_counter()
    for v in (hdr,1,0xC0DE0000+i): w(cpus,0x7C,v)
    while r(cpux,0x6C)&0xF<3:
        if time.perf_counter()-t0>0.5: break
    got=[r(cpux,0x7C) for _ in range(r(cpux,0x6C)&0xF)]
    rtts.append((time.perf_counter()-t0)*1e6)
    if got!=[hdr&0x00FFFFFF,1,0xC0DE0000+i]: bad+=1; print("bad",i,[hex(x) for x in got])
rtts.sort()
print("200 pings: bad=%d  rtt us: min %.0f  median %.0f  p99 %.0f  max %.0f"%(bad,rtts[0],rtts[100],rtts[198],rtts[-1]))
print("residue: cpux=%d cpus=%d"%(r(cpux,0x6C)&0xF,r(cpus,0x6C)&0xF))
