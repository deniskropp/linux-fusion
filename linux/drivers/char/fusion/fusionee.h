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
 
#ifndef __FUSIONEE_H__
#define __FUSIONEE_H__

#include <linux/poll.h>
#include <linux/fusion.h>

#include "types.h"


/* module init/cleanup */

int  fusionee_init (void);
void fusionee_reset (void);
void fusionee_cleanup (void);


/* internal functions */

int fusionee_new (int *id);

int fusionee_send_message (int id, int recipient, FusionMessageType msg_type,
                           int msg_id, int msg_size, const void *msg_data);

int fusionee_get_messages (int id, void *buf, int buf_size, int block);

unsigned int fusionee_poll (int id, struct file *file, poll_table * wait);

int fusionee_kill (int id);

int fusionee_destroy (int id);

#endif
