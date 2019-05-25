# put this in in bash_rc
CUR_PATH=$(pwd)

# we need to be in IMF directory for relative path resolve to work (to be fixed)
cd ~/bs500a-dalet-a-ov

CPL=CPL_52a5343e-1184-44b4-86e3-43477636ae69.xml
ASSETMAP=ASSETMAP.xml

# hack for development.
export LD_LIBRARY_PATH=${CUR_PATH}/third_party/openssl/lib

#valgrind --tool=callgrind ${CUR_PATH}/imf_fs ${CPL} ${ASSETMAP} > /dev/null
#exit 1

# write to file test
${CUR_PATH}/imf_fs ${CPL} ${ASSETMAP} > ~/x.nut
ffmpeg -i ~/x.nut -f mp4 -y ~/winhome/Downloads/x.mp4
rm ~/x.nut

# piping test
#${CUR_PATH}/imf_fs ${CPL} ${ASSETMAP} | ffmpeg -i - -f mp4 -y ~/winhome/Downloads/x.mp4

