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
 
#include <linux/module.h>
#include <linux/config.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/miscdevice.h>
#include <linux/proc_fs.h>
#include <linux/init.h>
#include <asm/uaccess.h>

#include <linux/fusion.h>

#include "fusiondev.h"
#include "fusionee.h"
#include "property.h"
#include "reactor.h"
#include "ref.h"
#include "skirmish.h"

#define DEBUG(x...)  printk (KERN_DEBUG "Fusion: " x)

MODULE_LICENSE("GPL");

struct proc_dir_entry *proc_fusion_dir;

/******************************************************************************/

static int
fusion_open (struct inode *inode, struct file *file)
{
  int ret;
  int fusion_id;

  ret = fusionee_new (&fusion_id);
  if (ret)
    return ret;

  file->private_data = (void*) fusion_id;

  return 0;
}

static int
fusion_release (struct inode *inode, struct file *file)
{
  int fusion_id = (int) file->private_data;

  fusionee_destroy (fusion_id);

  return 0;
}

static ssize_t
fusion_read (struct file *file, char *buf, size_t count, loff_t *ppos)
{
  int fusion_id = (int) file->private_data;

  return fusionee_get_messages (fusion_id, buf, count,
                                !(file->f_flags & O_NONBLOCK));
}

static int
fusion_ioctl (struct inode *inode, struct file *file,
              unsigned int cmd, unsigned long arg)
{
  int id;
  int ret;
  int refs;
  int fusion_id = (int) file->private_data;
  FusionSendMessage send;
  FusionReactorDispatch dispatch;

  switch (cmd)
    {
    case FUSION_GET_ID:
      if (put_user (fusion_id, (int*) arg))
        return -EFAULT;

      break;

    case FUSION_SEND_MESSAGE:
      if (copy_from_user (&send, (FusionSendMessage*) arg, sizeof(send)))
        return -EFAULT;

      if (send.msg_size <= 0)
        return -EINVAL;

      /* message data > 64k should be stored in shared memory */
      if (send.msg_size > 0x10000)
        return -EMSGSIZE;

      return fusionee_send_message (send.fusion_id, FMT_SEND, send.msg_id,
                                    send.msg_size, send.msg_data);


    case FUSION_REF_NEW:
      ret = fusion_ref_new (&id);
      if (ret)
        return ret;

      if (put_user (id, (int*) arg))
        {
          fusion_ref_destroy (id);
          return -EFAULT;
        }
      break;

    case FUSION_REF_UP:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_ref_up (id, fusion_id);

    case FUSION_REF_UP_GLOBAL:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_ref_up (id, 0);

    case FUSION_REF_DOWN:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_ref_down (id, fusion_id);

    case FUSION_REF_DOWN_GLOBAL:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_ref_down (id, 0);

    case FUSION_REF_ZERO_LOCK:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_ref_zero_lock (id);

    case FUSION_REF_ZERO_TRYLOCK:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_ref_zero_trylock (id);

    case FUSION_REF_UNLOCK:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_ref_unlock (id);

    case FUSION_REF_STAT:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      ret = fusion_ref_stat (id, &refs);
      if (ret)
        return ret;

      return refs;

    case FUSION_REF_DESTROY:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_ref_destroy (id);


    case FUSION_SKIRMISH_NEW:
      ret = fusion_skirmish_new (&id);
      if (ret)
        return ret;

      if (put_user (id, (int*) arg))
        {
          fusion_skirmish_destroy (id);
          return -EFAULT;
        }
      break;

    case FUSION_SKIRMISH_PREVAIL:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_skirmish_prevail (id, fusion_id);

    case FUSION_SKIRMISH_SWOOP:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_skirmish_swoop (id, fusion_id);

    case FUSION_SKIRMISH_DISMISS:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_skirmish_dismiss (id, fusion_id);

    case FUSION_SKIRMISH_DESTROY:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_skirmish_destroy (id);


    case FUSION_PROPERTY_NEW:
      ret = fusion_property_new (&id);
      if (ret)
        return ret;

      if (put_user (id, (int*) arg))
        {
          fusion_property_destroy (id);
          return -EFAULT;
        }
      break;

    case FUSION_PROPERTY_LEASE:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_property_lease (id, fusion_id);

    case FUSION_PROPERTY_PURCHASE:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_property_purchase (id, fusion_id);

    case FUSION_PROPERTY_CEDE:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_property_cede (id, fusion_id);

    case FUSION_PROPERTY_DESTROY:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_property_destroy (id);


    case FUSION_REACTOR_NEW:
      ret = fusion_reactor_new (&id);
      if (ret)
        return ret;

      if (put_user (id, (int*) arg))
        {
          fusion_reactor_destroy (id);
          return -EFAULT;
        }
      break;

    case FUSION_REACTOR_ATTACH:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_reactor_attach (id, fusion_id);

    case FUSION_REACTOR_DETACH:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_reactor_detach (id, fusion_id);

    case FUSION_REACTOR_DISPATCH:
      if (copy_from_user (&dispatch,
                          (FusionReactorDispatch*) arg, sizeof(dispatch)))
        return -EFAULT;

      if (dispatch.msg_size <= 0)
        return -EINVAL;

      /* message data > 64k should be stored in shared memory */
      if (dispatch.msg_size > 0x10000)
        return -EMSGSIZE;

      return fusion_reactor_dispatch (dispatch.reactor_id,
                                      dispatch.self ? 0 : fusion_id,
                                      dispatch.msg_size, dispatch.msg_data);

    case FUSION_REACTOR_DESTROY:
      if (get_user (id, (int*) arg))
        return -EFAULT;

      return fusion_reactor_destroy (id);


    default:
      return -ENOTTY;
    }

  return 0;
}

static struct file_operations fusion_fops = {
  .owner   = THIS_MODULE,
  .open    = fusion_open,
  .release = fusion_release,
  .read    = fusion_read,
  .ioctl   = fusion_ioctl
};

static struct miscdevice fusion_miscdev = {
  .minor   = FUSION_MINOR,
  .name    = "fusion",
  .fops    = &fusion_fops
};

/******************************************************************************/

int __init
fusion_init(void)
{
  int ret;

  proc_fusion_dir = proc_mkdir ("fusion", NULL);

  ret = fusionee_init();
  if (ret)
    goto error_fusionee;

  ret = fusion_ref_init();
  if (ret)
    goto error_ref;

  ret = fusion_skirmish_init();
  if (ret)
    goto error_skirmish;

  ret = fusion_property_init();
  if (ret)
    goto error_property;

  ret = fusion_reactor_init();
  if (ret)
    goto error_reactor;

  ret = misc_register (&fusion_miscdev);
  if (ret)
    goto error_misc;

  return 0;


 error_misc:
  fusion_reactor_cleanup();

 error_reactor:
  fusion_property_cleanup();

 error_property:
  fusion_skirmish_cleanup();

 error_skirmish:
  fusion_ref_cleanup();

 error_ref:
  fusionee_cleanup();

 error_fusionee:
  return ret;
}

void __exit
fusion_exit(void)
{
  fusion_reactor_cleanup();
  fusion_property_cleanup();
  fusion_skirmish_cleanup();
  fusion_ref_cleanup();
  fusionee_cleanup();

  misc_deregister (&fusion_miscdev);
}

module_init(fusion_init);
module_exit(fusion_exit);
