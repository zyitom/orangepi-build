# Build on the board against the installed linux-headers package:
#   make            -> amp_timestamp.ko
#   sudo make install   (to /lib/modules/$(uname -r)/updates + depmod)
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

install: all
	install -D -m644 amp_timestamp.ko /lib/modules/$(shell uname -r)/updates/amp_timestamp.ko
	depmod -a

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
