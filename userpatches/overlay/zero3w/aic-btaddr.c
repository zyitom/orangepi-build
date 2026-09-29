/*
 * aic-btaddr — give the AIC8800 Bluetooth controller this board's own address.
 *
 * hciattach_opi's "aic" init always programs the preset 10:11:12:13:14:15
 * (vendor command 0xFC70) and ignores both its bdaddr argument and the
 * addr_mgt file, so every board shows up with the same address. This
 * sends 0xFC70 again with the address addr_mgt derives from the chip ID
 * (/sys/class/addr_mgt/addr_bt) and cycles hci0 so the kernel reads it back.
 *
 *   aic-btaddr [hciN] [XX:XX:XX:XX:XX:XX]
 */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#define BTPROTO_HCI	1
#define HCI_CHANNEL_RAW	0
#define SOL_HCI		0
#define HCI_FILTER	2
#define HCIDEVUP	_IOW('H', 201, int)
#define HCIDEVDOWN	_IOW('H', 202, int)
#define HCIGETDEVINFO	_IOR('H', 211, int)

struct sockaddr_hci { sa_family_t hci_family; unsigned short hci_dev, hci_channel; };
struct hci_filter { unsigned int type_mask; unsigned int event_mask[2]; unsigned short opcode; };
struct hci_dev_stats { unsigned int err_rx, err_tx, cmd_tx, evt_rx, acl_tx, acl_rx, sco_tx, sco_rx, byte_rx, byte_tx; };
struct hci_dev_info {
	unsigned short dev_id; char name[8]; unsigned char bdaddr[6]; unsigned int flags; unsigned char type;
	unsigned char features[8]; unsigned int pkt_type, link_policy, link_mode;
	unsigned short acl_mtu, acl_pkts, sco_mtu, sco_pkts; struct hci_dev_stats stat;
};

#define AIC_SET_BDADDR	0xFC70	/* OGF 0x3f, OCF 0x070; 6 bytes, little endian */

static int parse_addr(const char *s, unsigned char a[6])
{
	unsigned int b[6];

	if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		a[i] = (unsigned char)b[i];
	return 0;
}

int main(int argc, char **argv)
{
	int dev = 0;
	char txt[32] = "";
	unsigned char a[6];

	if (argc > 1 && !strncmp(argv[1], "hci", 3))
		dev = atoi(argv[1] + 3), argc--, argv++;
	if (argc > 1) {
		snprintf(txt, sizeof(txt), "%s", argv[1]);
	} else {
		FILE *f = fopen("/sys/class/addr_mgt/addr_bt", "r");

		if (!f || !fgets(txt, sizeof(txt), f)) {
			fprintf(stderr, "aic-btaddr: no address given and /sys/class/addr_mgt/addr_bt unreadable\n");
			return 1;
		}
		fclose(f);
	}
	if (parse_addr(txt, a)) {
		fprintf(stderr, "aic-btaddr: bad address '%s'\n", txt);
		return 1;
	}

	int s = socket(AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI);
	struct sockaddr_hci sa = { AF_BLUETOOTH, (unsigned short)dev, HCI_CHANNEL_RAW };
	struct hci_filter flt = { .type_mask = 1u << 4 /* event */, .event_mask = { 1u << 0x0e /* cmd complete */, 0 } };

	if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
	    setsockopt(s, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
		perror("aic-btaddr: hci socket");
		return 1;
	}

	/* H4 command packet: type, opcode (LE), length, address (LE) */
	unsigned char cmd[10] = { 0x01, AIC_SET_BDADDR & 0xff, AIC_SET_BDADDR >> 8, 6 };
	for (int i = 0; i < 6; i++)
		cmd[4 + i] = a[5 - i];
	/*
	 * Raw writes need the device up. At boot hci0 is registered but down
	 * until something opens it (hciattach may still be finishing its own
	 * setup), so bring it up first and retry while it reports ENETDOWN.
	 */
	for (int tries = 0; ; tries++) {
		if (ioctl(s, HCIDEVUP, dev) < 0 && errno != EALREADY && tries == 9) {
			perror("aic-btaddr: HCIDEVUP");
			return 1;
		}
		if (write(s, cmd, sizeof(cmd)) == sizeof(cmd))
			break;
		if (errno != ENETDOWN || tries == 9) {
			perror("aic-btaddr: write");
			return 1;
		}
		sleep(1);
	}

	/* wait for Command Complete of 0xFC70: 04 0e len ncmd op_lo op_hi status */
	unsigned char ev[64];
	struct pollfd p = { s, POLLIN, 0 };
	int ok = 0;

	while (poll(&p, 1, 2000) > 0) {
		ssize_t n = read(s, ev, sizeof(ev));

		if (n >= 7 && ev[1] == 0x0e && ev[4] == (AIC_SET_BDADDR & 0xff) && ev[5] == (AIC_SET_BDADDR >> 8)) {
			ok = ev[6] == 0;
			break;
		}
	}
	if (!ok) {
		fprintf(stderr, "aic-btaddr: controller did not accept 0xFC70\n");
		return 1;
	}

	/* the kernel caches the address from its setup; cycle the device */
	ioctl(s, HCIDEVDOWN, dev);
	if (ioctl(s, HCIDEVUP, dev) < 0 && errno != EALREADY) {
		perror("aic-btaddr: HCIDEVUP");
		return 1;
	}
	struct hci_dev_info di = { .dev_id = (unsigned short)dev };
	if (ioctl(s, HCIGETDEVINFO, &di) == 0)
		printf("hci%d address %02X:%02X:%02X:%02X:%02X:%02X\n", dev, di.bdaddr[5], di.bdaddr[4],
		       di.bdaddr[3], di.bdaddr[2], di.bdaddr[1], di.bdaddr[0]);
	return 0;
}
