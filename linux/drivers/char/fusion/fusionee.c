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
#include <asm/uaccess.h>

#include <linux/fusion.h>

#include "call.h"
#include "fifo.h"
#include "list.h"
#include "fusiondev.h"
#include "fusionee.h"
#include "property.h"
#include "reactor.h"
#include "ref.h"
#include "skirmish.h"

#if 0
#define DEBUG(x...)  printk (KERN_DEBUG "Fusion: " x)
#else
#define DEBUG(x...)  do {} while (0)
#endif

typedef struct {
     FusionLink        link;

     struct semaphore  lock;

     int               id;
     int               pid;

     FusionFifo        messages;

     int               rcv_total;  /* Total number of messages received. */
     int               snd_total;  /* Total number of messages sent. */

     wait_queue_head_t wait;
} Fusionee;

typedef struct {
     FusionLink         link;

     FusionMessageType  type;
     int                id;
     int                size;
     void              *data;
} Message;

/******************************************************************************/

static int  lookup_fusionee (FusionDev *dev, int id, Fusionee **ret_fusionee);
static int  lock_fusionee   (FusionDev *dev, int id, Fusionee **ret_fusionee);
static void unlock_fusionee (Fusionee *fusionee);

/******************************************************************************/

static int
fusionees_read_proc(char *buf, char **start, off_t offset,
                    int len, int *eof, void *private)
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     if (down_interruptible (&dev->fusionee.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->fusionee.list) {
          Fusionee *fusionee = (Fusionee*) l;

          written += sprintf(buf+written, "(%5d) 0x%08x (%4d messages waiting, %7d received, %7d sent)\n",
                             fusionee->pid, fusionee->id, fusionee->messages.count, fusionee->rcv_total, fusionee->snd_total);
          if (written < offset) {
               offset -= written;
               written = 0;
          }

          if (written >= len)
               break;
     }

     up (&dev->fusionee.lock);

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
fusionee_init (FusionDev *dev)
{
     init_waitqueue_head (&dev->fusionee.wait);

     init_MUTEX (&dev->fusionee.lock);

     create_proc_read_entry("fusionees", 0, dev->proc_dir,
                            fusionees_read_proc, dev);

     return 0;
}

void
fusionee_deinit (FusionDev *dev)
{
     FusionLink *l;

     down (&dev->fusionee.lock);

     remove_proc_entry ("fusionees", dev->proc_dir);

     l = dev->fusionee.list;
     while (l) {
          FusionLink *next     = l->next;
          Fusionee   *fusionee = (Fusionee *) l;

          while (fusionee->messages.count) {
               Message *message = (Message*) fusion_fifo_get (&fusionee->messages);

               kfree (message);
          }

          kfree (fusionee);

          l = next;
     }

     up (&dev->fusionee.lock);
}

/******************************************************************************/

int
fusionee_new (FusionDev *dev, int *id)
{
     Fusionee *fusionee;

     fusionee = kmalloc (sizeof(Fusionee), GFP_KERNEL);
     if (!fusionee)
          return -ENOMEM;

     memset (fusionee, 0, sizeof(Fusionee));

     if (down_interruptible (&dev->fusionee.lock)) {
          kfree (fusionee);
          return -EINTR;
     }

     fusionee->id  = ++dev->fusionee.last_id;
     fusionee->pid = current->pid;

     init_MUTEX (&fusionee->lock);

     init_waitqueue_head (&fusionee->wait);

     fusion_list_prepend (&dev->fusionee.list, &fusionee->link);

     up (&dev->fusionee.lock);

     *id = fusionee->id;

     return 0;
}

int
fusionee_send_message (FusionDev *dev, int id, int recipient,
                       FusionMessageType msg_type, int msg_id,
                       int msg_size, const void *msg_data)
{
     int       ret;
     Message  *message;
     Fusionee *sender   = NULL;
     Fusionee *fusionee;

     DEBUG( "fusionee_send_message (%d -> %d, type %d, id %d, size %d)\n",
            id, recipient, msg_type, msg_id, msg_size );

     ret = lock_fusionee (dev, recipient, &fusionee);
     if (ret)
          return ret;

     if (id) {
          if (id == recipient) {
               sender = fusionee;
          }
          else {
               ret = lock_fusionee (dev, id, &sender);
               if (ret) {
                    unlock_fusionee (fusionee);
                    return ret == -EINVAL ? -EIO : ret;
               }
          }
     }

     message = kmalloc (sizeof(Message) + msg_size, GFP_KERNEL);
     if (!message) {
          if (sender && sender != fusionee)
               unlock_fusionee (sender);
          unlock_fusionee (fusionee);
          return -ENOMEM;
     }

     message->data = message + 1;

     if (msg_type == FMT_CALL)
          memcpy (message->data, msg_data, msg_size);
     else if (copy_from_user (message->data, msg_data, msg_size)) {
          kfree (message);
          if (sender && sender != fusionee)
               unlock_fusionee (sender);
          unlock_fusionee (fusionee);
          return -EFAULT;
     }

     message->type = msg_type;
     message->id   = msg_id;
     message->size = msg_size;

     fusion_fifo_put (&fusionee->messages, &message->link);

     fusionee->rcv_total++;
     if (sender)
          sender->snd_total++;

     wake_up_interruptible_all (&fusionee->wait);

     if (sender && sender != fusionee)
          unlock_fusionee (sender);

     unlock_fusionee (fusionee);

     return 0;
}

int
fusionee_get_messages (FusionDev *dev,
                       int id, void *buf, int buf_size, bool block)
{
     int       ret;
     int       written  = 0;
     Fusionee *fusionee;

     ret = lock_fusionee (dev, id, &fusionee);
     if (ret)
          return ret;

     while (!fusionee->messages.count) {
          if (!block) {
               unlock_fusionee (fusionee);
               return -EAGAIN;
          }

          fusion_sleep_on (&fusionee->wait, &fusionee->lock, 0);

          if (signal_pending(current))
               return -EINTR;

          ret = lock_fusionee (dev, id, &fusionee);
          if (ret)
               return ret;
     }

     while (fusionee->messages.count) {
          FusionReadMessage  header;
          Message           *message = (Message*) fusionee->messages.first;
          int                bytes   = message->size + sizeof(header);

          if (bytes > buf_size) {
               if (!written) {
                    unlock_fusionee (fusionee);
                    return -EMSGSIZE;
               }

               break;
          }

          header.msg_type = message->type;
          header.msg_id   = message->id;
          header.msg_size = message->size;

          if (copy_to_user (buf, &header, sizeof(header)) ||
              copy_to_user (buf + sizeof(header), message->data, message->size)) {
               unlock_fusionee (fusionee);
               return -EFAULT;
          }

          written  += bytes;
          buf      += bytes;
          buf_size -= bytes;

          fusion_fifo_get (&fusionee->messages);

          kfree (message);
     }

     unlock_fusionee (fusionee);

     return written;
}

unsigned int
fusionee_poll (FusionDev *dev, int id, struct file *file, poll_table * wait)
{
     int       ret;
     Fusionee *fusionee;

     ret = lock_fusionee (dev, id, &fusionee);
     if (ret)
          return ret;

     unlock_fusionee (fusionee);


     poll_wait (file, &fusionee->wait, wait);


     ret = lock_fusionee (dev, id, &fusionee);
     if (ret)
          return ret;

     if (!fusionee)
          return -EINVAL;

     if (fusionee->messages.count) {
          unlock_fusionee (fusionee);

          return POLLIN | POLLRDNORM;
     }

     unlock_fusionee (fusionee);

     return 0;
}

int
fusionee_kill (FusionDev *dev, int id, int target, int signal, int timeout_ms)
{
     long timeout = -1;

     while (true) {
          int         ret;
          FusionLink *l;
          Fusionee   *fusionee;
          int         killed   = 0;

          ret = lookup_fusionee (dev, id, &fusionee);
          if (ret)
               return ret;

          fusion_list_foreach (l, dev->fusionee.list) {
               Fusionee *f = (Fusionee*) l;

               if (f->id != id && (!target || target == f->id)) {
                    kill_proc (f->pid, signal, 0);
                    killed++;
               }
          }

          if (!killed || timeout_ms < 0)
               break;

          if (timeout_ms) {
               switch (timeout) {
                    case 0:  /* timed out */
                         up (&dev->fusionee.lock);
                         return -ETIMEDOUT;

                    case -1: /* setup timeout */
                         timeout = (timeout_ms * HZ + 500) / 1000;
                         if (!timeout)
                              timeout = 1;

                         /* fall through */

                    default:
                         fusion_sleep_on (&dev->fusionee.wait,
                                          &dev->fusionee.lock, &timeout);
                         break;
               }
          }
          else
               fusion_sleep_on (&dev->fusionee.wait, &dev->fusionee.lock, NULL);

          if (signal_pending(current))
               return -EINTR;
     }

     up (&dev->fusionee.lock);

     return 0;
}

int
fusionee_destroy (FusionDev *dev, int id)
{
     int       ret;
     Fusionee *fusionee;

     ret = lookup_fusionee (dev, id, &fusionee);
     if (ret)
          return ret;

     down (&fusionee->lock);

     fusion_list_remove (&dev->fusionee.list, &fusionee->link);

     wake_up_interruptible_all (&dev->fusionee.wait);

     up (&dev->fusionee.lock);


     fusion_call_destroy_all (dev, id);
     fusion_skirmish_dismiss_all (dev, id);
     fusion_reactor_detach_all (dev, id);
     fusion_property_cede_all (dev, id);
     fusion_ref_clear_all_local (dev, id);

     while (fusionee->messages.count) {
          Message *message = (Message*) fusion_fifo_get (&fusionee->messages);

          kfree (message);
     }

     up (&fusionee->lock);

     kfree (fusionee);

     return ret;
}

/******************************************************************************/

static int
lookup_fusionee (FusionDev *dev, int id, Fusionee **ret_fusionee)
{
     FusionLink *l;

     if (down_interruptible (&dev->fusionee.lock))
          return -EINTR;

     fusion_list_foreach (l, dev->fusionee.list) {
          Fusionee *fusionee = (Fusionee *) l;

          if (fusionee->id == id) {
               *ret_fusionee = fusionee;
               return 0;
          }
     }

     up (&dev->fusionee.lock);

     return -EINVAL;
}

static int
lock_fusionee (FusionDev *dev, int id, Fusionee **ret_fusionee)
{
     int       ret;
     Fusionee *fusionee;

     ret = lookup_fusionee (dev, id, &fusionee);
     if (ret)
          return ret;

     if (fusionee) {
          fusion_list_move_to_front (&dev->fusionee.list, &fusionee->link);

          if (down_interruptible (&fusionee->lock)) {
               up (&dev->fusionee.lock);
               return -EINTR;
          }

          up (&dev->fusionee.lock);
     }

     *ret_fusionee = fusionee;

     return 0;
}

static void
unlock_fusionee (Fusionee *fusionee)
{
     up (&fusionee->lock);
}

