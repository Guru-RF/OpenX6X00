#include "usb_devices.h"

#include "scheduler.h"

#include <libudev.h>
#include <chrono>
#include <thread>
#include <cstdio>


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

void usb_devices_monitor_init() {
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
