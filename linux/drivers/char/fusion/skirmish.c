/*
 *	Fusion Kernel Module
 *
 *	(c) Copyright 2002-2003  Convergence GmbH
 *
 *      Written by Denis Oliver Kropp <dok@directfb.org>
 *
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/sched.h>

#include <linux/fusion.h>

#include "fusiondev.h"
#include "fusionee.h"
#include "list.h"
#include "skirmish.h"

typedef struct {
     FusionLink         link;

     struct semaphore   lock;

     int                id;
     int                pid;

     int                lock_fid;  /* non-zero if locked */
     int                lock_pid;
     int                lock_count;

     wait_queue_head_t  wait;
} FusionSkirmish;

/******************************************************************************/

static int  lookup_skirmish (FusionDev *dev, int id, FusionSkirmish **ret_skirmish);
static int  lock_skirmish   (FusionDev *dev, int id, FusionSkirmish **ret_skirmish);
static void unlock_skirmish (FusionSkirmish *skirmish);

/******************************************************************************/

static int
skirmishs_read_proc(char *buf, char **start, off_t offset,
                    int len, int *eof, void *private)
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     if (down_interruptible (&dev->skirmish.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->skirmish.list) {
          FusionSkirmish *skirmish = (FusionSkirmish*) l;

          if (skirmish->lock_fid) {
               written += sprintf(buf+written, "(%5d) 0x%08x (locked 0x%08x "
                                  "%d)\n", skirmish->pid, skirmish->id,
                                  skirmish->lock_fid, skirmish->lock_pid);
          }
          else {
               written += sprintf(buf+written, "(%5d) 0x%08x\n",
                                  skirmish->pid, skirmish->id);
          }
          if (written < offset) {
               offset -= written;
               written = 0;
          }

          if (written >= len)
               break;
     }

     up (&dev->skirmish.lock);

     *start = buf + offset;
     written -= offset;
     if (written > len) {
          *eof = 0;
          return len;
     }

     *eof = 1;
     return(written<0) ? 0 : written;
}

int
fusion_skirmish_init (FusionDev *dev)
{
     init_MUTEX (&dev->skirmish.lock);

     create_proc_read_entry("skirmishs", 0, dev->proc_dir,
                            skirmishs_read_proc, dev);

     return 0;
}

void
fusion_skirmish_deinit (FusionDev *dev)
{
     FusionLink *l;

     down (&dev->skirmish.lock);

     remove_proc_entry ("skirmishs", dev->proc_dir);

     l = dev->skirmish.list;
     while (l) {
          FusionLink     *next     = l->next;
          FusionSkirmish *skirmish = (FusionSkirmish *) l;

          kfree (skirmish);

          l = next;
     }

     up (&dev->skirmish.lock);
}

/******************************************************************************/

int
fusion_skirmish_new (FusionDev *dev, int *id)
{
     FusionSkirmish *skirmish;

     skirmish = kmalloc (sizeof(FusionSkirmish), GFP_KERNEL);
     if (!skirmish)
          return -ENOMEM;

     memset (skirmish, 0, sizeof(FusionSkirmish));

     if (down_interruptible (&dev->skirmish.lock)) {
          kfree (skirmish);
          return -EINTR;
     }

     skirmish->id   = dev->skirmish.ids++;
     skirmish->pid  = current->pid;

     init_MUTEX (&skirmish->lock);

     init_waitqueue_head (&skirmish->wait);

     fusion_list_prepend (&dev->skirmish.list, &skirmish->link);

     up (&dev->skirmish.lock);

     *id = skirmish->id;

     return 0;
}

int
fusion_skirmish_prevail (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     FusionSkirmish *skirmish;

     dev->stat.skirmish_prevail_swoop++;

     while (true) {
          ret = lock_skirmish (dev, id, &skirmish);
          if (ret)
               return ret;

          if (skirmish->lock_fid) {
               if (skirmish->lock_pid == current->pid) {
                    skirmish->lock_count++;
                    unlock_skirmish (skirmish);
                    return 0;
               }

               fusion_sleep_on (&skirmish->wait, &skirmish->lock, 0);

               if (signal_pending(current))
                    return -ERESTARTSYS;
          }
          else
               break;
     }

     skirmish->lock_fid   = fusion_id;
     skirmish->lock_pid   = current->pid;
     skirmish->lock_count = 1;

     unlock_skirmish (skirmish);

     return 0;
}

int
fusion_skirmish_swoop (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     FusionSkirmish *skirmish;

     ret = lock_skirmish (dev, id, &skirmish);
     if (ret)
          return ret;

     dev->stat.skirmish_prevail_swoop++;

     if (skirmish->lock_fid) {
          if (skirmish->lock_pid == current->pid) {
               skirmish->lock_count++;
               unlock_skirmish (skirmish);
               return 0;
          }

          unlock_skirmish (skirmish);

          return -EAGAIN;
     }

     skirmish->lock_fid   = fusion_id;
     skirmish->lock_pid   = current->pid;
     skirmish->lock_count = 1;

     unlock_skirmish (skirmish);

     return 0;
}

int
fusion_skirmish_dismiss (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     FusionSkirmish *skirmish;

     ret = lock_skirmish (dev, id, &skirmish);
     if (ret)
          return ret;

     dev->stat.skirmish_dismiss++;

     if (skirmish->lock_pid != current->pid) {
          unlock_skirmish (skirmish);
          return -EIO;
     }

     if (--skirmish->lock_count == 0) {
          skirmish->lock_fid = 0;
          skirmish->lock_pid = 0;

          wake_up_interruptible_all (&skirmish->wait);
     }

     unlock_skirmish (skirmish);

     return 0;
}

int
fusion_skirmish_destroy (FusionDev *dev, int id)
{
     int             ret;
     FusionSkirmish *skirmish;

     ret = lookup_skirmish (dev, id, &skirmish);
     if (ret)
          return ret;

     if (down_interruptible (&skirmish->lock)) {
          up (&dev->skirmish.lock);
          return -EINTR;
     }

     fusion_list_remove (&dev->skirmish.list, &skirmish->link);

     up (&dev->skirmish.lock);

     wake_up_interruptible_all (&skirmish->wait);

     up (&skirmish->lock);

     kfree (skirmish);

     return 0;
}

void
fusion_skirmish_dismiss_all (FusionDev *dev, int fusion_id)
{
     FusionLink *l;

     down (&dev->skirmish.lock);

     fusion_list_foreach (l, dev->skirmish.list) {
          FusionSkirmish *skirmish = (FusionSkirmish *) l;

          down (&skirmish->lock);

          if (skirmish->lock_fid == fusion_id) {
               skirmish->lock_fid   = 0;
               skirmish->lock_pid   = 0;
               skirmish->lock_count = 0;

               wake_up_interruptible_all (&skirmish->wait);
          }

          up (&skirmish->lock);
     }

     up (&dev->skirmish.lock);
}

void
fusion_skirmish_dismiss_all_from_pid (FusionDev *dev, int pid)
{
     FusionLink *l;

     down (&dev->skirmish.lock);

     fusion_list_foreach (l, dev->skirmish.list) {
          FusionSkirmish *skirmish = (FusionSkirmish *) l;

          down (&skirmish->lock);

          if (skirmish->lock_pid == pid) {
               skirmish->lock_fid   = 0;
               skirmish->lock_pid   = 0;
               skirmish->lock_count = 0;

               wake_up_interruptible_all (&skirmish->wait);
          }

          up (&skirmish->lock);
     }

     up (&dev->skirmish.lock);
}

/******************************************************************************/

static int
lookup_skirmish (FusionDev *dev, int id, FusionSkirmish **ret_skirmish)
{
     FusionLink *l;

     if (down_interruptible (&dev->skirmish.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->skirmish.list) {
          FusionSkirmish *skirmish = (FusionSkirmish *) l;

          if (skirmish->id == id) {
               *ret_skirmish = skirmish;
               return 0;
          }
     }

     up (&dev->skirmish.lock);

     return -EINVAL;
}

static int
lock_skirmish (FusionDev *dev, int id, FusionSkirmish **ret_skirmish)
{
     int         ret;
     FusionSkirmish *skirmish;

     ret = lookup_skirmish (dev, id, &skirmish);
     if (ret)
          return ret;

     if (skirmish) {
          fusion_list_move_to_front (&dev->skirmish.list, &skirmish->link);

          if (down_interruptible (&skirmish->lock)) {
               up (&dev->skirmish.lock);
               return -EINTR;
          }

          up (&dev->skirmish.lock);
     }

     *ret_skirmish = skirmish;

     return 0;
}

static void
unlock_skirmish (FusionSkirmish *skirmish)
{
     up (&skirmish->lock);
}

