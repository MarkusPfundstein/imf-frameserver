WIP project for IMF Frameserver 

Goals: parse interoperable master format CPL & OPL and serve jpeg2000 frames transformed by OPL macros

- Highly WIP! Contains quite some shortcuts and unsecure code (e.g. strcpy). Please use with caution :-)

*Features so far*

- take CPL & ASSETMAP as input, parses resources and mxf assets and outputs to stdout raw video frames
- raw video frames can be piped into ffmpeg to do whatever you want

*Install*

(Ubuntu (WSL) & OSX)
- apt install libtool, automake, autoconf, cmake (or brew)
- run sh install-deps.sh
- run make
- set LD_LIBRARY_PATH correctly (see test.sh)

*Run it*

see test.sh for how to pipe video into ffmpeg

*License*

not yet decided. But until then its not for commercial use. If you want to use it, please contribute back to this open source project.
