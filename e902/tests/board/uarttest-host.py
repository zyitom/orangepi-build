import serial,time,random,re
s=serial.Serial('/dev/ttyUSB1',115200,timeout=0.2)
def xfer(tx,wait):
    s.reset_input_buffer(); s.write(tx); t=time.time(); b=b''
    while time.time()-t<wait: b+=s.read(4096)
    return re.sub(rb'\r?\n?\[tick\] \d+\r?\n',b'',b)
alphabet=b'abcdefgijklmnoquvwxyzABCDEFGHIJKLMNOPQRSUVWXYZ0123456789'
random.seed(1)
for n in (200,1000):
    tx=bytes(random.choice(alphabet) for _ in range(n))
    rx=xfer(tx,1.0+n/10000)
    print("echo %4d bytes: got %4d, exact match: %s"%(n,len(rx),rx==tx))
    if rx!=tx: print("   first diff at",next((i for i in range(min(len(rx),len(tx))) if rx[i]!=tx[i]),min(len(rx),len(tx))))
for k in b'srtp':
    out=xfer(bytes([k]),1.0).decode(errors='replace').replace('\r','').strip()
    print("key %s -> %s"%(chr(k),out))
