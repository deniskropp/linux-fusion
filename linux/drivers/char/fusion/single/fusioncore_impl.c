/*
   (c) Copyright 2002-2011  The world wide DirectFB Open Source Community (directfb.org)
   (c) Copyright 2002-2004  Convergence (integrated media) GmbH

   All rights reserved.

   Written by Denis Oliver Kropp <dok@directfb.org>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version
   2 of the License, or (at your option) any later version.
*/

#include <linux/version.h>
#include <linux/module.h>
#ifdef HAVE_LINUX_CONFIG_H
#include <linux/config.h>
#endif
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include "debug.h"

#include "fusioncore.h"



FusionCoreResult
fusion_core_enter( int          cpu_index,
                   FusionCore **ret_core )
{
     FusionCore *core;

     D_ASSERT( ret_core != NULL );

     core = kmalloc( sizeof(FusionCore), GFP_KERNEL );
     if (!core)
          return FC_FAILURE;

     core->cpu_index = cpu_index;

     spin_lock_init( &core->lock );

     *ret_core = core;

     return FC_OK;
}

void
fusion_core_exit( FusionCore *core )
{
     kfree( core );
}


void *
fusion_core_malloc( FusionCore *core,
                    size_t      size )
{
     return kmalloc( size, GFP_ATOMIC );
}

void
fusion_core_free( FusionCore *core,
                  void       *ptr )
{
     kfree( ptr );
}


void
fusion_core_lock( FusionCore *core )
{
     spin_lock( &core->lock );
}

void
fusion_core_unlock( FusionCore *core )
{
     spin_unlock( &core->lock );
}


FusionCoreResult
fusion_core_wq_init( FusionCore      *core,
                     FusionWaitQueue *queue )
{
     init_waitqueue_head( &queue->q );

     return FC_OK;
}

void
fusion_core_wq_deinit( FusionCore      *core,
                       FusionWaitQueue *queue )
{
}

void
fusion_core_wq_wait( FusionCore      *core,
                     FusionWaitQueue *queue,
                     int             *timeout_ms )
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 0)
     DEFINE_WAIT(wait);

     prepare_to_wait( &queue->q, &wait, TASK_INTERRUPTIBLE );

     fusion_core_unlock( core );

     if (timeout_ms)
          *timeout_ms = schedule_timeout(*timeout_ms);
     else
          schedule();

     fusion_core_lock( core );

     finish_wait( &queue->q, &wait );
#else
     wait_queue_t wait;

     init_waitqueue_entry(&wait, current);

     current->state = TASK_INTERRUPTIBLE;

     write_lock( &queue->q.lock);
     __add_wait_queue( &queue->q, &wait);
     write_unlock( &queue->q.lock );

     fusion_core_unlock( core );

     if (timeout_ms)
          *timeout_ms = schedule_timeout(*timeout_ms);
     else
          schedule();

     fusion_core_lock( core );

     write_lock( &queue->q.lock );
     __remove_wait_queue( &queue->q, &wait );
     write_unlock( &queue->q.lock );
#endif
}

void
fusion_core_wq_wake( FusionCore      *core,
                     FusionWaitQueue *queue )
{
     wake_up_all( &queue->q );
}

