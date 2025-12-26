#include <stdio.h>
#include <string.h>
#include "lib/src/libultra_internal.h"
#include "macros.h"

#undef CONT_C
#undef CONT_B
#undef CONT_A
#undef CONT_START
#undef CONT_DPAD_UP
#undef CONT_DPAD_DOWN
#undef CONT_DPAD_LEFT
#undef CONT_DPAD_RIGHT
#undef CONT_Z
#undef CONT_Y
#undef CONT_X
#undef CONT_D
#undef CONT_DPAD2_UP
#undef CONT_DPAD2_DOWN
#undef CONT_DPAD2_LEFT
#undef CONT_DPAD2_RIGHT

extern OSMgrArgs piMgrArgs;

u64 osClockRate = 62500000;
void n64_memcpy(void* dst, const void* src, size_t size);

s32 osPiStartDma(UNUSED OSIoMesg *mb, UNUSED s32 priority, UNUSED s32 direction,
                 uintptr_t devAddr, void *vAddr, size_t nbytes,
                 UNUSED OSMesgQueue *mq) {
    n64_memcpy(vAddr, (const void *) devAddr, nbytes);
    return 0;
}

void osCreateMesgQueue(OSMesgQueue *mq, OSMesg *msgBuf, s32 count) {
    mq->validCount = 0;
    mq->first = 0;
    mq->msgCount = count;
    mq->msg = msgBuf;
    return;
}

void osSetEventMesg(UNUSED OSEvent e, UNUSED OSMesgQueue *mq, UNUSED OSMesg msg) {
}
s32 osJamMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg msg, UNUSED s32 flag) {
    return 0;
}
s32 osSendMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg msg, UNUSED s32 flag) {
#ifdef VERSION_EU
    s32 index;
    if (mq->validCount >= mq->msgCount) {
        return -1;
    }
    index = (mq->first + mq->validCount) % mq->msgCount;
    mq->msg[index] = msg;
    mq->validCount++;
#endif
    return 0;
}
s32 osRecvMesg(UNUSED OSMesgQueue *mq, UNUSED OSMesg *msg, UNUSED s32 flag) {
#if VERSION_EU
    if (mq->validCount == 0) {
        return -1;
    }
    if (msg != NULL) {
        *msg = *(mq->first + mq->msg);
    }
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
#endif
    return 0;
}

uintptr_t osVirtualToPhysical(void *addr) {
    return (uintptr_t) addr;
}

void osCreateViManager(UNUSED OSPri pri) {
}
void osViSetMode(UNUSED OSViMode *mode) {
}
void osViSetEvent(UNUSED OSMesgQueue *mq, UNUSED OSMesg msg, UNUSED u32 retraceCount) {
}
void osViBlack(UNUSED u8 active) {
}
void osViSetSpecialFeatures(UNUSED u32 func) {
}
void osViSwapBuffer(UNUSED void *vaddr) {
}

#if defined(TARGET_PSP)
#include <psprtc.h>
OSTime osGetTime(void) {
    unsigned long long int temp;
    sceRtcGetCurrentTick(&temp);
    return (unsigned int) (temp);
}
#else 
OSTime osGetTime(void) {
    return 0;
}
#endif

void osWritebackDCacheAll(void) {
}

void osWritebackDCache(UNUSED void *a, UNUSED size_t b) {
}

void osInvalDCache(UNUSED void *a, UNUSED size_t b) {
}

u32 osGetCount(void) {
    static u32 counter;
    return counter++;
}

s32 osAiSetFrequency(u32 freq) {
    u32 a1;
    s32 a2;
    u32 D_8033491C;

#ifdef VERSION_EU
    D_8033491C = 0x02E6025C;
#else
    D_8033491C = 0x02E6D354;
#endif

    a1 = D_8033491C / (float) freq + .5f;

    if (a1 < 0x84) {
        return -1;
    }

    a2 = (a1 / 66) & 0xff;
    if (a2 > 16) {
        a2 = 16;
    }

    return D_8033491C / (s32) a1;
}

#include "120_star_save.h"

#include <kos.h>
static file_t eeprom_file = -1;
static mutex_t eeprom_lock;
static int eeprom_init = 0;
#if 0
// thanks @zcrc
#include <kos/oneshot_timer.h>
/* 2s timer, to delay closing the VMU file.
 * This is because the emulator might open/modify/close often, and we want the
 * VMU VFS driver to only write to the VMU once we're done modifying the file. */

 static oneshot_timer_t *timer;

void eeprom_flush(UNUSED void *arg) {
	mutex_lock_scoped(&eeprom_lock);

    if (eeprom_file != -1) {
        fs_close(eeprom_file);
        eeprom_file = -1;
    }
}
#endif
uint8_t icondata[512]; 
char *ico_fn = "/rd/mario.ico";

s32 osEepromProbe(UNUSED OSMesgQueue* mq) {
	maple_device_t *vmudev = NULL;

    if (!eeprom_init) {
        mutex_init(&eeprom_lock, MUTEX_TYPE_NORMAL);
        eeprom_init = 1;
//        timer = oneshot_timer_create(eeprom_flush, NULL, 2000);
    }

    vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
	if (!vmudev) {
		//dbgio_printf("eeprom probe: could not enum\n");
        vid_border_color(255,0,0);
		return 0;
	}
   // printf("EEPROM PROBE:\n");
    vid_border_color(0,0,255);
    eeprom_file = fs_open("/vmu/a1/mario64.rec", O_RDONLY | O_META);
	if (-1 == eeprom_file) {
       // printf("\t/vmu/a1/mario64.rec did not exist on VMU a1\n");
        eeprom_file = fs_open("/vmu/a1/mario64.rec", O_RDWR | O_CREAT | O_META);
		if (-1 == eeprom_file) {
			printf("\tcant open /vmu/a1/mario64.rec for rdwr|creat\n");
            vid_border_color(255,0,0);
			return 1;
		}

        vmu_pkg_t pkg;
        memset(&pkg, 0, sizeof(vmu_pkg_t));
        strcpy(pkg.desc_short,"Saved Games");
        strcpy(pkg.desc_long, "Super Mario 64");
        strcpy(pkg.app_id, "Super Mario 64");
        pkg.icon_cnt = 1;
        pkg.icon_data = icondata;
        pkg.icon_anim_speed = 0;
        pkg.data_len = 512;
        pkg.data = eeprom;
        vmu_pkg_load_icon(&pkg, ico_fn);
        uint8_t *pkg_out;
        ssize_t pkg_size;
	    vmu_pkg_build(&pkg, &pkg_out, &pkg_size);
	    if (!pkg_out || pkg_size <= 0) {
		   // printf("vmu_pkg_build failed\n");
		    fs_close(eeprom_file);
            eeprom_file = -1;
            vid_border_color(255,0,0);
		    return 0;
	    }
        vid_border_color(0,255,0);
        fs_write(eeprom_file, pkg_out, pkg_size);
        fs_close(eeprom_file);
        eeprom_file = -1;
        free(pkg_out);
        osEepromLongWrite(NULL, 0, eeprom, 512);
       // printf("\tcreated mario64.rec\n");
    } else {
       // printf("\teeprom file existed on vmu a1\n");
        fs_close(eeprom_file);
        eeprom_file = -1;
        eeprom_file = fs_open("/vmu/a1/mario64.rec", O_RDWR | O_META);
		if (-1 == eeprom_file) {
			printf("\tcant open /vmu/a1/mario64.rec for rdwr\n");
            vid_border_color(255,0,0);
			return 0;
		}
    }

    vid_border_color(0,0,0);
   // printf("successfully returning from EEPROM probe\n");

//    oneshot_timer_reset(timer);
    if (eeprom_file != FILEHND_INVALID) {
                    fs_close(eeprom_file);
            eeprom_file = FILEHND_INVALID;

    }
return EEPROM_TYPE_4K;
}

s32 osEepromLongRead(UNUSED OSMesgQueue* mq, unsigned char address, unsigned char* buffer, s32 length) {
    if (eeprom_file == -1) {
        maple_device_t *vmudev = NULL;
        vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
        if (!vmudev) {
            //dbgio_printf("eeprom read: could not enum\n");
            vid_border_color(255,0,0);
            return 1;
        }

        eeprom_file = fs_open("/vmu/a1/mario64.rec", O_RDWR | O_META);
    }
   // printf("%s(%04x,%08x,%d)\n", __func__, address, buffer, length);

   // printf("EEPROM READ:\n");
    vid_border_color(0,0,255);

    if (-1 != eeprom_file) {
        mutex_lock_scoped(&eeprom_lock);
        ssize_t size = fs_total(eeprom_file);
       // printf("\tKOS claims /vmu/a1/mario64.rec exists on vmu A1 with size: %d\n", size);
        if (size != 1536) {
            fs_close(eeprom_file);
            eeprom_file = -1;
           // printf("\tbut the size was wrong (%d, expect 1536)\n", size);
            vid_border_color(255,0,0);
            return 1;
        }
        // skip header
        vid_border_color(0,255,0);
        fs_seek(eeprom_file, (512*2) + (address * 8), SEEK_SET);
        ssize_t rv = fs_read(eeprom_file, buffer, length);
        if (rv != length) {
    if (eeprom_file != FILEHND_INVALID) {
                    fs_close(eeprom_file);
            eeprom_file = FILEHND_INVALID;

    }
           // printf("\tcould not read %d bytes from /vmu/a1/mario64.rec\n", length);
            vid_border_color(255,0,0);
            return 1;
        }
    if (eeprom_file != FILEHND_INVALID) {
                    fs_close(eeprom_file);
            eeprom_file = FILEHND_INVALID;

    }

        vid_border_color(0,0,0);
       // printf("success reading EEPROM file\n");
        //oneshot_timer_reset(timer);
        return 0;
    } else {
        vid_border_color(255,0,0);
        return 1;
    }
}

s32 osEepromRead(OSMesgQueue* mq, u8 address, u8* buffer) {
    return osEepromLongRead(mq, address, buffer, 8);
}

s32 osEepromLongWrite(UNUSED OSMesgQueue* mq, unsigned char address, unsigned char* buffer, s32 length) {
    if (eeprom_file == -1) {
        maple_device_t *vmudev = NULL;
        vmudev = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
        if (!vmudev) {
            //dbgio_printf("eeprom write: could not enum\n");
            vid_border_color(255,0,0);
            return 1;
        }

        eeprom_file = fs_open("/vmu/a1/mario64.rec", O_RDWR | O_META);
    }
   // printf("%s(%04x,%08x,%d)\n", __func__, address, buffer, length);

   // printf("EEPROM WRITE:\n");
    vid_border_color(0,0,255);

    if (-1 != eeprom_file) {
        mutex_lock_scoped(&eeprom_lock);
        ssize_t size = fs_total(eeprom_file);
       // printf("\tKOS claims mario64.rec exists on vmu A1 with size: %d\n", size);
        if (size != 1536) {
            fs_close(eeprom_file);
            eeprom_file = -1;
           // printf("\tbut the size was wrong (%d, expect 1536)\n", size);
            vid_border_color(255,0,0);
            return 1;
        }
        // skip header
        vid_border_color(0,0,255);

        fs_seek(eeprom_file, (512*2) + (address * 8), SEEK_SET);
        ssize_t rv = fs_write(eeprom_file, buffer, length);
        if (rv != length) {
    if (eeprom_file != FILEHND_INVALID) {
                    fs_close(eeprom_file);
            eeprom_file = FILEHND_INVALID;

    }
           // printf("\tcould not write %d bytes to /vmu/a1/mario64.rec\n", length);
            vid_border_color(255,0,0);
            return 1;
        }

        vid_border_color(0,0,0);
       // printf("success writing EEPROM file\n");
//        oneshot_timer_reset(timer);
    if (eeprom_file != FILEHND_INVALID) {
                    fs_close(eeprom_file);
            eeprom_file = FILEHND_INVALID;

    }
    return 0;
    } else {
        vid_border_color(255,0,0);
        return 1;
    }
}

s32 osEepromWrite(OSMesgQueue* mq, unsigned char address, unsigned char* buffer) {
    return osEepromLongWrite(mq, address, buffer, 8);
}