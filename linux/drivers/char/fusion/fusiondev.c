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

static int
fusion_ioctl (struct inode *inode, struct file *file,
              unsigned int cmd, unsigned long arg)
{
  int id;
  int ret;
  int refs;
  int fusion_id = (int) file->private_data;

  switch (cmd)
    {
    case FUSION_GET_ID:
      put_user (fusion_id, (int*) arg);
      break;

    case FUSION_REF_NEW:
      ret = fusion_ref_new (&id);
      if (ret)
        return ret;

      put_user (id, (int*) arg);
      break;

    case FUSION_REF_UP:
      get_user (id, (int*) arg);

      return fusion_ref_up (id, fusion_id);

    case FUSION_REF_UP_GLOBAL:
      get_user (id, (int*) arg);

      return fusion_ref_up (id, 0);

    case FUSION_REF_DOWN:
      get_user (id, (int*) arg);

      return fusion_ref_down (id, fusion_id);

    case FUSION_REF_DOWN_GLOBAL:
      get_user (id, (int*) arg);

      return fusion_ref_down (id, 0);

    case FUSION_REF_ZERO_LOCK:
      get_user (id, (int*) arg);

      return fusion_ref_zero_lock (id);

    case FUSION_REF_ZERO_TRYLOCK:
      get_user (id, (int*) arg);

      return fusion_ref_zero_trylock (id);

    case FUSION_REF_UNLOCK:
      get_user (id, (int*) arg);

      return fusion_ref_unlock (id);

    case FUSION_REF_STAT:
      get_user (id, (int*) arg);

      ret = fusion_ref_stat (id, &refs);
      if (ret)
        return ret;

      return refs;

    case FUSION_REF_DESTROY:
      get_user (id, (int*) arg);

      return fusion_ref_destroy (id);

    case FUSION_SKIRMISH_NEW:
      ret = fusion_skirmish_new (&id);
      if (ret)
        return ret;

      put_user (id, (int*) arg);
      break;

    case FUSION_SKIRMISH_PREVAIL:
      get_user (id, (int*) arg);

      return fusion_skirmish_prevail (id, fusion_id);

    case FUSION_SKIRMISH_SWOOP:
      get_user (id, (int*) arg);

      return fusion_skirmish_swoop (id, fusion_id);

    case FUSION_SKIRMISH_DISMISS:
      get_user (id, (int*) arg);

      return fusion_skirmish_dismiss (id, fusion_id);

    case FUSION_SKIRMISH_DESTROY:
      get_user (id, (int*) arg);

      return fusion_skirmish_destroy (id);

    default:
      return -ENOTTY;
    }

  return 0;
}

static struct file_operations fusion_fops = {
  .owner   = THIS_MODULE,
  .ioctl   = fusion_ioctl,
  .open    = fusion_open,
  .release = fusion_release,
};

static struct miscdevice fusion_miscdev = {
  .minor   = FUSION_MINOR,
  .name    = "fusion",
  .fops    = &fusion_fops,
};

/******************************************************************************/

static int __init
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

  ret = misc_register (&fusion_miscdev);
  if (ret)
    goto error_misc;

  return 0;


 error_misc:
  fusion_skirmish_cleanup();

 error_skirmish:
  fusion_ref_cleanup();

 error_ref:
  fusionee_cleanup();

 error_fusionee:
  return ret;
}

static void __exit
fusion_exit(void)
{
  misc_deregister (&fusion_miscdev);
  
  fusion_skirmish_cleanup();
  fusion_ref_cleanup();
  fusionee_cleanup();
}

module_init(fusion_init);
module_exit(fusion_exit);
