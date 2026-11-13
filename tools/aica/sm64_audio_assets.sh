#!/bin/sh
# Every song and sound, from the ROM: records the game's audio driver on this machine
# (tools/aica/recorder), turns each log into AICAflow assets (sm64_afx_batch.py) and packs them per
# scene (sm64_scene_assets.py, sm64_scene_banks.py, with tools/aica/sm64_sound_choices.json). Assets
# are not in the repository: like the rest of the game's data they come from your ROM. The Makefile
# runs this as part of `make TARGET_DC=1` (into build/us_dc/afx_stage); it takes some minutes.
#
#   tools/aica/sm64_audio_assets.sh <stage>     # the songs and sounds go to <stage>/afx
#
# DC_GEN: a Dreamcast build's generated tree. AFX_SM64: tools/aica/afx_sm64.c built against
# AICAforge (third_party/aicaforge). OUT: where the recordings and the work files go.
set -e
STAGE=${1:-build/us_dc/afx_stage}
DC_GEN=${DC_GEN:-build/us_dc}
MAKE=${MAKE:-gmake}
OUT=${OUT:-$DC_GEN/audio_rec}
SONGS=01,02,03,04,05,06,07,08,09,0A,0B,0C,0D,0E,0F,10,11,12,13,14,15,16,17,18,19,1A,1B,1C,1D,1E,1F,20,21,22

$MAKE -f tools/aica/recorder/Makefile DC_GEN="$DC_GEN" OUT=$OUT -j8
rm -rf $OUT/logs
mkdir -p $OUT/logs
# Each sound in a run of its own: songs load their banks over the sound player's, and a sound
# script leaves its channel's vibrato and IO values to the next sound on it.
$OUT/sm64_audio_rec $OUT/logs/ $SONGS "" > /dev/null
for sound in $(python3 tools/aica/sm64_sound_list.py | tr , ' '); do
    $OUT/sm64_audio_rec $OUT/logs/ "" $sound > /dev/null
done
# Pan within 2 steps and exact levels: the fades go on to the AICA's envelope (--hw-env).
AFX_SIMPLIFY=2,0 python3 tools/aica/sm64_afx_batch.py $OUT/logs --build "$DC_GEN" --stage "$STAGE" --work $OUT/work ${AFX_SM64:+--afx-sm64 "$AFX_SM64"}
python3 tools/aica/sm64_scene_assets.py --stage "$STAGE" --json $OUT/scene_assets.json
python3 tools/aica/sm64_scene_banks.py $OUT/scene_assets.json --stage "$STAGE" \
    --choices tools/aica/sm64_sound_choices.json
