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


typedef struct {
     FusionLink        link;

     spinlock_t        lock;

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

static Fusionee *lookup_fusionee (FusionDev *dev, int id);

static Fusionee *lock_fusionee   (FusionDev *dev, int id);
static void      unlock_fusionee (Fusionee *fusionee);

/******************************************************************************/

static int
fusionees_read_proc(char *buf, char **start, off_t offset,
                    int len, int *eof, void *private)
{
     FusionLink *l;
     FusionDev  *dev     = private;
     int         written = 0;

     spin_lock (&dev->fusionee.lock);

     fusion_list_foreach (l, dev->fusionee.list) {
          Fusionee *fusionee = (Fusionee*) l;

          written += sprintf(buf+written, "(%5d) 0x%08x (%4d messages waiting, %5d received, %5d sent)\n",
                             fusionee->pid, fusionee->id, fusionee->messages.count, fusionee->rcv_total, fusionee->snd_total);
          if (written < offset) {
               offset -= written;
               written = 0;
          }

          if (written >= len)
               break;
     }

     spin_unlock (&dev->fusionee.lock);

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

     dev->fusionee.lock = SPIN_LOCK_UNLOCKED;
     
     create_proc_read_entry("fusionees", 0, dev->proc_dir,
                            fusionees_read_proc, dev);

     return 0;
}

void
fusionee_deinit (FusionDev *dev)
{
     FusionLink *l;

     spin_lock (&dev->fusionee.lock);

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

     spin_unlock (&dev->fusionee.lock);
}

/******************************************************************************/

int
fusionee_new (FusionDev *dev, int *id)
{
     Fusionee *fusionee;

     fusionee = kmalloc (sizeof(Fusionee), GFP_ATOMIC);
     if (!fusionee)
          return -ENOMEM;

     memset (fusionee, 0, sizeof(Fusionee));

     spin_lock (&dev->fusionee.lock);

     fusionee->id   = ++dev->fusionee.last_id;
     fusionee->pid  = current->pid;
     fusionee->lock = SPIN_LOCK_UNLOCKED;

     init_waitqueue_head (&fusionee->wait);

     fusion_list_prepend (&dev->fusionee.list, &fusionee->link);

     spin_unlock (&dev->fusionee.lock);

     *id = fusionee->id;

     return 0;
}

int
fusionee_send_message (FusionDev *dev, int id, int recipient,
                       FusionMessageType msg_type, int msg_id,
                       int msg_size, const void *msg_data)
{
     Message  *message;
     Fusionee *sender;
     Fusionee *fusionee = lock_fusionee (dev, recipient);

     if (!fusionee)
          return -EINVAL;

     sender = lock_fusionee (dev, id);
     if (!sender) {
          unlock_fusionee (fusionee);
          return -EIO;
     }

     message = kmalloc (sizeof(Message) + msg_size, GFP_ATOMIC);
     if (!message) {
          unlock_fusionee (sender);
          unlock_fusionee (fusionee);
          return -ENOMEM;
     }

     message->data = message + 1;

     if (msg_type == FMT_CALL)
          memcpy (message->data, msg_data, msg_size);
     else if (copy_from_user (message->data, msg_data, msg_size)) {
          kfree (message);
          unlock_fusionee (sender);
          unlock_fusionee (fusionee);
          return -EFAULT;
     }

     message->type = msg_type;
     message->id   = msg_id;
     message->size = msg_size;

     fusion_fifo_put (&fusionee->messages, &message->link);

     fusionee->rcv_total++;
     sender->snd_total++;

     wake_up_interruptible_all (&fusionee->wait);

     unlock_fusionee (sender);
     unlock_fusionee (fusionee);

     return 0;
}

int
fusionee_get_messages (FusionDev *dev,
                       int id, void *buf, int buf_size, bool block)
{
     int       written  = 0;
     Fusionee *fusionee = lock_fusionee (dev, id);

     if (!fusionee)
          return -EINVAL;

     while (!fusionee->messages.count) {
          if (!block) {
               unlock_fusionee (fusionee);
               return -EAGAIN;
          }

          fusion_sleep_on (&fusionee->wait, &fusionee->lock, 0);

          if (signal_pending(current))
               return -ERESTARTSYS;

          fusionee = lock_fusionee (dev, id);
          if (!fusionee)
               return -EINVAL;
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
     Fusionee *fusionee = lock_fusionee (dev, id);

     if (!fusionee)
          return -EINVAL;

     unlock_fusionee (fusionee);


     poll_wait (file, &fusionee->wait, wait);


     fusionee = lock_fusionee (dev, id);

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
          FusionLink *l;
          Fusionee   *fusionee = lookup_fusionee (dev, id);
          int         killed   = 0;

          if (!fusionee)
               return -EINVAL;

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
                         spin_unlock (&dev->fusionee.lock);
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
               return -ERESTARTSYS;
     }

     spin_unlock (&dev->fusionee.lock);

     return 0;
}

int
fusionee_destroy (FusionDev *dev, int id)
{
     Fusionee *fusionee = lookup_fusionee (dev, id);

     if (!fusionee)
          return -EINVAL;

     spin_lock (&fusionee->lock);

     fusion_list_remove (&dev->fusionee.list, &fusionee->link);

     wake_up_interruptible_all (&dev->fusionee.wait);

     spin_unlock (&dev->fusionee.lock);


     fusion_call_destroy_all (dev, id);
     fusion_skirmish_dismiss_all (dev, id);
     fusion_reactor_detach_all (dev, id);
     fusion_property_cede_all (dev, id);
     fusion_ref_clear_all_local (dev, id);

     while (fusionee->messages.count) {
          Message *message = (Message*) fusion_fifo_get (&fusionee->messages);

          kfree (message);
     }

     spin_unlock (&fusionee->lock);

     kfree (fusionee);

     return 0;
}

/******************************************************************************/

static Fusionee *
lookup_fusionee (FusionDev *dev, int id)
{
     FusionLink *l;

     spin_lock (&dev->fusionee.lock);

     fusion_list_foreach (l, dev->fusionee.list) {
          Fusionee *fusionee = (Fusionee *) l;

          if (fusionee->id == id)
               return fusionee;
     }

     spin_unlock (&dev->fusionee.lock);

     return NULL;
}

static Fusionee *
lock_fusionee (FusionDev *dev, int id)
{
     Fusionee *fusionee = lookup_fusionee (dev, id);

     if (fusionee) {
          spin_lock (&fusionee->lock);
          spin_unlock (&dev->fusionee.lock);
     }

     return fusionee;
}

static void
unlock_fusionee (Fusionee *fusionee)
{
     spin_unlock (&fusionee->lock);
}

