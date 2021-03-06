ROOT_DIR=$(pwd)
echo "now in ${ROOT_DIR}"

mkdir -p third_party

# ------- OPENJPEG -------
if [ ! -e third_party/openjpeg/include/openjpeg-2.3/openjpeg.h ]; then
  echo "install openjpeg"
  mkdir -p third_party/openjpeg

  cd openjpeg
  mkdir -p build
  cd build
  cmake .. -DCMAKE_BUILD_TYPE=RELEASE -DCMAKE_INSTALL_PREFIX=${ROOT_DIR}/third_party/openjpeg

  make
  make install
else
  echo "openjpeg already installed"
fi

cd $ROOT_DIR

# ------- openssl --------
if [ ! -e third_party/openssl/include/openssl ]; then
  echo "install openssl"
  mkdir -p third_party/openssl

  cd openssl
  ./config no-asm no-tests shared --prefix=${ROOT_DIR}/third_party/openssl
  make
  make install
  make clean
else
  echo "openssl already installed"
fi

cd $ROOT_DIR

# ------- ASDCPLIB -------
if [ ! -e third_party/asdcplib/include/ ]; then
  echo "install asdcplib"
  mkdir -p third_party/asdcplib

  cd asdcplib
  autoreconf -if
  ./configure --enable-freedist --enable-as-02 --enable-dev-headers --prefix=${ROOT_DIR}/third_party/asdcplib --with-openssl=${ROOT_DIR}/third_party/openssl
  make
  make install
  make clean
else
  echo "asdcplib already installed"
fi

cd $ROOT_DIR

# -------- libxml2 ----------
if [ ! -e third_party/libxml2/include ]; then
  echo "install libxml2"
  mkdir -p third_party/libxml2

  cd libxml2-2.7.2
  ./configure --prefix=${ROOT_DIR}/third_party/libxml2
  make
  make install
  make clean
else
  echo "libxml2 already installed"
fi

cd $ROOT_DIR

# -------- ffmpeg libs -------
if [ ! -e third_party/ffmpeg/include ]; then
    echo "install ffmpeg libs"
    mkdir -p third_party/ffmpeg
    
    cd FFmpeg
    #./configure --prefix=${ROOT_DIR}/third_party/ffmpeg --disable-everything --disable-x86asm --enable-encoder=r210 --enable-muxer=nut --enable-encoder=pcm --enable-protocol=pipe
    ./configure --prefix=${ROOT_DIR}/third_party/ffmpeg --disable-x86asm 
    make
    make install
    make clean
else
    echo "ffmpeg libs already installed"
fi
