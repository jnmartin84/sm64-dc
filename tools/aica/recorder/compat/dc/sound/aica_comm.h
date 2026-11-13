// KOS's AICA command packets as src/audio/aica_synth.c builds them. They go nowhere
// (snd_sh4_to_aica in tools/aica/recorder/main.c).
#ifndef COMPAT_AICA_COMM_H
#define COMPAT_AICA_COMM_H
#include <stdint.h>
typedef struct {
    uint32_t size, cmd, timestamp, cmd_id, misc[4];
    uint8_t cmd_data[];
} aica_cmd_t;
typedef struct {
    uint32_t cmd, base, type, length, loop, loopstart, loopend, freq, vol, pan, pos;
} aica_channel_t;
#define AICA_CMDSTR_CHANNEL(T, CMDR, CHANR) \
    uint8_t T[sizeof(aica_cmd_t) + sizeof(aica_channel_t)]; \
    aica_cmd_t *CMDR = (aica_cmd_t *) T; \
    aica_channel_t *CHANR = (aica_channel_t *) (CMDR->cmd_data)
#define AICA_CMDSTR_CHANNEL_SIZE ((sizeof(aica_cmd_t) + sizeof(aica_channel_t)) / 4)
#define AICA_CMD_CHAN 1
#define AICA_CH_CMD_START 1
#define AICA_CH_CMD_STOP 2
#define AICA_CH_CMD_UPDATE 3
#define AICA_CH_UPDATE_SET_FREQ 0x100
#define AICA_CH_UPDATE_SET_VOL 0x200
#define AICA_CH_UPDATE_SET_PAN 0x400
#define AICA_SM_16BIT 0
#define AICA_SM_8BIT 1
#define AICA_SM_ADPCM 2
#endif
