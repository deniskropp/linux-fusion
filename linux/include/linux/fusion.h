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
 * Inheriting local count from other reference
 */
typedef struct {
     int                id;        /* own reference id */
     int                from;      /* id of the reference to inherit from */
} FusionRefInherit;

/*
 * Killing other fusionees (experimental)
 */
typedef struct {
     int fusion_id;    /* fusionee to kill, zero means all but ourself */
     int signal;       /* signal to be delivered, e.g. SIGTERM */
     int timeout_ms;   /* -1 means no timeout, 0 means infinite, otherwise the
                          max. time to wait until the fusionee(s) terminated */
} FusionKill;


typedef enum {
     FT_LOUNGE,
     FT_MESSAGING,
     FT_CALL,
     FT_REF,
     FT_SKIRMISH,
     FT_PROPERTY,
     FT_REACTOR
} FusionType;


/*
 * Set attributes like 'name' for an entry of the specified type.
 */
#define FUSION_ENTRY_INFO_NAME_LENGTH   24

typedef struct {
     FusionType type;
     int        id;

     char       name[FUSION_ENTRY_INFO_NAME_LENGTH];
} FusionEntryInfo;


typedef struct {
     struct {
          int major;
          int minor;
     } api;

     int fusion_id;
} FusionEnter;

#define FUSION_API_MAJOR   1  /* Increased if backward compatibility is dropped. */
#define FUSION_API_MINOR   1  /* Increased if new features are added. */


#define FUSION_ENTER               _IOR(FT_LOUNGE,    0x00, FusionEnter)
#define FUSION_KILL                _IOW(FT_LOUNGE,    0x01, FusionKill)

#define FUSION_ENTRY_SET_INFO      _IOW(FT_LOUNGE,    0x02, FusionEntryInfo)
#define FUSION_ENTRY_GET_INFO      _IOW(FT_LOUNGE,    0x03, FusionEntryInfo)

#define FUSION_SEND_MESSAGE        _IOW(FT_MESSAGING, 0x00, FusionSendMessage)

#define FUSION_CALL_NEW            _IOW(FT_CALL,      0x00, FusionCallNew)
#define FUSION_CALL_EXECUTE        _IOW(FT_CALL,      0x01, FusionCallExecute)
#define FUSION_CALL_RETURN         _IOW(FT_CALL,      0x02, FusionCallReturn)
#define FUSION_CALL_DESTROY        _IOW(FT_CALL,      0x03, int)

#define FUSION_REF_NEW             _IOW(FT_REF,       0x00, int)
#define FUSION_REF_UP              _IOW(FT_REF,       0x01, int)
#define FUSION_REF_UP_GLOBAL       _IOW(FT_REF,       0x02, int)
#define FUSION_REF_DOWN            _IOW(FT_REF,       0x03, int)
#define FUSION_REF_DOWN_GLOBAL     _IOW(FT_REF,       0x04, int)
#define FUSION_REF_ZERO_LOCK       _IOW(FT_REF,       0x05, int)
#define FUSION_REF_ZERO_TRYLOCK    _IOW(FT_REF,       0x06, int)
#define FUSION_REF_UNLOCK          _IOW(FT_REF,       0x07, int)
#define FUSION_REF_STAT            _IOW(FT_REF,       0x08, int)
#define FUSION_REF_WATCH           _IOW(FT_REF,       0x09, FusionRefWatch)
#define FUSION_REF_INHERIT         _IOW(FT_REF,       0x0A, FusionRefInherit)
#define FUSION_REF_DESTROY         _IOW(FT_REF,       0x0B, int)

#define FUSION_SKIRMISH_NEW        _IOW(FT_SKIRMISH,  0x00, int)
#define FUSION_SKIRMISH_PREVAIL    _IOW(FT_SKIRMISH,  0x01, int)
#define FUSION_SKIRMISH_SWOOP      _IOW(FT_SKIRMISH,  0x02, int)
#define FUSION_SKIRMISH_DISMISS    _IOW(FT_SKIRMISH,  0x03, int)
#define FUSION_SKIRMISH_DESTROY    _IOW(FT_SKIRMISH,  0x04, int)

#define FUSION_PROPERTY_NEW        _IOW(FT_PROPERTY,  0x00, int)
#define FUSION_PROPERTY_LEASE      _IOW(FT_PROPERTY,  0x01, int)
#define FUSION_PROPERTY_PURCHASE   _IOW(FT_PROPERTY,  0x02, int)
#define FUSION_PROPERTY_CEDE       _IOW(FT_PROPERTY,  0x03, int)
#define FUSION_PROPERTY_HOLDUP     _IOW(FT_PROPERTY,  0x04, int)
#define FUSION_PROPERTY_DESTROY    _IOW(FT_PROPERTY,  0x05, int)

#define FUSION_REACTOR_NEW         _IOW(FT_REACTOR,   0x00, int)
#define FUSION_REACTOR_ATTACH      _IOW(FT_REACTOR,   0x01, int)
#define FUSION_REACTOR_DETACH      _IOW(FT_REACTOR,   0x02, int)
#define FUSION_REACTOR_DISPATCH    _IOW(FT_REACTOR,   0x03, FusionReactorDispatch)
#define FUSION_REACTOR_DESTROY     _IOW(FT_REACTOR,   0x04, int)

#endif
