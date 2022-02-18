serial-discovery: serial-discovery.c
	gcc -O2 -Wall -o serial-discovery serial-discovery.c -ludev -lpthread

clean:
	rm -f serial-discovery

