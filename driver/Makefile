obj-m := hunt_reader.o
KDIR := /lib/modules/$(shell uname -r)/build
EXTRA_CFLAGS := -I$(src)/../include

all:
	make -C $(KDIR) M=$(PWD) modules

clean:
	make -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod hunt_reader.ko

unload:
	sudo rmmod hunt_reader

reload: unload load
