# WIP project for IMF Frameserver 

## Goal

Open Source Program to decode Interoperable Master Format (IMF) CPL & OPL into raw audio and video so that we can pipe it into ffmpeg or other processors

**This is a WIP! Contains quite some shortcuts and unsecure code (e.g. strcpy). Please use with caution :-)**

## Features so far

- take CPL & ASSETMAP as input and output .nut file with r210 10-bit RGB444 and pcms24le
- supports multiple segments with start points and repeat counts
- output can be piped into ffmpeg to produce whatever you want

## Drawbacks

- only CDCI yet
- audio only wav s24le
- only .nut pipe output
- so far only tested on MrMXF bs500a IMF package (cdci, 25fps, 48000hz aud) ([Link](http://imf-mm-api.cloud/media/bs500/delivery/bs500a-dalet-a-ov.zip)). Requires thus more testing (e.g. with broadcast framerates such as 23.97)
- running it still a bit clumsy
- compiles under Ubuntu (nothing else tested)

## Install

Ubuntu (WSL)
- apt install libtool, automake, autoconf, cmake
- run sh install-deps.sh
- run make

## Run it

(see test.sh)

```
CUR_PATH=$(pwd)
IMF_ENC=$CUR_PATH/imf_fs

# we need to be in IMF directory for relative path resolve to work (to be fixed)
cd ~/bs500a-dalet-a-ov

CPL=CPL_52a5343e-1184-44b4-86e3-43477636ae69.xml
ASSETMAP=ASSETMAP.xml

# hack
export LD_LIBRARY_PATH=${CUR_PATH}/third_party/openssl/lib

#direct piping
${IMF_ENC} ${CPL} ${ASSETMAP} | ffmpeg -i - -f mp4 -y ~/xpipe.mp4
```

## License

GPL (like ffmpeg)
