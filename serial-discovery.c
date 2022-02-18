#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/select.h>
#include <libudev.h>            // sudo apt-get install libudev-dev

#define STATE_ALIVE 0
#define STATE_IDLE  1
#define STATE_RUN   2
#define STATE_SYNC  3
#define STATE_DEAD  4

pthread_t thread1;
volatile int state = STATE_ALIVE;

pthread_mutex_t mutex1;
pthread_mutex_t mutex2;
pthread_mutex_t mutex3;
void lock_list(void) { pthread_mutex_lock( &mutex1 ); }
void unlock_list(void) { pthread_mutex_unlock( &mutex1 ); }
void lock_json(void) { pthread_mutex_lock( &mutex2 ); }
void unlock_json(void) { pthread_mutex_unlock( &mutex2 ); fflush(stdout); }
void lock_initial_scan(void) { pthread_mutex_lock( &mutex3 ); }
void unlock_initial_scan(void) { pthread_mutex_unlock( &mutex3 ); }

void die(const char *format, ...) __attribute__ ((format (printf, 1, 2)));
size_t strlcpy(char *dst, const char *src, size_t maxlen);


// to be accessed only with list mutex locked
struct serial_struct {
	char sysname[2048];
	char devname[512];
	int vid;
	int pid;
	int ver;
	char serialnum[256];
	struct serial_struct *prev;
	struct serial_struct *next;
};
struct serial_struct *serial_list=NULL;

/*
void logfile(const char *format, ...)
{
	va_list args;
	static FILE *fp=NULL;

	if (!fp) {
		fp = fopen("/tmp/logfile.txt", "a");
		if (!fp) return;
	}
	va_start(args, format);
	vfprintf(fp, format, args);
	va_end(args);
	fflush(fp);
}
*/

void json(const char *format, ...)
{
	va_list args;
	lock_json();
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	unlock_json();
}


void print_serial_add(const struct serial_struct *t);
void print_serial_remove(const struct serial_struct *t);


struct serial_struct *copy_serial_list(void)
{
	const struct serial_struct *t;
	struct serial_struct *n, *last=NULL, *list=NULL;

	lock_list();
	for (t = serial_list; t; t = t->next) {
		n = (struct serial_struct *)malloc(sizeof(struct serial_struct));
		if (!n) break;
		memcpy(n, t, sizeof(struct serial_struct));
		if (!list) list = n;
		n->next = NULL;
		n->prev = last;
		if (last) last->next = n;
		last = n;
		//logfile("   copy to list, loc=%s\n", n->location);
	}
	unlock_list();
	return list;
}

void free_serial_list(struct serial_struct * list)
{
	struct serial_struct *p, *t;
	t = list;
	while (t) {
		p = t;
		t = t->next;	
		free(p);
	}
}


void print_serial(const struct serial_struct *t)
{
	printf("{\n");
	printf("\"address\": \"%s\",\n", t->devname);
	printf("\"label\": \"%s\",\n", t->devname);
	printf("\"protocol\": \"serial\",\n");
	printf("\"protocolLabel\": \"Serial Ports\",\n");
	printf("\"properties\": {\n");
	if (t->vid && t->pid) {
		printf("\"pid\": \"0x%04X\",\n", t->pid);
		printf("\"vid\": \"0x%04X\",\n", t->vid);
		printf("\"ver\": \"0x%04X\"%s\n", t->ver, (t->serialnum[0] ? "," : ""));
	}
	if (t->serialnum[0]) {
		printf("\"serialNumber\": \"%s\"\n", t->serialnum);
	}
	printf("}\n");
	printf("}\n");
}

void print_serial_add(const struct serial_struct *t)
{
	lock_json();
	printf("{\n");
	printf("\"eventType\": \"add\",\n");
	printf("\"port\": ");
	print_serial(t);
	printf("}\n");
	unlock_json();
}

void print_serial_remove(const struct serial_struct *t)
{
	json("{\n"
		"\"eventType\": \"remove\",\n"
		"\"port\": {\n"
		"\"address\": \"%s\",\n"
		"\"protocol\": \"serial\"\n"
		"}\n"
		"}\n", t->devname);
}

void print_serial_list(const struct serial_struct *list)
{
	const struct serial_struct *t;
	int count = 0;
	
	for (t = list; t; t = t->next) {
		count++;
	}
	//logfile("board count = %d\n", count);
	lock_json();
	printf("{\n");
	printf("\"eventType\": \"list\",\n");
	printf("\"ports\": [\n");
	for (t = list; t; t = t->next) {
		print_serial(t);
		if (--count > 0) printf(",\n");
	}
	printf("]\n");
	printf("}\n");
	unlock_json();
}

void print_serial_list_add(const struct serial_struct *list)
{
	const struct serial_struct *t;
	
	for (t = list; t; t = t->next) {
		print_serial_add(t);
	}
}


void serial_add(const char *sys, const char *dev, int vid, int pid, int ver, const char *ser)
{
	lock_list();
	struct serial_struct *t, *tn;
	for (t = serial_list; t; t = t->next) {
		if (strcmp(t->sysname, sys) == 0) break;
	}
	if (!t) {
		t = (struct serial_struct *)malloc(sizeof(struct serial_struct));
		if (!t) { // unable to allocate memory
			unlock_list();
			return;
		}
		memset(t, 0, sizeof(struct serial_struct));

		strlcpy(t->sysname, sys, sizeof(t->sysname));
		if (serial_list == NULL) {
			serial_list = t;
		} else {
			for (tn = serial_list; tn->next; tn = tn->next) ;
			tn->next = t;
			t->prev = tn;
		}
	}
	strlcpy(t->devname, dev, sizeof(t->devname));
	if (ser) {
		strlcpy(t->serialnum, ser, sizeof(t->serialnum));
	} else {
		*t->serialnum = 0;
	}
	t->vid = vid;
	t->pid = pid;
	t->ver = ver;
	unlock_list();
	//logfile("   serial add %s\n", t->devname);
	if (state == STATE_SYNC) print_serial_add(t);
}

void serial_remove(const char *sys)
{
	lock_list();
	struct serial_struct *t;
	for (t = serial_list; t; t = t->next) {
		if (strcmp(t->sysname, sys) == 0) {
			if (t->prev) {
				t->prev->next = t->next;
			} else {
				serial_list = t->next;
			}
			if (t->next) {
				t->next->prev = t->prev;
			}
			break;
		}
	}
	unlock_list();
	if (t) {
		if (state == STATE_SYNC) print_serial_remove(t);
		free(t);
	}
}


void udev_add(struct udev_device *dev)
{
	const char *syspath = udev_device_get_syspath(dev);
	const char *devnode = udev_device_get_devnode(dev);
	if (!syspath || !devnode) return;
	//logfile("udev_add %s %s\n", devnode, syspath);

	struct udev_device *p;
	for (p = dev; p != NULL; p = udev_device_get_parent(p)) {
		const char *subsys = udev_device_get_subsystem(p);
		//logfile("  %s %s\n", subsys, udev_device_get_syspath(p));
		if (!subsys) {
			break;
		} else if (strcmp(subsys, "tty") == 0) {
			continue;
		} else if (strcmp(subsys, "platform") == 0) {
			break;
		} else if (strncmp(subsys, "usb", 3) == 0) {
			const char *vidname = udev_device_get_sysattr_value(p, "idVendor");
			const char *pidname = udev_device_get_sysattr_value(p, "idProduct");
			const char *vername = udev_device_get_sysattr_value(p, "bcdDevice");
			const char *sernum = udev_device_get_sysattr_value(p, "serial");
			if (!vidname || !pidname || !vername) continue;
			int vid, pid, ver;
			if (sscanf(vidname, "%x", &vid) != 1) continue;
			if (sscanf(pidname, "%x", &pid) != 1) continue;
			if (sscanf(vername, "%x", &ver) != 1) continue;
			serial_add(syspath, devnode, vid, pid, ver, sernum);
			break;
		} else if (strcmp(subsys, "pnp") == 0) {
			serial_add(syspath, devnode, 0, 0, 0, NULL);
			break;
		} else {
			// unknown subsystem
		}
	}
}

void udev_remove(struct udev_device *dev)
{
	//logfile("udev_remove\n");
	const char *syspath = udev_device_get_syspath(dev);
	if (syspath) serial_remove(syspath);
}



void *usb_scan_thread(void *unused)
{
	struct udev *udev = udev_new();
	if (!udev) die("unable to use udev");
	struct udev_monitor *mon = udev_monitor_new_from_netlink(udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(mon, "tty", NULL);
	udev_monitor_enable_receiving(mon);

	struct udev_enumerate *enumerate = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(enumerate, "tty");
	udev_enumerate_scan_devices(enumerate);
	struct udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
	struct udev_list_entry *entry;
	udev_list_entry_foreach(entry, devices) {
		const char *path = udev_list_entry_get_name(entry);
		struct udev_device *dev = udev_device_new_from_syspath(udev, path);
		if (dev) udev_add(dev);
	}
	udev_enumerate_unref(enumerate);
	//struct timespec ts;
	//clock_gettime(CLOCK_MONOTONIC, &ts);
	//logfile("   usb initial scan complete at %lu.%03u\n", ts.tv_sec, ts.tv_nsec/1000000);
	unlock_initial_scan();
	int mon_fd = udev_monitor_get_fd(mon);
	int errcount = 0;
	while (1) {
		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(mon_fd, &rd);
		int r = select(mon_fd + 1, &rd, NULL, NULL, NULL);
		if (r < 0) {
			if (++errcount > 100) die("udev monitor error");
		} else if (r == 0) {
			continue; // timeout
		} else {
			errcount = 0;
			if (FD_ISSET(mon_fd, &rd)) {
				//logfile("  usb_device_scan:\n");
				struct udev_device *dev = udev_monitor_receive_device(mon);
				if (dev) {
					const char *action = udev_device_get_action(dev);
					//logfile("    action = %s\n", action);
					if (action) {
						if (strcmp(action, "add") == 0) {
							udev_add(dev);
						} else if (strcmp(action, "remove") == 0) {
							udev_remove(dev);
						}
					}
					udev_device_unref(dev);
				}
			}
		}
	}
	return NULL;
}


int main(int argc, char **argv)
{
	char line[256], program[256];
	int version, wait_for_initial_scan=1;
	struct serial_struct *list=NULL;
	//struct timespec ts;

	pthread_mutex_init(&mutex1, NULL);
	pthread_mutex_init(&mutex2, NULL);
	pthread_mutex_init(&mutex3, NULL);
	//logfile("\n\n***** serial_discovery start *****\n");
	lock_initial_scan();
	pthread_create(&thread1, NULL, usb_scan_thread, NULL);
	while (fgets(line, sizeof(line), stdin)) {
		//clock_gettime(CLOCK_MONOTONIC, &ts);
		if (strncmp(line, "LIST", 4) == 0) {
			//logfile("LIST at %lu.%03u\n", ts.tv_sec, ts.tv_nsec/1000000);
			if (wait_for_initial_scan) {
				lock_initial_scan();
				wait_for_initial_scan = 0;
				unlock_initial_scan();
			}
			list = copy_serial_list();
			print_serial_list(list);
			free_serial_list(list);
			list = NULL;
		} else
		if (sscanf(line, "HELLO %d \"%[^\"\r\n]\"", &version, program) == 2) {
			//logfile("HELLO at %lu.%03u\n", ts.tv_sec, ts.tv_nsec/1000000);
			json("{\n\"eventType\": \"hello\",\n"
				"\"protocolVersion\": 1,\n"
				"\"message\": \"OK\"\n}\n");
			state = STATE_IDLE;
		} else
		if (strncmp(line, "START_SYNC", 10) == 0) {
			state = STATE_IDLE;
			//logfile("START_SYNC at %lu.%03u\n", ts.tv_sec, ts.tv_nsec/1000000);
			json("{\n\"eventType\": \"start_sync\",\n"
  				"\"message\": \"OK\"\n}\n");
			if (wait_for_initial_scan) {
				lock_initial_scan();
				wait_for_initial_scan = 0;
				unlock_initial_scan();
			}
			list = copy_serial_list();
			print_serial_list_add(list);
			state = STATE_SYNC;
			free_serial_list(list);
			list = NULL;
		} else
		if (strncmp(line, "START", 5) == 0) {
			//logfile("START at %lu.%03u\n", ts.tv_sec, ts.tv_nsec/1000000);
			json("{\n\"eventType\": \"start\",\n"
  				"\"message\": \"OK\"\n}\n");
			state = STATE_RUN;
		} else
		if (strncmp(line, "STOP", 4) == 0) {
			state = STATE_IDLE;
			//logfile("STOP at %lu.%03u\n", ts.tv_sec, ts.tv_nsec/1000000);
			json("{\n\"eventType\": \"stop\",\n"
  				"\"message\": \"OK\"\n}\n");
		} else
		if (strncmp(line, "QUIT", 4) == 0) {
			//logfile("QUIT at %lu.%03u\n", ts.tv_sec, ts.tv_nsec/1000000);
			json("{\n\"eventType\": \"quit\",\n"
  				"\"message\": \"OK\"\n}\n");
			state = STATE_DEAD;
		} else {
			//logfile("main unknown command: %s\n", line);
			// TODO: JSON error response
		}
		//clock_gettime(CLOCK_MONOTONIC, &ts);
		//logfile(" reply complete at %lu.%03u\n", ts.tv_sec, ts.tv_nsec/1000000);
		if (state == STATE_DEAD) break;
	}
	return 0;
}

void die(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	fprintf(stderr, "mktinyfat: ");
	vfprintf(stderr, format, args);
	va_end(args);
	exit(1);
}

size_t strlcpy(char *dst, const char *src, size_t maxlen)
{
	const size_t srclen = strlen(src);
	if (srclen + 1 < maxlen) {
		memcpy(dst, src, srclen + 1);
	} else if (maxlen != 0) {
		memcpy(dst, src, maxlen - 1);
		dst[maxlen-1] = '\0';
	}
	return srclen;
}

