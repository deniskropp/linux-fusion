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

#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/sched.h>

#include <linux/fusion.h>

#include "fusiondev.h"
#include "fusionee.h"
#include "list.h"
#include "reactor.h"

typedef struct {
     FusionLink         link;

     int                fusion_id;

     int                count;     /* number of attach calls */
} ReactorNode;

typedef struct {
     FusionEntry        entry;

     FusionLink        *nodes;

     int                dispatch_count;
} FusionReactor;

/******************************************************************************/

static ReactorNode *get_node      ( FusionReactor *reactor,
                                    int            fusion_id );

static void         remove_node   ( FusionReactor *reactor,
                                    int            fusion_id );

static void         free_all_nodes( FusionReactor *reactor );

/******************************************************************************/

static void
fusion_reactor_destruct( FusionEntry *entry,
                         void        *ctx )
{
     FusionReactor *reactor = (FusionReactor*) entry;

     free_all_nodes( reactor );
}

static int
fusion_reactor_print( FusionEntry *entry,
                      void        *ctx,
                      char        *buf )
{
     int            num     = 0;
     FusionReactor *reactor = (FusionReactor*) entry;
     FusionLink    *node    = reactor->nodes;

     fusion_list_foreach (node, reactor->nodes) {
          num++;
     }

     return sprintf( buf, "%5dx dispatch, %d nodes\n", reactor->dispatch_count, num );
}


FUSION_ENTRY_CLASS( FusionReactor, reactor, NULL,
                    fusion_reactor_destruct, fusion_reactor_print )

/******************************************************************************/

int
fusion_reactor_init (FusionDev *dev)
{
     fusion_entries_init( &dev->reactor, &reactor_class, dev );

     create_proc_read_entry( "reactors", 0, dev->proc_dir,
                             fusion_entries_read_proc, &dev->reactor );

     return 0;
}

void
fusion_reactor_deinit (FusionDev *dev)
{
     remove_proc_entry ("reactors", dev->proc_dir);

     fusion_entries_deinit( &dev->reactor );
}

/******************************************************************************/

int
fusion_reactor_new (FusionDev *dev, int *ret_id)
{
     return fusion_entry_create( &dev->reactor, ret_id );
}

int
fusion_reactor_attach (FusionDev *dev, int id, int fusion_id)
{
     int            ret;
     ReactorNode   *node;
     FusionReactor *reactor;

     ret = fusion_reactor_lock( &dev->reactor, id, &reactor );
     if (ret)
          return ret;

     dev->stat.reactor_attach++;

     node = get_node (reactor, fusion_id);
     if (!node) {
          node = kmalloc (sizeof(ReactorNode), GFP_KERNEL);
          if (!node) {
               fusion_reactor_unlock( reactor );
               return -ENOMEM;
          }

          node->fusion_id = fusion_id;
          node->count     = 1;

          fusion_list_prepend (&reactor->nodes, &node->link);
     }
     else
          node->count++;

     fusion_reactor_unlock( reactor );

     return 0;
}

int
fusion_reactor_detach (FusionDev *dev, int id, int fusion_id)
{
     int            ret;
     ReactorNode   *node;
     FusionReactor *reactor;

     ret = fusion_reactor_lock( &dev->reactor, id, &reactor );
     if (ret)
          return ret;

     dev->stat.reactor_detach++;

     node = get_node (reactor, fusion_id);
     if (!node) {
          fusion_reactor_unlock( reactor );
          return -EIO;
     }

     if (! --node->count) {
          fusion_list_remove (&reactor->nodes, &node->link);
          kfree (node);
     }

     fusion_reactor_unlock( reactor );

     return 0;
}

int
fusion_reactor_dispatch (FusionDev *dev, int id, int fusion_id,
                         int msg_size, const void *msg_data)
{
     int            ret;
     FusionLink    *l;
     FusionReactor *reactor;

     ret = fusion_reactor_lock( &dev->reactor, id, &reactor );
     if (ret)
          return ret;

     reactor->dispatch_count++;

     fusion_list_foreach (l, reactor->nodes) {
          ReactorNode *node = (ReactorNode *) l;

          if (node->fusion_id == fusion_id)
               continue;

          fusionee_send_message (dev, fusion_id, node->fusion_id, FMT_REACTOR,
                                 reactor->entry.id, msg_size, msg_data);
     }

     fusion_reactor_unlock( reactor );

     return 0;
}

int
fusion_reactor_destroy (FusionDev *dev, int id)
{
     return fusion_entry_destroy( &dev->reactor, id );
}

void
fusion_reactor_detach_all (FusionDev *dev, int fusion_id)
{
     FusionLink *l;

     down (&dev->reactor.lock);

     fusion_list_foreach (l, dev->reactor.list) {
          FusionReactor *reactor = (FusionReactor *) l;

          remove_node (reactor, fusion_id);
     }

     up (&dev->reactor.lock);
}

/******************************************************************************/

static ReactorNode *
get_node (FusionReactor *reactor,
          int            fusion_id)
{
     ReactorNode *node;

     fusion_list_foreach (node, reactor->nodes) {
          if (node->fusion_id == fusion_id)
               return node;
     }

     return NULL;
}

static void
remove_node (FusionReactor *reactor, int fusion_id)
{
     ReactorNode *node;

     down (&reactor->entry.lock);

     fusion_list_foreach (node, reactor->nodes) {
          if (node->fusion_id == fusion_id) {
               fusion_list_remove (&reactor->nodes, &node->link);
               break;
          }
     }

     up (&reactor->entry.lock);
}

static void
free_all_nodes (FusionReactor *reactor)

{
     FusionLink  *n;
     ReactorNode *node;

     fusion_list_foreach_safe (node, n, reactor->nodes) {
          kfree (node);
     }

     reactor->nodes = NULL;
}
