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
 
#ifndef __FUSION__REF_H__
#define __FUSION__REF_H__

#include "types.h"


/* module init/cleanup */

int  fusion_ref_init (void);
void fusion_ref_cleanup (void);


/* public API */

int fusion_ref_new (int *id);
int fusion_ref_up (int id, int fusion_id);
int fusion_ref_down (int id, int fusion_id);
int fusion_ref_zero_lock (int id);
int fusion_ref_zero_trylock (int id);
int fusion_ref_unlock (int id);
int fusion_ref_stat (int id, int *refs);
int fusion_ref_destroy (int id);


/* internal functions */

void fusion_ref_clear_all_local (int fusion_id);

#endif
