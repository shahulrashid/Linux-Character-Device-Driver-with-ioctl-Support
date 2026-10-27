/* SPDX-License-Identifier: GPL-2.0 */
/*
 * mychardev.h - interface shared by the kernel module and user space.
 *
 * Keeping the ioctl numbers in one header guarantees that the driver and
 * the test application always agree on the command encoding.
 */
#ifndef MYCHARDEV_H
#define MYCHARDEV_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define MYCHARDEV_NAME        "mychardev"
#define MYCHARDEV_DEFAULT_BUF 1024          /* bytes, overridable at insmod */
#define MYCHARDEV_MAX_BUF     65536

/* 'k' is an arbitrary "magic" letter; see Documentation/userspace-api/ioctl */
#define MYCHARDEV_IOC_MAGIC   'k'

/* Erase the buffer and reset its length to 0. No argument. */
#define MYCHARDEV_CLEAR_BUFFER  _IO(MYCHARDEV_IOC_MAGIC, 0)
/* Return the number of valid bytes currently stored. */
#define MYCHARDEV_GET_LENGTH    _IOR(MYCHARDEV_IOC_MAGIC, 1, unsigned int)
/* Return the total capacity of the buffer in bytes. */
#define MYCHARDEV_GET_CAPACITY  _IOR(MYCHARDEV_IOC_MAGIC, 2, unsigned int)

#define MYCHARDEV_IOC_MAXNR   2

#endif /* MYCHARDEV_H */
