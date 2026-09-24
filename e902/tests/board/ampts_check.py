# Check the amp_timestamp driver against CLOCK_MONOTONIC and the raw register.
import mmap, os, time
d = "/sys/bus/platform/devices/8010000.amp-timestamp/"
rd = lambda a: int(open(d + a).read())
fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
v = memoryview(mmap.mmap(fd, 4096, offset=0x08010000, prot=mmap.PROT_READ)).cast("I")
def raw():
    while True:
        h = v[1]; l = v[0]
        if v[1] == h:
            return (h << 32) | l
print("freqid", rd("freqid"))
c0, r0, t0 = rd("counter"), raw(), time.monotonic_ns()
u0 = rd("usec")
time.sleep(10)
c1, r1, t1 = rd("counter"), raw(), time.monotonic_ns()
u1 = rd("usec")
dt_us = (t1 - t0) / 1000
print("sysfs counter rate : %.6f MHz" % ((c1 - c0) / dt_us))
print("sysfs usec / mono  : %.6f" % ((u1 - u0) / dt_us))
print("sysfs vs raw reg   : %d ticks apart (%.1f us, back-to-back reads)" % (r1 - c1, (r1 - c1) / 24))
