/*
 * Most of this source has been derived from the Linux USB
 * project:
 * (C) Copyright Linus Torvalds 1999
 * (C) Copyright Johannes Erdfelt 1999-2001
 * (C) Copyright Andreas Gal 1999
 * (C) Copyright Gregory P. Smith 1999
 * (C) Copyright Deti Fliegl 1999 (new USB architecture)
 * (C) Copyright Randy Dunlap 2000
 * (C) Copyright David Brownell 2000 (kernel hotplug, usb_device_id)
 * (C) Copyright Yggdrasil Computing, Inc. 2000
 *     (usb_device_id matching changes by Adam J. Richter)
 *
 * Adapted for U-Boot:
 * (C) Copyright 2001 Denis Peter, MPL AG Switzerland
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

/****************************************************************************
 * HUB "Driver"
 * Probes device for being a hub and configurate it
 */

#include <common.h>
#include <command.h>
#include <linux/ctype.h>
#include <asm/addrspace.h>
#include <asm/byteorder.h>
#include <asm/unaligned.h>

#include <usb.h>
#ifdef CONFIG_4xx
#include <asm/4xx_pci.h>
#endif

//#define USB_HUB_DEBUG

#ifdef USB_HUB_DEBUG
#define USB_HUB_PRINTF(fmt,args...)	printf (fmt ,##args)
#else
#define USB_HUB_PRINTF(fmt,args...)
#endif

#define USB_BUFSIZ	512

#define CONFIG_SYS_HZ	CFG_HZ

static struct usb_hub_device hub_dev[USB_MAX_HUB];
static int usb_hub_index;

void usb_hub_reset_devices(int port)
{
	return;
}

static int usb_get_hub_descriptor(struct usb_device *dev, void *data, int size)
{
	return usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
		USB_REQ_GET_DESCRIPTOR, USB_DIR_IN | USB_RT_HUB,
		USB_DT_HUB << 8, 0, data, size, USB_CNTL_TIMEOUT);
}

static int usb_clear_port_feature(struct usb_device *dev, int port, int feature)
{
	return usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
				USB_REQ_CLEAR_FEATURE, USB_RT_PORT, feature,
				port, NULL, 0, USB_CNTL_TIMEOUT);
}

static int usb_set_port_feature(struct usb_device *dev, int port, int feature)
{
	return usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
				USB_REQ_SET_FEATURE, USB_RT_PORT, feature,
				port, NULL, 0, USB_CNTL_TIMEOUT);
}

static int usb_get_hub_status(struct usb_device *dev, void *data)
{
	return usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
			USB_REQ_GET_STATUS, USB_DIR_IN | USB_RT_HUB, 0, 0,
			data, sizeof(struct usb_hub_status), USB_CNTL_TIMEOUT);
}

static int usb_get_port_status(struct usb_device *dev, int port, void *data)
{
	return usb_control_msg(dev, usb_rcvctrlpipe(dev, 0),
			USB_REQ_GET_STATUS, USB_DIR_IN | USB_RT_PORT, 0, port,
			data, sizeof(struct usb_hub_status), USB_CNTL_TIMEOUT);
}

static void usb_hub_power_on(struct usb_hub_device *hub)
{
	int i;
	struct usb_device *dev;
	unsigned pgood_delay = hub->desc.bPwrOn2PwrGood * 2;

	dev = hub->pusb_dev;

	USB_HUB_PRINTF("enabling power on all ports\n");
	for (i = 0; i < dev->maxchild; i++) {
		usb_set_port_feature(dev, i + 1, USB_PORT_FEAT_POWER);
		USB_HUB_PRINTF("port %d returns %lX\n", i + 1, dev->status);
	}

	/*
	 * Wait for power to become stable,
	 * plus spec-defined max time for device to connect
	 */
	mdelay(pgood_delay + 200);
}

void usb_hub_reset(void)
{
	usb_hub_index = 0;
}

static struct usb_hub_device *usb_hub_allocate(void)
{
	if (usb_hub_index < USB_MAX_HUB)
		return &hub_dev[usb_hub_index++];

	printf("ERROR: USB_MAX_HUB (%d) reached\n", USB_MAX_HUB);
	return NULL;
}

#define MAX_TRIES 5

static inline char *portspeed(int portstatus)
{
	char *speed_str;

	switch (portstatus & USB_PORT_STAT_SPEED_MASK) {
	case USB_PORT_STAT_SUPER_SPEED:
		speed_str = "5 Gb/s";
		break;
	case USB_PORT_STAT_HIGH_SPEED:
		speed_str = "480 Mb/s";
		break;
	case USB_PORT_STAT_LOW_SPEED:
		speed_str = "1.5 Mb/s";
		break;
	default:
		speed_str = "12 Mb/s";
		break;
	}

	return speed_str;
}

int hub_port_reset(struct usb_device *dev, int port,
			unsigned short *portstat)
{
	int err, tries;
	ALLOC_CACHE_ALIGN_BUFFER(struct usb_port_status, portsts_buf, 1);
	struct usb_port_status *portsts = KSEG1ADDR(portsts_buf);
	unsigned short portstatus, portchange;

	USB_HUB_PRINTF("hub_port_reset: resetting port %d...\n", port);
	for (tries = 0; tries < MAX_TRIES; tries++) {
		err = usb_set_port_feature(dev, port + 1, USB_PORT_FEAT_RESET);
		if (err < 0)
			return err;

		mdelay(200);

		if (usb_get_port_status(dev, port + 1, portsts) < 0) {
			USB_HUB_PRINTF("get_port_status failed status %lX\n",
			      dev->status);
			return -1;
		}
		portstatus = le16_to_cpu(portsts->wPortStatus);
		portchange = le16_to_cpu(portsts->wPortChange);

		USB_HUB_PRINTF("portstatus %x, change %x, %s\n", portstatus, portchange,
							portspeed(portstatus));

		USB_HUB_PRINTF("STAT_C_CONNECTION = %d STAT_CONNECTION = %d" \
		      "  USB_PORT_STAT_ENABLE %d\n",
		      (portchange & USB_PORT_STAT_C_CONNECTION) ? 1 : 0,
		      (portstatus & USB_PORT_STAT_CONNECTION) ? 1 : 0,
		      (portstatus & USB_PORT_STAT_ENABLE) ? 1 : 0);

		/*
		 * Perhaps we should check for the following here:
		 * - C_CONNECTION hasn't been set.
		 * - CONNECTION is still set.
		 *
		 * Doing so would ensure that the device is still connected
		 * to the bus, and hasn't been unplugged or replaced while the
		 * USB bus reset was going on.
		 *
		 * However, if we do that, then (at least) a San Disk Ultra
		 * USB 3.0 16GB device fails to reset on (at least) an NVIDIA
		 * Tegra Jetson TK1 board. For some reason, the device appears
		 * to briefly drop off the bus when this second bus reset is
		 * executed, yet if we retry this loop, it'll eventually come
		 * back after another reset or two.
		 */

		if (portstatus & USB_PORT_STAT_ENABLE)
			break;

		mdelay(200);
	}

	if (tries == MAX_TRIES) {
		USB_HUB_PRINTF("Cannot enable port %i after %i retries, " \
		      "disabling port.\n", port + 1, MAX_TRIES);
		USB_HUB_PRINTF("Maybe the USB cable is bad?\n");
		return -1;
	}

	usb_clear_port_feature(dev, port + 1, USB_PORT_FEAT_C_RESET);
	*portstat = portstatus;
	return 0;
}


void usb_hub_port_connect_change(struct usb_device *dev, int port)
{
	struct usb_device *usb;
	ALLOC_CACHE_ALIGN_BUFFER(struct usb_port_status, portsts_buf, 1);
	struct usb_port_status *portsts = KSEG1ADDR(portsts_buf);
	unsigned short portstatus;

	/* Check status */
	if (usb_get_port_status(dev, port + 1, portsts) < 0) {
		USB_HUB_PRINTF("get_port_status failed\n");
		return;
	}

	portstatus = le16_to_cpu(portsts->wPortStatus);
	USB_HUB_PRINTF("portstatus %x, change %x, %s\n",
	      portstatus,
	      le16_to_cpu(portsts->wPortChange),
	      portspeed(portstatus));

	/* Clear the connection change status */
	usb_clear_port_feature(dev, port + 1, USB_PORT_FEAT_C_CONNECTION);

	/* Disconnect any existing devices under this port */
	if (((!(portstatus & USB_PORT_STAT_CONNECTION)) &&
	     (!(portstatus & USB_PORT_STAT_ENABLE))) || (dev->children[port])) {
		USB_HUB_PRINTF("usb_disconnect(&hub->children[port]);\n");
		/* Return now if nothing is connected */
		if (!(portstatus & USB_PORT_STAT_CONNECTION))
			return;
	}
	mdelay(200);

	/* Reset the port */
	if (hub_port_reset(dev, port, &portstatus) < 0) {
		printf("cannot reset port %i!?\n", port + 1);
		return;
	}

	mdelay(200);

	/* Allocate a new device struct for it */
	usb = usb_alloc_new_device(dev->controller);

	switch (portstatus & USB_PORT_STAT_SPEED_MASK) {
	case USB_PORT_STAT_SUPER_SPEED:
		usb->speed = USB_SPEED_SUPER;
		break;
	case USB_PORT_STAT_HIGH_SPEED:
		usb->speed = USB_SPEED_HIGH;
		break;
	case USB_PORT_STAT_LOW_SPEED:
		usb->speed = USB_SPEED_LOW;
		break;
	default:
		usb->speed = USB_SPEED_FULL;
		break;
	}

	dev->children[port] = usb;
	usb->parent = dev;
	usb->portnr = port + 1;
	/* Run it through the hoops (find a driver, etc) */
	if (usb_new_device(usb)) {
		/* Woops, disable the port */
		usb_free_device();
		dev->children[port] = NULL;
		USB_HUB_PRINTF("hub: disabling port %d\n", port + 1);
		usb_clear_port_feature(dev, port + 1, USB_PORT_FEAT_ENABLE);
	}
}


static int usb_hub_configure(struct usb_device *dev)
{
	int i, length;
	ALLOC_CACHE_ALIGN_BUFFER(unsigned char, buffer_buf, USB_BUFSIZ);
	unsigned char *buffer = KSEG1ADDR(buffer_buf);
	unsigned char *bitmap;
	short hubCharacteristics;
	struct usb_hub_descriptor *descriptor;
	struct usb_hub_device *hub;
	struct usb_hub_status *hubsts;

	/* "allocate" Hub device */
	hub = usb_hub_allocate();
	if (hub == NULL)
		return -1;
	hub->pusb_dev = dev;
	/* Get the the hub descriptor */
	if (usb_get_hub_descriptor(dev, buffer, 4) < 0) {
		USB_HUB_PRINTF("usb_hub_configure: failed to get hub " \
		      "descriptor, giving up %lX\n", dev->status);
		return -1;
	}
	descriptor = (struct usb_hub_descriptor *)buffer;

	length = min(descriptor->bLength, sizeof(struct usb_hub_descriptor));

	if (usb_get_hub_descriptor(dev, buffer, length) < 0) {
		USB_HUB_PRINTF("usb_hub_configure: failed to get hub " \
		      "descriptor 2nd giving up %lX\n", dev->status);
		return -1;
	}
	memcpy((unsigned char *)&hub->desc, buffer, length);
	/* adjust 16bit values */
	put_unaligned(le16_to_cpu(get_unaligned(
			&descriptor->wHubCharacteristics)),
			&hub->desc.wHubCharacteristics);
	/* set the bitmap */
	bitmap = (unsigned char *)&hub->desc.DeviceRemovable[0];
	/* devices not removable by default */
	memset(bitmap, 0xff, (USB_MAXCHILDREN+1+7)/8);
	bitmap = (unsigned char *)&hub->desc.PortPowerCtrlMask[0];
	memset(bitmap, 0xff, (USB_MAXCHILDREN+1+7)/8); /* PowerMask = 1B */

	for (i = 0; i < ((hub->desc.bNbrPorts + 1 + 7)/8); i++)
		hub->desc.DeviceRemovable[i] = descriptor->DeviceRemovable[i];

	for (i = 0; i < ((hub->desc.bNbrPorts + 1 + 7)/8); i++)
		hub->desc.PortPowerCtrlMask[i] = descriptor->PortPowerCtrlMask[i];

	dev->maxchild = descriptor->bNbrPorts;
	USB_HUB_PRINTF("%d ports detected\n", dev->maxchild);

#ifdef USB_HUB_DEBUG
	hubCharacteristics = get_unaligned(&hub->desc.wHubCharacteristics);
	switch (hubCharacteristics & HUB_CHAR_LPSM) {
	case 0x00:
		USB_HUB_PRINTF("ganged power switching\n");
		break;
	case 0x01:
		USB_HUB_PRINTF("individual port power switching\n");
		break;
	case 0x02:
	case 0x03:
		USB_HUB_PRINTF("unknown reserved power switching mode\n");
		break;
	}

	if (hubCharacteristics & HUB_CHAR_COMPOUND)
		USB_HUB_PRINTF("part of a compound device\n");
	else
		USB_HUB_PRINTF("standalone hub\n");

	switch (hubCharacteristics & HUB_CHAR_OCPM) {
	case 0x00:
		USB_HUB_PRINTF("global over-current protection\n");
		break;
	case 0x08:
		USB_HUB_PRINTF("individual port over-current protection\n");
		break;
	case 0x10:
	case 0x18:
		USB_HUB_PRINTF("no over-current protection\n");
		break;
	}

	USB_HUB_PRINTF("power on to power good time: %dms\n",
	      descriptor->bPwrOn2PwrGood * 2);
	USB_HUB_PRINTF("hub controller current requirement: %dmA\n",
	      descriptor->bHubContrCurrent);

	for (i = 0; i < dev->maxchild; i++)
		USB_HUB_PRINTF("port %d is%s removable\n", i + 1,
		      hub->desc.DeviceRemovable[(i + 1) / 8] & \
		      (1 << ((i + 1) % 8)) ? " not" : "");
#endif

	if (sizeof(struct usb_hub_status) > USB_BUFSIZ) {
		USB_HUB_PRINTF("usb_hub_configure: failed to get Status - " \
		      "too long: %d\n", descriptor->bLength);
		return -1;
	}

	if (usb_get_hub_status(dev, buffer) < 0) {
		USB_HUB_PRINTF("usb_hub_configure: failed to get Status %lX\n",
		      dev->status);
		return -1;
	}


#ifdef USB_HUB_DEBUG
	hubsts = (struct usb_hub_status *)buffer;

	USB_HUB_PRINTF("get_hub_status returned status %X, change %X\n",
	      le16_to_cpu(hubsts->wHubStatus),
	      le16_to_cpu(hubsts->wHubChange));
	USB_HUB_PRINTF("local power source is %s\n",
	      (le16_to_cpu(hubsts->wHubStatus) & HUB_STATUS_LOCAL_POWER) ? \
	      "lost (inactive)" : "good");
	USB_HUB_PRINTF("%sover-current condition exists\n",
	      (le16_to_cpu(hubsts->wHubStatus) & HUB_STATUS_OVERCURRENT) ? \
	      "" : "no ");
#endif

	usb_hub_power_on(hub);

	/*
	 * Reset any devices that may be in a bad state when applying
	 * the power.  This is a __weak function.  Resetting of the devices
	 * should occur in the board file of the device.
	 */
	for (i = 0; i < dev->maxchild; i++)
		usb_hub_reset_devices(i + 1);

	for (i = 0; i < dev->maxchild; i++) {
		ALLOC_CACHE_ALIGN_BUFFER(struct usb_port_status, portsts_buf, 1);
		struct usb_port_status *portsts = KSEG1ADDR(portsts_buf);
		unsigned short portstatus, portchange;
		int ret;
		uint delay = CONFIG_SYS_HZ / 2;
		ulong start = get_timer(0);

		/*
		 * Wait for (whichever finishes first)
		 *  - A maximum of 10 seconds
		 *    This is a purely observational value driven by connecting
		 *    a few broken pen drives and taking the max * 1.5 approach
		 *  - connection_change and connection state to report same
		 *    state
		 */
		do {
			ret = usb_get_port_status(dev, i + 1, portsts);
			if (ret < 0) {
				USB_HUB_PRINTF("get_port_status failed\n");
				break;
			}

			portstatus = le16_to_cpu(portsts->wPortStatus);
			portchange = le16_to_cpu(portsts->wPortChange);

			/* No connection change happened, wait a bit more. */
			if (!(portchange & USB_PORT_STAT_C_CONNECTION))
				continue;

			/* Test if the connection came up, and if so, exit. */
			if (portstatus & USB_PORT_STAT_CONNECTION)
				break;

		} while (get_timer(start) < delay);

		if (ret < 0)
			continue;

		mdelay(1);

		USB_HUB_PRINTF("Port %d Status %X Change %X\n",
		      i + 1, portstatus, portchange);

		if (portchange & USB_PORT_STAT_C_CONNECTION) {
			USB_HUB_PRINTF("port %d connection change\n", i + 1);
			usb_hub_port_connect_change(dev, i);
		}
		if (portchange & USB_PORT_STAT_C_ENABLE) {
			USB_HUB_PRINTF("port %d enable change, status %x\n",
			      i + 1, portstatus);
			usb_clear_port_feature(dev, i + 1,
						USB_PORT_FEAT_C_ENABLE);
			/*
			 * The following hack causes a ghost device problem
			 * to Faraday EHCI
			 */
#ifndef CONFIG_USB_EHCI_FARADAY
			/* EM interference sometimes causes bad shielded USB
			 * devices to be shutdown by the hub, this hack enables
			 * them again. Works at least with mouse driver */
			if (!(portstatus & USB_PORT_STAT_ENABLE) &&
			     (portstatus & USB_PORT_STAT_CONNECTION) &&
			     ((dev->children[i]))) {
				USB_HUB_PRINTF("already running port %i "  \
				      "disabled by hub (EMI?), " \
				      "re-enabling...\n", i + 1);
				      usb_hub_port_connect_change(dev, i);
			}
#endif
		}
		if (portstatus & USB_PORT_STAT_SUSPEND) {
			USB_HUB_PRINTF("port %d suspend change\n", i + 1);
			usb_clear_port_feature(dev, i + 1,
						USB_PORT_FEAT_SUSPEND);
		}

		if (portchange & USB_PORT_STAT_C_OVERCURRENT) {
			USB_HUB_PRINTF("port %d over-current change\n", i + 1);
			usb_clear_port_feature(dev, i + 1,
						USB_PORT_FEAT_C_OVER_CURRENT);
			usb_hub_power_on(hub);
		}

		if (portchange & USB_PORT_STAT_C_RESET) {
			USB_HUB_PRINTF("port %d reset change\n", i + 1);
			usb_clear_port_feature(dev, i + 1,
						USB_PORT_FEAT_C_RESET);
		}
	} /* end for i all ports */

	return 0;
}

int usb_hub_probe(struct usb_device *dev, int ifnum)
{
	struct usb_interface *iface;
	struct usb_endpoint_descriptor *ep;
	int ret;

	iface = &dev->config.if_desc[ifnum];
	/* Is it a hub? */
	if (iface->desc.bInterfaceClass != USB_CLASS_HUB)
		return 0;
	/* Some hubs have a subclass of 1, which AFAICT according to the */
	/*  specs is not defined, but it works */
	if ((iface->desc.bInterfaceSubClass != 0) &&
	    (iface->desc.bInterfaceSubClass != 1))
		return 0;
	/* Multiple endpoints? What kind of mutant ninja-hub is this? */
	if (iface->desc.bNumEndpoints != 1)
		return 0;
	ep = &iface->ep_desc[0];
	/* Output endpoint? Curiousier and curiousier.. */
	if (!(ep->bEndpointAddress & USB_DIR_IN))
		return 0;
	/* If it's not an interrupt endpoint, we'd better punt! */
	if ((ep->bmAttributes & 3) != 3)
		return 0;
	/* We found a hub */
	USB_HUB_PRINTF("USB hub found\n");
	ret = usb_hub_configure(dev);
	return ret;
}

