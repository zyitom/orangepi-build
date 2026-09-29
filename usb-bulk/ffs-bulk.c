/*
 * ffs-bulk.c -- FunctionFS bulk echo/stream daemon for the A733.
 *
 * The board becomes a vendor-specific USB device with two BULK
 * endpoints (IN 0x81 / OUT 0x01, high-speed 512 B). The PC talks to it
 * with libusb/pyusb bulk transfers; every buffer received on OUT is
 * echoed back on IN, which exercises both directions and lets the host
 * measure real throughput.
 *
 *   ./ffs-bulk                       # echo mode
 *   ./ffs-bulk stream                # stream mode: push a pattern on IN
 *
 * Run bulk-gadget.sh first: it sets up configfs, mounts functionfs and
 * binds the UDC, then starts this daemon.
 *
 * Build: aarch64-linux-gnu-gcc -O2 -o ffs-bulk ffs-bulk.c   (no deps)
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <endian.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#define FFS_DIR        "/dev/ffs-bulk"
#define BULK_BUF_SIZE  (256 * 1024)

/* --- descriptors (board-kernel FFS v2 ABI, uapi functionfs.h) ------- */
struct ffs_head_v2 {			/* magic, length, flags */
	uint32_t magic, length, flags;
} __attribute__((packed));

#define FFS_MAGIC_V2		3u	/* FUNCTIONFS_DESCRIPTORS_MAGIC_V2 */
#define FFS_MAGIC_STRINGS	2u	/* FUNCTIONFS_STRINGS_MAGIC */
#define FFS_HAS_FS		1u
#define FFS_HAS_HS		2u

/* one interface (vendor class) + 2 bulk EPs, described for both full
 * and high speed; the fs/hs blobs follow the counts in the v2 header */
static const struct {
	struct ffs_head_v2 head;
	uint32_t fs_count, hs_count;
	/* full-speed blob */
	struct usb_interface_descriptor intf;
	struct usb_endpoint_descriptor_no_audio ep_sink;	/* OUT host->dev */
	struct usb_endpoint_descriptor_no_audio ep_source;	/* IN dev->host */
	/* high-speed blob (same content, 512 B packets) */
	struct usb_interface_descriptor intf2;
	struct usb_endpoint_descriptor_no_audio ep_sink2;
	struct usb_endpoint_descriptor_no_audio ep_source2;
} __attribute__((packed)) descs = {
	.head = { FFS_MAGIC_V2, 66, FFS_HAS_FS | FFS_HAS_HS },
	.fs_count = 2,
	.hs_count = 2,
	.intf = {
		.bLength = sizeof(descs.intf),
		.bDescriptorType = USB_DT_INTERFACE,
		.bNumEndpoints = 2,
		.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
		.iInterface = 1,
	},
	.ep_sink = {
		.bLength = sizeof(descs.ep_sink),
		.bDescriptorType = USB_DT_ENDPOINT,
		.bEndpointAddress = 1 | USB_DIR_OUT,
		.bmAttributes = USB_ENDPOINT_XFER_BULK,
		.wMaxPacketSize = 512,
	},
	.ep_source = {
		.bLength = sizeof(descs.ep_source),
		.bDescriptorType = USB_DT_ENDPOINT,
		.bEndpointAddress = 1 | USB_DIR_IN,
		.bmAttributes = USB_ENDPOINT_XFER_BULK,
		.wMaxPacketSize = 512,
	},
	.intf2 = {
		.bLength = sizeof(descs.intf2),
		.bDescriptorType = USB_DT_INTERFACE,
		.bNumEndpoints = 2,
		.bInterfaceClass = USB_CLASS_VENDOR_SPEC,
		.iInterface = 1,
	},
	.ep_sink2 = {
		.bLength = sizeof(descs.ep_sink2),
		.bDescriptorType = USB_DT_ENDPOINT,
		.bEndpointAddress = 1 | USB_DIR_OUT,
		.bmAttributes = USB_ENDPOINT_XFER_BULK,
		.wMaxPacketSize = 512,
	},
	.ep_source2 = {
		.bLength = sizeof(descs.ep_source2),
		.bDescriptorType = USB_DT_ENDPOINT,
		.bEndpointAddress = 1 | USB_DIR_IN,
		.bmAttributes = USB_ENDPOINT_XFER_BULK,
		.wMaxPacketSize = 512,
	},
};

static const struct {
	struct ffs_head_v2 head;	/* magic=2; length; str_count; lang_count */
	uint32_t str_count, lang_count;
	struct { uint32_t lang; char data[16]; } __attribute__((packed)) str0;
} __attribute__((packed)) strings = {
	.head = { FFS_MAGIC_STRINGS, 32, 0 },
	.str_count = 1,
	.lang_count = 1,
	.str0 = { 0x0409, "A733 bulk pipe" },
};

int main(int argc, char **argv)
{
	int stream = argc > 1 && !strcmp(argv[1], "stream");
	char path[128];
	int ep0, epin = -1, epout = -1;
	ssize_t n;
	uint8_t *buf = malloc(BULK_BUF_SIZE);
	unsigned long long total = 0;
	struct timespec t0, t1;

	if (!buf)
		return 1;

	snprintf(path, sizeof(path), FFS_DIR "/ep0");
	ep0 = open(path, O_RDWR);
	if (ep0 < 0) {
		perror("ep0 (run bulk-gadget.sh first)");
		return 1;
	}
	if (write(ep0, &descs, sizeof(descs)) != sizeof(descs) ||
	    write(ep0, &strings, sizeof(strings)) != sizeof(strings)) {
		perror("ep0 descriptors");
		return 1;
	}

	/* wait for SET_CONFIGURATION: endpoints become readable */
	for (;;) {
		struct usb_functionfs_event ev;
		n = read(ep0, &ev, sizeof(ev));
		if (n != sizeof(ev))
			continue;
		if (ev.type == FUNCTIONFS_ENABLE)
			break;
	}

	snprintf(path, sizeof(path), FFS_DIR "/ep1");	/* IN  */
	epin = open(path, O_RDWR);
	snprintf(path, sizeof(path), FFS_DIR "/ep2");	/* OUT */
	epout = open(path, O_RDWR);
	if (epin < 0 || epout < 0) {
		perror("bulk endpoints");
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	fprintf(stderr, "ffs-bulk: online, %s mode\n",
		stream ? "stream" : "echo");

	for (;;) {
		if (stream) {
			memset(buf, 0xA5, BULK_BUF_SIZE);
			n = write(epin, buf, BULK_BUF_SIZE);
			if (n < 0) {
				if (errno == ESHUTDOWN || errno == EPIPE)
					break;	/* host detached */
				perror("write IN");
				break;
			}
		} else {
			n = read(epout, buf, BULK_BUF_SIZE);
			if (n < 0) {
				if (errno == ESHUTDOWN || errno == EPIPE)
					break;
				perror("read OUT");
				break;
			}
			if (write(epin, buf, n) != n) {
				perror("write IN");
				break;
			}
		}
		total += n;
		clock_gettime(CLOCK_MONOTONIC, &t1);
		if (t1.tv_sec > t0.tv_sec) {
			double mb = total / 1048576.0;
			double dt = (t1.tv_sec - t0.tv_sec) +
				    (t1.tv_nsec - t0.tv_nsec) / 1e9;
			fprintf(stderr, "ffs-bulk: %.1f MB in %.1f s = %.1f MB/s\n",
				mb, dt, mb / dt);
			total = 0;
			t0 = t1;
		}
	}

	fprintf(stderr, "ffs-bulk: host detached, exit\n");
	return 0;
}
