import mmap,os,sys
fd=os.open("/dev/mem",os.O_RDWR|os.O_SYNC)
m=mmap.mmap(fd,4096,offset=0x07094000); v=memoryview(m).cast('I')
print("status before", v[0x6C//4]&0xF)
v[0x7C//4]=int(sys.argv[1],0)   # single 32-bit store
print("wrote", sys.argv[1])
