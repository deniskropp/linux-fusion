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

  spinlock_t         lock;

  int                id;
  int                pid;

  FusionLink        *nodes;
} FusionReactor;

/******************************************************************************/

static FusionReactor *lookup_reactor     (int id);

static FusionReactor *lock_reactor       (int id);
static void           unlock_reactor     (FusionReactor *reactor);

static ReactorNode   *get_node           (FusionReactor *reactor,
                                          int            fusion_id);
static void           remove_node        (FusionReactor *reactor,
                                          int            fusion_id);
static void           free_all_nodes     (FusionReactor *reactor);

/******************************************************************************/

static int         ids           = 0;
static FusionLink *reactors      = NULL;
static spinlock_t  reactors_lock = SPIN_LOCK_UNLOCKED;

/******************************************************************************/

static int
fusion_reactor_read_proc(char *buf, char **start, off_t offset,
                         int len, int *eof, void *private)
{
  FusionLink *l;
  int written = 0;

  spin_lock (&reactors_lock);

  fusion_list_foreach (l, reactors)
    {
      FusionReactor *reactor = (FusionReactor*) l;

      written += sprintf(buf+written, "(%5d) 0x%08x %s\n", reactor->pid,
                         reactor->id, reactor->nodes ? "" : "(none attached)");
      if (written < offset)
        {
          offset -= written;
          written = 0;
        }

      if (written >= len)
        break;
    }

  spin_unlock (&reactors_lock);

  *start = buf + offset;
  written -= offset;
  if(written > len)
    {
      *eof = 0;
      return len;
    }

  *eof = 1;
  return (written<0) ? 0 : written;
}

int
fusion_reactor_init()
{
  create_proc_read_entry("reactors", 0, proc_fusion_dir,
                         fusion_reactor_read_proc, NULL);

  return 0;
}

void
fusion_reactor_reset()
{
  FusionLink *l;

  spin_lock (&reactors_lock);

  l = reactors;
  while (l)
    {
      FusionLink    *next    = l->next;
      FusionReactor *reactor = (FusionReactor *) l;

      free_all_nodes (reactor);
          
      kfree (reactor);

      l = next;
    }

  reactors = NULL;

  spin_unlock (&reactors_lock);
}

void
fusion_reactor_cleanup()
{
  fusion_reactor_reset();

  remove_proc_entry ("reactors", proc_fusion_dir);
}

/******************************************************************************/

int
fusion_reactor_new (int *id)
{
  FusionReactor *reactor;

  reactor = kmalloc (sizeof(FusionReactor), GFP_KERNEL);
  if (!reactor)
    return -ENOMEM;

  memset (reactor, 0, sizeof(FusionReactor));

  spin_lock (&reactors_lock);

  reactor->id   = ids++;
  reactor->pid  = current->pid;
  reactor->lock = SPIN_LOCK_UNLOCKED;

  fusion_list_prepend (&reactors, &reactor->link);

  spin_unlock (&reactors_lock);

  *id = reactor->id;

  return 0;
}

int
fusion_reactor_attach (int id, int fusion_id)
{
  ReactorNode   *node;
  FusionReactor *reactor = lock_reactor (id);

  if (!reactor)
    return -EINVAL;

  node = get_node (reactor, fusion_id);
  if (!node)
    {
      node = kmalloc (sizeof(ReactorNode), GFP_KERNEL);
      if (!node)
        {
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
fusion_reactor_detach (int id, int fusion_id)
{
  ReactorNode   *node;
  FusionReactor *reactor = lock_reactor (id);

  if (!reactor)
    return -EINVAL;

  node = get_node (reactor, fusion_id);
  if (!node)
    {
      unlock_reactor (reactor);
      return -EIO;
    }

  if (! --node->count)
    fusion_list_remove (&reactor->nodes, &node->link);

  unlock_reactor (reactor);

  return 0;
}

int
fusion_reactor_dispatch (int id, int fusion_id,
                         int msg_size, const void *msg_data)
{
  FusionLink    *l;
  FusionReactor *reactor = lock_reactor (id);

  if (!reactor)
    return -EINVAL;

  fusion_list_foreach (l, reactor->nodes)
    {
      ReactorNode *node = (ReactorNode *) l;

      if (node->fusion_id == fusion_id)
        continue;

      fusionee_send_message (node->fusion_id, FMT_REACTOR,
                             reactor->id, msg_size, msg_data);
    }

  unlock_reactor (reactor);

  return 0;
}

int
fusion_reactor_destroy (int id)
{
  FusionReactor *reactor = lookup_reactor (id);

  if (!reactor)
    return -EINVAL;

  spin_lock (&reactor->lock);

  fusion_list_remove (&reactors, &reactor->link);

  spin_unlock (&reactors_lock);

  free_all_nodes (reactor);

  kfree (reactor);

  return 0;
}

void
fusion_reactor_detach_all (int fusion_id)
{
  FusionLink *l;

  spin_lock (&reactors_lock);

  fusion_list_foreach (l, reactors)
    {
      FusionReactor *reactor = (FusionReactor *) l;

      remove_node (reactor, fusion_id);
    }

  spin_unlock (&reactors_lock);
}

/******************************************************************************/

static FusionReactor *
lookup_reactor (int id)
{
  FusionLink *l;

  spin_lock (&reactors_lock);

  fusion_list_foreach (l, reactors)
    {
      FusionReactor *reactor = (FusionReactor *) l;

      if (reactor->id == id)
        return reactor;
    }

  spin_unlock (&reactors_lock);

  return NULL;
}

static FusionReactor *
lock_reactor (int id)
{
  FusionReactor *reactor = lookup_reactor (id);

  if (reactor)
    {
      spin_lock (&reactor->lock);
      spin_unlock (&reactors_lock);
    }

  return reactor;
}

static void
unlock_reactor (FusionReactor *reactor)
{
  spin_unlock (&reactor->lock);
}

static ReactorNode *
get_node (FusionReactor *reactor,
          int            fusion_id)
{
  FusionLink *l;

  fusion_list_foreach (l, reactor->nodes)
    {
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

  spin_lock (&reactor->lock);

  fusion_list_foreach (l, reactor->nodes)
    {
      ReactorNode *node = (ReactorNode *) l;

      if (node->fusion_id == fusion_id)
        {
          fusion_list_remove (&reactor->nodes, l);
          break;
        }
    }

  spin_unlock (&reactor->lock);
}

static void
free_all_nodes (FusionReactor *reactor)

{
  FusionLink *l = reactor->nodes;

  while (l)
    {
      FusionLink *next = l->next;

      kfree (l);

      l = next;
    }

  reactor->nodes = NULL;
}
