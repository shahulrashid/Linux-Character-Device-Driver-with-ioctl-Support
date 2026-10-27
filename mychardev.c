// SPDX-License-Identifier: GPL-2.0
/*
 * mychardev.c - a virtual character device backed by a kernel memory buffer.
 *
 * User space talks to it through /dev/mychardev using open/read/write/
 * lseek/ioctl/close. A mutex serialises every access to the shared buffer.
 *
 * Semantics
 *   write(): copies data into the buffer at the file offset (or at the end of
 *            the data if O_APPEND). Writes that do not fit are truncated
 *            (partial write); when the buffer is full it returns -ENOSPC.
 *   read():  copies data from the file offset. Returns 0 (EOF) at the end of
 *            the stored data.
 *   lseek(): moves the offset within [0, capacity].
 *   ioctl(): CLEAR_BUFFER, GET_LENGTH, GET_CAPACITY. Anything else: -ENOTTY.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "mychardev.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("Virtual character device with ioctl support");
MODULE_VERSION("1.0");

static unsigned int buffer_size = MYCHARDEV_DEFAULT_BUF;
module_param(buffer_size, uint, 0444);
MODULE_PARM_DESC(buffer_size, "Buffer capacity in bytes (1..65536, default 1024)");

struct mychardev {
	struct cdev    cdev;
	dev_t          devt;
	struct class  *cls;
	struct device *dev;
	struct mutex   lock;   /* protects buf and len */
	char          *buf;
	size_t         cap;    /* fixed after init */
	size_t         len;    /* number of valid bytes; bytes past len are 0 */
};

static struct mychardev *mydev;

/* ------------------------------------------------------------------ */
/* file operations                                                    */
/* ------------------------------------------------------------------ */

static int mychardev_open(struct inode *inode, struct file *file)
{
	struct mychardev *d = container_of(inode->i_cdev, struct mychardev, cdev);

	file->private_data = d;
	pr_debug("opened (pid %d)\n", current->pid);
	return 0;
}

static int mychardev_release(struct inode *inode, struct file *file)
{
	pr_debug("released (pid %d)\n", current->pid);
	return 0;
}

static ssize_t mychardev_read(struct file *file, char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct mychardev *d = file->private_data;
	ssize_t ret;
	size_t n;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;

	if (*ppos < 0 || (size_t)*ppos >= d->len) {
		ret = 0;                         /* EOF / empty buffer */
		goto out;
	}

	n = min_t(size_t, count, d->len - (size_t)*ppos);   /* partial read */
	if (copy_to_user(ubuf, d->buf + *ppos, n)) {
		ret = -EFAULT;
		goto out;
	}
	*ppos += n;
	ret = n;
out:
	mutex_unlock(&d->lock);
	return ret;
}

static ssize_t mychardev_write(struct file *file, const char __user *ubuf,
			       size_t count, loff_t *ppos)
{
	struct mychardev *d = file->private_data;
	ssize_t ret;
	size_t pos, n;

	if (!count)
		return 0;
	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;

	pos = (file->f_flags & O_APPEND) ? d->len : (size_t)*ppos;
	if (pos >= d->cap) {
		ret = -ENOSPC;                   /* buffer full */
		goto out;
	}

	n = min_t(size_t, count, d->cap - pos);             /* partial write */
	if (copy_from_user(d->buf + pos, ubuf, n)) {
		/* keep the "bytes past len are zero" invariant */
		if (pos + n > d->len)
			memset(d->buf + max(pos, d->len), 0,
			       pos + n - max(pos, d->len));
		ret = -EFAULT;
		goto out;
	}

	if (pos + n > d->len)
		d->len = pos + n;
	*ppos = pos + n;
	ret = n;
out:
	mutex_unlock(&d->lock);
	return ret;
}

static loff_t mychardev_llseek(struct file *file, loff_t off, int whence)
{
	struct mychardev *d = file->private_data;
	loff_t newpos;
	loff_t ret;

	/* also rules out arithmetic overflow below */
	if (off < -(loff_t)d->cap || off > (loff_t)d->cap)
		return -EINVAL;
	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;

	switch (whence) {
	case SEEK_SET: newpos = off;                         break;
	case SEEK_CUR: newpos = file->f_pos + off;           break;
	case SEEK_END: newpos = (loff_t)d->len + off;        break;
	default:       ret = -EINVAL; goto out;
	}

	if (newpos < 0 || newpos > (loff_t)d->cap) {
		ret = -EINVAL;
		goto out;
	}
	file->f_pos = newpos;
	ret = newpos;
out:
	mutex_unlock(&d->lock);
	return ret;
}

static long mychardev_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct mychardev *d = file->private_data;
	unsigned int val;

	/* reject commands that are not ours before touching anything */
	if (_IOC_TYPE(cmd) != MYCHARDEV_IOC_MAGIC ||
	    _IOC_NR(cmd) > MYCHARDEV_IOC_MAXNR)
		return -ENOTTY;

	switch (cmd) {
	case MYCHARDEV_CLEAR_BUFFER:
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		memset(d->buf, 0, d->cap);
		d->len = 0;
		mutex_unlock(&d->lock);
		pr_info("buffer cleared\n");
		return 0;

	case MYCHARDEV_GET_LENGTH:
		if (mutex_lock_interruptible(&d->lock))
			return -ERESTARTSYS;
		val = d->len;
		mutex_unlock(&d->lock);
		return put_user(val, (unsigned int __user *)arg);

	case MYCHARDEV_GET_CAPACITY:
		val = d->cap;                    /* immutable: no lock needed */
		return put_user(val, (unsigned int __user *)arg);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations mychardev_fops = {
	.owner          = THIS_MODULE,
	.open           = mychardev_open,
	.release        = mychardev_release,
	.read           = mychardev_read,
	.write          = mychardev_write,
	.llseek         = mychardev_llseek,
	.unlocked_ioctl = mychardev_ioctl,
};

/* ------------------------------------------------------------------ */
/* module init / exit                                                 */
/* ------------------------------------------------------------------ */

/* Makes udev create /dev/mychardev with mode 0666 so the test app does not
 * need root. Fine for a learning project; tighten for anything real. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
static char *mychardev_devnode(const struct device *dev, umode_t *mode)
#else
static char *mychardev_devnode(struct device *dev, umode_t *mode)
#endif
{
	if (mode)
		*mode = 0666;
	return NULL;
}

static int __init mychardev_init(void)
{
	struct mychardev *d;
	int ret;

	if (buffer_size < 1 || buffer_size > MYCHARDEV_MAX_BUF) {
		pr_err("buffer_size must be 1..%d\n", MYCHARDEV_MAX_BUF);
		return -EINVAL;
	}

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->buf = kzalloc(buffer_size, GFP_KERNEL);
	if (!d->buf) {
		ret = -ENOMEM;
		goto err_free_dev;
	}
	d->cap = buffer_size;
	mutex_init(&d->lock);

	ret = alloc_chrdev_region(&d->devt, 0, 1, MYCHARDEV_NAME);
	if (ret) {
		pr_err("alloc_chrdev_region failed: %d\n", ret);
		goto err_free_buf;
	}

	cdev_init(&d->cdev, &mychardev_fops);
	d->cdev.owner = THIS_MODULE;
	ret = cdev_add(&d->cdev, d->devt, 1);
	if (ret) {
		pr_err("cdev_add failed: %d\n", ret);
		goto err_unreg;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	d->cls = class_create(MYCHARDEV_NAME);
#else
	d->cls = class_create(THIS_MODULE, MYCHARDEV_NAME);
#endif
	if (IS_ERR(d->cls)) {
		ret = PTR_ERR(d->cls);
		pr_err("class_create failed: %d\n", ret);
		goto err_cdev;
	}
	d->cls->devnode = mychardev_devnode;

	d->dev = device_create(d->cls, NULL, d->devt, NULL, MYCHARDEV_NAME);
	if (IS_ERR(d->dev)) {
		ret = PTR_ERR(d->dev);
		pr_err("device_create failed: %d\n", ret);
		goto err_class;
	}

	mydev = d;
	pr_info("loaded: /dev/%s major=%d minor=%d capacity=%zu bytes\n",
		MYCHARDEV_NAME, MAJOR(d->devt), MINOR(d->devt), d->cap);
	return 0;

err_class:
	class_destroy(d->cls);
err_cdev:
	cdev_del(&d->cdev);
err_unreg:
	unregister_chrdev_region(d->devt, 1);
err_free_buf:
	kfree(d->buf);
err_free_dev:
	kfree(d);
	return ret;
}

static void __exit mychardev_exit(void)
{
	struct mychardev *d = mydev;

	device_destroy(d->cls, d->devt);
	class_destroy(d->cls);
	cdev_del(&d->cdev);
	unregister_chrdev_region(d->devt, 1);
	kfree(d->buf);
	kfree(d);
	pr_info("unloaded\n");
}

module_init(mychardev_init);
module_exit(mychardev_exit);
