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
 
#ifndef __FUSION__PROPERTY_H__
#define __FUSION__PROPERTY_H__

#include "types.h"


/* module init/cleanup */

int  fusion_property_init (void);
void fusion_property_reset (void);
void fusion_property_cleanup (void);


/* public API */

int fusion_property_new (int *id);
int fusion_property_lease (int id, int fusion_id);
int fusion_property_purchase (int id, int fusion_id);
int fusion_property_cede (int id, int fusion_id);
int fusion_property_destroy (int id);


/* internal functions */

void fusion_property_cede_all (int fusion_id);

#endif
