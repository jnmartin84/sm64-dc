# The game's sequencer and AICA voice driver on the build machine: no console, no graphics. For each
# song and sound it writes the log tools/aica/sm64_afx.py turns into AICAflow assets. On the
# console the same log took an hour; here it takes seconds, and its notes match the console's. Run from the top of the repository:
#
#   gmake TARGET_DC=1 BUILD_DIR_BASE=build/dc -j8                   # once: generated sources
#   gmake -f tools/aica/recorder/Makefile -j8
#
# DC_GEN must be a Dreamcast build without DC_AICAFLOW, for its sample table.

DC_GEN ?= build/dc/us_dc
OUT ?= build/audio_rec
PYTHON ?= python3
SHZ_INCLUDE ?= $(KOS_BASE)/addons/include/sh4zam

SRCS := $(addprefix src/audio/,aica_synth.c data.c effects.c external.c globals_start.c heap.c load.c \
        playback.c seqplayer.c synthesis.c) lib/src/alBnkfNew.c src/buffers/buffers.c tools/aica/recorder/main.c
OBJS := $(patsubst %.c,$(OUT)/%.o,$(SRCS)) $(OUT)/sound_data.o $(OUT)/aica_sample_table.o
DEFINES := -D_LANGUAGE_C -DVERSION_US -DNON_MATCHING -DAVOID_UB -DF3DEX_GBI_2E -DTARGET_DC \
           -DDC_AUDIO_RECORD -DAICA_RECORD_HOST -DNO_SEGMENTED_MEMORY -DDC_TRIG_TABLE=1 -DDC_SHZ_FSCA=1
INCLUDES := -Itools/aica/recorder/compat -Iinclude -I$(DC_GEN) -I$(DC_GEN)/include -Isrc -I. -I$(OUT)/shz
CFLAGS := -std=gnu11 -D_FORTIFY_SOURCE=0 -O2 -g -fno-strict-aliasing -fwrapv -fsigned-char -w $(DEFINES) $(INCLUDES)

$(OUT)/sm64_audio_rec: $(OBJS)
	$(CC) -o $@ $^ -lm

$(OBJS): | $(OUT)/shz/sh4zam
$(OUT)/shz/sh4zam:
	@mkdir -p $(dir $@)
	ln -sfn $(SHZ_INCLUDE) $@

$(OUT)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<
$(OUT)/aica_sample_table.o: $(DC_GEN)/sound/aica_sample_table.c
	$(CC) $(CFLAGS) -c -o $@ $<

# assemble_sound.py lays out pointers at the target's width, so the banks and sequence tables are
# rebuilt for 64 bits from the Dreamcast build's samples and assembled sequences.
SND := $(OUT)/snd/sound
BITS := --endian little --bitwidth 64
SEQS := $(wildcard sound/sequences/us/*.m64) $(wildcard sound/sequences/*.m64) \
        $(patsubst %.s,$(DC_GEN)/%.m64,$(wildcard sound/sequences/us/*.s) $(wildcard sound/sequences/*.s))
$(SND)/sound_data.ctl: $(wildcard sound/sound_banks/*.json)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/assemble_sound.py $(DC_GEN)/sound/samples/ sound/sound_banks/ $@ $(SND)/sound_data.tbl -DVERSION_US $(BITS)
$(SND)/sound_data.tbl: $(SND)/sound_data.ctl
	@true
$(SND)/sequences.bin: $(wildcard sound/sound_banks/*.json) sound/sequences.json $(SEQS)
	@mkdir -p $(dir $@)
	$(PYTHON) tools/assemble_sound.py --sequences $@ $(SND)/bank_sets sound/sound_banks/ sound/sequences.json $(SEQS) -DVERSION_US $(BITS)
$(SND)/bank_sets: $(SND)/sequences.bin
	@true
$(SND)/%.inc.c: $(SND)/%
	hexdump -v -e '1/1 "0x%X,"' $< > $@
	echo >> $@
$(OUT)/sound_data.o: sound/sound_data.c $(addprefix $(SND)/,sound_data.ctl.inc.c sound_data.tbl.inc.c sequences.bin.inc.c bank_sets.inc.c)
	$(CC) -I$(OUT)/snd $(CFLAGS) -c -o $@ $<
