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
${CUR_PATH}/imf_fs ${CPL} ${ASSETMAP} &
ffmpeg -f s24le -ar 48k -ac 2 -i /tmp/imf-fs-pcm.fifo -y bs500a_audio.wav &
ffmpeg -f rawvideo -pix_fmt gbrp10le -s:v 1920x1080 -r 25 -i /tmp/imf-fs-rgb444.fifo -c:v r210 -y bs500a_video.mov &
wait

ffmpeg -i bs500a_audio.wav -i bs500a_video.mov -c:v copy -c:a copy -y bs500a.mov

mv bs500a.mov ~/winhome/Downloads/

ffmpeg -i ~/winhome/Downloads/bs500a.mov -c:v libx264 -y ~/winhome/Downloads/bs500aXYZ-mov.mp4
