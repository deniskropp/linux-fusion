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
typedef struct {
     int                call_id;   /* new call id returned */

     void              *handler;   /* function pointer of handler to install */
     void              *ctx;       /* optional handler context */
} FusionCallNew;

typedef struct {
     int   ret_val;              /* return value of the call */

     int   call_id;              /* id of the requested call,
                                    each call has a fixed owner */

     int   call_arg;             /* optional int argument */
     void *call_ptr;             /* optional pointer argument (shared memory) */
} FusionCallExecute;

typedef struct {
     int   call_id;              /* id of currently executing call */

     int   val;                  /* value to return */
} FusionCallReturn;

typedef struct {
     void              *handler;   /* function pointer of handler to call */
     void              *ctx;       /* optional handler context */

     int                caller;    /* fusion id of the caller
                                      or zero if the call comes from Fusion */
     int                call_arg;  /* optional call parameter */
     void              *call_ptr;  /* optional call parameter */
} FusionCallMessage;

/*
 * Watching a reference
 *
 * This information is needed to have a specific call being executed if the
 * reference count reaches zero. Currently one watch per reference is allowed.
 *
 * The call is made by Fusion and therefor has a caller id of zero.
 * 
 */
typedef struct {
     int                id;        /* id of the reference to watch */

     int                call_id;   /* id of the call to execute */
     int                call_arg;  /* optional call parameter, e.g. the id of a
                                      user space resource associated with that
                                      reference */
} FusionRefWatch;

/*
 * Killing other fusionees (experimental)
 */
typedef struct {
     int fusion_id;    /* fusionee to kill, zero means all but ourself */
     int signal;       /* signal to be delivered, e.g. SIGTERM */
     int timeout_ms;   /* -1 means no timeout, 0 means infinite, otherwise the
                          max. time to wait until the fusionee(s) terminated */
} FusionKill;


#define FUSION_GET_ID                   _IOR('F', 0x00, int)

#define FUSION_SEND_MESSAGE             _IOW('F', 0x01, FusionSendMessage)

#define FUSION_CALL_NEW                 _IOW('F', 0x02, FusionCallNew)
#define FUSION_CALL_EXECUTE             _IOW('F', 0x03, FusionCallExecute)
#define FUSION_CALL_RETURN              _IOW('F', 0x04, FusionCallReturn)
#define FUSION_CALL_DESTROY             _IOW('F', 0x05, int)

#define FUSION_KILL                     _IOW('F', 0x06, FusionKill)

#define FUSION_REF_NEW                  _IOW('F', 0x07, int)
#define FUSION_REF_UP                   _IOW('F', 0x08, int)
#define FUSION_REF_UP_GLOBAL            _IOW('F', 0x09, int)
#define FUSION_REF_DOWN                 _IOW('F', 0x0A, int)
#define FUSION_REF_DOWN_GLOBAL          _IOW('F', 0x0B, int)
#define FUSION_REF_ZERO_LOCK            _IOW('F', 0x0C, int)
#define FUSION_REF_ZERO_TRYLOCK         _IOW('F', 0x0D, int)
#define FUSION_REF_UNLOCK               _IOW('F', 0x0E, int)
#define FUSION_REF_STAT                 _IOW('F', 0x0F, int)
#define FUSION_REF_WATCH                _IOW('F', 0x10, FusionRefWatch)
#define FUSION_REF_DESTROY              _IOW('F', 0x11, int)

#define FUSION_SKIRMISH_NEW             _IOW('F', 0x12, int)
#define FUSION_SKIRMISH_PREVAIL         _IOW('F', 0x13, int)
#define FUSION_SKIRMISH_SWOOP           _IOW('F', 0x14, int)
#define FUSION_SKIRMISH_DISMISS         _IOW('F', 0x15, int)
#define FUSION_SKIRMISH_DESTROY         _IOW('F', 0x16, int)

#define FUSION_PROPERTY_NEW             _IOW('F', 0x17, int)
#define FUSION_PROPERTY_LEASE           _IOW('F', 0x18, int)
#define FUSION_PROPERTY_PURCHASE        _IOW('F', 0x19, int)
#define FUSION_PROPERTY_CEDE            _IOW('F', 0x1A, int)
#define FUSION_PROPERTY_HOLDUP          _IOW('F', 0x1B, int)
#define FUSION_PROPERTY_DESTROY         _IOW('F', 0x1C, int)

#define FUSION_REACTOR_NEW              _IOW('F', 0x1D, int)
#define FUSION_REACTOR_ATTACH           _IOW('F', 0x1E, int)
#define FUSION_REACTOR_DETACH           _IOW('F', 0x1F, int)
#define FUSION_REACTOR_DISPATCH         _IOW('F', 0x20, FusionReactorDispatch)
#define FUSION_REACTOR_DESTROY          _IOW('F', 0x21, int)

#endif
