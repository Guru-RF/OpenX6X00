#include "usb_devices.h"

#include "scheduler.h"

#include <libudev.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <dirent.h>


extern "C" {
    #include <sys/select.h>
    #include "lvgl/lvgl.h"
    #include "pubsub_ids.h"
}

static struct udev *udev;
static struct udev_device *dev;
static struct udev_monitor *mon;
static int fd;

static void notify_device_added(void *) {
    lv_msg_send(MSG_USB_DEVICE_CHANGED, (void *)USB_DEV_ADDED);
}
static void notify_device_removed(void *) {
    lv_msg_send(MSG_USB_DEVICE_CHANGED, (void *)USB_DEV_REMOVED);
}

static void wait_new_device() {
	while (1) {
		fd_set fds;
		struct timeval tv;
		int ret;

		FD_ZERO(&fds);
		FD_SET(fd, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = 0;

		ret = select(fd+1, &fds, NULL, NULL, &tv);
		if (ret > 0 && FD_ISSET(fd, &fds)) {
			dev = udev_monitor_receive_device(mon);
            if (dev) {
                const char* action = udev_device_get_action(dev);
                if (action && strcmp(action, "add") == 0) {
                    scheduler_put_noargs(notify_device_added);
                } else if (action && strcmp(action, "remove") == 0) {
                    scheduler_put_noargs(notify_device_removed);
                }
                udev_device_unref(dev);
            }
			// if (dev && (strcmp(udev_device_get_action(dev), "add") == 0)) {
			// 	/* free dev */
			// 	udev_device_unref(dev);
            //     return;
			// }
		}
		/* 500 milliseconds */
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}
}

#ifdef BOARD_X6200
/* One sysfs attribute of a USB device, its newline cut; "" when it has none */
static void usb_attr(const char *node, const char *name, char *out, size_t size) {
    char  path[288];
    FILE *f;

    out[0] = '\0';
    snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/%s", node, name);
    if ((f = fopen(path, "r"))) {
        if (fgets(out, size, f)) out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}

/* What is on the HOST port, into the log at the start: bus 3, the SoC's OTG port as a host (the bench X6200's USB
 * Ethernet showed up as 3-1; its own WiFi is on bus 1). For when the WR12 can be told from what it presents there:
 * its USB ids, if it has any, are not known yet */
static void log_host_port() {
    DIR           *d = opendir("/sys/bus/usb/devices");
    struct dirent *e;
    int            n = 0;

    if (!d) return;
    while ((e = readdir(d))) {
        char vid[8], pid[8], product[64];

        if (strncmp(e->d_name, "3-", 2) || strchr(e->d_name, ':')) continue;     /* bus 3's devices, not interfaces */
        usb_attr(e->d_name, "idVendor", vid, sizeof(vid));
        usb_attr(e->d_name, "idProduct", pid, sizeof(pid));
        usb_attr(e->d_name, "product", product, sizeof(product));
        LV_LOG_USER("USB on HOST: %s %s:%s %s", e->d_name, vid, pid, product);
        n++;
    }
    closedir(d);
    if (!n) LV_LOG_USER("USB on HOST: nothing");
}
#endif

void usb_devices_monitor_init() {
#ifdef BOARD_X6200
    log_host_port();
#endif
    /* create udev object */
	udev = udev_new();
	if (!udev) {
		LV_LOG_ERROR("Cannot create udev context.");
	}
    mon = udev_monitor_new_from_netlink(udev, "udev");
	udev_monitor_filter_add_match_subsystem_devtype(mon, "usb", NULL);
	udev_monitor_filter_add_match_subsystem_devtype(mon, "input", NULL);	/* Bluetooth keyboards */
	udev_monitor_enable_receiving(mon);
	fd = udev_monitor_get_fd(mon);

    std::thread t1(wait_new_device);
    t1.detach();
}
