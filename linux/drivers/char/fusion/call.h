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
 
#ifndef __FUSION__CALL_H__
#define __FUSION__CALL_H__

#include <linux/fusion.h>


/* module init/cleanup */

int  fusion_call_init (void);
void fusion_call_reset (void);
void fusion_call_cleanup (void);


/* public API */

int fusion_call_new (int fusion_id, FusionCallNew *call);
int fusion_call_execute (int fusion_id, FusionCallExecute *execute);
int fusion_call_return (int fusion_id, FusionCallReturn *call_ret);
int fusion_call_destroy (int fusion_id, int call_id);


/* internal functions */

void fusion_call_destroy_all (int fusion_id);

#endif
