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

#ifndef __FUSION__LIST_H__
#define __FUSION__LIST_H__

#include "types.h"


typedef struct _FusionLink {
  struct _FusionLink *next;
  struct _FusionLink *prev;
} FusionLink;

void fusion_list_prepend (FusionLink **list, FusionLink *link);
void fusion_list_remove  (FusionLink **list, FusionLink *link);

#define fusion_list_foreach(link, list)  for (link = list; link; link = link->next)

#endif /* __FUSION__LIST_H__ */

