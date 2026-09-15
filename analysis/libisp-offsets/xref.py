import re,sys,subprocess
so=sys.argv[1]; funcs=sys.argv[2:]
TC="/home/helios/Desktop/orangepi-build/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-"
data=open(so,'rb').read()
# section map for vaddr->file offset
hdr=subprocess.run([TC+"objdump","-h",so],capture_output=True,text=True).stdout
secs=[]
for l in hdr.splitlines():
    m=re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)",l)
    if m: secs.append((m.group(1),int(m.group(3),16),int(m.group(2),16),int(m.group(4),16)))
def cstr(va):
    for n,vma,sz,off in secs:
        if vma<=va<vma+sz:
            o=off+va-vma; e=data.find(b"\0",o); s=data[o:e]
            try: return n, s.decode()
            except: return n, None
    return None,None
dis=subprocess.run([TC+"objdump","-d","--no-show-raw-insn",so],capture_output=True,text=True).stdout
cur=None; reg={}
for l in dis.splitlines():
    m=re.match(r"^[0-9a-f]+ <(.+)>:",l)
    if m: cur=m.group(1); reg={}; continue
    if cur not in funcs: continue
    m=re.match(r"\s*([0-9a-f]+):\s+adrp\s+(x\d+),\s*([0-9a-f]+)",l)
    if m: reg[m.group(2)]=int(m.group(3),16); continue
    m=re.match(r"\s*([0-9a-f]+):\s+add\s+(x\d+),\s*(x\d+),\s*#0x([0-9a-f]+)",l)
    if m and m.group(3) in reg:
        va=reg[m.group(3)]+int(m.group(4),16); sec,s=cstr(va)
        if s: print(cur, m.group(1), sec, repr(s[:100]))
        continue
    m=re.match(r"\s*([0-9a-f]+):\s+bl\s+[0-9a-f]+ <(.+)>",l)
    if m: print(cur, m.group(1), "CALL", m.group(2))
