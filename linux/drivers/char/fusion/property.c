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

#include "entries.h"
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
     FusionEntry         entry;

     FusionPropertyState state;
     int                 fusion_id; /* non-zero if leased/purchased */
     unsigned long       purchase_stamp;
     int                 lock_pid;
     int                 count;    /* lock counter */
} FusionProperty;

FUSION_ENTRY_CLASS( FusionProperty, property, NULL, NULL )

/******************************************************************************/

static int
properties_read_proc( char *buf, char **start, off_t offset,
                      int len, int *eof, void *private )
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     if (down_interruptible( &dev->properties.lock ))
          return -EINTR;

     fusion_list_foreach (l, dev->properties.list) {
          FusionProperty *property = (FusionProperty*) l;

          if (property->state != FUSION_PROPERTY_AVAILABLE) {
               written += sprintf(buf+written, "(%5d) 0x%08x %s (0x%08x %d)\n",
                                  property->entry.pid, property->entry.id,
                                  property->state == FUSION_PROPERTY_LEASED ?
                                  "leased" : "purchased", property->fusion_id,
                                  property->lock_pid);
          }
          else {
               written += sprintf(buf+written, "(%5d) 0x%08x\n",
                                  property->entry.pid, property->entry.id);
          }

          if (written < offset) {
               offset -= written;
               written = 0;
          }

          if (written >= len)
               break;
     }

     up( &dev->properties.lock );

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
fusion_property_init( FusionDev *dev )
{
     fusion_entries_init( &dev->properties, &property_class, dev );

     create_proc_read_entry( "properties", 0, dev->proc_dir, properties_read_proc, dev );

     return 0;
}

void
fusion_property_deinit( FusionDev *dev )
{
     remove_proc_entry( "properties", dev->proc_dir );

     fusion_entries_deinit( &dev->properties );
}

/******************************************************************************/

int
fusion_property_new( FusionDev *dev, int *ret_id )
{
     return fusion_entry_create( &dev->properties, ret_id );
}

int
fusion_property_lease( FusionDev *dev, int id, int fusion_id )
{
     int             ret;
     FusionProperty *property;
     long            timeout = -1;

     dev->stat.property_lease_purchase++;

     ret = fusion_property_lock( &dev->properties, id, &property );
     if (ret)
          return ret;

     while (true) {
          switch (property->state) {
               case FUSION_PROPERTY_AVAILABLE:
                    property->state     = FUSION_PROPERTY_LEASED;
                    property->fusion_id = fusion_id;
                    property->lock_pid  = current->pid;
                    property->count     = 1;

                    fusion_property_unlock( property );
                    return 0;

               case FUSION_PROPERTY_LEASED:
                    if (property->lock_pid == current->pid) {
                         property->count++;

                         fusion_property_unlock( property );
                         return 0;
                    }

                    ret = fusion_property_wait( property, NULL );
                    if (ret)
                         return ret;

                    break;

               case FUSION_PROPERTY_PURCHASED:
                    if (property->lock_pid == current->pid) {
                         fusion_property_unlock( property );
                         return -EIO;
                    }

                    if (timeout == -1) {
                         if (jiffies - property->purchase_stamp > HZ / 10) {
                              fusion_property_unlock( property );
                              return -EAGAIN;
                         }

                         timeout = HZ / 10;
                    }

                    ret = fusion_property_wait( property, &timeout );
                    if (ret)
                         return ret;

                    break;

               default:
                    BUG();
          }
     }

     BUG();

     /* won't reach this */
     return -1;
}

int
fusion_property_purchase( FusionDev *dev, int id, int fusion_id )
{
     int             ret;
     FusionProperty *property;
     signed long     timeout = -1;

     dev->stat.property_lease_purchase++;

     ret = fusion_property_lock( &dev->properties, id, &property );
     if (ret)
          return ret;

     while (true) {
          switch (property->state) {
               case FUSION_PROPERTY_AVAILABLE:
                    property->state          = FUSION_PROPERTY_PURCHASED;
                    property->fusion_id      = fusion_id;
                    property->purchase_stamp = jiffies;
                    property->lock_pid       = current->pid;
                    property->count          = 1;

                    fusion_property_notify( property, true );

                    fusion_property_unlock( property );
                    return 0;

               case FUSION_PROPERTY_LEASED:
                    if (property->lock_pid == current->pid) {
                         fusion_property_unlock( property );
                         return -EIO;
                    }

                    ret = fusion_property_wait( property, NULL );
                    if (ret)
                         return ret;

                    break;

               case FUSION_PROPERTY_PURCHASED:
                    if (property->lock_pid == current->pid) {
                         property->count++;

                         fusion_property_unlock( property );
                         return 0;
                    }

                    if (timeout == -1) {
                         if (jiffies - property->purchase_stamp > HZ) {
                              fusion_property_unlock( property );
                              return -EAGAIN;
                         }

                         timeout = HZ;
                    }

                    ret = fusion_property_wait( property, &timeout );
                    if (ret)
                         return ret;

                    break;

               default:
                    BUG();
          }
     }

     BUG();

     /* won't reach this */
     return -1;
}

int
fusion_property_cede( FusionDev *dev, int id, int fusion_id )
{
     int             ret;
     FusionProperty *property;
     bool            purchased;

     dev->stat.property_cede++;

     ret = fusion_property_lock( &dev->properties, id, &property );
     if (ret)
          return ret;

     if (property->lock_pid != current->pid) {
          fusion_property_unlock( property );
          return -EIO;
     }

     if (--property->count) {
          fusion_property_unlock( property );
          return 0;
     }

     purchased = (property->state == FUSION_PROPERTY_PURCHASED);

     property->state     = FUSION_PROPERTY_AVAILABLE;
     property->fusion_id = 0;
     property->lock_pid  = 0;

     fusion_property_notify( property, true );

     fusion_property_unlock( property );

     if (purchased)
          yield();

     return 0;
}

int
fusion_property_holdup( FusionDev *dev, int id, int fusion_id )
{
     int             ret;
     FusionProperty *property;

     if (fusion_id > 1)
          return -EPERM;

     ret = fusion_property_lock( &dev->properties, id, &property );
     if (ret)
          return ret;

     if (property->state == FUSION_PROPERTY_PURCHASED) {
          if (property->fusion_id == fusion_id) {
               fusion_property_unlock( property );
               return -EIO;
          }

          fusionee_kill( dev, fusion_id, property->fusion_id, SIGKILL, -1 );
     }

     fusion_property_unlock( property );

     return 0;
}

int
fusion_property_destroy( FusionDev *dev, int id )
{
     return fusion_entry_destroy( &dev->properties, id );
}

void
fusion_property_cede_all( FusionDev *dev, int fusion_id )
{
     FusionLink *l;

     down( &dev->properties.lock );

     fusion_list_foreach (l, dev->properties.list) {
          FusionProperty *property = (FusionProperty *) l;

          down( &property->entry.lock );

          if (property->fusion_id == fusion_id) {
               property->state     = FUSION_PROPERTY_AVAILABLE;
               property->fusion_id = 0;
               property->lock_pid  = 0;

               wake_up_interruptible_all (&property->entry.wait);
          }

          up( &property->entry.lock );
     }

     up( &dev->properties.lock );
}

