/*
 *	Fusion Kernel Module
 *
 *	(c) Copyright 2002  Convergence GmbH
 *
 *      Written by Denis Oliver Kropp <dok@directfb.org>
 *
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */
 
#ifndef __FUSIONDEV_H__
#define __FUSIONDEV_H__

#include <linux/proc_fs.h>

extern struct proc_dir_entry *proc_fusion_dir;

/*
 * Special version of interruptible_sleep_on() that unlocks the spinlock
 * after adding the entry to the queue (just before schedule).
 */
void fusion_sleep_on(wait_queue_head_t *q,
                     spinlock_t        *lock,
                     signed long       *timeout_ms);

#endif
