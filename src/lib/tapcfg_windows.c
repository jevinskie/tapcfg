/**
 *  tapcfg - A cross-platform configuration utility for TAP driver
 *  Copyright (C) 2008  Juho Vähä-Herttua
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <windows.h>
#include <winioctl.h>

#include "tapcfg.h"
#include "taplog.h"

#define TAP_CONTROL_CODE(request,method)  CTL_CODE(FILE_DEVICE_UNKNOWN, request, method, FILE_ANY_ACCESS)
#define TAP_IOCTL_GET_MAC                 TAP_CONTROL_CODE(1, METHOD_BUFFERED)
#define TAP_IOCTL_GET_VERSION             TAP_CONTROL_CODE(2, METHOD_BUFFERED)
#define TAP_IOCTL_GET_MTU                 TAP_CONTROL_CODE(3, METHOD_BUFFERED)
#define TAP_IOCTL_GET_INFO                TAP_CONTROL_CODE(4, METHOD_BUFFERED)
#define TAP_IOCTL_CONFIG_POINT_TO_POINT   TAP_CONTROL_CODE(5, METHOD_BUFFERED)
#define TAP_IOCTL_SET_MEDIA_STATUS        TAP_CONTROL_CODE(6, METHOD_BUFFERED)
#define TAP_IOCTL_CONFIG_DHCP_MASQ        TAP_CONTROL_CODE(7, METHOD_BUFFERED)
#define TAP_IOCTL_GET_TAPLOG_LINE         TAP_CONTROL_CODE(8, METHOD_BUFFERED)
#define TAP_IOCTL_CONFIG_DHCP_SET_OPT     TAP_CONTROL_CODE(9, METHOD_BUFFERED)
#define TAP_IOCTL_CONFIG_TUN              TAP_CONTROL_CODE(10, METHOD_BUFFERED) 


#define TAP_REGISTRY_KEY                  "SYSTEM\\CurrentControlSet\\Control\\Network\\{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define TAP_ADAPTER_KEY	                  "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}"
#define TAP_DEVICE_DIR	                  "\\\\.\\Global\\"

#define TAP_WINDOWS_MIN_MAJOR               8
#define TAP_WINDOWS_MIN_MINOR               1

#define MAX_IFNAME 128

/* The fixup functions will use the defines, so this has to be here */
#include "tapcfg_windows_fixup.h"

struct tapcfg_s {
	int started;
	int enabled;

	HANDLE dev_handle;
	char ifname[MAX_IFNAME];

	int reading;
	OVERLAPPED overlapped_in;
	OVERLAPPED overlapped_out;

	char buffer[TAPCFG_BUFSIZE];
	DWORD buflen;
};

tapcfg_t *
tapcfg_init()
{
	tapcfg_t *tapcfg;

	tapcfg = calloc(1, sizeof(tapcfg_t));
	if (!tapcfg) {
		return NULL;
	}

	tapcfg->dev_handle = INVALID_HANDLE_VALUE;
	tapcfg->overlapped_in.hEvent =
		CreateEvent(NULL, FALSE, FALSE, NULL);
	tapcfg->overlapped_out.hEvent =
		CreateEvent(NULL, FALSE, FALSE, NULL);

	return tapcfg;
}

void
tapcfg_destroy(tapcfg_t *tapcfg)
{
	if (tapcfg) {
		tapcfg_stop(tapcfg);
		CloseHandle(tapcfg->overlapped_in.hEvent);
		CloseHandle(tapcfg->overlapped_out.hEvent);
	}
	free(tapcfg);
}

static int
tapcfg_iface_reset(tapcfg_t *tapcfg)
{
	char buffer[1024];
	int ret = 0;

	assert(tapcfg);

	if (!tapcfg->started) {
		return 0;
	}

	/* Make sure the string always ends in null byte */
	buffer[sizeof(buffer)-1] = '\0';

#ifndef DISABLE_IPV6
	taplog_log(TAPLOG_INFO,
	           "Resetting the IPv6 subsystem: \"%s\"\n",
	           tapcfg->ifname);

	snprintf(buffer, sizeof(buffer)-1,
	         "netsh interface ipv6 reset\n");

	taplog_log(TAPLOG_INFO, "Running netsh command: %s\n", buffer);
	if (system(buffer)) {
		taplog_log(TAPLOG_ERR,
		           "Error trying to reset IPv6 address\n");
		ret = -1;
	}
#endif

	taplog_log(TAPLOG_INFO,
	           "Flushing the system ARP table\n");

	snprintf(buffer, sizeof(buffer)-1, "arp -d *\n");

	taplog_log(TAPLOG_INFO, "Running command: %s\n", buffer);
	if (system(buffer)) {
		taplog_log(TAPLOG_ERR,
		           "Error trying to reset ARP table\n");
		ret = -1;
	}

	return ret;
}

int
tapcfg_start(tapcfg_t *tapcfg, const char *ifname)
{
	HKEY key, dev_handle;
	LONG status;
	int i;

	assert(tapcfg);

	strncpy(tapcfg->ifname, ifname, MAX_IFNAME-1);
	if (tapcfg_fixup_adapters(tapcfg->ifname, MAX_IFNAME) < 0) {
		taplog_log(TAPLOG_ERR, "TAP adapter not configured properly...\n");
		return -1;
	}

	taplog_log(TAPLOG_DEBUG, "TAP adapter configured properly\n");
	taplog_log(TAPLOG_DEBUG, "Interface name is '%s'\n", tapcfg->ifname);

	/* Open registry and look for network adapters */
	status = RegOpenKeyEx(HKEY_LOCAL_MACHINE, TAP_REGISTRY_KEY, 0, KEY_READ, &key);
	if (status != ERROR_SUCCESS) {
		taplog_log(TAPLOG_ERR, "Could not open the networking registry key\n");
		return -1;
	}

	dev_handle = INVALID_HANDLE_VALUE;
	for (i=0; dev_handle == INVALID_HANDLE_VALUE; i++) {
		char adapterid[1024];
		char tapname[1024];
		DWORD len;

		len = sizeof(adapterid);
		status = RegEnumKeyEx(key, i, adapterid, &len, 0, 0, 0, NULL);
		if (status != ERROR_SUCCESS)
			break;

		snprintf(tapname, sizeof(tapname), TAP_DEVICE_DIR "%s.tap", adapterid);

		taplog_log(TAPLOG_DEBUG, "Trying %s\n", tapname);
		dev_handle = CreateFile(tapname,
		                        GENERIC_WRITE | GENERIC_READ,
		                        0, /* ShareMode, don't let others open the device */
		                        0, /* SecurityAttributes */
		                        OPEN_EXISTING,
		                        FILE_ATTRIBUTE_SYSTEM | FILE_FLAG_OVERLAPPED,
		                        0); /* TemplateFile */

		if (dev_handle != INVALID_HANDLE_VALUE) {
			unsigned long info[3];

			if (!DeviceIoControl(dev_handle,
			                     TAP_IOCTL_GET_VERSION,
			                     &info, /* InBuffer */
			                     sizeof(info),
			                     &info, /* OutBuffer */
			                     sizeof(info),
			                     &len, NULL)) {
				taplog_log(TAPLOG_ERR,
				           "Error calling DeviceIoControl: %d",
				           (int) GetLastError());
				CloseHandle(dev_handle);
				dev_handle = INVALID_HANDLE_VALUE;
				continue;
			}

			taplog_log(TAPLOG_DEBUG,
			           "TAP Driver Version %d.%d %s\n",
			           (int) info[0],
			           (int) info[1],
			           info[2] ? "(DEBUG)" : "");

			if (info[0] > TAP_WINDOWS_MIN_MAJOR ||
			    (info[0] == TAP_WINDOWS_MIN_MAJOR && info[1] >= TAP_WINDOWS_MIN_MINOR)) {
				/* Found the device handle that we want to use */
				break;
			}

			taplog_log(TAPLOG_ERR,
			           "A TAP driver is required that is at least version %d.%d\n",
			           TAP_WINDOWS_MIN_MAJOR, TAP_WINDOWS_MIN_MINOR);
			taplog_log(TAPLOG_INFO,
			           "If you recently upgraded your TAP driver, a reboot is probably "
			           "required at this point to get Windows to see the new driver.\n");

			CloseHandle(dev_handle);
			dev_handle = INVALID_HANDLE_VALUE;
		}
	}
	RegCloseKey(key);

	if (dev_handle == INVALID_HANDLE_VALUE) {
		taplog_log(TAPLOG_ERR, "No working Tap device found!\n");
		return -1;
	}

	tapcfg->dev_handle = dev_handle;
	tapcfg->started = 1;
	tapcfg->enabled = 0;

	/* Reset all the IPv6 addresses when initializing the device */
	tapcfg_iface_reset(tapcfg);

	return 0;
}

void
tapcfg_stop(tapcfg_t *tapcfg)
{
	assert(tapcfg);

	if (tapcfg->started) {
		if (tapcfg->dev_handle != INVALID_HANDLE_VALUE) {
			CloseHandle(tapcfg->dev_handle);
			tapcfg->dev_handle = INVALID_HANDLE_VALUE;
		}
		tapcfg->started = 0;
		tapcfg->enabled = 0;
	}
}

static int
tapcfg_wait_for_data(tapcfg_t *tapcfg, DWORD timeout)
{
	DWORD retval, len;
	int ret = 0;

	assert(tapcfg);

	if (tapcfg->buflen) {
		taplog_log(TAPLOG_DEBUG, "Found %d bytes from buffer\n", tapcfg->buflen);
		return 1;
	} else if (!tapcfg->reading) {
		/* No data available, start a new read */
		tapcfg->reading = 1;

		tapcfg->overlapped_in.Offset = 0;
		tapcfg->overlapped_in.OffsetHigh = 0;

		taplog_log(TAPLOG_DEBUG, "Calling ReadFile function\n");
		retval = ReadFile(tapcfg->dev_handle,
		                  tapcfg->buffer,
		                  sizeof(tapcfg->buffer),
		                  &len,
		                  &tapcfg->overlapped_in);

		/* If read successful, mark reading finished */
		if (retval) {
			tapcfg->reading = 0;
			tapcfg->buflen = len;
			ret = 1;
			taplog_log(TAPLOG_DEBUG, "Finished reading %d bytes with ReadFile\n", len);
		} else if (GetLastError() != ERROR_IO_PENDING) {
			tapcfg->reading = 0;
			taplog_log(TAPLOG_ERR,
			           "Error calling ReadFile: %d\n",
			           GetLastError());
		}
	} else {
		retval = WaitForSingleObject(tapcfg->overlapped_in.hEvent, 0);

		if (retval == WAIT_OBJECT_0) {
			taplog_log(TAPLOG_DEBUG, "Calling GetOverlappedResult function\n");
			retval = GetOverlappedResult(tapcfg->dev_handle,
			                             &tapcfg->overlapped_in,
			                             &len, FALSE);
			if (!retval) {
				tapcfg->reading = 0;
				taplog_log(TAPLOG_ERR,
				           "Error calling GetOverlappedResult: %d\n",
				           GetLastError());
			} else {
				tapcfg->reading = 0;
				tapcfg->buflen = len;
				ret = 1;
				taplog_log(TAPLOG_DEBUG,
				           "Finished reading %d bytes with GetOverlappedResult\n",
				           len);
			}
		}
	}

	return ret;
}

int
tapcfg_can_read(tapcfg_t *tapcfg)
{
	assert(tapcfg);

	if (!tapcfg->started) {
		return 0;
	}

	return tapcfg_wait_for_data(tapcfg, 0);
}

int
tapcfg_read(tapcfg_t *tapcfg, void *buf, int count)
{
	int ret;

	assert(tapcfg);

	if (!tapcfg->started) {
		return -1;
	}

	if (!tapcfg_wait_for_data(tapcfg, INFINITE)) {
		return -1;
	}

	if (count < tapcfg->buflen) {
		taplog_log(TAPLOG_ERR,
		           "Buffer not big enough for reading, "
		           "need at least %d bytes\n",
		           tapcfg->buflen);
		return -1;
	}

	ret = tapcfg->buflen;
	memcpy(buf, tapcfg->buffer, tapcfg->buflen);
	tapcfg->buflen = 0;

	taplog_log(TAPLOG_DEBUG, "Read ethernet frame:\n");
	taplog_log_ethernet_info(buf, ret);

	return ret;
}

int
tapcfg_can_write(tapcfg_t *tapcfg)
{
	assert(tapcfg);

	if (!tapcfg->started) {
		return 0;
	}

	return 1;
}

int
tapcfg_write(tapcfg_t *tapcfg, void *buf, int count)
{
	DWORD retval, len;

	assert(tapcfg);

	if (!tapcfg->started) {
		return -1;
	}

	tapcfg->overlapped_out.Offset = 0;
	tapcfg->overlapped_out.OffsetHigh = 0;

	retval = WriteFile(tapcfg->dev_handle, buf, count,
	                   &len, &tapcfg->overlapped_out);
	if (!retval && GetLastError() == ERROR_IO_PENDING) {
		retval = WaitForSingleObject(tapcfg->overlapped_out.hEvent,
		                             INFINITE);
		if (!retval) {
			taplog_log(TAPLOG_ERR,
			           "Error calling WaitForSingleObject\n");
			return -1;
		}
		retval = GetOverlappedResult(tapcfg->dev_handle,
		                             &tapcfg->overlapped_out,
		                             &len, FALSE);
	}

	if (!retval) {
		taplog_log(TAPLOG_ERR,
		           "Error trying to write data to TAP device: %d\n",
		           GetLastError());
		return -1;
	}

	taplog_log(TAPLOG_DEBUG, "Wrote ethernet frame:\n");
	taplog_log_ethernet_info(buf, len);

	return len;
}

const char *
tapcfg_get_ifname(tapcfg_t *tapcfg)
{
	assert(tapcfg);

	if (tapcfg->started) {
		return NULL;
	}

	return tapcfg->ifname;
}

int
tapcfg_iface_get_status(tapcfg_t *tapcfg)
{
	assert(tapcfg);

	return tapcfg->enabled;
}

int
tapcfg_iface_change_status(tapcfg_t *tapcfg, int enabled)
{
	unsigned long status = enabled;
	DWORD len;

	assert(tapcfg);

	if (!tapcfg->started) {
		return -1;
	} else if (enabled == tapcfg->enabled) {
		/* Already enabled, nothing required */
		return 0;
	}

	taplog_log(TAPLOG_DEBUG, "Calling DeviceIoControl\n");
	if (!DeviceIoControl(tapcfg->dev_handle,
	                     TAP_IOCTL_SET_MEDIA_STATUS,
	                     &status, /* InBuffer */
	                     sizeof(status),
	                     &status, /* OutBuffer */
	                     sizeof(status),
	                     &len, NULL)) {
		return -1;
	}
	tapcfg->enabled = enabled;

	return 0;
}

int
tapcfg_iface_set_ipv4(tapcfg_t *tapcfg, char *addrstr, unsigned char netbits)
{
	char buffer[1024];
	unsigned int mask;

	assert(tapcfg);

	if (!tapcfg->started) {
		return 0;
	}

	if (netbits == 0 || netbits > 32) {
		return -1;
	}

	/* Make sure the string always ends in null byte */
	buffer[sizeof(buffer)-1] = '\0';

	/* Calculate the netmask from the network bit length */
	for (mask=0; netbits; netbits--)
		mask = (mask >> 1)|(1 << 31);

	/* Check that the given IPv4 address is valid */
	if (!tapcfg_address_is_valid(AF_INET, addrstr)) {
		return -1;
	}

	snprintf(buffer, sizeof(buffer)-1,
	         "netsh interface ip set address \"%s\" static %s %u.%u.%u.%u\n",
	         tapcfg->ifname, addrstr,
	         (mask >> 24) & 0xff,
	         (mask >> 16) & 0xff,
	         (mask >> 8)  & 0xff,
	         mask & 0xff);

	taplog_log(TAPLOG_INFO, "Running netsh command: %s\n", buffer);
	if (system(buffer)) {
		taplog_log(TAPLOG_ERR,
		           "Error trying to configure IPv4 address\n");
		return -1;
	}

	return 0;
}

#ifndef DISABLE_IPV6
int
tapcfg_iface_add_ipv6(tapcfg_t *tapcfg, char *addrstr, unsigned char netbits)
{
	char buffer[1024];
	struct sockaddr_in6 sockaddr;

	assert(tapcfg);

	if (!tapcfg->started) {
		return 0;
	}

	if (netbits == 0 || netbits > 128) {
		return -1;
	}

	/* Make sure the string always ends in null byte */
	buffer[sizeof(buffer)-1] = '\0';

	/* Check that the given IPv6 address is valid */
	if (!tapcfg_address_is_valid(AF_INET6, addrstr)) {
		return -1;
	}

	snprintf(buffer, sizeof(buffer)-1,
	         "netsh interface ipv6 add address \"%s\" %s unicast\n",
	         tapcfg->ifname, addrstr);

	taplog_log(TAPLOG_INFO, "Running netsh command: %s\n", buffer);
	if (system(buffer)) {
		taplog_log(TAPLOG_ERR,
		           "Error trying to configure IPv6 address\n");
		return -1;
	}

	snprintf(buffer, sizeof(buffer)-1,
	         "netsh interface ipv6 add route %s/%d \"%s\"\n",
	         addrstr, netbits, tapcfg->ifname);

	taplog_log(TAPLOG_INFO, "Running netsh command: %s\n", buffer);
	if (system(buffer)) {
		taplog_log(TAPLOG_ERR,
		           "Error trying to configure IPv6 address\n");
		return -1;
	}

	return 0;
}
#else /* DISABLE_IPV6 */
int
tapcfg_iface_add_ipv6(tapcfg_t *tapcfg, char *addr, unsigned char netbits)
{
	/* Always return an error */
	return -1;
}
#endif /* DISABLE_IPV6 */
