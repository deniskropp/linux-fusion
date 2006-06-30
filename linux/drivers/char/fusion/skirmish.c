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


#define MAX_PRE_ACQUISITIONS  32


typedef struct __FUSION_FusionSkirmish FusionSkirmish;

struct __FUSION_FusionSkirmish {
     FusionEntry entry;

     int         lock_fid;  /* non-zero if locked */
     int         lock_pid;
     int         lock_count;

     int         lock_total;

#ifdef FUSION_DEBUG_SKIRMISH_DEADLOCK
     int         pre_acquis[MAX_PRE_ACQUISITIONS];

     bool        outer;
#endif
};

static int
fusion_skirmish_print( FusionEntry *entry,
                       void        *ctx,
                       char        *buf )
{
     int             written  = 0;
     FusionSkirmish *skirmish = (FusionSkirmish*) entry;

#ifdef FUSION_DEBUG_SKIRMISH_DEADLOCK
     int             i, n;


     for (i=0, n=0; i<MAX_PRE_ACQUISITIONS; i++) {
          if (skirmish->pre_acquis[i]) {
               n++;
          }
     }

     written += sprintf( buf + written, "[%2d]%s", n, skirmish->outer ? "." : " " );

     for (i=0, n=0; i<MAX_PRE_ACQUISITIONS; i++) {
          if (skirmish->pre_acquis[i]) {
               written += sprintf( buf + written, "%s%02x", n ? "," : "", skirmish->pre_acquis[i] - 1 );

               n++;
          }
     }
#endif

     if (skirmish->lock_fid) {
          if (skirmish->entry.waiters)
               return sprintf( buf + written, " - %dx [0x%08x] (%d)  %d WAITING\n",
                               skirmish->lock_count, skirmish->lock_fid,
                               skirmish->lock_pid, skirmish->entry.waiters ) + written;
          else
               return sprintf( buf + written, " - %dx [0x%08x] (%d)\n",
                               skirmish->lock_count, skirmish->lock_fid,
                               skirmish->lock_pid ) + written;
     }

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
     return fusion_entry_create( &dev->skirmish, ret_id, NULL );
}

int
fusion_skirmish_prevail (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     FusionSkirmish *skirmish;
#ifdef FUSION_DEBUG_SKIRMISH_DEADLOCK
     FusionSkirmish *s;
     int             i;
     bool            outer = true;
#endif

     dev->stat.skirmish_prevail_swoop++;

     ret = fusion_skirmish_lock( &dev->skirmish, id, true, &skirmish );
     if (ret)
          return ret;

     if (skirmish->lock_pid == current->pid) {
          skirmish->lock_count++;
          skirmish->lock_total++;
          fusion_skirmish_unlock( skirmish );
          up( &dev->skirmish.lock );
          return 0;
     }

#ifdef FUSION_DEBUG_SKIRMISH_DEADLOCK
     /* look in currently acquired skirmishs for this one being
        a pre-acquisition, indicating a potential deadlock */
     fusion_list_foreach (s, dev->skirmish.list) {
          if (s->lock_pid != current->pid)
               continue;

          outer = false;

          for (i=0; i<MAX_PRE_ACQUISITIONS; i++) {
               if (s->pre_acquis[i] == id + 1) {
                    printk( KERN_DEBUG "FusionSkirmish: Potential deadlock "
                            "between locked 0x%x and to be locked 0x%x in world %d!\n",
                            s->entry.id, skirmish->entry.id, dev->index );
               }
          }
     }

     if (outer)
          skirmish->outer = true;

     /* remember all previously acquired skirmishs being pre-acquisitions for
        this one, to detect potential deadlocks due to a lock order twist */
     fusion_list_foreach (s, dev->skirmish.list) {
          int free = -1;

          if (s->lock_pid != current->pid)
               continue;

          for (i=0; i<MAX_PRE_ACQUISITIONS; i++) {
               if (skirmish->pre_acquis[i]) {
                    if (skirmish->pre_acquis[i] == s->entry.id + 1) {
                         break;
                    }
               }
               else
                    free = i;
          }

          /* not found? */
          if (i == MAX_PRE_ACQUISITIONS) {
               if (free != -1) {
                    skirmish->pre_acquis[free] = s->entry.id + 1;
               }
               else {
                    printk( KERN_DEBUG "FusionSkirmish: Too many pre-acquisitions to remember.\n" );

                    printk( KERN_DEBUG " [ '%s' ] <- ", skirmish->entry.name );

                    for (i=0; i<MAX_PRE_ACQUISITIONS; i++)
                         printk( "0x%03x ", skirmish->pre_acquis[i] - 1 );

                    printk( "\n" );
               }
          }
     }
#endif

     up( &dev->skirmish.lock );

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

     ret = fusion_skirmish_lock( &dev->skirmish, id, false, &skirmish );
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
fusion_skirmish_lock_count (FusionDev *dev, int id, int fusion_id, int *ret_lock_count)
{
     int             ret;
     FusionSkirmish *skirmish;

     ret = fusion_skirmish_lock( &dev->skirmish, id, false, &skirmish );
     if (ret)
          return ret;

     if (skirmish->lock_fid == fusion_id &&
         skirmish->lock_pid == current->pid)
     {
          *ret_lock_count = skirmish->lock_count;
     }
     else
     {
          *ret_lock_count = 0; 
     }

     fusion_skirmish_unlock( skirmish );

     return 0;
}

int
fusion_skirmish_dismiss (FusionDev *dev, int id, int fusion_id)
{
     int             ret;
     FusionSkirmish *skirmish;

     ret = fusion_skirmish_lock( &dev->skirmish, id, false, &skirmish );
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
#ifdef FUSION_DEBUG_SKIRMISH_DEADLOCK
     int             i;
     FusionSkirmish *s;

     /* Lock entries. */
     if (down_interruptible( &dev->skirmish.lock ))
          return -EINTR;

     /* remove from all pre-acquisition lists */
     fusion_list_foreach (s, dev->skirmish.list) {
          for (i=0; i<MAX_PRE_ACQUISITIONS; i++) {
               if (s->pre_acquis[i] == id + 1)
                    s->pre_acquis[i] = 0;
          }
     }

     up( &dev->skirmish.lock );

     /* FIXME: gap? */
#endif

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

