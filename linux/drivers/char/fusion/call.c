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
#include "call.h"

typedef struct {
     FusionLink        link;

     int               caller;

     int               ret_val;

     bool              executed;

     wait_queue_head_t wait;
} FusionCallExecution;

typedef struct {
     FusionLink         link;

     struct semaphore   lock;

     int                id;        /* call id */

     int                pid;       /* owner pid */
     int                fusion_id; /* owner fusion id */

     void              *handler;
     void              *ctx;

     FusionLink          *executions;      /* prepending! */
     FusionLink          *last;            /* points to the last item of executions */

     int                count;    /* number of calls ever made */
} FusionCall;

/******************************************************************************/

static int  lookup_call (FusionDev *dev, int id, FusionCall **ret_call);
static int  lock_call   (FusionDev *dev, int id, FusionCall **ret_call);
static void unlock_call (FusionCall *call);

static FusionCallExecution *add_execution       (FusionCall          *call,
                                                 int                  fusion_id,
                                                 FusionCallExecute   *execute);
static void                 remove_execution    (FusionCall          *call,
                                                 FusionCallExecution *execution);
static void                 free_all_executions (FusionCall          *call);

/******************************************************************************/

static int
fusion_call_read_proc (char *buf, char **start, off_t offset,
                       int len, int *eof, void *private)
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     if (down_interruptible (&dev->call.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->call.list) {
          bool        idle = true;
          FusionCall *call = (FusionCall*) l;

          if (call->executions)
               idle = ((FusionCallExecution*) call->executions)->executed;

          written += sprintf(buf+written,
                             "(%5d) 0x%08x (%d calls) %s\n",
                             call->pid, call->id, call->count,
                             idle ? "idle" : "executing");

          if (written < offset) {
               offset -= written;
               written = 0;
          }

          if (written >= len)
               break;
     }

     up (&dev->call.lock);

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
fusion_call_init (FusionDev *dev)
{
     create_proc_read_entry("calls", 0, dev->proc_dir,
                            fusion_call_read_proc, dev);

     init_MUTEX(&dev->call.lock);

     return 0;
}

void
fusion_call_deinit (FusionDev *dev)
{
     FusionLink *l;

     down (&dev->call.lock);

     remove_proc_entry ("calls", dev->proc_dir);

     l = dev->call.list;
     while (l) {
          FusionLink *next = l->next;
          FusionCall *call = (FusionCall *) l;

          free_all_executions (call);

          kfree (call);

          l = next;
     }

     up (&dev->call.lock);
}

/******************************************************************************/

int
fusion_call_new (FusionDev *dev, int fusion_id, FusionCallNew *call_new)
{
     FusionCall *call;

     call = kmalloc (sizeof(FusionCall), GFP_KERNEL);
     if (!call)
          return -ENOMEM;

     memset (call, 0, sizeof(FusionCall));

     if (down_interruptible (&dev->call.lock)) {
          kfree (call);
          return -EINTR;
     }

     call->id        = dev->call.ids++;
     call->pid       = current->pid;
     call->fusion_id = fusion_id;
     call->handler   = call_new->handler;
     call->ctx       = call_new->ctx;

     init_MUTEX (&call->lock);

     fusion_list_prepend (&dev->call.list, &call->link);

     up (&dev->call.lock);

     call_new->call_id = call->id;

     return 0;
}

int
fusion_call_execute (FusionDev *dev, int fusion_id, FusionCallExecute *execute)
{
     int                  ret;
     FusionCall          *call;
     FusionCallExecution *execution;
     FusionCallMessage    message;

     ret = lock_call (dev, execute->call_id, &call);
     if (ret)
          return ret;

     execution = add_execution (call, fusion_id, execute);
     if (!execution) {
          unlock_call (call);
          return -ENOMEM;
     }

     /* Send call message. */
     message.handler  = call->handler;
     message.ctx      = call->ctx;

     message.caller   = fusion_id;

     message.call_arg = execute->call_arg;
     message.call_ptr = execute->call_ptr;

     ret = fusionee_send_message (dev, fusion_id, call->fusion_id, FMT_CALL,
                                  call->id, sizeof(message), &message);
     if (ret) {
          remove_execution (call, execution);
          kfree (execution);
          unlock_call (call);
          return ret;
     }

     call->count++;

     if (fusion_id) {
          /* TODO: implement timeout */
          fusion_sleep_on (&execution->wait, &call->lock, 0);

          ret = lock_call (dev, execute->call_id, &call);
          if (ret)
               return ret == -EINVAL ? -EIDRM : ret;

          if (signal_pending(current)) {
               execution->caller = 0;
               unlock_call (call);
               return -EINTR;
          }

          execute->ret_val = execution->ret_val;

          remove_execution (call, execution);

          kfree (execution);
     }

     unlock_call (call);

     return 0;
}

int
fusion_call_return (FusionDev *dev, int fusion_id, FusionCallReturn *call_ret)
{
     int         ret;
     FusionLink *l;
     FusionCall *call;

     ret = lock_call (dev, call_ret->call_id, &call);
     if (ret)
          return ret;

     l = call->last;
     while (l) {
          FusionCallExecution *execution = (FusionCallExecution*) l;

          if (execution->executed) {
               l = l->prev;
               continue;
          }

          if (execution->caller) {
               execution->ret_val  = call_ret->val;
               execution->executed = true;

               wake_up_interruptible_all (&execution->wait);
          }
          else {
               remove_execution (call, execution);

               kfree (execution);
          }

          unlock_call (call);

          return 0;
     }

     unlock_call (call);

     return -EIO;
}

int
fusion_call_destroy (FusionDev *dev, int fusion_id, int call_id)
{
     int         ret;
     FusionCall *call;

     ret = lookup_call (dev, call_id, &call);
     if (ret)
          return ret;

     if (call->fusion_id != fusion_id) {
          up (&dev->call.lock);
          return -EIO;
     }

     if (down_interruptible (&call->lock)) {
          up (&dev->call.lock);
          return -EINTR;
     }

     fusion_list_remove (&dev->call.list, &call->link);

     free_all_executions (call);

     up (&dev->call.lock);

     up (&call->lock);

     kfree (call);

     return 0;
}

void
fusion_call_destroy_all (FusionDev *dev, int fusion_id)
{
     FusionLink *l;

     down (&dev->call.lock);

     l = dev->call.list;

     while (l) {
          FusionLink *next = l->next;
          FusionCall *call = (FusionCall *) l;

          down (&call->lock);

          if (call->fusion_id == fusion_id) {
               free_all_executions (call);

               fusion_list_remove (&dev->call.list, &call->link);

               up (&call->lock);

               kfree (call);
          }
          else
               up (&call->lock);

          l = next;
     }

     up (&dev->call.lock);
}

/******************************************************************************/

static int
lookup_call (FusionDev *dev, int id, FusionCall **ret_call)
{
     FusionLink *l;

     if (down_interruptible (&dev->call.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->call.list) {
          FusionCall *call = (FusionCall *) l;

          if (call->id == id) {
               *ret_call = call;
               return 0;
          }
     }

     up (&dev->call.lock);

     return -EINVAL;
}

static int
lock_call (FusionDev *dev, int id, FusionCall **ret_call)
{
     int         ret;
     FusionCall *call;

     ret = lookup_call (dev, id, &call);
     if (ret)
          return ret;

     if (call) {
          fusion_list_move_to_front (&dev->call.list, &call->link);

          if (down_interruptible (&call->lock)) {
               up (&dev->call.lock);
               return -EINTR;
          }

          up (&dev->call.lock);
     }

     *ret_call = call;

     return 0;
}

static void
unlock_call (FusionCall *call)
{
     up (&call->lock);
}

static FusionCallExecution *
add_execution (FusionCall        *call,
               int                fusion_id,
               FusionCallExecute *execute)
{
     FusionCallExecution *execution;

     /* Allocate execution. */
     execution = kmalloc (sizeof(FusionCallExecution), GFP_KERNEL);
     if (!execution)
          return NULL;

     /* Initialize execution. */
     memset (execution, 0, sizeof(FusionCallExecution));

     execution->caller = fusion_id;

     init_waitqueue_head (&execution->wait);

     /* Add execution. */
     fusion_list_prepend (&call->executions, &execution->link);

     if (!call->last)
          call->last = &execution->link;

     return execution;
}

static void
remove_execution (FusionCall          *call,
                  FusionCallExecution *execution)
{
     if (call->last == &execution->link)
          call->last = execution->link.prev;

     fusion_list_remove (&call->executions, &execution->link);
}

static void
free_all_executions (FusionCall *call)
{
     while (call->last) {
          FusionCallExecution *execution = (FusionCallExecution *) call->last;

          remove_execution (call, execution);

          wake_up_interruptible_all (&execution->wait);

          kfree (execution);
     }
}
