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

#include <linux/fusion.h>

#include "list.h"
#include "fusiondev.h"
#include "fusionee.h"
#include "ref.h"
#include "skirmish.h"


typedef struct {
  FusionLink link;

  spinlock_t lock;

  int        id;
  int        pid;
} Fusionee;


/******************************************************************************/

static Fusionee *lookup_fusionee (int id);

static Fusionee *lock_fusionee   (int id);
static void      unlock_fusionee (Fusionee *fusionee);

/******************************************************************************/

static int         ids            = 1;
static FusionLink *fusionees      = NULL;
static spinlock_t  fusionees_lock = SPIN_LOCK_UNLOCKED;

/******************************************************************************/

static int
fusionees_read_proc(char *buf, char **start, off_t offset,
                    int len, int *eof, void *private)
{
  FusionLink *l;
  int written = 0;

  spin_lock (&fusionees_lock);

  fusion_list_foreach (l, fusionees)
    {
      Fusionee *fusionee = (Fusionee*) l;

      written += sprintf(buf+written, "(%5d) 0x%08x\n",
                         fusionee->pid, fusionee->id);
      if (written < offset)
        {
          offset -= written;
          written = 0;
        }

      if (written >= len)
        break;
    }

  spin_unlock (&fusionees_lock);

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
fusionee_init()
{
  create_proc_read_entry("fusionees", 0, proc_fusion_dir,
                         fusionees_read_proc, NULL);

  return 0;
}

void
fusionee_cleanup()
{
  FusionLink *l = fusionees;

  while (l)
    {
      FusionLink *next     = l->next;
      Fusionee   *fusionee = (Fusionee *) l;

      kfree (fusionee);

      l = next;
    }

  fusionees = NULL;

  remove_proc_entry ("fusionees", proc_fusion_dir);
}

/******************************************************************************/

int
fusionee_new (int *id)
{
  Fusionee *fusionee;

  fusionee = kmalloc (sizeof(Fusionee), GFP_KERNEL);
  if (!fusionee)
    return -ENOMEM;

  memset (fusionee, 0, sizeof(Fusionee));

  spin_lock (&fusionees_lock);

  fusionee->id   = ids++;
  fusionee->pid  = current->pid;
  fusionee->lock = SPIN_LOCK_UNLOCKED;

  fusion_list_prepend (&fusionees, &fusionee->link);

  spin_unlock (&fusionees_lock);

  *id = fusionee->id;

  return 0;
}

int
fusionee_destroy (int id)
{
  Fusionee *fusionee = lookup_fusionee (id);

  if (!fusionee)
    return -EINVAL;

  spin_lock (&fusionee->lock);

  fusion_list_remove (&fusionees, &fusionee->link);

  fusion_skirmish_dismiss_all (id);
  fusion_ref_clear_all_local (id);

  spin_unlock (&fusionees_lock);

  kfree (fusionee);

  return 0;
}

/******************************************************************************/

static Fusionee *
lookup_fusionee (int id)
{
  FusionLink *l;

  spin_lock (&fusionees_lock);

  fusion_list_foreach (l, fusionees)
    {
      Fusionee *fusionee = (Fusionee *) l;

      if (fusionee->id == id)
        return fusionee;
    }

  spin_unlock (&fusionees_lock);

  return NULL;
}

static Fusionee *
lock_fusionee (int id)
{
  Fusionee *fusionee = lookup_fusionee (id);

  if (fusionee)
    {
      spin_lock (&fusionee->lock);
      spin_unlock (&fusionees_lock);
    }

  return fusionee;
}

static void
unlock_fusionee (Fusionee *fusionee)
{
  spin_unlock (&fusionee->lock);
}
