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
#include "entries.h"


void
fusion_entries_init( FusionEntries    *entries,
                     FusionEntryClass *class,
                     void             *ctx )
{
     FUSION_ASSERT( entries != NULL );
     FUSION_ASSERT( class != NULL );
     FUSION_ASSERT( class->object_size >= sizeof(FusionEntry) );

     memset( entries, 0, sizeof(FusionEntries) );

     entries->class = class;
     entries->ctx   = ctx;

     init_MUTEX( &entries->lock );
}

void
fusion_entries_deinit( FusionEntries *entries )
{
     FusionLink       *tmp;
     FusionEntry      *entry;
     FusionEntryClass *class;

     FUSION_ASSERT( entries != NULL );
     FUSION_ASSERT( entries->class != NULL );

     class = entries->class;

     down( &entries->lock );

     fusion_list_foreach_safe (entry, tmp, entries->list) {
          if (class->Destroy)
               class->Destroy( entry, entries->ctx );

          kfree( entry );
     }

     up( &entries->lock );
}


int
fusion_entry_create( FusionEntries *entries,
                     int           *ret_id )
{
     int               ret;
     FusionEntry      *entry;
     FusionEntryClass *class;

     FUSION_ASSERT( entries != NULL );
     FUSION_ASSERT( entries->class != NULL );
     FUSION_ASSERT( ret_id != NULL );

     class = entries->class;

     entry = kmalloc( class->object_size, GFP_KERNEL );
     if (!entry)
          return -ENOMEM;

     memset( entry, 0, class->object_size );

     if (down_interruptible( &entries->lock )) {
          kfree( entry );
          return -EINTR;
     }

     entry->entries = entries;
     entry->id      = entries->ids++;
     entry->pid     = current->pid;

     init_MUTEX( &entry->lock );

     init_waitqueue_head( &entry->wait );

     if (class->Init) {
          ret = class->Init( entry, entries->ctx );
          if (ret) {
               kfree( entry );
               return ret;
          }
     }

     fusion_list_prepend( &entries->list, &entry->link );

     up( &entries->lock );

     *ret_id = entry->id;

     return 0;
}

int
fusion_entry_destroy( FusionEntries  *entries,
                      int             id )
{
     FusionEntry      *entry;
     FusionEntryClass *class;

     FUSION_ASSERT( entries != NULL );
     FUSION_ASSERT( entries->class != NULL );

     class = entries->class;

     /* Lock entries. */
     if (down_interruptible( &entries->lock ))
          return -EINTR;

     /* Lookup the entry. */
     fusion_list_foreach (entry, entries->list) {
          if (entry->id == id)
               break;
     }

     /* Check if no entry was found. */
     if (!entry) {
          up( &entries->lock );
          return -EINVAL;
     }

     /* Lock the entry. */
     if (down_interruptible( &entry->lock )) {
          up( &entries->lock );
          return -EINTR;
     }

     /* Remove the entry from the list. */
     fusion_list_remove( &entries->list, &entry->link );

     /* Wake up any waiting process. */
     wake_up_interruptible_all( &entry->wait );

     /* Unlock entries. */
     up( &entries->lock );


     /* Call the destroy function. */
     if (class->Destroy)
          class->Destroy( entry, entries->ctx );

     /* Deallocate the entry. */
     kfree( entry );

     return 0;
}

int
fusion_entry_lock( FusionEntries  *entries,
                   int             id,
                   FusionEntry   **ret_entry )
{
     FusionEntry *entry;

     FUSION_ASSERT( entries != NULL );
     FUSION_ASSERT( ret_entry != NULL );

     /* Lock entries. */
     if (down_interruptible( &entries->lock ))
          return -EINTR;

     /* Lookup the entry. */
     fusion_list_foreach (entry, entries->list) {
          if (entry->id == id)
               break;
     }

     /* Check if no entry was found. */
     if (!entry) {
          up( &entries->lock );
          return -EINVAL;
     }

     FUSION_ASSERT( entry->lock_pid != current->pid );

     /* Move the entry to the front of all entries. */
     fusion_list_move_to_front( &entries->list, &entry->link );

     /* Lock the entry. */
     if (down_interruptible( &entry->lock )) {
          up( &entries->lock );
          return -EINTR;
     }

     /* Mark as locked. */
     entry->lock_pid = current->pid;

     /* Unlock entries. */
     up( &entries->lock );

     /* Return the locked entry. */
     *ret_entry = entry;

     return 0;
}

void
fusion_entry_unlock( FusionEntry *entry )
{
     FUSION_ASSERT( entry != NULL );
     FUSION_ASSERT( entry->lock_pid == current->pid );

     entry->lock_pid = 0;

     /* Unlock the entry. */
     up( &entry->lock );
}

int
fusion_entry_wait( FusionEntry *entry, long *timeout )
{
     int            ret;
     int            id;
     FusionEntries *entries;
     FusionEntry   *entry2;

     FUSION_ASSERT( entry != NULL );
     FUSION_ASSERT( entry->entries != NULL );
     FUSION_ASSERT( entry->lock_pid == current->pid );

     id      = entry->id;
     entries = entry->entries;

     fusion_sleep_on( &entry->wait, &entry->lock, timeout );

     if (timeout && !*timeout)
          return -ETIMEDOUT;

     if (signal_pending(current))
          return -EINTR;

     ret = fusion_entry_lock( entries, id, &entry2 );
     switch (ret) {
          case -EINVAL:
               return -EIDRM;

          case 0:
               if (entry != entry2)
                    BUG();
     }

     return ret;
}

void
fusion_entry_notify( FusionEntry *entry, bool all )
{
     FUSION_ASSERT( entry != NULL );
     FUSION_ASSERT( entry->lock_pid == current->pid );

     if (all)
          wake_up_interruptible_all( &entry->wait );
     else
          wake_up_interruptible( &entry->wait );
}

