/*
 *	Fusion Kernel Module
 *
 *	(c) Copyright 2002  convergence integrated media GmbH
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

  spinlock_t         lock;

  int                id;
  int                pid;

  int                lock_fid;  /* non-zero if locked */
  int                lock_pid;
  int                lock_count;

  wait_queue_head_t  wait;
} FusionSkirmish;

/******************************************************************************/

static FusionSkirmish *lookup_skirmish     (int id);

static FusionSkirmish *lock_skirmish       (int id);
static void            unlock_skirmish     (FusionSkirmish *skirmish);

/******************************************************************************/

static int         ids            = 0;
static FusionLink *skirmishs      = NULL;
static spinlock_t  skirmishs_lock = SPIN_LOCK_UNLOCKED;

/******************************************************************************/

static int
fusion_skirmish_read_proc(char *buf, char **start, off_t offset,
                     int len, int *eof, void *private)
{
  FusionLink *l;
  int written = 0;

  spin_lock (&skirmishs_lock);

  fusion_list_foreach (l, skirmishs)
    {
      FusionSkirmish *skirmish = (FusionSkirmish*) l;

      written += sprintf(buf+written, "(%5d) 0x%08x %s\n",
                         skirmish->pid, skirmish->id,
                         skirmish->lock_fid ? "(locked)" : "");
      if (written < offset)
        {
          offset -= written;
          written = 0;
        }

      if (written >= len)
        break;
    }

  spin_unlock (&skirmishs_lock);

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
fusion_skirmish_init()
{
  create_proc_read_entry("skirmishs", 0, proc_fusion_dir,
                         fusion_skirmish_read_proc, NULL);

  return 0;
}

void
fusion_skirmish_cleanup()
{
  FusionLink *l = skirmishs;

  while (l)
    {
      FusionLink     *next     = l->next;
      FusionSkirmish *skirmish = (FusionSkirmish *) l;

      kfree (skirmish);

      l = next;
    }

  skirmishs = NULL;

  remove_proc_entry ("skirmishs", proc_fusion_dir);
}

/******************************************************************************/

int
fusion_skirmish_new (int *id)
{
  FusionSkirmish *skirmish;

  skirmish = kmalloc (sizeof(FusionSkirmish), GFP_KERNEL);
  if (!skirmish)
    return -ENOMEM;

  memset (skirmish, 0, sizeof(FusionSkirmish));

  spin_lock (&skirmishs_lock);

  skirmish->id   = ids++;
  skirmish->pid  = current->pid;
  skirmish->lock = SPIN_LOCK_UNLOCKED;

  init_waitqueue_head (&skirmish->wait);

  fusion_list_prepend (&skirmishs, &skirmish->link);

  spin_unlock (&skirmishs_lock);

  *id = skirmish->id;

  return 0;
}

int
fusion_skirmish_prevail (int id, int fusion_id)
{
  FusionSkirmish *skirmish;

  while (true)
    {
      skirmish = lock_skirmish (id);
      if (!skirmish)
        return -EINVAL;

      if (skirmish->lock_fid)
        {
          if (skirmish->lock_pid == current->pid)
            {
              skirmish->lock_count++;
              unlock_skirmish (skirmish);
              return 0;
            }

          unlock_skirmish (skirmish);

          interruptible_sleep_on (&skirmish->wait);

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
fusion_skirmish_swoop (int id, int fusion_id)
{
  FusionSkirmish *skirmish = lock_skirmish (id);

  if (!skirmish)
    return -EINVAL;

  if (skirmish->lock_fid)
    {
      if (skirmish->lock_pid == current->pid)
        {
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
fusion_skirmish_dismiss (int id, int fusion_id)
{
  FusionSkirmish *skirmish = lock_skirmish (id);

  if (!skirmish)
    return -EINVAL;

  if (skirmish->lock_pid != current->pid)
    {
      unlock_skirmish (skirmish);
      return -EIO;
    }

  if (--skirmish->lock_count == 0)
    {
      skirmish->lock_fid = 0;
      skirmish->lock_pid = 0;

      wake_up_interruptible_all (&skirmish->wait);
    }

  unlock_skirmish (skirmish);

  return 0;
}

int
fusion_skirmish_destroy (int id)
{
  FusionSkirmish *skirmish = lookup_skirmish (id);

  if (!skirmish)
    return -EINVAL;

  spin_lock (&skirmish->lock);

  fusion_list_remove (&skirmishs, &skirmish->link);

  spin_unlock (&skirmishs_lock);

  wake_up_interruptible_all (&skirmish->wait);

  kfree (skirmish);

  return 0;
}

void
fusion_skirmish_dismiss_all (int fusion_id)
{
  FusionLink *l;

  spin_lock (&skirmishs_lock);

  fusion_list_foreach (l, skirmishs)
    {
      FusionSkirmish *skirmish = (FusionSkirmish *) l;

      spin_lock (&skirmish->lock);

      if (skirmish->lock_fid == fusion_id)
        {
          skirmish->lock_fid   = 0;
          skirmish->lock_pid   = 0;
          skirmish->lock_count = 0;

          wake_up_interruptible_all (&skirmish->wait);
        }

      spin_unlock (&skirmish->lock);
    }

  spin_unlock (&skirmishs_lock);
}

/******************************************************************************/

static FusionSkirmish *
lookup_skirmish (int id)
{
  FusionLink *l;

  spin_lock (&skirmishs_lock);

  fusion_list_foreach (l, skirmishs)
    {
      FusionSkirmish *skirmish = (FusionSkirmish *) l;

      if (skirmish->id == id)
        return skirmish;
    }

  spin_unlock (&skirmishs_lock);

  return NULL;
}

static FusionSkirmish *
lock_skirmish (int id)
{
  FusionSkirmish *skirmish = lookup_skirmish (id);

  if (skirmish)
    {
      spin_lock (&skirmish->lock);
      spin_unlock (&skirmishs_lock);
    }

  return skirmish;
}

static void
unlock_skirmish (FusionSkirmish *skirmish)
{
  spin_unlock (&skirmish->lock);
}
