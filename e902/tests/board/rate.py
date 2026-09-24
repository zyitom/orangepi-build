import mmap,os,struct,time
fd=os.open("/dev/mem",os.O_RDWR|os.O_SYNC)
tm=mmap.mmap(fd,4096,offset=0x07091000)
ts=mmap.mmap(fd,4096,offset=0x08010000)
hb=mmap.mmap(fd,4096,offset=0x0005E000)   # E902 0x4001E000 -> ARM 0x5E000
r=lambda m,o: struct.unpack_from("<I",m,o)[0]
def tsread():
    while True:
        h=r(ts,4); l=r(ts,0)
        if r(ts,4)==h: return (h<<32)|l
now=time.perf_counter
# 1) S_TIMER1 counter rate (down-counter, 100ms period): short windows without wrap
rates=[]
for i in range(20):
    a=r(tm,0x48); t0=now(); time.sleep(0.02); b=r(tm,0x48); t1=now()
    if b<a: rates.append((a-b)/(t1-t0))
rates.sort(); print("S_TIMER1 count rate: median %.3f MHz (n=%d)"%(rates[len(rates)//2]/1e6,len(rates)))
# 2) shared timestamp rate vs CLOCK_MONOTONIC over 5 s
a=tsread(); t0=now(); time.sleep(5); b=tsread(); t1=now()
print("TIMESTAMP rate: %.4f MHz"%((b-a)/(t1-t0)/1e6))
# 3) E902 tick rate: edge-to-edge on heartbeat word 14 (published every 10 ticks)
def edge():
    v=r(hb,14*4)
    while r(hb,14*4)==v: pass
    return r(hb,14*4), now()
v0,t0=edge(); time.sleep(20); v1,t1=edge()
print("E902 ticks: %d in %.3f s -> %.3f ticks/s (expect 10.000)"%(v1-v0,t1-t0,(v1-v0)/(t1-t0)))
w=r(hb,21*4); print("tick_src irq=%d polled=%d"%(w>>16,w&0xffff))
