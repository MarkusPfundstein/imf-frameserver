./imf_fs ~/Downloads/bs500a-dalet-a-ov/bs500-ov-bs500a_apl-App2-simple_v0.mxf | ffmpeg -f rawvideo -pix_fmt gbrp10le -s:v 1920x1080 -r 25 -i - -c:v libx264 -y output.mp4
