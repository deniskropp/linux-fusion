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
#include "list.h"
#include "call.h"
#include "ref.h"

typedef struct {
     FusionLink  link;
     int         fusion_id;
     int         refs;
} LocalRef;

typedef struct {
     FusionLink         link;

     spinlock_t         lock;

     int                id;
     int                pid;

     int                global;
     int                local;

     int                locked;    /* non-zero fusion id of lock owner */

     bool               watched;   /* true if watch has been installed */
     int                call_id;   /* id of call registered with a watch */
     int                call_arg;  /* optional call parameter */

     FusionLink        *local_refs;

     wait_queue_head_t  wait;
} FusionRef;

/******************************************************************************/

static FusionRef *lookup_ref     (FusionDev *dev, int id);

static FusionRef *lock_ref       (FusionDev *dev, int id);
static void       unlock_ref     (FusionRef *ref);

static int        add_local      (FusionRef *ref, int fusion_id, int add);
static void       clear_local    (FusionDev *dev, FusionRef *ref, int fusion_id);
static void       free_all_local (FusionRef *ref);

static void       notify_ref     (FusionDev *dev, FusionRef *ref);

/******************************************************************************/

static int
refs_read_proc(char *buf, char **start, off_t offset,
               int len, int *eof, void *private)
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     spin_lock (&dev->ref.lock);

     fusion_list_foreach (l, dev->ref.list) {
          FusionRef *ref = (FusionRef*) l;

          if (ref->locked)
               written += sprintf(buf+written, "(%5d) 0x%08x %2d %2d (locked by %d)\n",
                                  ref->pid, ref->id, ref->global, ref->local,
                                  ref->locked);
          else
               written += sprintf(buf+written, "(%5d) 0x%08x %2d %2d\n",
                                  ref->pid, ref->id, ref->global, ref->local);
          if (written < offset) {
               offset -= written;
               written = 0;
          }

          if (written >= len)
               break;
     }

     spin_unlock (&dev->ref.lock);

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
fusion_ref_init (FusionDev *dev)
{
     dev->ref.lock = SPIN_LOCK_UNLOCKED;

     create_proc_read_entry("refs", 0, dev->proc_dir,
                            refs_read_proc, dev);

     return 0;
}

void
fusion_ref_deinit (FusionDev *dev)
{
     FusionLink *l;

     spin_lock (&dev->ref.lock);

     remove_proc_entry ("refs", dev->proc_dir);
     
     l = dev->ref.list;
     while (l) {
          FusionLink *next = l->next;
          FusionRef  *ref  = (FusionRef *) l;

          free_all_local (ref);

          kfree (ref);

          l = next;
     }

     spin_unlock (&dev->ref.lock);
}

/******************************************************************************/

int
fusion_ref_new (FusionDev *dev, int *id)
{
     FusionRef *ref;

     ref = kmalloc (sizeof(FusionRef), GFP_ATOMIC);
     if (!ref)
          return -ENOMEM;

     memset (ref, 0, sizeof(FusionRef));

     spin_lock (&dev->ref.lock);

     ref->id   = dev->ref.ids++;
     ref->pid  = current->pid;
     ref->lock = SPIN_LOCK_UNLOCKED;

     init_waitqueue_head (&ref->wait);

     fusion_list_prepend (&dev->ref.list, &ref->link);

     spin_unlock (&dev->ref.lock);

     *id = ref->id;

     return 0;
}

int
fusion_ref_up (FusionDev *dev, int id, int fusion_id)
{
     FusionRef *ref = lock_ref (dev, id);

     if (!ref)
          return -EINVAL;

     if (ref->locked) {
          unlock_ref (ref);
          return -EAGAIN;
     }

     if (fusion_id) {
          int ret;

          ret = add_local (ref, fusion_id, 1);
          if (ret) {
               unlock_ref (ref);
               return ret;
          }

          ref->local++;
     }
     else
          ref->global++;

     unlock_ref (ref);

     return 0;
}

int
fusion_ref_down (FusionDev *dev, int id, int fusion_id)
{
     FusionRef *ref = lock_ref (dev, id);

     if (!ref)
          return -EINVAL;

     if (ref->locked) {
          unlock_ref (ref);
          return -EAGAIN;
     }

     if (fusion_id) {
          int ret;

          if (!ref->local)
               return -EIO;

          ret = add_local (ref, fusion_id, -1);
          if (ret) {
               unlock_ref (ref);
               return ret;
          }

          ref->local--;
     }
     else {
          if (!ref->global)
               return -EIO;

          ref->global--;
     }

     if (ref->local + ref->global == 0)
          notify_ref (dev, ref);

     unlock_ref (ref);

     return 0;
}

int
fusion_ref_zero_lock (FusionDev *dev, int id, int fusion_id)
{
     FusionRef *ref;

     while (true) {
          ref = lock_ref (dev, id);
          if (!ref)
               return -EINVAL;

          if (ref->watched) {
               unlock_ref (ref);
               return -EACCES;
          }

          if (ref->locked) {
               unlock_ref (ref);
               return ref->locked == fusion_id ? -EIO : -EAGAIN;
          }

          if (ref->global || ref->local) {
               fusion_sleep_on (&ref->wait, &ref->lock, 0);

               if (signal_pending(current))
                    return -ERESTARTSYS;
          }
          else
               break;
     }

     ref->locked = fusion_id;

     unlock_ref (ref);

     return 0;
}

int
fusion_ref_zero_trylock (FusionDev *dev, int id, int fusion_id)
{
     int        ret = 0;
     FusionRef *ref = lock_ref (dev, id);

     if (!ref)
          return -EINVAL;

     if (ref->locked) {
          unlock_ref (ref);
          return ref->locked == fusion_id ? -EIO : -EAGAIN;
     }

     if (ref->global || ref->local)
          ret = -ETOOMANYREFS;
     else
          ref->locked = fusion_id;

     unlock_ref (ref);

     return ret;
}

int
fusion_ref_unlock (FusionDev *dev, int id, int fusion_id)
{
     FusionRef *ref = lock_ref (dev, id);

     if (!ref)
          return -EINVAL;

     if (ref->locked != fusion_id) {
          unlock_ref (ref);
          return -EIO;
     }

     ref->locked = 0;

     unlock_ref (ref);

     return 0;
}

int
fusion_ref_stat (FusionDev *dev, int id, int *refs)
{
     FusionRef *ref = lock_ref (dev, id);

     if (!ref)
          return -EINVAL;

     *refs = ref->global + ref->local;

     unlock_ref (ref);

     return 0;
}

int
fusion_ref_watch (FusionDev      *dev,
                  int             id,
                  int             call_id,
                  int             call_arg)
{
     FusionRef *ref = lock_ref (dev, id);

     if (!ref)
          return -EINVAL;

     if (ref->pid != current->pid) {
          unlock_ref (ref);
          return -EACCES;
     }

     if (ref->global + ref->local == 0) {
          unlock_ref (ref);
          return -EIO;
     }

     if (ref->watched) {
          unlock_ref (ref);
          return -EBUSY;
     }

     ref->watched  = true;
     ref->call_id  = call_id;
     ref->call_arg = call_arg;
     
     wake_up_interruptible_all (&ref->wait);
     
     unlock_ref (ref);

     return 0;
}

int
fusion_ref_destroy (FusionDev *dev, int id)
{
     FusionRef *ref = lookup_ref (dev, id);

     if (!ref)
          return -EINVAL;

     spin_lock (&ref->lock);

     fusion_list_remove (&dev->ref.list, &ref->link);

     wake_up_interruptible_all (&ref->wait);

     spin_unlock (&dev->ref.lock);

     free_all_local (ref);

     spin_unlock (&ref->lock);

     kfree (ref);

     return 0;
}

void
fusion_ref_clear_all_local (FusionDev *dev, int fusion_id)
{
     FusionLink *l;

     spin_lock (&dev->ref.lock);

     fusion_list_foreach (l, dev->ref.list) {
          FusionRef *ref = (FusionRef *) l;

          clear_local (dev, ref, fusion_id);
     }

     spin_unlock (&dev->ref.lock);
}

/******************************************************************************/

static FusionRef *
lookup_ref (FusionDev *dev, int id)
{
     FusionLink *l;

     spin_lock (&dev->ref.lock);

     fusion_list_foreach (l, dev->ref.list) {
          FusionRef *ref = (FusionRef *) l;

          if (ref->id == id)
               return ref;
     }

     spin_unlock (&dev->ref.lock);

     return NULL;
}

static FusionRef *
lock_ref (FusionDev *dev, int id)
{
     FusionRef *ref = lookup_ref (dev, id);

     if (ref) {
          fusion_list_move_to_front (&dev->ref.list, &ref->link);

          spin_lock (&ref->lock);
          spin_unlock (&dev->ref.lock);
     }

     return ref;
}

static void
unlock_ref (FusionRef *ref)
{
     spin_unlock (&ref->lock);
}

static int
add_local (FusionRef *ref, int fusion_id, int add)
{
     FusionLink *l;
     LocalRef   *local;

     fusion_list_foreach (l, ref->local_refs) {
          local = (LocalRef *) l;

          if (local->fusion_id == fusion_id) {
               if (local->refs + add < 0)
                    return -EIO;

               local->refs += add;
               return 0;
          }
     }

     local = kmalloc (sizeof(LocalRef), GFP_ATOMIC);
     if (!local)
          return -ENOMEM;

     local->fusion_id = fusion_id;
     local->refs      = add;

     fusion_list_prepend (&ref->local_refs, &local->link);

     return 0;
}

static void
clear_local (FusionDev *dev, FusionRef *ref, int fusion_id)
{
     FusionLink *l;

     spin_lock (&ref->lock);

     if (ref->locked == fusion_id)
          ref->locked = 0;

     fusion_list_foreach (l, ref->local_refs) {
          LocalRef *local = (LocalRef *) l;

          if (local->fusion_id == fusion_id) {
               ref->local -= local->refs;

               if (ref->local + ref->global == 0)
                    notify_ref (dev, ref);

               fusion_list_remove (&ref->local_refs, l);

               break;
          }
     }

     spin_unlock (&ref->lock);
}

static void
free_all_local (FusionRef *ref)
{
     FusionLink *l = ref->local_refs;

     while (l) {
          FusionLink *next = l->next;

          kfree (l);

          l = next;
     }

     ref->local_refs = NULL;
}

static void
notify_ref (FusionDev *dev, FusionRef *ref)
{
     if (ref->watched) {
          FusionCallExecute execute;

          execute.call_id  = ref->call_id;
          execute.call_arg = ref->call_arg;
          execute.call_ptr = NULL;

          fusion_call_execute (dev, 0, &execute);
     }
     else
          wake_up_interruptible_all (&ref->wait);
}

