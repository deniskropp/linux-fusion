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
#include "list.h"
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

  bool               locked;

  FusionLink        *local_refs;

  wait_queue_head_t  wait;
} FusionRef;

/******************************************************************************/

static FusionRef *lookup_ref     (int id);

static FusionRef *lock_ref       (int id);
static void       unlock_ref     (FusionRef *ref);

static int        add_local      (FusionRef *ref, int fusion_id, int add);
static void       clear_local    (FusionRef *ref, int fusion_id);
static void       free_all_local (FusionRef *ref);

/******************************************************************************/

static int         ids       = 0;
static FusionLink *refs      = NULL;
static spinlock_t  refs_lock = SPIN_LOCK_UNLOCKED;

/******************************************************************************/

static int
fusion_ref_read_proc(char *buf, char **start, off_t offset,
                     int len, int *eof, void *private)
{
  FusionLink *l;
  int written = 0;

  spin_lock (&refs_lock);

  fusion_list_foreach (l, refs)
    {
      FusionRef *ref = (FusionRef*) l;

      written += sprintf(buf+written, "(%5d) 0x%08x %2d %2d %s\n",
                         ref->pid, ref->id, ref->global, ref->local,
                         ref->locked ? "(locked)" : "");
      if (written < offset)
        {
          offset -= written;
          written = 0;
        }

      if (written >= len)
        break;
    }

  spin_unlock (&refs_lock);

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
fusion_ref_init()
{
  create_proc_read_entry("refs", 0, proc_fusion_dir,
                         fusion_ref_read_proc, NULL);

  return 0;
}

void
fusion_ref_reset()
{
  FusionLink *l;

  spin_lock (&refs_lock);

  l = refs;
  while (l)
    {
      FusionLink *next = l->next;
      FusionRef  *ref  = (FusionRef *) l;

      free_all_local (ref);

      kfree (ref);

      l = next;
    }

  refs = NULL;

  spin_unlock (&refs_lock);
}

void
fusion_ref_cleanup()
{
  fusion_ref_reset();

  remove_proc_entry ("refs", proc_fusion_dir);
}

/******************************************************************************/

int
fusion_ref_new (int *id)
{
  FusionRef *ref;

  ref = kmalloc (sizeof(FusionRef), GFP_KERNEL);
  if (!ref)
    return -ENOMEM;

  memset (ref, 0, sizeof(FusionRef));

  spin_lock (&refs_lock);

  ref->id   = ids++;
  ref->pid  = current->pid;
  ref->lock = SPIN_LOCK_UNLOCKED;

  init_waitqueue_head (&ref->wait);

  fusion_list_prepend (&refs, &ref->link);

  spin_unlock (&refs_lock);

  *id = ref->id;

  return 0;
}

int
fusion_ref_up (int id, int fusion_id)
{
  FusionRef *ref = lock_ref (id);

  if (!ref)
    return -EINVAL;

  if (ref->locked)
    {
      unlock_ref (ref);
      return -EAGAIN;
    }

  if (fusion_id)
    {
      int ret;

      ret = add_local (ref, fusion_id, 1);
      if (ret)
        {
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
fusion_ref_down (int id, int fusion_id)
{
  FusionRef *ref = lock_ref (id);

  if (!ref)
    return -EINVAL;

  if (ref->locked)
    {
      unlock_ref (ref);
      return -EAGAIN;
    }

  if (fusion_id)
    {
      int ret;

      if (!ref->local)
        return -EIO;

      ret = add_local (ref, fusion_id, -1);
      if (ret)
        {
          unlock_ref (ref);
          return ret;
        }

      ref->local--;
    }
  else
    {
      if (!ref->global)
        return -EIO;

      ref->global--;
    }

  if (ref->local + ref->global == 0)
    wake_up_interruptible_all (&ref->wait);

  unlock_ref (ref);

  return 0;
}

int
fusion_ref_zero_lock (int id)
{
  FusionRef *ref;

  while (true)
    {
      ref = lock_ref (id);
      if (!ref)
        return -EINVAL;

      if (ref->locked)
        {
          unlock_ref (ref);
          return -EAGAIN;
        }

      if (ref->global || ref->local)
        {
          unlock_ref (ref);

          interruptible_sleep_on (&ref->wait);

          if (signal_pending(current))
            return -ERESTARTSYS;
        }
      else
        break;
    }

  ref->locked = true;

  unlock_ref (ref);

  return 0;
}

int
fusion_ref_zero_trylock (int id)
{
  int        ret = 0;
  FusionRef *ref = lock_ref (id);

  if (!ref)
    return -EINVAL;

  if (ref->locked)
    {
      unlock_ref (ref);
      return -EAGAIN;
    }

  if (ref->global || ref->local)
    ret = -ETOOMANYREFS;
  else
    ref->locked = true;

  unlock_ref (ref);

  return ret;
}

int
fusion_ref_unlock (int id)
{
  FusionRef *ref = lock_ref (id);

  if (!ref)
    return -EINVAL;

  ref->locked = false;

  unlock_ref (ref);

  return 0;
}

int
fusion_ref_stat (int id, int *refs)
{
  FusionRef *ref = lock_ref (id);

  if (!ref)
    return -EINVAL;

  *refs = ref->global + ref->local;

  unlock_ref (ref);

  return 0;
}

int
fusion_ref_destroy (int id)
{
  FusionRef *ref = lookup_ref (id);

  if (!ref)
    return -EINVAL;

  spin_lock (&ref->lock);

  fusion_list_remove (&refs, &ref->link);

  wake_up_interruptible_all (&ref->wait);

  spin_unlock (&refs_lock);

  free_all_local (ref);

  kfree (ref);

  return 0;
}

void
fusion_ref_clear_all_local (int fusion_id)
{
  FusionLink *l;

  spin_lock (&refs_lock);

  fusion_list_foreach (l, refs)
    {
      FusionRef *ref = (FusionRef *) l;

      clear_local (ref, fusion_id);
    }

  spin_unlock (&refs_lock);
}

/******************************************************************************/

static FusionRef *
lookup_ref (int id)
{
  FusionLink *l;

  spin_lock (&refs_lock);

  fusion_list_foreach (l, refs)
    {
      FusionRef *ref = (FusionRef *) l;

      if (ref->id == id)
        return ref;
    }

  spin_unlock (&refs_lock);

  return NULL;
}

static FusionRef *
lock_ref (int id)
{
  FusionRef *ref = lookup_ref (id);

  if (ref)
    {
      spin_lock (&ref->lock);
      spin_unlock (&refs_lock);
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

  fusion_list_foreach (l, ref->local_refs)
    {
      local = (LocalRef *) l;

      if (local->fusion_id == fusion_id)
        {
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
clear_local (FusionRef *ref, int fusion_id)
{
  FusionLink *l;

  spin_lock (&ref->lock);

  fusion_list_foreach (l, ref->local_refs)
    {
      LocalRef *local = (LocalRef *) l;

      if (local->fusion_id == fusion_id)
        {
          ref->local -= local->refs;

          if (ref->local + ref->global == 0)
            wake_up_interruptible_all (&ref->wait);

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

  while (l)
    {
      FusionLink *next = l->next;

      kfree (l);

      l = next;
    }

  ref->local_refs = NULL;
}
