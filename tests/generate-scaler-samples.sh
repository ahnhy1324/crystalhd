#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Synthetic, native MPEG-2 scaler stimuli; generating them never opens hardware.
# Each picture is duplicated to distinguish reproducible pixels from startup.
set -eu
if [ "$#" -ne 2 ] || [ ! -d "$1" ]; then
    echo "usage: $0 EXISTING_OUTPUT_DIRECTORY arithmetic|spatial" >&2
    exit 2
fi
scaler_output=$1
scaler_kind=$2
case "$scaler_kind" in
    arithmetic)
        # Three constants, two ramps, modest bright/dark stripes and steps.
        # All measurements must use decoded controls, not these ideal values.
        scaler_frames=32
        scaler_luma='if(lt(N,2),64,if(lt(N,4),128,if(lt(N,6),192,if(lt(N,8),64+floor(128*X/640),if(lt(N,10),64+floor(128*Y/360),if(lt(N,12),128+48*eq(X,320),if(lt(N,14),128+48*eq(X,321),if(lt(N,16),128-48*eq(X,320),if(lt(N,18),128+24*eq(X,320),if(lt(N,20),128+48*eq(Y,180),if(lt(N,22),128+48*eq(Y,181),if(lt(N,24),128-48*eq(Y,180),if(lt(N,26),128+48*gte(X,320),if(lt(N,28),128+48*gte(X,321),if(lt(N,30),128+48*gte(Y,180),128+48*gte(Y,181))))))))))))))))'
        ;;
    spatial)
        # Same DCT-block phase: x=128/129, 256/257, 320/321, 384/385, 512/513.
        # Same-parity decoded patches must match before comparing responses.
        scaler_frames=20
        scaler_luma='128+48*eq(X,if(lt(N,4),128,if(lt(N,8),256,if(lt(N,12),320,if(lt(N,16),384,512))))+mod(floor(N/2),2))'
        ;;
    *)
        echo "unknown scaler stimulus: $scaler_kind" >&2
        exit 2
        ;;
esac
scaler_destination=$scaler_output/$scaler_kind.m2v
if [ -e "$scaler_destination" ] || [ -L "$scaler_destination" ]; then
    echo "refusing existing scaler stimulus: $scaler_destination" >&2
    exit 1
fi
ffmpeg -nostdin -hide_banner -loglevel error -n \
    -f lavfi -i "nullsrc=size=640x360:rate=30,geq=lum='$scaler_luma':cb=128:cr=128" \
    -frames:v "$scaler_frames" -an -c:v mpeg2video -g 1 -bf 0 -q:v 1 \
    -threads 1 -flags +bitexact -pix_fmt yuv420p -f mpeg2video \
    "$scaler_destination"
