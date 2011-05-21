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

//#define FUSION_ENABLE_DEBUG

#ifdef HAVE_LINUX_CONFIG_H
#include <linux/config.h>
#endif
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 35)
#include <linux/smp_lock.h>
#endif
#include <linux/sched.h>

#include <linux/fusion.h>

#include "fusiondev.h"
#include "fusionee.h"
#include "list.h"
#include "skirmish.h"
#include "call.h"

typedef struct {
	FusionLink link;

	Fusionee *caller;

	int ret_val;

	bool executed;

	wait_queue_head_t wait;

	int call_id;
	unsigned int serial;
	int          caller_pid;
} FusionCallExecution;

typedef struct {
	FusionEntry entry;

	Fusionee *fusionee;		/* owner */

	void *handler;
	void *ctx;

	FusionLink *executions;

	int count;		/* number of calls ever made */

	unsigned int serial;
} FusionCall;

/* collection, required for 1-param-only passing */
struct fusion_construct_ctx {
	Fusionee *fusionee;
	FusionCallNew *call_new;
};

/******************************************************************************/

static FusionCallExecution *add_execution(FusionCall * call,
					  Fusionee * caller,
					  unsigned int serial);
static void remove_execution(FusionCall * call,
			     FusionCallExecution * execution);
static void free_all_executions(FusionCall * call);

/******************************************************************************/

static int
fusion_call_construct(FusionEntry * entry, void *ctx, void *create_ctx)
{
	FusionCall *call = (FusionCall *) entry;

	struct fusion_construct_ctx *cc =
	    (struct fusion_construct_ctx *)create_ctx;

	call->fusionee = cc->fusionee;
	call->handler = cc->call_new->handler;
	call->ctx = cc->call_new->ctx;

	cc->call_new->call_id = entry->id;

	return 0;
}

static void fusion_call_destruct(FusionEntry * entry, void *ctx)
{
	FusionCall *call = (FusionCall *) entry;

	free_all_executions(call);
}

__attribute__((unused))
static void print_call( FusionCall* call )
{
	FusionEntry *entry;
	FusionLink *e;

	entry = &call->entry;

	printk( KERN_CRIT "%-2d %s, fid:%lu, %d calls)",
		   entry->id,
		   entry->name[0] ? entry->name : "???",
		   call->fusionee->id,
		   call->count);

	fusion_list_foreach(e, call->executions) {
		FusionCallExecution *exec = (FusionCallExecution *) e;

		printk( "/%lx:%s",
			   exec->caller   ? fusionee_id(exec->caller) : -1,
			   exec->executed ? "idle" : "busy" );
	}

	printk("\n");
}

static void
fusion_call_print(FusionEntry * entry, void *ctx, struct seq_file *p)
{
	FusionLink *e;
	bool idle = true;
	FusionCall *call = (FusionCall *) entry;

	if (call->executions)
		idle = ((FusionCallExecution *) call->executions)->executed;

	seq_printf(p, "(%d calls) %s",
		   call->count, idle ? "idle" : "executing");

	fusion_list_foreach(e, call->executions) {
		FusionCallExecution *exec = (FusionCallExecution *) e;

		seq_printf(p, "  [0x%08lx]",
			   exec->caller ? fusionee_id(exec->caller) : 0);
	}

	seq_printf(p, "\n");
}

FUSION_ENTRY_CLASS(FusionCall, call, fusion_call_construct,
		   fusion_call_destruct, fusion_call_print)

/******************************************************************************/

int fusion_call_init(FusionDev * dev)
{
	fusion_entries_init(&dev->call, &call_class, dev, dev);

	fusion_entries_create_proc_entry(dev, "calls", &dev->call);

	return 0;
}

void fusion_call_deinit(FusionDev * dev)
{
	fusion_entries_destroy_proc_entry(dev, "calls");

	fusion_entries_deinit(&dev->call);
}

/******************************************************************************/

int fusion_call_new(FusionDev * dev, Fusionee *fusionee, FusionCallNew * call_new)
{
	int id;
	int ret;

	struct fusion_construct_ctx cc = { fusionee, call_new };

	ret = fusion_entry_create(&dev->call, &id, &cc);
	if (ret)
		return ret;

	return 0;
}

int
fusion_call_execute(FusionDev * dev, Fusionee * fusionee,
		    FusionCallExecute * execute)
{
	int ret;
	FusionCall *call;
	FusionCallExecution *execution = NULL;
	FusionCallMessage message;
	unsigned int serial;

	FUSION_DEBUG( "%s( dev %p, fusionee %p, execute %p )\n", __FUNCTION__, dev, fusionee, execute );

	/* Lookup and lock call. */
	ret = fusion_call_lookup(&dev->call, execute->call_id, &call);
	if (ret)
		return ret;

	FUSION_DEBUG( "  -> call %u '%s'\n", call->entry.id, call->entry.name );

	do {
		serial = ++call->serial;
	} while (!serial);

	/* Add execution to receive the result. */
	if (fusionee && !(execute->flags & FCEF_ONEWAY)) {
		execution = add_execution(call, fusionee, serial);
		if (!execution)
			return -ENOMEM;

		FUSION_DEBUG( "  -> execution %p, serial %u\n", execution, execution->serial );
	}

	/* Fill call message. */
	message.handler = call->handler;
	message.ctx = call->ctx;

	message.caller = fusionee ? fusionee_id(fusionee) : 0;

	message.call_arg = execute->call_arg;
	message.call_ptr = execute->call_ptr;

	message.serial = execution ? serial : 0;

	FUSION_DEBUG( "  -> sending call message, caller %u\n", message.caller );

	/* Put message into queue of callee. */
	ret = fusionee_send_message2(dev, fusionee, call->fusionee, FMT_CALL,
				    call->entry.id, 0, sizeof(message),
				    &message, NULL, NULL, 1, NULL, 0);
	if (ret) {
		FUSION_DEBUG( "  -> MESSAGE SENDING FAILED! (ret %u)\n", ret );
		if (execution) {
			remove_execution(call, execution);
			kfree(execution);
		}
		return ret;
	}

	call->count++;

	/* When waiting for a result... */
	if (execution) {
		FUSION_DEBUG( "  -> message sent, transfering all skirmishs...\n" );

		/* Transfer held skirmishs (locks). */
		fusion_skirmish_transfer_all(dev, call->fusionee->id,
						fusionee_id(fusionee),
						current->pid,
						serial);

		/* Unlock call and wait for execution result. TODO: add timeout? */

		FUSION_DEBUG( "  -> skirmishs transferred, sleeping on call...\n" );
		fusion_sleep_on( dev, &execution->wait, 0 );

		if (signal_pending(current)) {
			FUSION_DEBUG( "  -> woke up, SIGNAL PENDING!\n" );
			/* Indicate that a signal was received and execution won't be freed by caller. */
			execution->caller = NULL;
			return -EINTR;
		}

		/* Return result to calling process. */
		execute->ret_val = execution->ret_val;

		/* Free execution, which has already been removed by callee. */
		kfree(execution);

		FUSION_DEBUG( "  -> woke up, ret val %u, reclaiming skirmishs...\n", execute->ret_val );

		/* Reclaim skirmishs. */
		fusion_skirmish_reclaim_all(dev, current->pid);
	} else {
		FUSION_DEBUG( "  -> message sent, not waiting.\n" );
	}

	return 0;
}

int
fusion_call_execute2(FusionDev * dev, Fusionee * fusionee,
		    FusionCallExecute2 * execute)
{
	int ret;
	FusionCall *call;
	FusionCallExecution *execution = NULL;
	FusionCallMessage message;
	unsigned int serial;

	FUSION_DEBUG( "%s( dev %p, fusionee %p, execute %p )\n", __FUNCTION__, dev, fusionee, execute );

	/* Lookup and lock call. */
	ret = fusion_call_lookup(&dev->call, execute->call_id, &call);
	if (ret)
		return ret;

	FUSION_DEBUG( "  -> call %u '%s'\n", call->entry.id, call->entry.name );

	do {
		serial = ++call->serial;
	} while (!serial);

	/* Add execution to receive the result. */
	if (fusionee && !(execute->flags & FCEF_ONEWAY)) {
		execution = add_execution(call, fusionee, serial);
		if (!execution)
			return -ENOMEM;

		FUSION_DEBUG( "  -> execution %p, serial %u\n", execution, execution->serial );
	}

	/* Fill call message. */
	message.handler = call->handler;
	message.ctx = call->ctx;

	message.caller = fusionee ? fusionee_id(fusionee) : 0;

	message.call_arg = execute->call_arg;
	message.call_ptr = NULL;

	message.serial = execution ? serial : 0;

	FUSION_DEBUG( "  -> sending call message, caller %u\n", message.caller );

	/* Put message into queue of callee. */
	ret = fusionee_send_message2(dev, fusionee, call->fusionee, FMT_CALL,
				    call->entry.id, 0, sizeof(FusionCallMessage),
				    &message, NULL, NULL, 1, execute->ptr, execute->length);
	if (ret) {
		FUSION_DEBUG( "  -> MESSAGE SENDING FAILED! (ret %u)\n", ret );
		if (execution) {
			remove_execution(call, execution);
			kfree(execution);
		}
		return ret;
	}

	call->count++;

	/* When waiting for a result... */
	if (execution) {
		FUSION_DEBUG( "  -> message sent, transfering all skirmishs...\n" );

		/* Transfer held skirmishs (locks). */
		fusion_skirmish_transfer_all(dev, call->fusionee->id,
						fusionee_id(fusionee),
						current->pid,
						serial);

		/* Unlock call and wait for execution result. TODO: add timeout? */

		FUSION_DEBUG( "  -> skirmishs transferred, sleeping on call...\n" );
		fusion_sleep_on( dev, &execution->wait, 0 );

		if (signal_pending(current)) {
			FUSION_DEBUG( "  -> woke up, SIGNAL PENDING!\n" );
			/* Indicate that a signal was received and execution won't be freed by caller. */
			execution->caller = NULL;
			return -EINTR;
		}

		/* Return result to calling process. */
		execute->ret_val = execution->ret_val;

		/* Free execution, which has already been removed by callee. */
		kfree(execution);

		FUSION_DEBUG( "  -> woke up, ret val %u, reclaiming skirmishs...\n", execute->ret_val );

		/* Reclaim skirmishs. */
		fusion_skirmish_reclaim_all(dev, current->pid);
	} else {
		FUSION_DEBUG( "  -> message sent, not waiting.\n" );
	}

	return 0;
}

int
fusion_call_return(FusionDev * dev, int fusion_id, FusionCallReturn * call_ret)
{
	int ret;
	FusionCall *call;
	FusionCallExecution *execution;

	if ( (dev->api.major >= 4) && (call_ret->serial == 0) )
		return -EOPNOTSUPP;

	/* Lookup and lock call. */
	ret = fusion_call_lookup(&dev->call, call_ret->call_id, &call);
	if (ret)
		return ret;

	/* Search for execution, starting with oldest. */
	direct_list_foreach (execution, call->executions) {
		if ((execution->executed)
		    || (execution->call_id != call_ret->call_id)
		    || ((dev->api.major >= 4)
			&& (execution->serial != call_ret->serial))) {
			continue;
		}

		/*
		 * Check if caller received a signal while waiting for the result.
		 *
		 * TODO: This is not completely solved. Restarting the system call
		 * should be possible without causing another execution.
		 */
		FUSION_ASSUME(execution->caller != NULL);
		if (!execution->caller) {
			/* Remove and free execution. */
			remove_execution(call, execution);
			kfree(execution);
			return -EIDRM;
		}

		/* Write result to execution. */
		execution->ret_val = call_ret->val;
		execution->executed = true;

		/* Remove execution, freeing is up to caller. */
		remove_execution(call, execution);

		/* FIXME: Caller might still have received a signal since check above. */
		FUSION_ASSERT(execution->caller != NULL);

		/* Return skirmishs. */
		fusion_skirmish_return_all(dev, fusion_id, execution->caller_pid, execution->serial);

		/* Wake up caller. */
		wake_up_interruptible(&execution->wait);

		return 0;
	}

	/* DirectFB 1.0.x does not handle one-way-calls properly */
	if (dev->api.major <= 3)
		return 0;

	return -ENOMSG;
}

int fusion_call_destroy(FusionDev * dev, Fusionee *fusionee, int call_id)
{
	int ret;
	FusionCall *call;
	FusionCallExecution *execution;

	do {
		/* Wait for all messages being processed. */
		ret =
		    fusionee_wait_processing(dev, fusionee->id, FMT_CALL, call_id);
		if (ret)
			return ret;

		ret = fusion_call_lookup(&dev->call, call_id, &call);
		if (ret)
			return ret;

		/* Check if we own the call. */
		if (call->fusionee != fusionee)
			return -EIO;

		/* If an execution is pending... */
		execution = (FusionCallExecution *) call->executions;
		if (execution) {
			/* Unlock call and wait for execution. TODO: add timeout? */
			fusion_sleep_on( dev, &execution->wait, 0);

			if (signal_pending(current))
				return -EINTR;
		}
	} while (execution);

	fusion_entry_destroy_locked(call->entry.entries, &call->entry);

	return 0;
}

void fusion_call_destroy_all(FusionDev * dev, Fusionee *fusionee)
{
	FusionLink *l;

	FUSION_DEBUG( "%s( dev %p, fusion_id %u )\n", __FUNCTION__, dev, fusion_id );

	l = dev->call.list;

	while (l) {
		FusionLink *next = l->next;
		FusionCall *call = (FusionCall *) l;

		if (call->fusionee == fusionee)
			fusion_entry_destroy_locked(call->entry.entries,
						    &call->entry);

		l = next;
	}
}

/******************************************************************************/

static FusionCallExecution *add_execution(FusionCall * call,
					  Fusionee * caller,
					  unsigned int serial)
{
	FusionCallExecution *execution;

	FUSION_DEBUG( "%s( call %p [%u], caller %p [%u], serial %i )\n", __FUNCTION__, call, call->entry.id, caller, caller->id, serial );

	/* Allocate execution. */
	execution = kmalloc(sizeof(FusionCallExecution), GFP_ATOMIC);
	if (!execution)
		return NULL;

	/* Initialize execution. */
	memset(execution, 0, sizeof(FusionCallExecution));

	execution->caller = caller;
	execution->caller_pid = current->pid;
	execution->call_id = call->entry.id;
	execution->serial = serial;

	init_waitqueue_head(&execution->wait);

	/* Add execution. */
	direct_list_append(&call->executions, &execution->link);

	return execution;
}

static void remove_execution(FusionCall * call, FusionCallExecution * execution)
{
	FUSION_DEBUG( "%s( call %p [%u], execution %p )\n", __FUNCTION__, call, call->entry.id, execution );

	fusion_list_remove(&call->executions, &execution->link);
}

static void free_all_executions(FusionCall * call)
{
	FusionCallExecution *execution, *next;

	FUSION_DEBUG( "%s( call %p [%u] )\n", __FUNCTION__, call, call->entry.id );

	direct_list_foreach_safe (execution, next, call->executions) {
		remove_execution( call, execution );

		wake_up_interruptible_all( &execution->wait );

		if (!execution->caller)
			kfree( execution );
	}
}

