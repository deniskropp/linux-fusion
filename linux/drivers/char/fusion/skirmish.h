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

#ifndef __FUSION__SKIRMISH_H__
#define __FUSION__SKIRMISH_H__

#include "fusiondev.h"
#include "types.h"


/* module init/cleanup */

int  fusion_skirmish_init   (FusionDev *dev);
void fusion_skirmish_deinit (FusionDev *dev);


/* public API */

int fusion_skirmish_new     (FusionDev *dev,
                             int       *id);

int fusion_skirmish_prevail (FusionDev *dev,
                             int        id,
                             int        fusion_id);

int fusion_skirmish_swoop   (FusionDev *dev,
                             int        id,
                             int        fusion_id);

int fusion_skirmish_dismiss (FusionDev *dev,
                             int        id,
                             int        fusion_id);

int fusion_skirmish_destroy (FusionDev *dev,
                             int        id);


/* internal functions */

void fusion_skirmish_dismiss_all (FusionDev *dev,
                                  int        fusion_id);

void fusion_skirmish_dismiss_all_from_pid (FusionDev *dev,
                                           int        pid);

#endif
