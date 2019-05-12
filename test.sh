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
${CUR_PATH}/imf_fs ${CPL} ${ASSETMAP} | ffmpeg \
  -f rawvideo \
  -pix_fmt gbrp10le \
  -s:v 1920x1080 \
  -r 25 \
  -i - \
  -y ~/vid_output.mp4
#~/vid_output.mp4

mv ~/vid_output.mp4 ~/winhome/Downloads/
