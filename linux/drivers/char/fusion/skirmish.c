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
     FusionEntry        entry;

     int                lock_fid;  /* non-zero if locked */
     int                lock_pid;
     int                lock_count;

     int                lock_total;
} FusionSkirmish;

static int
fusion_skirmish_print( FusionEntry *entry,
                       void        *ctx,
                       char        *buf )
{
     int             written;
     FusionSkirmish *skirmish = (FusionSkirmish*) entry;

     written = sprintf( buf, "%6dx total", skirmish->lock_total );

     if (skirmish->lock_fid)
          return sprintf( buf + written, ", now %dx by 0x%08x (%d)\n",
                          skirmish->lock_count, skirmish->lock_fid, skirmish->lock_pid) + written;

     return sprintf( buf + written, "\n" ) + written;
}

FUSION_ENTRY_CLASS( FusionSkirmish, skirmish, NULL, NULL, fusion_skirmish_print )

/******************************************************************************/

int
fusion_skirmish_init (FusionDev *dev)
{
     fusion_entries_init( &dev->skirmish, &skirmish_class, dev );

     create_proc_read_entry( "skirmishs", 0, dev->proc_dir,
                             fusion_entries_read_proc, &dev->skirmish );

     return 0;
}

void
fusion_skirmish_deinit (FusionDev *dev)
{
     remove_proc_entry ("skirmishs", dev->proc_dir);

     fusion_entries_deinit( &dev->skirmish );
}

/******************************************************************************/

int
fusion_skirmish_new (FusionDev *dev, int *ret_id)
{
     return fusion_entry_create( &dev->skirmish, ret_id );
}

int
fusion_skirmish_prevail (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     FusionSkirmish *skirmish;

     dev->stat.skirmish_prevail_swoop++;

     ret = fusion_skirmish_lock( &dev->skirmish, id, &skirmish );
     if (ret)
          return ret;

     if (skirmish->lock_pid == current->pid) {
          skirmish->lock_count++;
          skirmish->lock_total++;
          fusion_skirmish_unlock( skirmish );
          return 0;
     }

     while (skirmish->lock_pid) {
          ret = fusion_skirmish_wait( skirmish, NULL );
          if (ret)
               return ret;
     }

     skirmish->lock_fid   = fusion_id;
     skirmish->lock_pid   = current->pid;
     skirmish->lock_count = 1;

     skirmish->lock_total++;

     fusion_skirmish_unlock( skirmish );

     return 0;
}

int
fusion_skirmish_swoop (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     FusionSkirmish *skirmish;

     ret = fusion_skirmish_lock( &dev->skirmish, id, &skirmish );
     if (ret)
          return ret;

     dev->stat.skirmish_prevail_swoop++;

     if (skirmish->lock_fid) {
          if (skirmish->lock_pid == current->pid) {
               skirmish->lock_count++;
               skirmish->lock_total++;
               fusion_skirmish_unlock( skirmish );
               return 0;
          }

          fusion_skirmish_unlock( skirmish );

          return -EAGAIN;
     }

     skirmish->lock_fid   = fusion_id;
     skirmish->lock_pid   = current->pid;
     skirmish->lock_count = 1;

     skirmish->lock_total++;

     fusion_skirmish_unlock( skirmish );

     return 0;
}

int
fusion_skirmish_dismiss (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     FusionSkirmish *skirmish;

     ret = fusion_skirmish_lock( &dev->skirmish, id, &skirmish );
     if (ret)
          return ret;

     dev->stat.skirmish_dismiss++;

     if (skirmish->lock_pid != current->pid) {
          fusion_skirmish_unlock( skirmish );
          return -EIO;
     }

     if (--skirmish->lock_count == 0) {
          skirmish->lock_fid = 0;
          skirmish->lock_pid = 0;

          fusion_skirmish_notify( skirmish, true );
     }

     fusion_skirmish_unlock( skirmish );

     return 0;
}

int
fusion_skirmish_destroy (FusionDev *dev, int id)
{
     return fusion_entry_destroy( &dev->skirmish, id );
}

void
fusion_skirmish_dismiss_all (FusionDev *dev, int fusion_id)
{
     FusionLink *l;

     down (&dev->skirmish.lock);

     fusion_list_foreach (l, dev->skirmish.list) {
          FusionSkirmish *skirmish = (FusionSkirmish *) l;

          down (&skirmish->entry.lock);

          if (skirmish->lock_fid == fusion_id) {
               skirmish->lock_fid   = 0;
               skirmish->lock_pid   = 0;
               skirmish->lock_count = 0;

               wake_up_interruptible_all (&skirmish->entry.wait);
          }

          up (&skirmish->entry.lock);
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

          down (&skirmish->entry.lock);

          if (skirmish->lock_pid == pid) {
               skirmish->lock_fid   = 0;
               skirmish->lock_pid   = 0;
               skirmish->lock_count = 0;

               wake_up_interruptible_all (&skirmish->entry.wait);
          }

          up (&skirmish->entry.lock);
     }

     up (&dev->skirmish.lock);
}

