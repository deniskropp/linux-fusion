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
#include "property.h"

typedef enum {
     FUSION_PROPERTY_AVAILABLE = 0,
     FUSION_PROPERTY_LEASED,
     FUSION_PROPERTY_PURCHASED
} FusionPropertyState;

typedef struct {
     FusionLink          link;

     spinlock_t          lock;

     int                 id;
     int                 pid;

     FusionPropertyState state;
     int                 fusion_id; /* non-zero if leased/purchased */
     unsigned long       purchase_stamp;

     wait_queue_head_t   wait;
} FusionProperty;

/******************************************************************************/

static FusionProperty *lookup_property     (FusionDev *dev, int id);

static FusionProperty *lock_property       (FusionDev *dev, int id);
static void            unlock_property     (FusionProperty *property);

/******************************************************************************/

static int
properties_read_proc(char *buf, char **start, off_t offset,
                     int len, int *eof, void *private)
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     spin_lock (&dev->property.lock);

     fusion_list_foreach (l, dev->property.list) {
          FusionProperty *property = (FusionProperty*) l;

          written += sprintf(buf+written, "(%5d) 0x%08x %s\n",
                             property->pid, property->id,
                             property->state ?
                             (property->state == FUSION_PROPERTY_LEASED ?
                              "leased" : "purchased") :
                             "");
          if (written < offset) {
               offset -= written;
               written = 0;
          }

          if (written >= len)
               break;
     }

     spin_unlock (&dev->property.lock);

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
     dev->property.lock = SPIN_LOCK_UNLOCKED;

     create_proc_read_entry("properties", 0, dev->proc_dir,
                            properties_read_proc, dev);

     return 0;
}

void
fusion_property_deinit (FusionDev *dev)
{
     FusionLink *l;

     spin_lock (&dev->property.lock);

     remove_proc_entry ("properties", dev->proc_dir);
     
     l = dev->property.list;
     while (l) {
          FusionLink     *next     = l->next;
          FusionProperty *property = (FusionProperty *) l;

          kfree (property);

          l = next;
     }

     spin_unlock (&dev->property.lock);
}

/******************************************************************************/

int
fusion_property_new (FusionDev *dev, int *id)
{
     FusionProperty *property;

     property = kmalloc (sizeof(FusionProperty), GFP_ATOMIC);
     if (!property)
          return -ENOMEM;

     memset (property, 0, sizeof(FusionProperty));

     spin_lock (&dev->property.lock);

     property->id   = dev->property.ids++;
     property->pid  = current->pid;
     property->lock = SPIN_LOCK_UNLOCKED;

     init_waitqueue_head (&property->wait);

     fusion_list_prepend (&dev->property.list, &property->link);

     spin_unlock (&dev->property.lock);

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
          property = lock_property (dev, id);
          if (!property)
               return -EINVAL;

          switch (property->state) {
               case FUSION_PROPERTY_AVAILABLE:
                    property->state     = FUSION_PROPERTY_LEASED;
                    property->fusion_id = fusion_id;

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
          property = lock_property (dev, id);
          if (!property)
               return -EINVAL;

          switch (property->state) {
               case FUSION_PROPERTY_AVAILABLE:
                    property->state          = FUSION_PROPERTY_PURCHASED;
                    property->fusion_id      = fusion_id;
                    property->purchase_stamp = jiffies;

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
     bool            purchased;
     FusionProperty *property = lock_property (dev, id);

     dev->stat.property_cede++;
     
     if (!property)
          return -EINVAL;

     if (property->fusion_id != fusion_id) {
          unlock_property (property);
          return -EIO;
     }

     purchased = (property->state == FUSION_PROPERTY_PURCHASED);

     property->state     = FUSION_PROPERTY_AVAILABLE;
     property->fusion_id = 0;

     wake_up_interruptible_all (&property->wait);

     unlock_property (property);

     if (purchased)
          yield();

     return 0;
}

int
fusion_property_holdup (FusionDev *dev, int id, int fusion_id)
{
     FusionProperty *property = lock_property (dev, id);

     if (!property)
          return -EINVAL;

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
     FusionProperty *property = lookup_property (dev, id);

     if (!property)
          return -EINVAL;

     spin_lock (&property->lock);

     fusion_list_remove (&dev->property.list, &property->link);

     wake_up_interruptible_all (&property->wait);

     spin_unlock (&dev->property.lock);

     spin_unlock (&property->lock);

     kfree (property);

     return 0;
}

void
fusion_property_cede_all (FusionDev *dev, int fusion_id)
{
     FusionLink *l;

     spin_lock (&dev->property.lock);

     fusion_list_foreach (l, dev->property.list) {
          FusionProperty *property = (FusionProperty *) l;

          spin_lock (&property->lock);

          if (property->fusion_id == fusion_id) {
               property->state     = FUSION_PROPERTY_AVAILABLE;
               property->fusion_id = 0;

               wake_up_interruptible_all (&property->wait);
          }

          spin_unlock (&property->lock);
     }

     spin_unlock (&dev->property.lock);
}

/******************************************************************************/

static FusionProperty *
lookup_property (FusionDev *dev, int id)
{
     FusionLink *l;

     spin_lock (&dev->property.lock);

     fusion_list_foreach (l, dev->property.list) {
          FusionProperty *property = (FusionProperty *) l;

          if (property->id == id)
               return property;
     }

     spin_unlock (&dev->property.lock);

     return NULL;
}

static FusionProperty *
lock_property (FusionDev *dev, int id)
{
     FusionProperty *property = lookup_property (dev, id);

     if (property) {
          fusion_list_move_to_front (&dev->property.list, &property->link);

          spin_lock (&property->lock);
          spin_unlock (&dev->property.lock);
     }

     return property;
}

static void
unlock_property (FusionProperty *property)
{
     spin_unlock (&property->lock);
}
