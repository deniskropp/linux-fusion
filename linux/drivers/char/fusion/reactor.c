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
     FusionLink         link;

     struct semaphore   lock;

     int                id;
     int                pid;

     FusionLink        *nodes;
} FusionReactor;

/******************************************************************************/

static int  lookup_reactor (FusionDev *dev, int id, FusionReactor **ret_reactor);
static int  lock_reactor   (FusionDev *dev, int id, FusionReactor **ret_reactor);
static void unlock_reactor (FusionReactor *reactor);

static ReactorNode *get_node           (FusionReactor *reactor,
                                        int            fusion_id);
static void         remove_node        (FusionReactor *reactor,
                                        int            fusion_id);
static void         free_all_nodes     (FusionReactor *reactor);

/******************************************************************************/

static int
reactors_read_proc(char *buf, char **start, off_t offset,
                   int len, int *eof, void *private)
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     if (down_interruptible (&dev->reactor.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->reactor.list) {
          FusionReactor *reactor = (FusionReactor*) l;

          written += sprintf(buf+written, "(%5d) 0x%08x %s\n", reactor->pid,
                             reactor->id, reactor->nodes ? "" : "(none attached)");
          if (written < offset) {
               offset -= written;
               written = 0;
          }

          if (written >= len)
               break;
     }

     up (&dev->reactor.lock);

     *start = buf + offset;
     written -= offset;
     if (written > len) {
          *eof = 0;
          return len;
     }

     *eof = 1;
     return(written<0) ? 0 : written;
}

int
fusion_reactor_init (FusionDev *dev)
{
     init_MUTEX (&dev->reactor.lock);

     create_proc_read_entry("reactors", 0, dev->proc_dir,
                            reactors_read_proc, dev);

     return 0;
}

void
fusion_reactor_deinit (FusionDev *dev)
{
     FusionLink *l;

     down (&dev->reactor.lock);

     remove_proc_entry ("reactors", dev->proc_dir);

     l = dev->reactor.list;
     while (l) {
          FusionLink    *next    = l->next;
          FusionReactor *reactor = (FusionReactor *) l;

          free_all_nodes (reactor);

          kfree (reactor);

          l = next;
     }

     up (&dev->reactor.lock);
}

/******************************************************************************/

int
fusion_reactor_new (FusionDev *dev, int *id)
{
     FusionReactor *reactor;

     reactor = kmalloc (sizeof(FusionReactor), GFP_KERNEL);
     if (!reactor)
          return -ENOMEM;

     memset (reactor, 0, sizeof(FusionReactor));

     if (down_interruptible (&dev->reactor.lock)) {
          kfree (reactor);
          return -EINTR;
     }

     reactor->id  = dev->reactor.ids++;
     reactor->pid = current->pid;

     init_MUTEX (&reactor->lock);

     fusion_list_prepend (&dev->reactor.list, &reactor->link);

     up (&dev->reactor.lock);

     *id = reactor->id;

     return 0;
}

int
fusion_reactor_attach (FusionDev *dev, int id, int fusion_id)
{
     int            ret;
     ReactorNode   *node;
     FusionReactor *reactor;

     ret = lock_reactor (dev, id, &reactor);
     if (ret)
          return ret;

     dev->stat.reactor_attach++;

     node = get_node (reactor, fusion_id);
     if (!node) {
          node = kmalloc (sizeof(ReactorNode), GFP_KERNEL);
          if (!node) {
               unlock_reactor (reactor);
               return -ENOMEM;
          }

          node->fusion_id = fusion_id;
          node->count     = 1;

          fusion_list_prepend (&reactor->nodes, &node->link);
     }
     else
          node->count++;

     unlock_reactor (reactor);

     return 0;
}

int
fusion_reactor_detach (FusionDev *dev, int id, int fusion_id)
{
     int            ret;
     ReactorNode   *node;
     FusionReactor *reactor;

     ret = lock_reactor (dev, id, &reactor);
     if (ret)
          return ret;

     dev->stat.reactor_detach++;

     node = get_node (reactor, fusion_id);
     if (!node) {
          unlock_reactor (reactor);
          return -EIO;
     }

     if (! --node->count) {
          fusion_list_remove (&reactor->nodes, &node->link);
          kfree (node);
     }

     unlock_reactor (reactor);

     return 0;
}

int
fusion_reactor_dispatch (FusionDev *dev, int id, int fusion_id,
                         int msg_size, const void *msg_data)
{
     int            ret;
     FusionLink    *l;
     FusionReactor *reactor;

     ret = lock_reactor (dev, id, &reactor);
     if (ret)
          return ret;

     fusion_list_foreach (l, reactor->nodes) {
          ReactorNode *node = (ReactorNode *) l;

          if (node->fusion_id == fusion_id)
               continue;

          fusionee_send_message (dev, fusion_id, node->fusion_id, FMT_REACTOR,
                                 reactor->id, msg_size, msg_data);
     }

     unlock_reactor (reactor);

     return 0;
}

int
fusion_reactor_destroy (FusionDev *dev, int id)
{
     int            ret;
     FusionReactor *reactor;

     ret = lookup_reactor (dev, id, &reactor);
     if (ret)
          return ret;

     if (down_interruptible (&reactor->lock)) {
          up (&dev->reactor.lock);
          return -EINTR;
     }

     fusion_list_remove (&dev->reactor.list, &reactor->link);

     up (&dev->reactor.lock);

     free_all_nodes (reactor);

     up (&reactor->lock);

     kfree (reactor);

     return 0;
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

static int
lookup_reactor (FusionDev *dev, int id, FusionReactor **ret_reactor)
{
     FusionLink *l;

     if (down_interruptible (&dev->reactor.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->reactor.list) {
          FusionReactor *reactor = (FusionReactor *) l;

          if (reactor->id == id) {
               *ret_reactor = reactor;
               return 0;
          }
     }

     up (&dev->reactor.lock);

     return -EINVAL;
}

static int
lock_reactor (FusionDev *dev, int id, FusionReactor **ret_reactor)
{
     int         ret;
     FusionReactor *reactor;

     ret = lookup_reactor (dev, id, &reactor);
     if (ret)
          return ret;

     if (reactor) {
          fusion_list_move_to_front (&dev->reactor.list, &reactor->link);

          if (down_interruptible (&reactor->lock)) {
               up (&dev->reactor.lock);
               return -EINTR;
          }

          up (&dev->reactor.lock);
     }

     *ret_reactor = reactor;

     return 0;
}

static void
unlock_reactor (FusionReactor *reactor)
{
     up (&reactor->lock);
}

static ReactorNode *
get_node (FusionReactor *reactor,
          int            fusion_id)
{
     FusionLink *l;

     fusion_list_foreach (l, reactor->nodes) {
          ReactorNode *node = (ReactorNode *) l;

          if (node->fusion_id == fusion_id)
               return node;
     }

     return NULL;
}

static void
remove_node (FusionReactor *reactor, int fusion_id)
{
     FusionLink *l;

     down (&reactor->lock);

     fusion_list_foreach (l, reactor->nodes) {
          ReactorNode *node = (ReactorNode *) l;

          if (node->fusion_id == fusion_id) {
               fusion_list_remove (&reactor->nodes, l);
               break;
          }
     }

     up (&reactor->lock);
}

static void
free_all_nodes (FusionReactor *reactor)

{
     FusionLink *l = reactor->nodes;

     while (l) {
          FusionLink *next = l->next;

          kfree (l);

          l = next;
     }

     reactor->nodes = NULL;
}
