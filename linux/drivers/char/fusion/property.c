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

#ifndef yield
#define yield schedule
#endif

#include <linux/fusion.h>

#include "fusiondev.h"
#include "fusionee.h"
#include "list.h"
#include "property.h"

typedef enum {
     FUSION_PROPERTY_AVAILABLE = 0,
     FUSION_PROPERTY_LEASED,
     FUSION_PROPERTY_PURCHASED
} FusionPropertyState;

typedef struct {
     FusionLink          link;

     struct semaphore    lock;

     int                 id;
     int                 pid;

     FusionPropertyState state;
     int                 fusion_id; /* non-zero if leased/purchased */
     unsigned long       purchase_stamp;
     int                 lock_pid;
     int                 count;    /* lock counter */

     wait_queue_head_t   wait;
} FusionProperty;

/******************************************************************************/

static int  lookup_property (FusionDev *dev, int id, FusionProperty **ret_property);
static int  lock_property   (FusionDev *dev, int id, FusionProperty **ret_property);
static void unlock_property (FusionProperty *property);

/******************************************************************************/

static int
properties_read_proc(char *buf, char **start, off_t offset,
                     int len, int *eof, void *private)
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     if (down_interruptible (&dev->property.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->property.list) {
          FusionProperty *property = (FusionProperty*) l;

          if (property->state != FUSION_PROPERTY_AVAILABLE) {
               written += sprintf(buf+written, "(%5d) 0x%08x %s (0x%08x %d)\n",
                                  property->pid, property->id,
                                  property->state == FUSION_PROPERTY_LEASED ?
                                  "leased" : "purchased", property->fusion_id,
                                  property->lock_pid);
          }
          else {
               written += sprintf(buf+written, "(%5d) 0x%08x\n",
                                  property->pid, property->id);
          }

          if (written < offset) {
               offset -= written;
               written = 0;
          }

          if (written >= len)
               break;
     }

     up (&dev->property.lock);

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
fusion_property_init (FusionDev *dev)
{
     init_MUTEX (&dev->property.lock);

     create_proc_read_entry("properties", 0, dev->proc_dir,
                            properties_read_proc, dev);

     return 0;
}

void
fusion_property_deinit (FusionDev *dev)
{
     FusionLink *l;

     down (&dev->property.lock);

     remove_proc_entry ("properties", dev->proc_dir);

     l = dev->property.list;
     while (l) {
          FusionLink     *next     = l->next;
          FusionProperty *property = (FusionProperty *) l;

          kfree (property);

          l = next;
     }

     up (&dev->property.lock);
}

/******************************************************************************/

int
fusion_property_new (FusionDev *dev, int *id)
{
     FusionProperty *property;

     property = kmalloc (sizeof(FusionProperty), GFP_KERNEL);
     if (!property)
          return -ENOMEM;

     memset (property, 0, sizeof(FusionProperty));

     if (down_interruptible (&dev->property.lock)) {
          kfree (property);
          return -EINTR;
     }

     property->id   = dev->property.ids++;
     property->pid  = current->pid;

     init_MUTEX (&property->lock);

     init_waitqueue_head (&property->wait);

     fusion_list_prepend (&dev->property.list, &property->link);

     up (&dev->property.lock);

     *id = property->id;

     return 0;
}

int
fusion_property_lease (FusionDev *dev, int id, int fusion_id)
{
     FusionProperty *property;
     signed long     timeout = -1;

     dev->stat.property_lease_purchase++;

     while (true) {
          int ret;

          ret = lock_property (dev, id, &property);
          if (ret)
               return ret;

          switch (property->state) {
               case FUSION_PROPERTY_AVAILABLE:
                    property->state     = FUSION_PROPERTY_LEASED;
                    property->fusion_id = fusion_id;
                    property->lock_pid  = current->pid;
                    property->count     = 1;

                    unlock_property (property);
                    return 0;

               case FUSION_PROPERTY_LEASED:
                    if (property->lock_pid == current->pid) {
                         property->count++;

                         unlock_property (property);
                         return 0;
                    }

                    fusion_sleep_on (&property->wait, &property->lock, NULL);

                    if (signal_pending(current))
                         return -ERESTARTSYS;

                    break;

               case FUSION_PROPERTY_PURCHASED:
                    switch (timeout) {
                         case -1:
                              if (jiffies - property->purchase_stamp > HZ / 10) {
                         case 0:
                                   unlock_property (property);
                                   return -EAGAIN;
                              }
                              else
                                   timeout = HZ / 10;

                              /* fall through */

                         default:
                              fusion_sleep_on (&property->wait, &property->lock, &timeout);

                              if (signal_pending(current))
                                   return -ERESTARTSYS;

                              break;
                    }

                    break;
          }
     }

     /* won't reach this */
     return -1;
}

int
fusion_property_purchase (FusionDev *dev, int id, int fusion_id)
{
     FusionProperty *property;
     signed long     timeout = -1;

     dev->stat.property_lease_purchase++;

     while (true) {
          int ret;

          ret = lock_property (dev, id, &property);
          if (ret)
               return ret;

          switch (property->state) {
               case FUSION_PROPERTY_AVAILABLE:
                    property->state          = FUSION_PROPERTY_PURCHASED;
                    property->fusion_id      = fusion_id;
                    property->purchase_stamp = jiffies;
                    property->lock_pid       = current->pid;
                    property->count          = 1;

                    wake_up_interruptible_all (&property->wait);

                    unlock_property (property);
                    return 0;

               case FUSION_PROPERTY_LEASED:
                    fusion_sleep_on (&property->wait, &property->lock, NULL);

                    if (signal_pending(current))
                         return -ERESTARTSYS;

                    break;

               case FUSION_PROPERTY_PURCHASED:
                    switch (timeout) {
                         case -1:
                              if (jiffies - property->purchase_stamp > HZ) {
                         case 0:
                                   unlock_property (property);
                                   return -EAGAIN;
                              }
                              else
                                   timeout = HZ;

                              /* fall through */

                         default:
                              fusion_sleep_on (&property->wait, &property->lock, &timeout);

                              if (signal_pending(current))
                                   return -ERESTARTSYS;

                              break;
                    }

                    break;
          }
     }

     /* won't reach this */
     return -1;
}

int
fusion_property_cede (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     bool            purchased;
     FusionProperty *property;

     ret = lock_property (dev, id, &property);
     if (ret)
          return ret;

     dev->stat.property_cede++;

     if (property->lock_pid != current->pid) {
          unlock_property (property);
          return -EIO;
     }

     if (--property->count) {
          unlock_property (property);
          return 0;
     }

     purchased = (property->state == FUSION_PROPERTY_PURCHASED);

     property->state     = FUSION_PROPERTY_AVAILABLE;
     property->fusion_id = 0;
     property->lock_pid  = 0;

     wake_up_interruptible_all (&property->wait);

     unlock_property (property);

     if (purchased)
          yield();

     return 0;
}

int
fusion_property_holdup (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     FusionProperty *property;

     ret = lock_property (dev, id, &property);
     if (ret)
          return ret;

     if (property->state == FUSION_PROPERTY_PURCHASED) {
          if (property->fusion_id == fusion_id) {
               unlock_property (property);
               return -EIO;
          }

          fusionee_kill (dev, fusion_id, property->fusion_id, SIGKILL, -1);
     }

     unlock_property (property);

     return 0;
}

int
fusion_property_destroy (FusionDev *dev, int id)
{
     int             ret;
     FusionProperty *property;

     ret = lookup_property (dev, id, &property);
     if (ret)
          return ret;

     if (down_interruptible (&property->lock)) {
          up (&dev->property.lock);
          return -EINTR;
     }

     fusion_list_remove (&dev->property.list, &property->link);

     wake_up_interruptible_all (&property->wait);

     up (&dev->property.lock);

     up (&property->lock);

     kfree (property);

     return 0;
}

void
fusion_property_cede_all (FusionDev *dev, int fusion_id)
{
     FusionLink *l;

     down (&dev->property.lock);

     fusion_list_foreach (l, dev->property.list) {
          FusionProperty *property = (FusionProperty *) l;

          down (&property->lock);

          if (property->fusion_id == fusion_id) {
               property->state     = FUSION_PROPERTY_AVAILABLE;
               property->fusion_id = 0;
               property->lock_pid  = 0;

               wake_up_interruptible_all (&property->wait);
          }

          up (&property->lock);
     }

     up (&dev->property.lock);
}

/******************************************************************************/

static int
lookup_property (FusionDev *dev, int id, FusionProperty **ret_property)
{
     FusionLink *l;

     if (down_interruptible (&dev->property.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->property.list) {
          FusionProperty *property = (FusionProperty *) l;

          if (property->id == id) {
               *ret_property = property;
               return 0;
          }
     }

     up (&dev->property.lock);

     return -EINVAL;
}

static int
lock_property (FusionDev *dev, int id, FusionProperty **ret_property)
{
     int             ret;
     FusionProperty *property;

     ret = lookup_property (dev, id, &property);
     if (ret)
          return ret;

     if (property) {
          fusion_list_move_to_front (&dev->property.list, &property->link);

          if (down_interruptible (&property->lock)) {
               up (&dev->property.lock);
               return -EINTR;
          }

          up (&dev->property.lock);
     }

     *ret_property = property;

     return 0;
}

static void
unlock_property (FusionProperty *property)
{
     up (&property->lock);
}

