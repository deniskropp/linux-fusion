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

typedef struct __Fusion_FusionRef FusionRef;

typedef struct {
     FusionLink  link;
     int         fusion_id;
     int         refs;
} LocalRef;

typedef struct {
     FusionLink  link;
     FusionRef  *ref;
} Inheritor;

struct __Fusion_FusionRef {
     FusionLink         link;

     struct semaphore   lock;

     int                id;
     int                pid;

     int                global;
     int                local;

     int                locked;    /* non-zero fusion id of lock owner */

     bool               watched;   /* true if watch has been installed */
     int                call_id;   /* id of call registered with a watch */
     int                call_arg;  /* optional call parameter */

     FusionRef         *inherited;
     FusionLink        *inheritors;

     FusionLink        *local_refs;

     wait_queue_head_t  wait;
};

/******************************************************************************/

static int  lookup_ref (FusionDev *dev, bool locked, int id, FusionRef **ret_ref);
static int  lock_ref   (FusionDev *dev, bool locked, int id, FusionRef **ret_ref);
static void unlock_ref (FusionRef *ref);

static int  add_local       (FusionRef *ref, int fusion_id, int add);
static void clear_local     (FusionDev *dev, FusionRef *ref, int fusion_id);
static void free_all_local  (FusionRef *ref);

static int  propagate_local (FusionDev *dev, FusionRef *ref, int diff);

static void notify_ref      (FusionDev *dev, FusionRef *ref);

static int  add_inheritor   (FusionRef *ref, FusionRef *from);
static void remove_inheritor(FusionRef *ref, FusionRef *from);
static void drop_inheritors (FusionDev *dev, FusionRef *ref);

/******************************************************************************/

static int
refs_read_proc(char *buf, char **start, off_t offset,
               int len, int *eof, void *private)
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     if (down_interruptible (&dev->ref.lock))
          return -EINTR;

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

     up (&dev->ref.lock);

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
     init_MUTEX (&dev->ref.lock);

     create_proc_read_entry("refs", 0, dev->proc_dir,
                            refs_read_proc, dev);

     return 0;
}

void
fusion_ref_deinit (FusionDev *dev)
{
     FusionLink *l;

     down (&dev->ref.lock);

     remove_proc_entry ("refs", dev->proc_dir);

     l = dev->ref.list;
     while (l) {
          FusionLink *next = l->next;
          FusionRef  *ref  = (FusionRef *) l;

          free_all_local (ref);

          kfree (ref);

          l = next;
     }

     up (&dev->ref.lock);
}

/******************************************************************************/

int
fusion_ref_new (FusionDev *dev, int *id)
{
     FusionRef *ref;

     ref = kmalloc (sizeof(FusionRef), GFP_KERNEL);
     if (!ref)
          return -ENOMEM;

     memset (ref, 0, sizeof(FusionRef));

     if (down_interruptible (&dev->ref.lock)) {
          kfree (ref);
          return -EINTR;
     }

     ref->id   = dev->ref.ids++;
     ref->pid  = current->pid;

     init_MUTEX (&ref->lock);

     init_waitqueue_head (&ref->wait);

     fusion_list_prepend (&dev->ref.list, &ref->link);

     up (&dev->ref.lock);

     *id = ref->id;

     return 0;
}

int
fusion_ref_up (FusionDev *dev, int id, int fusion_id)
{
     int        ret;
     FusionRef *ref;

     ret = lookup_ref (dev, false, id, &ref);
     if (ret)
          return ret;

     if (down_interruptible (&ref->lock)) {
          up (&dev->ref.lock);
          return -EINTR;
     }

     dev->stat.ref_up++;

     if (ref->locked) {
          ret = -EAGAIN;
          goto out;
     }

     if (fusion_id) {
          ret = add_local (ref, fusion_id, 1);
          if (ret)
               goto out;

          ret = propagate_local( dev, ref, 1 );
     }
     else
          ref->global++;


out:
     up (&dev->ref.lock);
     unlock_ref (ref);

     return ret;
}

int
fusion_ref_down (FusionDev *dev, int id, int fusion_id)
{
     int        ret;
     FusionRef *ref;

     ret = lookup_ref (dev, false, id, &ref);
     if (ret)
          return ret;

     if (down_interruptible (&ref->lock)) {
          up (&dev->ref.lock);
          return -EINTR;
     }

     dev->stat.ref_down++;

     if (ref->locked) {
          ret = -EAGAIN;
          goto out;
     }

     if (fusion_id) {
          ret = -EIO;
          if (!ref->local)
               goto out;

          ret = add_local (ref, fusion_id, -1);
          if (ret)
               goto out;

          ret = propagate_local( dev, ref, -1 );
     }
     else {
          if (!ref->global) {
               ret = -EIO;
               goto out;
          }

          ref->global--;

          if (ref->local + ref->global == 0)
               notify_ref (dev, ref);
     }


out:
     up (&dev->ref.lock);
     unlock_ref (ref);

     return ret;
}

int
fusion_ref_zero_lock (FusionDev *dev, int id, int fusion_id)
{
     int        ret;
     FusionRef *ref;

     while (true) {
          ret = lock_ref (dev, false, id, &ref);
          if (ret)
               return ret;

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
                    return -EINTR;
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
     int        ret;
     FusionRef *ref;

     ret = lock_ref (dev, false, id, &ref);
     if (ret)
          return ret;

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
     int        ret;
     FusionRef *ref;

     ret = lock_ref (dev, false, id, &ref);
     if (ret)
          return ret;

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
     int        ret;
     FusionRef *ref;

     ret = lock_ref (dev, false, id, &ref);
     if (ret)
          return ret;

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
     int        ret;
     FusionRef *ref;

     ret = lock_ref (dev, false, id, &ref);
     if (ret)
          return ret;

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
fusion_ref_inherit (FusionDev *dev,
                    int        id,
                    int        from_id)
{
     int        ret;
     FusionRef *ref;
     FusionRef *from = NULL;

     ret = lookup_ref (dev, false, id, &ref);
     if (ret)
          return ret;

     if (down_interruptible (&ref->lock)) {
          up (&dev->ref.lock);
          return -EINTR;
     }

     ret = -EBUSY;
     if (ref->inherited)
          goto out;

     ret = lock_ref (dev, true, from_id, &from);
     if (ret)
          goto out;

     ret = add_inheritor( ref, from );
     if (ret)
          goto out;

     ret = propagate_local( dev, ref, from->local );
     if (ret)
          goto out;

     ref->inherited = from;

out:
     if (from)
          unlock_ref (from);

     unlock_ref (ref);

     up (&dev->ref.lock);

     return ret;
}

int
fusion_ref_destroy (FusionDev *dev, int id)
{
     int        ret;
     FusionRef *ref;

     ret = lookup_ref (dev, false, id, &ref);
     if (ret)
          return ret;

     if (down_interruptible (&ref->lock)) {
          up (&dev->ref.lock);
          return -EINTR;
     }

     drop_inheritors( dev, ref );

     if (ref->inherited)
          remove_inheritor( ref, ref->inherited );

     fusion_list_remove (&dev->ref.list, &ref->link);

     wake_up_interruptible_all (&ref->wait);

     up (&dev->ref.lock);

     free_all_local (ref);

     up (&ref->lock);

     kfree (ref);

     return 0;
}

void
fusion_ref_clear_all_local (FusionDev *dev, int fusion_id)
{
     FusionLink *l;

     down (&dev->ref.lock);

     fusion_list_foreach (l, dev->ref.list) {
          FusionRef *ref = (FusionRef *) l;

          clear_local (dev, ref, fusion_id);
     }

     up (&dev->ref.lock);
}

/******************************************************************************/

static int
lookup_ref (FusionDev *dev, bool locked, int id, FusionRef **ret_ref)
{
     FusionLink *l;

     if (!locked && down_interruptible (&dev->ref.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->ref.list) {
          FusionRef *ref = (FusionRef *) l;

          if (ref->id == id) {
               *ret_ref = ref;
               return 0;
          }
     }

     if (!locked)
          up (&dev->ref.lock);

     return -EINVAL;
}

static int
lock_ref (FusionDev *dev, bool locked, int id, FusionRef **ret_ref)
{
     int         ret;
     FusionRef *ref;

     ret = lookup_ref (dev, locked, id, &ref);
     if (ret)
          return ret;

     if (ref) {
          fusion_list_move_to_front (&dev->ref.list, &ref->link);

          if (down_interruptible (&ref->lock)) {
               if (!locked)
                    up (&dev->ref.lock);
               return -EINTR;
          }

          if (!locked)
               up (&dev->ref.lock);
     }

     *ret_ref = ref;

     return 0;
}

static void
unlock_ref (FusionRef *ref)
{
     up (&ref->lock);
}

static int
add_local (FusionRef *ref, int fusion_id, int add)
{
     FusionLink *l;
     LocalRef   *local;

     fusion_list_foreach (l, ref->local_refs) {
          local = (LocalRef *) l;

          if (local->fusion_id == fusion_id) {
               fusion_list_move_to_front( &ref->local_refs, l );

               if (local->refs + add < 0)
                    return -EIO;

               local->refs += add;
               return 0;
          }
     }

     local = kmalloc (sizeof(LocalRef), GFP_KERNEL);
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

     down (&ref->lock);

     if (ref->locked == fusion_id) {
          ref->locked = 0;
          wake_up_interruptible_all (&ref->wait);
     }

     fusion_list_foreach (l, ref->local_refs) {
          LocalRef *local = (LocalRef *) l;

          if (local->fusion_id == fusion_id) {
               if (local->refs)
                    propagate_local( dev, ref, - local->refs );

               fusion_list_remove( &ref->local_refs, l );

               kfree (l);
               break;
          }
     }

     up (&ref->lock);
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

static int
propagate_local( FusionDev *dev, FusionRef *ref, int diff )
{
     FusionLink *l;

     /* Recurse into inheritors. */
     fusion_list_foreach (l, ref->inheritors) {
          FusionRef *inheritor = ((Inheritor*) l)->ref;

          if (down_interruptible( &inheritor->lock )) {
               printk( KERN_ERR "fusion_ref: propagate_local() interrupted!\n" );
               //return -EINTR;
          }

          propagate_local( dev, inheritor, diff );

          up( &inheritor->lock );
     }

     /* Apply difference. */
     ref->local += diff;

     /* Notify zero count. */
     if (ref->local + ref->global == 0)
          notify_ref( dev, ref );

     return 0;
}

static int
add_inheritor(FusionRef *ref, FusionRef *from)
{
     Inheritor *inheritor;

     inheritor = kmalloc (sizeof(Inheritor), GFP_KERNEL);
     if (!inheritor)
          return -ENOMEM;

     inheritor->ref = ref;

     fusion_list_prepend( &from->inheritors, &inheritor->link );

     return 0;
}

static void
remove_inheritor(FusionRef *ref, FusionRef *from)
{
     FusionLink *l;

     down( &from->lock );

     fusion_list_foreach (l, from->inheritors) {
          Inheritor *inheritor = (Inheritor*) l;

          if (inheritor->ref == ref) {
               fusion_list_remove( &from->inheritors, &inheritor->link );

               kfree( l );
               break;
          }
     }

     up( &from->lock );
}

static void
drop_inheritors( FusionDev *dev, FusionRef *ref )
{
     FusionLink *l = ref->inheritors;

     while (l) {
          FusionLink *next      = l->next;
          FusionRef  *inheritor = ((Inheritor*) l)->ref;

          if (down_interruptible( &inheritor->lock )) {
               printk( KERN_ERR "fusion_ref: drop_inheritors() interrupted!\n" );
               //return;
          }

          propagate_local( dev, inheritor, - ref->local );

          inheritor->inherited = NULL;

          up( &inheritor->lock );


          kfree (l);

          l = next;
     }

     ref->inheritors = NULL;
}

