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

  wait_queue_head_t   wait;
} FusionProperty;

/******************************************************************************/

static FusionProperty *lookup_property     (int id);

static FusionProperty *lock_property       (int id);
static void            unlock_property     (FusionProperty *property);

/******************************************************************************/

static int         ids             = 0;
static FusionLink *properties      = NULL;
static spinlock_t  properties_lock = SPIN_LOCK_UNLOCKED;

/******************************************************************************/

static int
fusion_property_read_proc(char *buf, char **start, off_t offset,
                     int len, int *eof, void *private)
{
  FusionLink *l;
  int written = 0;

  spin_lock (&properties_lock);

  fusion_list_foreach (l, properties)
    {
      FusionProperty *property = (FusionProperty*) l;

      written += sprintf(buf+written, "(%5d) 0x%08x %s\n",
                         property->pid, property->id,
                         property->state ?
                           (property->state == FUSION_PROPERTY_LEASED ?
                              "leased" : "purchased") :
                           "");
      if (written < offset)
        {
          offset -= written;
          written = 0;
        }

      if (written >= len)
        break;
    }

  spin_unlock (&properties_lock);

  *start = buf + offset;
  written -= offset;
  if(written > len)
    {
      *eof = 0;
      return len;
    }

  *eof = 1;
  return (written<0) ? 0 : written;
}

int
fusion_property_init()
{
  create_proc_read_entry("properties", 0, proc_fusion_dir,
                         fusion_property_read_proc, NULL);

  return 0;
}

void
fusion_property_reset()
{
  FusionLink *l;

  spin_lock (&properties_lock);

  l = properties;
  while (l)
    {
      FusionLink     *next     = l->next;
      FusionProperty *property = (FusionProperty *) l;

      kfree (property);

      l = next;
    }

  properties = NULL;

  spin_unlock (&properties_lock);
}

void
fusion_property_cleanup()
{
  fusion_property_reset();

  remove_proc_entry ("properties", proc_fusion_dir);
}

/******************************************************************************/

int
fusion_property_new (int *id)
{
  FusionProperty *property;

  property = kmalloc (sizeof(FusionProperty), GFP_KERNEL);
  if (!property)
    return -ENOMEM;

  memset (property, 0, sizeof(FusionProperty));

  spin_lock (&properties_lock);

  property->id   = ids++;
  property->pid  = current->pid;
  property->lock = SPIN_LOCK_UNLOCKED;

  init_waitqueue_head (&property->wait);

  fusion_list_prepend (&properties, &property->link);

  spin_unlock (&properties_lock);

  *id = property->id;

  return 0;
}

int
fusion_property_lease (int id, int fusion_id)
{
  FusionProperty *property;

  while (true)
    {
      property = lock_property (id);
      if (!property)
        return -EINVAL;

      switch (property->state)
        {
        case FUSION_PROPERTY_AVAILABLE:
          property->state     = FUSION_PROPERTY_LEASED;
          property->fusion_id = fusion_id;

          unlock_property (property);
          return 0;

        case FUSION_PROPERTY_LEASED:
          unlock_property (property);

          interruptible_sleep_on (&property->wait);

          if (signal_pending(current))
            return -ERESTARTSYS;

          break;

        case FUSION_PROPERTY_PURCHASED:
          unlock_property (property);
          return -EAGAIN;
        }
    }

  /* won't reach this */
  return 0;
}

int
fusion_property_purchase (int id, int fusion_id)
{
  FusionProperty *property;

  while (true)
    {
      property = lock_property (id);
      if (!property)
        return -EINVAL;

      switch (property->state)
        {
        case FUSION_PROPERTY_AVAILABLE:
          property->state     = FUSION_PROPERTY_PURCHASED;
          property->fusion_id = fusion_id;

          wake_up_interruptible_all (&property->wait);

          unlock_property (property);
          return 0;

        case FUSION_PROPERTY_LEASED:
          unlock_property (property);

          interruptible_sleep_on (&property->wait);

          if (signal_pending(current))
            return -ERESTARTSYS;

          break;

        case FUSION_PROPERTY_PURCHASED:
          unlock_property (property);
          return -EAGAIN;
        }
    }

  /* won't reach this */
  return 0;
}

int
fusion_property_cede (int id, int fusion_id)
{
  FusionProperty *property = lock_property (id);

  if (!property)
    return -EINVAL;

  if (property->fusion_id != fusion_id)
    {
      unlock_property (property);
      return -EIO;
    }

  property->state     = FUSION_PROPERTY_AVAILABLE;
  property->fusion_id = 0;

  wake_up_interruptible_all (&property->wait);

  unlock_property (property);

  return 0;
}

int
fusion_property_holdup (int id, int fusion_id)
{
  FusionProperty *property = lock_property (id);

  if (!property)
    return -EINVAL;

  if (property->state == FUSION_PROPERTY_PURCHASED)
    {
      if (property->fusion_id == fusion_id)
        {
          unlock_property (property);
          return -EIO;
        }

      fusionee_kill (property->fusion_id);
    }

  unlock_property (property);

  return 0;
}

int
fusion_property_destroy (int id)
{
  FusionProperty *property = lookup_property (id);

  if (!property)
    return -EINVAL;

  spin_lock (&property->lock);

  fusion_list_remove (&properties, &property->link);

  wake_up_interruptible_all (&property->wait);

  spin_unlock (&properties_lock);

  kfree (property);

  return 0;
}

void
fusion_property_cede_all (int fusion_id)
{
  FusionLink *l;

  spin_lock (&properties_lock);

  fusion_list_foreach (l, properties)
    {
      FusionProperty *property = (FusionProperty *) l;

      spin_lock (&property->lock);

      if (property->fusion_id == fusion_id)
        {
          property->state     = FUSION_PROPERTY_AVAILABLE;
          property->fusion_id = 0;

          wake_up_interruptible_all (&property->wait);
        }

      spin_unlock (&property->lock);
    }

  spin_unlock (&properties_lock);
}

/******************************************************************************/

static FusionProperty *
lookup_property (int id)
{
  FusionLink *l;

  spin_lock (&properties_lock);

  fusion_list_foreach (l, properties)
    {
      FusionProperty *property = (FusionProperty *) l;

      if (property->id == id)
        return property;
    }

  spin_unlock (&properties_lock);

  return NULL;
}

static FusionProperty *
lock_property (int id)
{
  FusionProperty *property = lookup_property (id);

  if (property)
    {
      spin_lock (&property->lock);
      spin_unlock (&properties_lock);
    }

  return property;
}

static void
unlock_property (FusionProperty *property)
{
  spin_unlock (&property->lock);
}
