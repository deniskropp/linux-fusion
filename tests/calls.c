/*
 *      Fusion Kernel Module
 *
 *      (c) Copyright 2002  Convergence GmbH
 *
 *      Written by Denis Oliver Kropp <dok@directfb.org>
 *
 *
 *      This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sched.h>

#include <linux/fusion.h>

#include <pthread.h>


static int        fd;       /* File descriptor of the Fusion Kernel Device */
static pthread_t  receiver; /* Thread reading messages from the device. */

static int        counter = 0;

/*
 * A simple example call handler
 */
typedef int (*CallHandler)(int   caller,   /* fusion id of the caller */
                           int   call_arg, /* optional call parameter */
                           void *call_ptr, /* optional call parameter */
                           void *ctx       /* optional handler context */);

static int
call_handler (int   caller,   /* fusion id of the caller */
              int   call_arg, /* optional call parameter */
              void *call_ptr, /* optional call parameter */
              void *ctx       /* optional handler context */)
{
  counter++;

  return 0;
}

/*
 * Generic call dispatcher
 */
static void
process_call_message (int call_id, FusionCallMessage *msg)
{
  FusionCallReturn call_ret;
  CallHandler      handler = msg->handler;

  call_ret.call_id = call_id;
  call_ret.val     = handler (msg->caller,
                              msg->call_arg,
                              msg->call_ptr,
                              msg->ctx);

  if (ioctl (fd, FUSION_CALL_RETURN, &call_ret))
    perror ("FUSION_CALL_RETURN");
}

/*
 * Message receiver thread
 */
static void *
receiver_thread (void *arg)
{
  int  len;
  char buf[1024];

  /* Read as many messages as possible at once. */
  while ((len = read (fd, buf, 1024)) > 0 || errno == EINTR)
    {
      /* Current position within the buffer. */
      char *buf_p = buf;

      /* Shutdown? */
      pthread_testcancel();

      /* Possibly interrupted during blocking read. */
      if (len <= 0)
        continue;

      /* While there are still messages in the buffer... */
      while (buf_p < buf + len)
        {
          /* Header of next message is at the current buffer location. */
          FusionReadMessage *header = (FusionReadMessage*) buf_p;

          /* Its data follows immediately. */
          void *data = buf_p + sizeof(FusionReadMessage);

          /* Process the message depending on its type (origin). */
          switch (header->msg_type)
            {
            case FMT_CALL:
              process_call_message (header->msg_id, data);
              break;
            default:
              break;
            }

          /* Shutdown? */
          pthread_testcancel();

          /* Next message header follows immediately. */
          buf_p = data + header->msg_size;
        }
    }

  perror ("receiver thread failure");

  return NULL;
}

int
main (int argc, char *argv[])
{
  long              d = 0;
  FusionCallNew     call_new;
  FusionCallExecute call_exec;
  struct timeval    t1, t2;

  FusionEnter enter = {{ FUSION_API_MAJOR, FUSION_API_MINOR }};

  /* Open the Fusion Kernel Device. */
  fd = open ("/dev/fusion/0", O_RDWR);
  if (fd < 0)
    {
      perror ("opening /dev/fusion failed");
      return -1;
    }

  /* Query our fusion id. */
  if (ioctl (fd, FUSION_ENTER, &enter))
    {
      perror ("FUSION_ENTER failed");
      close (fd);
      return -2;
    }

  /* Install the call handler. */
  call_new.handler = call_handler;
  call_new.ctx     = NULL;

  if (ioctl (fd, FUSION_CALL_NEW, &call_new))
    {
      perror ("FUSION_CALL_NEW failed");
      close (fd);
      return -3;
    }

  /* Start the receiver thread. */
  pthread_create (&receiver, NULL, receiver_thread, NULL);

  /* Flush disk buffers and wait for the receiver being up. */
  sync();
  usleep (100000);

  /* Specify call id to execute. */
  call_exec.call_id = call_new.call_id;

  /* Stop time before call loop. */
  gettimeofday (&t1, NULL);

  /* Do as many calls as possible (in 3 seconds). */
  while (d < 3000)
    {
      if (ioctl (fd, FUSION_CALL_EXECUTE, &call_exec))
        perror ("FUSION_CALL_EXECUTE");

      /* Stop time every 1000 calls. */
      if (counter % 1000 == 0)
        {
          gettimeofday (&t2, NULL);

          d = (t2.tv_sec - t1.tv_sec) * 1000 + (t2.tv_usec - t1.tv_usec) / 1000;
        }
    }

  /* Print message. */
  printf ("Executed %ld synchronous RPC calls per second.\n",
          counter * 1000 / d);

  /* Stop the receiver. */
  pthread_cancel (receiver);
  pthread_join (receiver, NULL);

  /* Destroy the call. */
  if (ioctl (fd, FUSION_CALL_DESTROY, &call_new.call_id))
    perror ("FUSION_CALL_DESTROY");

  /* Close the Fusion Kernel Device. */
  close (fd);

  return 0;
}
