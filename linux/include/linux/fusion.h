#ifndef __LINUX__FUSION_H__
#define __LINUX__FUSION_H__

#include <asm/ioctl.h>
#include <asm/types.h>

/*
 * Sending
 */
typedef struct {
  int         fusion_id;      /* recipient */

  int         msg_id;         /* optional message identifier */
  int         msg_size;       /* message size, must be greater than zero */
  const void *msg_data;       /* message data, must not be NULL */
} FusionSendMessage;

/*
 * Receiving
 */
typedef enum {
  FMT_SEND,
  FMT_CALL,                   /* msg_id is the call id */
  FMT_REACTOR                 /* msg_id is the reactor id */
} FusionMessageType;

typedef struct {
  FusionMessageType msg_type;

  int               msg_id;
  int               msg_size;

  /* message data follows */
} FusionReadMessage;

/*
 * Dispatching
 */
typedef struct {
  int         reactor_id;
  int         self;

  int         msg_size;       /* message size, must be greater than zero */
  const void *msg_data;       /* message data, must not be NULL */
} FusionReactorDispatch;

/*
 * Calling (synchronous RPC)
 */
typedef int (*FusionCallHandler) (int   caller,   /* fusion id of the caller */
                                  int   call_arg, /* optional call parameter */
                                  void *call_ptr, /* optional call parameter */
                                  void *ctx       /* optional handler context */
                                  );

typedef struct {
  int                call_id;   /* new call id returned */

  FusionCallHandler  handler;   /* function pointer of handler to install */
  void              *ctx;       /* optional handler context */
} FusionCallNew;

typedef struct {
  int   ret_val;              /* return value of the call */

  int   call_id;              /* each call has a fixed owner */
  
  int   call_arg;             /* optional int argument */
  void *call_ptr;             /* optional pointer argument (e.g. shared memory) */
} FusionCallExecute;


typedef struct {
  int   call_id;              /* id of currently executing call */

  int   val;                  /* value to return */
} FusionCallReturn;

typedef struct {
  FusionCallHandler  handler;   /* function pointer of handler to call */
  void              *ctx;       /* optional handler context */

  int                caller;    /* fusion id of the caller */
  int                call_arg;  /* optional call parameter */
  void              *call_ptr;  /* optional call parameter */
} FusionCallMessage;
  


#define FUSION_GET_ID                   _IOR('F', 0x00, sizeof(int))

#define FUSION_SEND_MESSAGE             _IOW('F', 0x01, sizeof(FusionSendMessage))

#define FUSION_CALL_NEW                 _IOW('F', 0x02, sizeof(FusionCallNew))
#define FUSION_CALL_EXECUTE             _IOW('F', 0x03, sizeof(FusionCallExecute))
#define FUSION_CALL_RETURN              _IOW('F', 0x04, sizeof(FusionCallReturn))
#define FUSION_CALL_DESTROY             _IOW('F', 0x05, sizeof(int))

#define FUSION_REF_NEW                  _IOW('F', 0x10, sizeof(int))
#define FUSION_REF_UP                   _IOW('F', 0x11, sizeof(int))
#define FUSION_REF_UP_GLOBAL            _IOW('F', 0x12, sizeof(int))
#define FUSION_REF_DOWN                 _IOW('F', 0x13, sizeof(int))
#define FUSION_REF_DOWN_GLOBAL          _IOW('F', 0x14, sizeof(int))
#define FUSION_REF_ZERO_LOCK            _IOW('F', 0x15, sizeof(int))
#define FUSION_REF_ZERO_TRYLOCK         _IOW('F', 0x16, sizeof(int))
#define FUSION_REF_UNLOCK               _IOW('F', 0x17, sizeof(int))
#define FUSION_REF_STAT                 _IOW('F', 0x18, sizeof(int))
#define FUSION_REF_DESTROY              _IOW('F', 0x19, sizeof(int))

#define FUSION_SKIRMISH_NEW             _IOW('F', 0x20, sizeof(int))
#define FUSION_SKIRMISH_PREVAIL         _IOW('F', 0x21, sizeof(int))
#define FUSION_SKIRMISH_SWOOP           _IOW('F', 0x22, sizeof(int))
#define FUSION_SKIRMISH_DISMISS         _IOW('F', 0x23, sizeof(int))
#define FUSION_SKIRMISH_DESTROY         _IOW('F', 0x24, sizeof(int))

#define FUSION_PROPERTY_NEW             _IOW('F', 0x30, sizeof(int))
#define FUSION_PROPERTY_LEASE           _IOW('F', 0x31, sizeof(int))
#define FUSION_PROPERTY_PURCHASE        _IOW('F', 0x32, sizeof(int))
#define FUSION_PROPERTY_CEDE            _IOW('F', 0x33, sizeof(int))
#define FUSION_PROPERTY_HOLDUP          _IOW('F', 0x34, sizeof(int))
#define FUSION_PROPERTY_DESTROY         _IOW('F', 0x35, sizeof(int))

#define FUSION_REACTOR_NEW              _IOW('F', 0x50, sizeof(int))
#define FUSION_REACTOR_ATTACH           _IOW('F', 0x51, sizeof(int))
#define FUSION_REACTOR_DETACH           _IOW('F', 0x52, sizeof(int))
#define FUSION_REACTOR_DISPATCH         _IOW('F', 0x53, sizeof(FusionReactorDispatch))
#define FUSION_REACTOR_DESTROY          _IOW('F', 0x54, sizeof(int))

#endif
