/*
 * Oculus VR driver for Linux
 *
 * Copyright (c) 2013 Lee Cooper <lee.cooper@oculusvr.com>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

/*
 * Driver for Oculus VR devices. Based on hidraw driver.
 */

#include <linux/cdev.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/hidraw.h>
#include "hid-ids.h"

#if defined(CONFIG_SOC_EXYNOS7420)
#define WLAN_RPS_CONTROL
#define WLAN_DEV_NAME "wlan0"
#define WLAN_DEV_NAME_LEN (5)
#define SET_CPUS "f0"
#define SET_CPUS_LEN (2)
#elif defined(CONFIG_ARCH_APQ8084)
#define WLAN_RPS_CONTROL
#define WLAN_DEV_NAME "wlan0"
#define WLAN_DEV_NAME_LEN (5)
#define SET_CPUS "c"
#define SET_CPUS_LEN (1)

#define WLAN_IRQ_CONTROL
#define WLAN_IRQ_NUM (276)
#define SET_IRQ_AFFINITY "2"
#define SET_IRQ_AFFINITY_LEN (1)
#define DEFAULT_IRQ_AFFINITY "f"
#define DEFAULT_IRQ_AFFINITY_LEN (1)
#endif

#define USB_TRACKER_INTERFACE_PROTOCOL	0

/* number of reports to buffer */
#define OVR_HIDRAW_BUFFER_SIZE 64
#define OVR_HIDRAW_MAX_DEVICES 64
#define OVR_FIRST_MINOR 0
#define OVR_HIDRAW_MAX_SERIAL 256

static char ovr_serial[OVR_HIDRAW_MAX_SERIAL] = {0,};
static char ovr_serial_len = 0;

static struct class *ovr_class;
static struct hidraw *ovr_hidraw_table[OVR_HIDRAW_MAX_DEVICES];
static DEFINE_MUTEX(minors_lock);
static DEFINE_SPINLOCK(list_lock);

static int ovr_major;
static struct cdev ovr_cdev;

#define MONITOR_MAX 32
static int opens = 0;
static unsigned long monitor_info[MONITOR_MAX][4] = {{0,},};
static unsigned int isr_count = 0;
static unsigned long last_isr = 0;

static unsigned int ovr_minor = 0;
static void ovr_monitor_work(struct work_struct *work);
static struct workqueue_struct *ovr_wq;
static DECLARE_DELAYED_WORK(ovr_work, ovr_monitor_work);

static ssize_t ovr_hidraw_read(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
	struct hidraw_list *list = file->private_data;
	int ret = 0, len;
	DECLARE_WAITQUEUE(wait, current);

	mutex_lock(&list->read_mutex);

	while (ret == 0) {
		if (list->head == list->tail) {
			add_wait_queue(&list->hidraw->wait, &wait);
			set_current_state(TASK_INTERRUPTIBLE);

			while (list->head == list->tail) {
				if (signal_pending(current)) {
					ret = -ERESTARTSYS;
					break;
				}
				if (!list->hidraw->exist) {
					ret = -EIO;
					break;
				}
				if (file->f_flags & O_NONBLOCK) {
					ret = -EAGAIN;
					break;
				}

				/* allow O_NONBLOCK to work well from other threads */
				mutex_unlock(&list->read_mutex);
				schedule();
				mutex_lock(&list->read_mutex);
				set_current_state(TASK_INTERRUPTIBLE);
			}

			set_current_state(TASK_RUNNING);
			remove_wait_queue(&list->hidraw->wait, &wait);
		}

		if (ret)
			goto out;

		len = list->buffer[list->tail].len > count ?
			count : list->buffer[list->tail].len;

		if (list->buffer[list->tail].value) {
			if (copy_to_user(buffer, list->buffer[list->tail].value, len)) {
				ret = -EFAULT;
				goto out;
			}
			ret = len;

			if (opens > 0)
			{
				int i;
				for (i=0; i<MONITOR_MAX; i++) {
					if (monitor_info[i][0] == (unsigned long)file) {
						monitor_info[i][1]++;
						monitor_info[i][2] = jiffies;
						break;
					}
				}
			}
		}

		kfree(list->buffer[list->tail].value);
		list->buffer[list->tail].value = NULL;
		list->tail = (list->tail + 1) & (OVR_HIDRAW_BUFFER_SIZE - 1);
	}
out:
	mutex_unlock(&list->read_mutex);

	return ret;
}

/* The first byte is expected to be a report number.
 * This function is to be called with the minors_lock mutex held */
static ssize_t ovr_hidraw_send_report(struct file *file, const char __user *buffer, size_t count, unsigned char report_type)
{
	unsigned int minor = iminor(file->f_path.dentry->d_inode);
	struct hid_device *dev;
	__u8 *buf;
	int ret = 0;

	if (!ovr_hidraw_table[minor]) {
		ret = -ENODEV;
		goto out;
	}

	dev = ovr_hidraw_table[minor]->hid;

	if (!dev->hid_output_raw_report) {
		ret = -ENODEV;
		goto out;
	}

	if (count > HID_MAX_BUFFER_SIZE) {
		hid_warn(dev, "ovr - pid %d passed too large report\n",
			 task_pid_nr(current));
		ret = -EINVAL;
		goto out;
	}

	if (count < 2) {
		hid_warn(dev, "ovr - pid %d passed too short report\n",
			 task_pid_nr(current));
		ret = -EINVAL;
		goto out;
	}

	buf = kmalloc(count * sizeof(__u8), GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(buf, buffer, count)) {
		ret = -EFAULT;
		goto out_free;
	}

	ret = dev->hid_output_raw_report(dev, buf, count, report_type);
out_free:
	kfree(buf);
out:
	return ret;
}

/* the first byte is expected to be a report number */
static ssize_t ovr_hidraw_write(struct file *file, const char __user *buffer, size_t count, loff_t *ppos)
{
	ssize_t ret;
	mutex_lock(&minors_lock);
	ret = ovr_hidraw_send_report(file, buffer, count, HID_OUTPUT_REPORT);
	mutex_unlock(&minors_lock);
	return ret;
}

static void ovr_monitor_work(struct work_struct *work)
{
	int i;
	unsigned long now = jiffies;
	struct hid_device *dev;
	int ret = 0;
	__u8 *buf;
	size_t count = 24;
	unsigned char report_number = 0x31;
	unsigned char report_type = HID_FEATURE_REPORT;

	mutex_lock(&minors_lock);
	if (opens > 0 && ovr_minor >= 0 && ovr_hidraw_table[ovr_minor] && ovr_hidraw_table[ovr_minor]->exist) {
		dev = ovr_hidraw_table[ovr_minor]->hid;
		if (dev && dev->hid_get_raw_report) {
			buf = kmalloc(count * sizeof(__u8), GFP_KERNEL);
			if (buf) {
				ret = dev->hid_get_raw_report(dev, report_number, buf, count, report_type);
				if (ret < 0) {
					printk("OVR: hid_get_raw_report error %d\n", ret);
				} else {
					printk("OVR: timestamp(0x%2.2X%2.2X%2.2X%2.2X) sensor(0x%2.2X%2.2X%2.2X%2.2X) pui(0x%2.2X%2.2X%2.2X%2.2X) proxy(%d) mainloop(0x%2.2X) (%2.2X %2.2X %2.2X %2.2X %2.2X %2.2X)\n",
						buf[7], buf[6], buf[5], buf[4], buf[11], buf[10], buf[9], buf[8], buf[15], buf[14], buf[13], buf[12], buf[16], buf[17], buf[18], buf[19], buf[20], buf[21], buf[22], buf[23]);
				}
				kfree(buf);
			} else {
				printk("OVR: no mem for monitor report\n");
			}
		}

		printk("OVR: isr(%d), diff(isr):%ums\n", isr_count, jiffies_to_msecs(now-last_isr));
		isr_count = 0;

		for (i=0; i<MONITOR_MAX; i++) {
			if (monitor_info[i][0]) {
				printk("OVR: 0x%x %lu(%lu), diff(read):%u secs\n", (unsigned int)monitor_info[i][0], monitor_info[i][3], monitor_info[i][1], jiffies_to_msecs(now-monitor_info[i][2])/1000);
				monitor_info[i][1] = 0;
			}
		}

		queue_delayed_work(ovr_wq, &ovr_work, msecs_to_jiffies(2000));
	}
	mutex_unlock(&minors_lock);
}

/* This function performs a Get_Report transfer over the control endpoint
 * per section 7.2.1 of the HID specification, version 1.1.  The first byte
 * of buffer is the report number to request, or 0x0 if the defice does not
 * use numbered reports. The report_type parameter can be HID_FEATURE_REPORT
 * or HID_INPUT_REPORT.  This function is to be called with the minors_lock
 *  mutex held. */
static ssize_t ovr_hidraw_get_report(struct file *file, char __user *buffer, size_t count, unsigned char report_type)
{
	unsigned int minor = iminor(file->f_path.dentry->d_inode);
	struct hid_device *dev;
	__u8 *buf;
	int ret = 0, len;
	unsigned char report_number;

	dev = ovr_hidraw_table[minor]->hid;

	if (!dev->hid_get_raw_report) {
		ret = -ENODEV;
		goto out;
	}

	if (count > HID_MAX_BUFFER_SIZE) {
		printk(KERN_WARNING "ovr - hidraw: pid %d passed too large report\n",
				task_pid_nr(current));
		ret = -EINVAL;
		goto out;
	}

	if (count < 2) {
		printk(KERN_WARNING "ovr - hidraw: pid %d passed too short report\n",
				task_pid_nr(current));
		ret = -EINVAL;
		goto out;
	}

	buf = kmalloc(count * sizeof(__u8), GFP_KERNEL);
	if (!buf) {
		ret = -ENOMEM;
		goto out;
	}

	/* Read the first byte from the user. This is the report number,
	 * which is passed to dev->hid_get_raw_report(). */
	if (copy_from_user(&report_number, buffer, 1)) {
		ret = -EFAULT;
		goto out_free;
	}

	ret = dev->hid_get_raw_report(dev, report_number, buf, count, report_type);

	if (ret < 0)
		goto out_free;

	len = (ret < count) ? ret : count;

	if (copy_to_user(buffer, buf, len)) {
		ret = -EFAULT;
		goto out_free;
	}

	ret = len;

out_free:
	kfree(buf);
out:
	return ret;
}

static unsigned int ovr_hidraw_poll(struct file *file, poll_table *wait)
{
	struct hidraw_list *list = file->private_data;

	poll_wait(file, &list->hidraw->wait, wait);
	if (list->head != list->tail)
		return POLLIN | POLLRDNORM;
	if (!list->hidraw->exist)
		return POLLERR | POLLHUP;
	return 0;
}

static int ovr_hidraw_open(struct inode *inode, struct file *file)
{
	unsigned int minor = iminor(inode);
	struct hidraw *dev;
	struct hidraw_list *list;
	int err = 0;

	if (!(list = kzalloc(sizeof(struct hidraw_list), GFP_KERNEL))) {
		err = -ENOMEM;
		goto out;
	}

	mutex_lock(&minors_lock);
	if (!ovr_hidraw_table[minor]) {
		err = -ENODEV;
		goto out_unlock;
	}

	printk("OVR: open %d (%d:%s) >>>\n", minor, current->pid, current->comm);

	list->hidraw = ovr_hidraw_table[minor];
	mutex_init(&list->read_mutex);

	spin_lock_irq(&list_lock);
	list_add_tail(&list->node, &ovr_hidraw_table[minor]->list);
	spin_unlock_irq(&list_lock);

	file->private_data = list;

	dev = ovr_hidraw_table[minor];
	dev->open++;

	if (minor == ovr_minor) {
		int i;

		for (i=0; i<MONITOR_MAX; i++) {
			if (monitor_info[i][0] == 0) {
				monitor_info[i][0] = (unsigned long)file;
				monitor_info[i][1] = 0;
				monitor_info[i][2] = jiffies;
				monitor_info[i][3] = current->pid;
				break;
			}
		}

		opens = dev->open;
		if (opens == 1) {
			queue_delayed_work(ovr_wq, &ovr_work, msecs_to_jiffies(2000));
		}
	}

	printk("OVR: open(%d) err %d <<<\n", opens, err);

out_unlock:
	mutex_unlock(&minors_lock);
out:
	if (err < 0)
		kfree(list);
	return err;
}

static int ovr_hidraw_fasync(int fd, struct file *file, int on)
{
	struct hidraw_list *list = file->private_data;

	return fasync_helper(fd, file, on, &list->fasync);
}

static int ovr_hidraw_release(struct inode * inode, struct file * file)
{
	unsigned int minor = iminor(inode);
	struct hidraw *dev;
	struct hidraw_list *list = file->private_data;
	int ret;
	int i;
	unsigned long flags;

	mutex_lock(&minors_lock);
	if (!ovr_hidraw_table[minor]) {
		ret = -ENODEV;
		goto unlock;
	}

	printk("OVR: release %d (%d:%s) >>>\n", minor, current->pid, current->comm);

	spin_lock_irqsave(&list_lock, flags);
	list_del(&list->node);
	spin_unlock_irqrestore(&list_lock, flags);

	dev = ovr_hidraw_table[minor];
	--dev->open;

	if (minor == ovr_minor) {
		for (i=0; i<MONITOR_MAX; i++) {
			if (monitor_info[i][0] == (unsigned long)file) {
				monitor_info[i][0] = 0;
				break;
			}
		}

		opens = dev->open;
	}

	if (!dev->open) {
		if (!list->hidraw->exist) {
			printk("OVR: freed ovr_hidraw_table %d\n", minor);
			kfree(list->hidraw);
			ovr_hidraw_table[minor] = NULL;
		}
	}

	for (i = 0; i < OVR_HIDRAW_BUFFER_SIZE; ++i)
		kfree(list->buffer[i].value);
	kfree(list);
	ret = 0;

	printk("OVR: release(%d) <<<\n", opens);

unlock:
	mutex_unlock(&minors_lock);

	return ret;
}

static int ovr_report_event(struct hid_device *hid, u8 *data, int len)
{
	struct hidraw *dev = hid->hidovr;
	struct hidraw_list *list;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&list_lock, flags);
	list_for_each_entry(list, &dev->list, node) {
		int new_head = (list->head + 1) & (OVR_HIDRAW_BUFFER_SIZE - 1);

		if (new_head == list->tail)
			continue;

		if (!(list->buffer[list->head].value = kmemdup(data, len, GFP_ATOMIC))) {
			ret = -ENOMEM;
			spin_unlock_irqrestore(&list_lock, flags);
			break;
		}

		list->buffer[list->head].len = len;
		list->head = new_head;
		kill_fasync(&list->fasync, SIGIO, POLL_IN);
	}
	spin_unlock_irqrestore(&list_lock, flags);

	wake_up_interruptible(&dev->wait);

	return ret;
}

#ifdef WLAN_RPS_CONTROL
#include <linux/netdevice.h>

static int default_cpus_len = 0;
static char default_cpus[256] = {0,};

static ssize_t get_rps_cpus(char *name, int name_size, char *buf)
{
	size_t len = 0;

#ifdef CONFIG_RPS
	struct net_device *dev;
	struct netdev_rx_queue *queue = NULL;
	struct rps_map *map;
	cpumask_var_t mask;
	int i;

	if (name_size <= 0) {
		return len;
	}

	dev = first_net_device(&init_net);
	while (dev) {
		if (!memcmp(name, dev->name, name_size)) {
			queue = dev->_rx;
			if (!queue) {
				return -1;
			}

			if (!zalloc_cpumask_var(&mask, GFP_KERNEL))
				return -ENOMEM;

			rcu_read_lock();
			map = rcu_dereference(queue->rps_map);
			if (map)
				for (i = 0; i < map->len; i++)
					cpumask_set_cpu(map->cpus[i], mask);

			len += cpumask_scnprintf(buf + len, PAGE_SIZE, mask);
			if (PAGE_SIZE - len < 3) {
				rcu_read_unlock();
				free_cpumask_var(mask);
				return -EINVAL;
			}
			rcu_read_unlock();

			free_cpumask_var(mask);
			sprintf(buf + len, "\n");

			break;
		}

		dev = next_net_device(dev);
	}
#endif

	return len;
}

static int set_rps_cpus(char *name, int name_size, char *buf, size_t len)
{
	int ret = -1;

#ifdef CONFIG_RPS
	struct net_device *dev;
	struct netdev_rx_queue *queue = NULL;
	struct rps_map *old_map, *map;
	cpumask_var_t mask;
	int err, cpu, i;
	static DEFINE_SPINLOCK(rps_map_lock);

	if (name_size <= 0 || len < 0 || len > 2) {
		return ret;
	}

	dev = first_net_device(&init_net);
	while (dev) {
		if (!memcmp(name, dev->name, name_size)) {
			queue = dev->_rx;
			if (!queue) {
				return -1;
			}

			if (len == 0 || (len == 1 && buf[0] == '0') || (len == 2 && buf[0] == '0' && buf[1] == '0')) {
				map = rcu_dereference_protected(queue->rps_map, 1);
				if (map) {
					RCU_INIT_POINTER(queue->rps_map, NULL);
					kfree_rcu(map, rcu);
				}

				return 0;
			}

			if (!alloc_cpumask_var(&mask, GFP_KERNEL)) {
				return -ENOMEM;
			}

			err = bitmap_parse(buf, len, cpumask_bits(mask), nr_cpumask_bits);
			if (err) {
				free_cpumask_var(mask);
				return err;
			}

			map = kzalloc(max_t(unsigned int,
				RPS_MAP_SIZE(cpumask_weight(mask)), L1_CACHE_BYTES),
				GFP_KERNEL);
			if (!map) {
				free_cpumask_var(mask);
				return -ENOMEM;
			}

			i = 0;
			for_each_cpu(cpu, mask)
				map->cpus[i++] = cpu;

			if (i)
				map->len = i;
			else {
				kfree(map);
				map = NULL;
				free_cpumask_var(mask);
				return -1;
			}

			spin_lock(&rps_map_lock);
			old_map = rcu_dereference_protected(queue->rps_map,
				lockdep_is_held(&rps_map_lock));
			rcu_assign_pointer(queue->rps_map, map);
			spin_unlock(&rps_map_lock);

			if (map)
				static_key_slow_inc(&rps_needed);
			if (old_map) {
				kfree_rcu(old_map, rcu);
				static_key_slow_dec(&rps_needed);
			}
			free_cpumask_var(mask);
			ret = map->len;

			break;
		}

		dev = next_net_device(dev);
	}
#endif

	return ret;
}
#endif

#ifdef WLAN_IRQ_CONTROL
#include <linux/interrupt.h>
extern int irq_select_affinity_usr(unsigned int irq, struct cpumask *mask);

static int write_irq_affinity(unsigned int irq, const char __user *buffer, size_t count)
{
	int err = -1;

#ifdef CONFIG_SMP
	cpumask_var_t new_value;

	if (count < 1 || count > 2) {
		return err;
	}

	if (!irq_can_set_affinity(irq))
		return -EIO;

	if (!alloc_cpumask_var(&new_value, GFP_KERNEL))
		return -ENOMEM;

	err = cpumask_parse_user(buffer, count, new_value);
	if (err)
		goto free_cpumask;

	if (!cpumask_intersects(new_value, cpu_online_mask)) {
		err = irq_select_affinity_usr(irq, new_value) ? -EINVAL : count;
	} else {
		irq_set_affinity(irq, new_value);
		err = count;
	}

free_cpumask:
	free_cpumask_var(new_value);
#endif

	return err;
}
#endif

static int ovr_connect(struct hid_device *hid)
{
	int minor, result, i;
	struct hidraw *dev;

	/* we accept any HID device, no matter the applications */

	dev = kzalloc(sizeof(struct hidraw), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	result = -EINVAL;

	mutex_lock(&minors_lock);

	for (minor = 0; minor < OVR_HIDRAW_MAX_DEVICES; minor++)
	{
		if (ovr_hidraw_table[minor]) {
			printk("OVR: old ovr_hidraw_table %d\n", minor);
			continue;
		}

		ovr_hidraw_table[minor] = dev;
		result = 0;
		break;
	}

	printk("OVR: connect %d %d (%d:%s) >>>\n", minor, result, current->pid, current->comm);

	if (result) {
		mutex_unlock(&minors_lock);
		kfree(dev);
		goto out;
	}

	dev->dev = device_create(ovr_class, &hid->dev, MKDEV(ovr_major, minor),
				 NULL, "%s%d", "ovr", minor);

	if (IS_ERR(dev->dev)) {
		ovr_hidraw_table[minor] = NULL;
		mutex_unlock(&minors_lock);
		result = PTR_ERR(dev->dev);
		kfree(dev);
		goto out;
	}

	for (i=0; i<MONITOR_MAX; i++)
		monitor_info[i][0] = 0;

	opens = 0;
	ovr_minor = minor;

	printk("OVR: connect <<<\n");

	mutex_unlock(&minors_lock);
	init_waitqueue_head(&dev->wait);
	INIT_LIST_HEAD(&dev->list);

	dev->hid = hid;
	dev->minor = minor;

	dev->exist = 1;
	hid->hidovr = dev;

#ifdef WLAN_RPS_CONTROL
	default_cpus_len = get_rps_cpus(WLAN_DEV_NAME, WLAN_DEV_NAME_LEN, default_cpus);
	set_rps_cpus(WLAN_DEV_NAME, WLAN_DEV_NAME_LEN, SET_CPUS, SET_CPUS_LEN);
#endif
#ifdef WLAN_IRQ_CONTROL
	write_irq_affinity(WLAN_IRQ_NUM, SET_IRQ_AFFINITY, SET_IRQ_AFFINITY_LEN);
#endif

out:
	return result;
}

static void ovr_disconnect(struct hid_device *hid)
{
	struct hidraw *hidraw = hid->hidovr;

	mutex_lock(&minors_lock);

	printk("OVR: disconnect %d %d (%d:%s) >>>\n", hidraw->minor, hidraw->open, current->pid, current->comm);

	if (hidraw->minor == ovr_minor) {
		opens = 0;
		ovr_minor = -1;
	}

	hidraw->exist = 0;

	device_destroy(ovr_class, MKDEV(ovr_major, hidraw->minor));

	if (hidraw->open) {
		wake_up_interruptible(&hidraw->wait);
	} else {
		printk("OVR: freed ovr_hidraw_table %d\n", hidraw->minor);
		ovr_hidraw_table[hidraw->minor] = NULL;
		kfree(hidraw);
	}

	printk("OVR: disconnect <<<\n");

	mutex_unlock(&minors_lock);

#ifdef WLAN_RPS_CONTROL
	set_rps_cpus(WLAN_DEV_NAME, WLAN_DEV_NAME_LEN, default_cpus, default_cpus_len);
#endif
#ifdef WLAN_IRQ_CONTROL
	write_irq_affinity(WLAN_IRQ_NUM, DEFAULT_IRQ_AFFINITY, DEFAULT_IRQ_AFFINITY_LEN);
#endif
}

static long ovr_hidraw_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file->f_path.dentry->d_inode;
	unsigned int minor = iminor(inode);
	long ret = 0;
	struct hidraw *dev;
	void __user *user_arg = (void __user*) arg;

	mutex_lock(&minors_lock);
	dev = ovr_hidraw_table[minor];
	if (!dev || (dev && !dev->exist)) {
		ret = -ENODEV;
		goto out;
	}

	switch (cmd) {
		case HIDIOCGRDESCSIZE:
			if (put_user(dev->hid->rsize, (int __user *)arg))
				ret = -EFAULT;
			break;

		case HIDIOCGRDESC:
			{
				__u32 len;

				if (get_user(len, (int __user *)arg))
					ret = -EFAULT;
				else if (len > HID_MAX_DESCRIPTOR_SIZE - 1)
					ret = -EINVAL;
				else if (copy_to_user(user_arg + offsetof(
					struct hidraw_report_descriptor,
					value[0]),
					dev->hid->rdesc,
					min(dev->hid->rsize, len)))
					ret = -EFAULT;
				break;
			}
		case HIDIOCGRAWINFO:
			{
				struct hidraw_devinfo dinfo;

				dinfo.bustype = dev->hid->bus;
				dinfo.vendor = dev->hid->vendor;
				dinfo.product = dev->hid->product;
				if (copy_to_user(user_arg, &dinfo, sizeof(dinfo)))
					ret = -EFAULT;
				break;
			}
		default:
			{
				struct hid_device *hid = dev->hid;
				if (_IOC_TYPE(cmd) != 'H') {
					if (_IOC_TYPE(cmd) == 'S' &&
						_IOC_NR(cmd) == _IOC_NR(HIDIOCGFEATURE(0))) {
						if (ovr_serial_len > 0) {
							ret = copy_to_user(user_arg, ovr_serial, ovr_serial_len) ?
							-EFAULT : ovr_serial_len;
							break;
						}
					}

					ret = -EINVAL;
					break;
				}

				if (_IOC_NR(cmd) == _IOC_NR(HIDIOCSFEATURE(0))) {
					int len = _IOC_SIZE(cmd);
					ret = ovr_hidraw_send_report(file, user_arg, len, HID_FEATURE_REPORT);
					break;
				}
				if (_IOC_NR(cmd) == _IOC_NR(HIDIOCGFEATURE(0))) {
					int len = _IOC_SIZE(cmd);
					ret = ovr_hidraw_get_report(file, user_arg, len, HID_FEATURE_REPORT);
					break;
				}

				/* Begin Read-only ioctls. */
				if (_IOC_DIR(cmd) != _IOC_READ) {
					ret = -EINVAL;
					break;
				}

				if (_IOC_NR(cmd) == _IOC_NR(HIDIOCGRAWNAME(0))) {
					int len = strlen(hid->name) + 1;
					if (len > _IOC_SIZE(cmd))
						len = _IOC_SIZE(cmd);
					ret = copy_to_user(user_arg, hid->name, len) ?
						-EFAULT : len;
					break;
				}

				if (_IOC_NR(cmd) == _IOC_NR(HIDIOCGRAWPHYS(0))) {
					int len = strlen(hid->phys) + 1;
					if (len > _IOC_SIZE(cmd))
						len = _IOC_SIZE(cmd);
					ret = copy_to_user(user_arg, hid->phys, len) ?
						-EFAULT : len;
					break;
				}
			}

		ret = -ENOTTY;
	}
out:
	mutex_unlock(&minors_lock);
	return ret;
}

static const struct file_operations ovr_ops = {
	.owner = THIS_MODULE,
	.read = ovr_hidraw_read,
	.write = ovr_hidraw_write,
	.poll = ovr_hidraw_poll,
	.open = ovr_hidraw_open,
	.release = ovr_hidraw_release,
	.unlocked_ioctl = ovr_hidraw_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = ovr_hidraw_ioctl,
#endif
	.fasync = ovr_hidraw_fasync,
	.llseek = noop_llseek,
};

static int ovr_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int retval;

	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	retval = hid_parse(hdev);
	if (retval) {
		hid_err(hdev, "ovr - parse failed\n");
		goto exit;
	}

	retval = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
	if (retval) {
		hid_err(hdev, "ovr - hw start failed\n");
		goto exit;
	}

	if (!intf || intf->cur_altsetting->desc.bInterfaceProtocol
			!= USB_TRACKER_INTERFACE_PROTOCOL) {
		return 0;
	}

	if (intf) {
		struct usb_device *udev = interface_to_usbdev(intf);
		if (udev) {
			ovr_serial_len = strnlen(udev->serial, OVR_HIDRAW_MAX_SERIAL);
			if (ovr_serial_len > 0) {
				strncpy(ovr_serial, udev->serial, ovr_serial_len);
				printk("OVR: %s(%d)\n", udev->serial, ovr_serial_len);
			}
		}
	}

	retval = ovr_connect(hdev);

	if (retval) {
		hid_err(hdev, "ovr - Couldn't connect\n");
		goto exit_stop;
	}

	retval = hid_hw_power(hdev, PM_HINT_FULLON);
	if (retval < 0) {
		hid_err(hdev, "ovr - Couldn't feed power\n");
		ovr_disconnect(hdev);
		goto exit_stop;
	}

	retval = hid_hw_open(hdev);
	if (retval < 0) {
		hid_err(hdev, "ovr - Couldn't open hid\n");
		hid_hw_power(hdev, PM_HINT_NORMAL);
		ovr_disconnect(hdev);
		goto exit_stop;
	}

	return 0;

exit_stop:
	hid_hw_stop(hdev);
exit:
	return retval;
}

static void ovr_remove(struct hid_device *hdev)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	if (intf->cur_altsetting->desc.bInterfaceProtocol
			!= USB_TRACKER_INTERFACE_PROTOCOL) {
		hid_hw_stop(hdev);
		return;
	}

	hid_hw_close(hdev);

	hid_hw_power(hdev, PM_HINT_NORMAL);

	ovr_disconnect(hdev);

	hid_hw_stop(hdev);
}

static int ovr_raw_event(struct hid_device *hdev, struct hid_report *report, u8 *data, int size)
{
	int retval = 0;

	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);
	if (intf->cur_altsetting->desc.bInterfaceProtocol
			!= USB_TRACKER_INTERFACE_PROTOCOL) {
		return 0;
	}

	isr_count++;
	last_isr = jiffies;

	if (hdev->hidovr) {
		retval = ovr_report_event(hdev, data, size);
		if (retval < 0)
			printk("OVR: raw event err %d\n", retval);
	}

	return retval;
}

static const struct hid_device_id ovr_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_OVR, USB_DEVICE_ID_OVR_TRACKER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_OVR, USB_DEVICE_ID_OVR_KTRACKER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_OVR, USB_DEVICE_ID_OVR_LATENCY_TESTER) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SAMSUNG_ELECTRONICS, USB_DEVICE_ID_SAMSUNG_GEARVR_1) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SAMSUNG_ELECTRONICS, USB_DEVICE_ID_SAMSUNG_GEARVR_2) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SAMSUNG_ELECTRONICS, USB_DEVICE_ID_SAMSUNG_GEARVR_3) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SAMSUNG_ELECTRONICS, USB_DEVICE_ID_SAMSUNG_GEARVR_4) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SAMSUNG_ELECTRONICS, USB_DEVICE_ID_SAMSUNG_GEARVR_5) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_SAMSUNG_ELECTRONICS, USB_DEVICE_ID_SAMSUNG_GEARVR_6) },
	{ }
};

MODULE_DEVICE_TABLE(hid, ovr_devices);

static struct hid_driver ovr_driver = {
		.name = "ovr",
		.id_table = ovr_devices,
		.probe = ovr_probe,
		.remove = ovr_remove,
		.raw_event = ovr_raw_event
};

static int __init ovr_init(void)
{
	int retval;
	dev_t dev_id;

	ovr_class = class_create(THIS_MODULE, "ovr");
	if (IS_ERR(ovr_class)) {
		return PTR_ERR(ovr_class);
	}

	retval = hid_register_driver(&ovr_driver);
	if (retval < 0) {
		pr_warn("ovr_init - Can't register drive.\n");
		goto out_class;
	}

	retval = alloc_chrdev_region(&dev_id, OVR_FIRST_MINOR,
			OVR_HIDRAW_MAX_DEVICES, "ovr");
	if (retval < 0) {
		pr_warn("ovr_init - Can't allocate chrdev region.\n");
		goto out_register;
	}

	ovr_major = MAJOR(dev_id);
	cdev_init(&ovr_cdev, &ovr_ops);
	cdev_add(&ovr_cdev, dev_id, OVR_HIDRAW_MAX_DEVICES);

	ovr_wq = create_workqueue("ovr_work");

	return 0;

out_register:
	hid_unregister_driver(&ovr_driver);

out_class:
	class_destroy(ovr_class);

	return retval;
}

static void __exit ovr_exit(void)
{
	dev_t dev_id = MKDEV(ovr_major, 0);

	cdev_del(&ovr_cdev);

	unregister_chrdev_region(dev_id, OVR_HIDRAW_MAX_DEVICES);

	hid_unregister_driver(&ovr_driver);

	class_destroy(ovr_class);
}

module_init(ovr_init);
module_exit(ovr_exit);

MODULE_AUTHOR("Lee Cooper");
MODULE_DESCRIPTION("USB Oculus VR char device driver.");
MODULE_LICENSE("GPL v2");
