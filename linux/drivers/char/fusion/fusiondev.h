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
 
#ifndef __FUSIONDEV_H__
#define __FUSIONDEV_H__

#include <linux/proc_fs.h>

#include "list.h"

typedef struct {
     int refs;
     
     struct proc_dir_entry *proc_dir;
     
     struct {
          int         property_lease_purchase;
          int         property_cede;

          int         reactor_attach;
          int         reactor_detach;

          int         ref_up;
          int         ref_down;

          int         skirmish_prevail_swoop;
          int         skirmish_dismiss;
     } stat;
     
     struct {
          int         ids;
          FusionLink *list;
          spinlock_t  lock;
     } call;

     struct {
          int                last_id;
          FusionLink        *list;
          spinlock_t         lock;
          wait_queue_head_t  wait;
     } fusionee;
     
     struct {
          int         ids;
          FusionLink *list;
          spinlock_t  lock;
     } property;
     
     struct {
          int         ids;
          FusionLink *list;
          spinlock_t  lock;
     } reactor;
     
     struct {
          int         ids;
          FusionLink *list;
          spinlock_t  lock;
     } ref;
     
     struct {
          int         ids;
          FusionLink *list;
          spinlock_t  lock;
     } skirmish;
} FusionDev;

/*
 * Special version of interruptible_sleep_on() that unlocks the spinlock
 * after adding the entry to the queue (just before schedule).
 */
void fusion_sleep_on (wait_queue_head_t *q,
                      spinlock_t        *lock,
                      signed long       *timeout_ms);

#endif
