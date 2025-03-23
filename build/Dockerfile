FROM ubuntu:16.04

ARG UID
ARG GID

RUN apt-get update && apt-get install -y sudo software-properties-common
RUN apt-get install -y \
    build-essential chrpath cpio debianutils diffstat file gawk gcc git iputils-ping libacl1 liblz4-tool locales python3 python3-git python3-jinja2 python3-pexpect python3-subunit socat texinfo unzip wget xz-utils zstd
RUN apt-get install -y \
    git-core gnupg flex bison gperf build-essential zip curl zlib1g-dev gcc-multilib g++-multilib libc6-dev-i386 lib32ncurses5-dev x11proto-core-dev libx11-dev lib32z-dev libxml-simple-perl libc6-dev libgl1-mesa-dev tofrodos python-markdown libxml2-utils xsltproc genisoimage
RUN apt-get install -y \
    gawk chrpath texinfo p7zip-full android-tools-fsutils
RUN apt-get install -y \
    ruby subversion libssl-dev
RUN apt-get install -y \
    upx pkg-config
RUN apt-get install -y \
    nano

RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add -
RUN apt-add-repository "deb http://apt.llvm.org/xenial/ llvm-toolchain-xenial-7 main"
RUN apt-get update && apt-get install -y clang-7 \
    libc++abi-7-dev \
    libc++-7-dev 
RUN ln -sf /usr/bin/clang++-7 /usr/bin/clang++

RUN wget https://github.com/git-lfs/git-lfs/releases/download/v3.6.1/git-lfs-linux-amd64-v3.6.1.tar.gz
RUN tar -zxf git-lfs-linux-amd64-v3.6.1.tar.gz
RUN cd git-lfs-3.6.1 && ./install.sh && cd ..

RUN wget https://go.dev/dl/go1.24.1.linux-amd64.tar.gz
RUN tar -zxf go1.24.1.linux-amd64.tar.gz
RUN mv go /usr/local/

# later versions require later glibc :(
RUN wget https://github.com/ccache/ccache/releases/download/v4.7.5/ccache-4.7.5-linux-x86_64.tar.xz
RUN tar -xf ccache-4.7.5-linux-x86_64.tar.xz
RUN mv ccache-4.7.5-linux-x86_64/ccache /usr/bin/
RUN chmod +x /usr/bin/ccache

RUN chmod 0755 /usr/local/bin
RUN wget https://github.com/ninja-build/ninja/releases/download/v1.8.2/ninja-linux.zip
RUN unzip ninja-linux.zip -d /usr/local/bin/
RUN update-alternatives --install /usr/bin/ninja ninja /usr/local/bin/ninja 1 --force

RUN ln -sf /bin/bash /bin/sh
RUN useradd -ms /bin/bash build && echo "build ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers
RUN usermod -u ${UID} build && groupmod -g ${GID} build
USER build
WORKDIR /home/build
CMD ["/bin/bash"]
