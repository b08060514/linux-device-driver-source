/*
 * jit.c -- the just-in-time module
 *
 * Copyright (C) 2001,2003 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001,2003 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 * $Id: jit.c,v 1.16 2004/09/26 07:02:43 gregkh Exp $
 */

//#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/time.h>
#include <linux/timer.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/fs.h>

#include <asm/hardirq.h>
/*
 * This module is a silly one: it only embeds short code fragments
 * that show how time delays can be handled in the kernel.
 */

struct proc_dir_entry {
		unsigned int low_ino;
		umode_t mode;
		nlink_t nlink;
		kuid_t uid;
		kgid_t gid;
		loff_t size;
		const struct inode_operations *proc_iops;
		const struct file_operations *proc_fops;
		struct proc_dir_entry *next, *parent, *subdir;
		void *data;
		atomic_t count;/*  use count */
		atomic_t in_use; /*  in_usenumber of callers into module in progress; */
		/*  negative -> it's going away RSN */
		struct completion *pde_unload_completion;
		struct list_head pde_openers;   /* swho did ->open, but not ->release */
		spinlock_t pde_unload_lock; /*  proc_fops checks and pde_users bumps */
		u8 namelen;
		char name[];
};

struct proc_inode {
		struct pid *pid;
		int fd;
		union proc_op;
		struct proc_dir_entry *pde;
		struct ctl_table_header *sysctl;
		struct ctl_table *sysctl_entry;
		struct proc_ns;
		struct inode vfs_inode;
};



int delay = HZ; /* the default delay, expressed in jiffies */

module_param(delay, int, 0);

MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("Dual BSD/GPL");

/* use these as data pointers, to implement four files in one function */
enum jit_files {
	JIT_BUSY,
	JIT_SCHED,
	JIT_QUEUE,
	JIT_SCHEDTO
};

/*
 * This function prints one line of data, after sleeping one second.
 * It can sleep in different ways, according to the data pointer
 
int jit_fn(char *buf, char **start, off_t offset,
	      int len, int *eof, void *data)
*/
int jit_fn(struct seq_file *m, void *v)
{
	unsigned long j0, j1; /* jiffies */
	wait_queue_head_t wait;

	struct proc_dir_entry *pde = m->private;
	if(pde)
	{
	    printk("No pde->data!");
		return 0;
	}
	init_waitqueue_head (&wait);
	j0 = jiffies;
	j1 = j0 + delay;

	switch(*(long *)(pde->data)) {
		case JIT_BUSY:
			while (time_before(jiffies, j1))
				cpu_relax();
			break;
		case JIT_SCHED:
			while (time_before(jiffies, j1)) {
				schedule();
			}
			break;
		case JIT_QUEUE:
			wait_event_interruptible_timeout(wait, 0, delay);
			break;
		case JIT_SCHEDTO:
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout (delay);
			break;
	}
	j1 = jiffies; /* actual value after we delayed */

	seq_printf(m, "%9li %9li\n", j0, j1);
	return 0;
}

/*
 * This file, on the other hand, returns the current time forever
 */
//int jit_currentime(char *buf, char **start, off_t offset,
//                   int len, int *eof, void *data)
int jit_currentime(struct seq_file *m,void *v)
{
	struct timeval tv1;
	struct timespec tv2;
	unsigned long j1;
	u64 j2;

	/* get them four */
	j1 = jiffies;
	j2 = get_jiffies_64();
	do_gettimeofday(&tv1);
	tv2 = current_kernel_time();

	/* print */
	seq_printf(m,"0x%08lx 0x%016Lx %10i.%06i\n"
		       "%40i.%09i\n",
		       j1, j2,
		       (int) tv1.tv_sec, (int) tv1.tv_usec,
		       (int) tv2.tv_sec, (int) tv2.tv_nsec);
	return 0;
}

/*
 * The timer example follows
 */

int tdelay = 10;
module_param(tdelay, int, 0);

/* This data structure used as "data" for the timer and tasklet functions */
struct jit_data {
	struct timer_list timer;
	struct tasklet_struct tlet;
	int hi; /* tasklet or tasklet_hi */
	wait_queue_head_t wait;
	unsigned long prevjiffies;
	unsigned char *buf;
	int loops;
};
#define JIT_ASYNC_LOOPS 5

void jit_timer_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	data->buf += sprintf(data->buf, "%9li  %3li     %i    %6i   %i   %s\n",
			     j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			     current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->timer.expires += tdelay;
		data->prevjiffies = j;
		add_timer(&data->timer);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* the /proc function: allocate everything to allow concurrency */
int jit_timer(char *buf, char **start, off_t offset,
	      int len, int *eof, void *unused_data)
{
	struct jit_data *data;
	char *buf2 = buf;
	unsigned long j = jiffies;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_timer(&data->timer);
	init_waitqueue_head (&data->wait);

	/* write the first lines in the buffer */
	buf2 += sprintf(buf2, "   time   delta  inirq    pid   cpu command\n");
	buf2 += sprintf(buf2, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* fill the data for our timer function */
	data->prevjiffies = j;
	data->buf = buf2;
	data->loops = JIT_ASYNC_LOOPS;
	
	/* register the timer */
	data->timer.data = (unsigned long)data;
	data->timer.function = jit_timer_fn;
	data->timer.expires = j + tdelay; /* parameter */
	add_timer(&data->timer);

	/* wait for the buffer to fill */
	wait_event_interruptible(data->wait, !data->loops);
	if (signal_pending(current))
		return -ERESTARTSYS;
	buf2 = data->buf;
	kfree(data);
	*eof = 1;
	return buf2 - buf;
}

void jit_tasklet_fn(unsigned long arg)
{
	struct jit_data *data = (struct jit_data *)arg;
	unsigned long j = jiffies;
	data->buf += sprintf(data->buf, "%9li  %3li     %i    %6i   %i   %s\n",
			     j, j - data->prevjiffies, in_interrupt() ? 1 : 0,
			     current->pid, smp_processor_id(), current->comm);

	if (--data->loops) {
		data->prevjiffies = j;
		if (data->hi)
			tasklet_hi_schedule(&data->tlet);
		else
			tasklet_schedule(&data->tlet);
	} else {
		wake_up_interruptible(&data->wait);
	}
}

/* the /proc function: allocate everything to allow concurrency */
int jit_tasklet(char *buf, char **start, off_t offset,
	      int len, int *eof, void *arg)
{
	struct jit_data *data;
	char *buf2 = buf;
	unsigned long j = jiffies;
	long hi = (long)arg;

	data = kmalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	init_waitqueue_head (&data->wait);

	/* write the first lines in the buffer */
	buf2 += sprintf(buf2, "   time   delta  inirq    pid   cpu command\n");
	buf2 += sprintf(buf2, "%9li  %3li     %i    %6i   %i   %s\n",
			j, 0L, in_interrupt() ? 1 : 0,
			current->pid, smp_processor_id(), current->comm);

	/* fill the data for our tasklet function */
	data->prevjiffies = j;
	data->buf = buf2;
	data->loops = JIT_ASYNC_LOOPS;
	
	/* register the tasklet */
	tasklet_init(&data->tlet, jit_tasklet_fn, (unsigned long)data);
	data->hi = hi;
	if (hi)
		tasklet_hi_schedule(&data->tlet);
	else
		tasklet_schedule(&data->tlet);

	/* wait for the buffer to fill */
	wait_event_interruptible(data->wait, !data->loops);

	if (signal_pending(current))
		return -ERESTARTSYS;
	buf2 = data->buf;
	kfree(data);
	*eof = 1;
	return buf2 - buf;
}

static int jit_currentime_open(struct inode *inode,struct file *file)
{
	return single_open(file,jit_currentime,NULL);
}

static void *jit_seq_start(struct seq_file *s, loff_t *pos)
{
	return 0;
}
static void *jit_seq_next(struct seq_file *s, loff_t *pos)
{
	return 0;
}
static void *jit_seq_stop(struct seq_file *s, loff_t *pos)
{
	return 0;
}
static struct seq_operations jit_fn_seq_ops = {
	.start = jit_seq_start,
	.next = jit_seq_next,
	.stop = jit_seq_stop,
	.show = jit_fn
};
static int jit_fn_open(struct inode *inode,struct file *file)
{
	//return  single_open(file,jit_fn,NULL);
	int ret = seq_open(file, &jit_fn_seq_ops);
	if (!ret) {
		struct seq_file *sf = file->private_data;
		sf->private = container_of(inode,struct proc_inode,vfs_inode);
	}
	return ret;
}
static const struct file_operations currentime_proc_fops = {
	.open = jit_currentime_open,
	.read = seq_read,
	.llseek = seq_lseek,
    .release = single_release,
};
static const struct file_operations jit_fn_fops = {
	.open = jit_fn_open,
	.read = seq_read,
	.llseek = seq_lseek,
    .release = seq_release,
};

int __init jit_init(void)
{
    //create_proc_read_entry("currentime", 0, NULL, jit_currentime, NULL);
	proc_create("currentime",0,NULL,&currentime_proc_fops);
	proc_create_data("jitschedto",0,NULL,&jit_fn_fops,(void *)JIT_SCHED);
//	create_proc_read_entry("jitbusy", 0, NULL, jit_fn, (void *)JIT_BUSY);
//	proc_create("jitbusy",0,NULL,&jit_fn_fops);
//	create_proc_read_entry("jitsched",0, NULL, jit_fn, (void *)JIT_SCHED);
//	create_proc_read_entry("jitqueue",0, NULL, jit_fn, (void *)JIT_QUEUE);
//	create_proc_read_entry("jitschedto", 0, NULL, jit_fn, (void *)JIT_SCHEDTO);

//	create_proc_read_entry("jitimer", 0, NULL, jit_timer, NULL);
//	create_proc_read_entry("jitasklet", 0, NULL, jit_tasklet, NULL);
//	create_proc_read_entry("jitasklethi", 0, NULL, jit_tasklet, (void *)1);
//
	return 0; /* success */
}

void __exit jit_cleanup(void)
{
	remove_proc_entry("currentime", NULL);
	remove_proc_entry("jitschedto", NULL);
/*	remove_proc_entry("jitbusy", NULL);
    remove_proc_entry("jitsched", NULL);
	remove_proc_entry("jitqueue", NULL);

	remove_proc_entry("jitimer", NULL);
	remove_proc_entry("jitasklet", NULL);
	remove_proc_entry("jitasklethi", NULL);
*/
}

module_init(jit_init);
module_exit(jit_cleanup);
