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

#include "fusionee.h"

static int fid = 1;

Fusionee *
fusionee_new()
{
  Fusionee *fusionee;

  fusionee = kmalloc (sizeof(Fusionee), GFP_KERNEL);
  if (!fusionee)
    return NULL;

  fusionee->fusion_id = fid++;

  return fusionee;
}

void
fusionee_destroy (Fusionee *fusionee)
{
  kfree (fusionee);
}
