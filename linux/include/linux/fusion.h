#ifndef __LINUX__FUSION_H__
#define __LINUX__FUSION_H__

#include <asm/ioctl.h>
#include <asm/types.h>

#define FUSION_GET_ID                   _IOR('F', 0x00, sizeof(int))

#define FUSION_REF_NEW                  _IOW('F', 0x10, sizeof(int))
#define FUSION_REF_UP                   _IOW('F', 0x11, sizeof(int))
#define FUSION_REF_UP_GLOBAL            _IOW('F', 0x12, sizeof(int))
#define FUSION_REF_DOWN                 _IOW('F', 0x13, sizeof(int))
#define FUSION_REF_DOWN_GLOBAL          _IOW('F', 0x14, sizeof(int))
#define FUSION_REF_ZERO_LOCK            _IOW('F', 0x15, sizeof(int))
#define FUSION_REF_ZERO_TRYLOCK         _IOW('F', 0x16, sizeof(int))
#define FUSION_REF_UNLOCK               _IOW('F', 0x17, sizeof(int))
#define FUSION_REF_STAT                 _IOR('F', 0x18, sizeof(int))
#define FUSION_REF_DESTROY              _IOW('F', 0x19, sizeof(int))

#define FUSION_SKIRMISH_NEW             _IOW('F', 0x20, sizeof(int))
#define FUSION_SKIRMISH_PREVAIL         _IOW('F', 0x21, sizeof(int))
#define FUSION_SKIRMISH_SWOOP           _IOW('F', 0x22, sizeof(int))
#define FUSION_SKIRMISH_DISMISS         _IOW('F', 0x23, sizeof(int))
#define FUSION_SKIRMISH_DESTROY         _IOW('F', 0x24, sizeof(int))

#endif

