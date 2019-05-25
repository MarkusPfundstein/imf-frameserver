ffmpeg -f rawvideo -pix_fmt gbrp10le -s:v 1920x1080 -r 25 -i /tmp/imf-fs-rgb444.fifo -f s16le -ar 48k -ac 2 -i /tmp/imf-fs-pcm.fifo -c:v -y libx264 out.mp4
