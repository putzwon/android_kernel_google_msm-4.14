// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rtc.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/suspend.h>
#include <linux/slab.h>
#include <linux/syscore_ops.h>
#include <linux/miscdevice.h>

#define LOG_BUFFER_ENTRIES      1024
#define LOG_BUFFER_ENTRY_SIZE   256
#define ID_LENGTH		50

struct logbuffer {
	int logbuffer_head;
	int logbuffer_tail;
	// protects buffer
	spinlock_t logbuffer_lock;
	u8 *buffer;
	char id[ID_LENGTH];

	struct miscdevice misc;
};

/* Device suspended since last logged. */
static bool suspend_since_last_logged;

static void __logbuffer_log(struct logbuffer *instance,
			    const char *tmpbuffer, bool record_utc)
{
	u64 ts_nsec = local_clock();
	unsigned long rem_nsec = do_div(ts_nsec, 1000000000);

	if (record_utc) {
		struct timespec ts;
		struct rtc_time tm;

		getnstimeofday(&ts);
		rtc_time_to_tm(ts.tv_sec, &tm);
		scnprintf(instance->buffer + (instance->logbuffer_head *
			  LOG_BUFFER_ENTRY_SIZE),
			  LOG_BUFFER_ENTRY_SIZE,
			  "[%5lu.%06lu] %d-%02d-%02d %02d:%02d:%02d.%09lu UTC",
			  (unsigned long)ts_nsec, rem_nsec / 1000,
			  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			  tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec);
	} else {
		scnprintf(instance->buffer + (instance->logbuffer_head *
			  LOG_BUFFER_ENTRY_SIZE),
			  LOG_BUFFER_ENTRY_SIZE, "[%5lu.%06lu] %s",
			  (unsigned long)ts_nsec, rem_nsec / 1000,
			  tmpbuffer);
	}

	instance->logbuffer_head = (instance->logbuffer_head + 1)
			% LOG_BUFFER_ENTRIES;
	if (instance->logbuffer_head == instance->logbuffer_tail) {
		instance->logbuffer_tail = (instance->logbuffer_tail + 1)
				      % LOG_BUFFER_ENTRIES;
	}
}

void logbuffer_vlog(struct logbuffer *instance, const char *fmt,
		    va_list args)
{
	char tmpbuffer[LOG_BUFFER_ENTRY_SIZE];
	unsigned long flags;

	/* Empty log msgs are passed from TCPM to log RTC.
	 * The RTC is printed if thats the first message
	 * printed after resume.
	 */
	vsnprintf(tmpbuffer, sizeof(tmpbuffer), fmt ? : "", args);

	spin_lock_irqsave(&instance->logbuffer_lock, flags);
	if (instance->logbuffer_head < 0 ||
	    instance->logbuffer_head >= LOG_BUFFER_ENTRIES) {
		pr_warn("Bad log buffer index %d\n", instance->logbuffer_head);
		goto abort;
	}

	/* Print UTC at the start of the buffer */
	if ((instance->logbuffer_head == instance->logbuffer_tail) ||
	    (instance->logbuffer_head == LOG_BUFFER_ENTRIES - 1)) {
		__logbuffer_log(instance, tmpbuffer, true);
	/* Print UTC when logging after suspend */
	} else if (suspend_since_last_logged) {
		__logbuffer_log(instance, tmpbuffer, true);
		suspend_since_last_logged = false;
	} else if (!fmt || !strcmp(fmt, "")) {
		goto abort;
	}

	__logbuffer_log(instance, tmpbuffer, false);

abort:
	spin_unlock_irqrestore(&instance->logbuffer_lock, flags);
}
EXPORT_SYMBOL_GPL(logbuffer_vlog);

void logbuffer_log(struct logbuffer *instance, const char *fmt, ...)
{
	va_list args;

	if (!instance)
		return;

	va_start(args, fmt);
	logbuffer_vlog(instance, fmt, args);
	va_end(args);
}
EXPORT_SYMBOL_GPL(logbuffer_log);

static int logbuffer_seq_show(struct seq_file *s, void *v)
{
	struct logbuffer *instance = (struct logbuffer *)s->private;
	int tail;

	spin_lock(&instance->logbuffer_lock);
	tail = instance->logbuffer_tail;
	while (tail != instance->logbuffer_head) {
		seq_printf(s, "%s\n", instance->buffer +
			   (tail * LOG_BUFFER_ENTRY_SIZE));
		tail = (tail + 1) % LOG_BUFFER_ENTRIES;
	}

	spin_unlock(&instance->logbuffer_lock);

	return 0;
}

static int logbuffer_dev_open(struct inode *inode, struct file *file)
{
	struct logbuffer *instance = container_of(file->private_data,
						  struct logbuffer,
						  misc);

	inode->i_private = instance;
	return single_open(file, logbuffer_seq_show, inode->i_private);
}

static const struct file_operations logbuffer_dev_operations = {
	.owner = THIS_MODULE,
	.open = logbuffer_dev_open,
	.read = seq_read,
	.release = single_release,
};

struct logbuffer *logbuffer_register(char *name)
{
	struct logbuffer *instance;
	char buf[50] = "logbuffer_";
	int ret;

	instance = kzalloc(sizeof(struct logbuffer), GFP_KERNEL);
	if (!instance) {
		pr_err("fialed to create instance %s\n", name);
		return ERR_PTR(-ENOMEM);
	}

	instance->buffer = vzalloc(LOG_BUFFER_ENTRIES * LOG_BUFFER_ENTRY_SIZE);
	if (!instance->buffer) {
		pr_err("failed to create buffer %s\n", name);
		instance = ERR_PTR(-ENOMEM);
		goto free_instance;
	}

	strlcat(buf, name, sizeof(buf));
	instance->misc.minor = MISC_DYNAMIC_MINOR;
	instance->misc.name = buf;
	instance->misc.fops = &logbuffer_dev_operations;

	ret = misc_register(&instance->misc);
	if (ret) {
		pr_err("Logbuffer error while doing misc_register ret=%d\n",
			ret);
		goto free_buffer;
	}

	strlcpy(instance->id, name, sizeof(instance->id));

	spin_lock_init(&instance->logbuffer_lock);

	pr_info(" id:%s registered\n", name);
	return instance;

free_buffer:
	vfree(instance->buffer);
free_instance:
	kfree(instance);

	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL_GPL(logbuffer_register);

void logbuffer_unregister(struct logbuffer *instance)
{
	misc_deregister(&instance->misc);
	vfree(instance->buffer);
	pr_info(" id:%s unregistered\n", instance->id);
	kfree(instance);
}
EXPORT_SYMBOL_GPL(logbuffer_unregister);

int logbuffer_suspend(void)
{
	suspend_since_last_logged = true;
	return 0;
}

static struct syscore_ops logbuffer_ops = {
	.suspend        = logbuffer_suspend,
};

static int __init logbuffer_dev_init(void)
{
	register_syscore_ops(&logbuffer_ops);

	return 0;
}

static void logbuffer_dev_exit(void)
{
	unregister_syscore_ops(&logbuffer_ops);
}
early_initcall(logbuffer_dev_init);
module_exit(logbuffer_dev_exit);
