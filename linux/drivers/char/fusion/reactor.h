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
 
#ifndef __FUSION__REACTOR_H__
#define __FUSION__REACTOR_H__

#include "types.h"


/* module init/cleanup */

int  fusion_reactor_init (void);
void fusion_reactor_reset (void);
void fusion_reactor_cleanup (void);


/* public API */

int fusion_reactor_new (int *id);
int fusion_reactor_attach (int id, int fusion_id);
int fusion_reactor_detach (int id, int fusion_id);
int fusion_reactor_dispatch (int id, int fusion_id,
                             int msg_size, const void *msg_data);
int fusion_reactor_destroy (int id);


/* internal functions */

void fusion_reactor_detach_all (int fusion_id);

#endif
