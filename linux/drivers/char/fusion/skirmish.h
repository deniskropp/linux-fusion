/*
 *	Fusion Kernel Module
 *
 *	(c) Copyright 2002  Convergence GmbH
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

#include "types.h"


/* module init/cleanup */

int  fusion_skirmish_init (void);
void fusion_skirmish_reset (void);
void fusion_skirmish_cleanup (void);


/* public API */

int fusion_skirmish_new (int *id);
int fusion_skirmish_prevail (int id, int fusion_id);
int fusion_skirmish_swoop (int id, int fusion_id);
int fusion_skirmish_dismiss (int id, int fusion_id);
int fusion_skirmish_destroy (int id);


/* internal functions */

void fusion_skirmish_dismiss_all (int fusion_id);

#endif
