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

#ifndef __FUSION__FUSIONDEV_H__
#define __FUSION__FUSIONDEV_H__

#include <linux/proc_fs.h>

#include "debug.h"
#include "entries.h"
#include "list.h"


#define NUM_MINORS  8
#define NUM_CLASSES 8



struct __Fusion_FusionDev {
     int refs;
     int index;
     struct {
          int major;
          int minor;
     } api;

     int enter_ok;
     FusionWaitQueue enter_wait;

     unsigned long shared_area;

     struct {
          int property_lease_purchase;
          int property_cede;

          int reactor_attach;
          int reactor_detach;
          int reactor_dispatch;

          int ref_up;
          int ref_down;

          int skirmish_prevail_swoop;
          int skirmish_dismiss;
          int skirmish_wait;
          int skirmish_notify;

          int shmpool_attach;
          int shmpool_detach;
     } stat;

     struct {
          int last_id;
          FusionLink *list;
          FusionWaitQueue wait;
     } fusionee;

     FusionEntries call;
     FusionEntries properties;
     FusionEntries reactor;
     FusionEntries ref;
     FusionEntries shmpool;
     FusionEntries skirmish;

     unsigned int next_class_index;
};


typedef struct {
     FusionDev  devs[NUM_MINORS];
} FusionShared;

extern FusionCore            *fusion_core;
extern struct proc_dir_entry *fusion_proc_dir[NUM_MINORS];

#endif
