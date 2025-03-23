#!/bin/bash

set -e

if [[  -d ../anki/anki-ble ]]; then
    cd ..
fi

if [[ ! -d anki/anki-ble ]]; then
    echo -e "\033[1;31mThis script must be run in the vicos-oelinux folder, as ./build/vm-ota-build.sh\033[0m"
    exit 1
fi

# create symlink for anki-deps if needed
if [[ ! -L "$HOME/.anki" && -d "$(pwd)/anki-deps" ]]; then
    echo -e "\033[1;36mcreating symlink for anki-deps\033[0m"
    ln -sf "$(pwd)/anki-deps" "$HOME/.anki"
fi

function usage() {
    echo "usage: ./build/vm-ota-build.sh <prod/oskr/ep> <incremental> <ota_key_password> <prod/oskr boot image signing password>"
    echo "example: ./build/vm-ota-build.sh prod 6079 <password> <password>"
    exit 0
}

if [[ $1 == *"h"* ]]; then
    usage
fi

if [[ $1 != "prod" && $1 != "oskr" && $1 != "ep" ]]; then
    usage
fi

if [[ ! $2 =~ ^-?[0000-9999]+$ ]]; then
    usage
fi

if [[ $3 == "" ]]; then
    usage
fi

if [[ $4 == "" && $1 != "dev" ]]; then
    usage
fi

PRODorOSKR=$1
INCREMENT=$2
OTA_SIGNING_PASSWORD=$3
BOOT_SIGNING_PASSWORD=$4

if ! grep -q "Ubuntu 16.04" /etc/os-release; then
    echo -e "\033[1;31mThis script must be run on ubuntu 16.04 xenial\033[0m"
    exit 1
fi

function install_dependencies() {
    
    sudo apt-get update
    
    sudo apt-get install -y \
        build-essential chrpath cpio debianutils diffstat file gawk gcc git iputils-ping libacl1 liblz4-tool \
        locales python3 python3-git python3-jinja2 python3-pexpect python3-subunit socat texinfo unzip wget xz-utils zstd
    
    sudo apt-get install -y \
        git-core gnupg flex bison gperf build-essential zip curl zlib1g-dev gcc-multilib g++-multilib libc6-dev-i386 \
        lib32ncurses5-dev x11proto-core-dev libx11-dev lib32z-dev libxml-simple-perl libc6-dev libgl1-mesa-dev \
        tofrodos python-markdown libxml2-utils xsltproc genisoimage
    
    sudo apt-get install -y \
        gawk chrpath texinfo p7zip-full android-tools-fsutils
    
    sudo apt-get install -y \
        ruby subversion libssl-dev
    
    sudo apt-get install -y \
        upx pkg-config ccache sudo software-properties-common nano

    ORIGDIR=$(pwd)
    cd $HOME

    wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | sudo apt-key add -
    sudo apt-add-repository "deb http://apt.llvm.org/xenial/ llvm-toolchain-xenial-7 main"
    sudo apt-get update && sudo apt-get install -y clang-7 libc++abi-7-dev libc++-7-dev
    sudo ln -sf /usr/bin/clang++-7 /usr/bin/clang++
    
    curl -s https://packagecloud.io/install/repositories/github/git-lfs/script.deb.sh | sudo bash
    sudo apt-get install -y git-lfs
    
    wget https://go.dev/dl/go1.24.0.linux-amd64.tar.gz
    sudo tar -zxf go1.24.0.linux-amd64.tar.gz -C /usr/local/
    rm go1.24.0.linux-amd64.tar.gz
    
    wget https://github.com/ninja-build/ninja/releases/download/v1.8.2/ninja-linux.zip
    sudo unzip ninja-linux.zip -d /usr/local/bin/
    sudo update-alternatives --install /usr/bin/ninja ninja /usr/local/bin/ninja 1 --force
    rm ninja-linux.zip
    
    sudo ln -sf /bin/bash /bin/sh
    cd $ORIGDIR

}

function check_sign_prod() {
    if openssl rsa -in ota/qtipri.encrypted.key -passin pass:"$BOOT_SIGNING_PASSWORD" -noout 2>/dev/null; then
        echo "Prod boot image key password confirmed to be correct!"
    else
        echo
        echo -e "\033[1;31mProd boot image signing password is incorrect. exiting.\033[0m"
        echo -e "\033[1;31mHINT: we are using an older version of the key which has the same password as the ABOOT key\033[0m"
        echo
        exit 1
    fi
}

function check_sign_oskr() {
    if openssl rsa -in ota/qtioskrpri.encrypted.key -passin pass:"$BOOT_SIGNING_PASSWORD" -noout 2>/dev/null; then
        echo "OSKR boot image key password confirmed to be correct!"
    else
        echo
        echo -e "\033[1;31mOSKR boot image signing password is incorrect. exiting.\033[0m"
        echo
        exit 1
    fi
}

# install all dependencies first
if [[ ! -f vm-deps-installed ]]; then
    install_dependencies
    touch vm-deps-installed
fi

# configure build based on type
if [[ ${PRODorOSKR} == "prod" ]]; then
    ToDo="build-victor-robot-user-image && export IMG_DIR=../poky/build/tmp-glibc/deploy/images/apq8009-robot-robot-perf && cd ~/vicos-oelinux/ota && export BOOT_IMAGE_SIGNING_PASSWORD=$BOOT_SIGNING_PASSWORD && make prodsign"
    ADEV=0
    check_sign_prod
    OTA_NAME=vicos-$(cat ANKI_VERSION).${INCREMENT}.ota
    PERForUSER="-user"
elif [[ ${PRODorOSKR} == "ep" ]]; then
    ToDo="build-victor-robot-ep-image && export IMG_DIR=../poky/build/tmp-glibc/deploy/images/apq8009-robot-robot-perf && cd ~/vicos-oelinux/ota && export BOOT_IMAGE_SIGNING_PASSWORD=$BOOT_SIGNING_PASSWORD && make prodsign"
    ADEV=0
    check_sign_prod
    OTA_NAME=vicos-$(cat ANKI_VERSION).${INCREMENT}ep.ota
    PERForUSER="-user"
else
    ToDo="build-victor-robot-oskr-image && export IMG_DIR=../poky/build/tmp-glibc/deploy/images/apq8009-robot-robot-perf && cd ~/vicos-oelinux/ota && export BOOT_IMAGE_SIGNING_PASSWORD=$BOOT_SIGNING_PASSWORD && make oskrsign"
    ADEV=1
    check_sign_oskr
    OTA_NAME=vicos-$(cat ANKI_VERSION).${INCREMENT}oskr.ota
    PERForUSER="-perf"
fi

if openssl rsa -in ota/ota_prod.key -passin pass:"$OTA_SIGNING_PASSWORD" -noout 2>/dev/null; then
    echo "OTA signing key password is confirmed to be correct!"
else
    echo
    echo -e "\033[1;31mOTA signing key is incorrect. exiting.\033[0m"
    echo
    exit 1
fi

#cd ~/vicos-oelinux/poky && source build/conf/set_bb_env.sh
#export ANKI_BUILD_VERSION=$INCREMENT
#source ~/vicos-oelinux/poky/build/conf/set_anki_build_version.sh
#setenv-8009-robot-image$PERForUSER
#bitbake -c cleanall victor anki-version rampost machine-robot-image core-image-anki-initramfs
#eval $ToDo
#export OTA_MANIFEST_SIGNING_KEY=$OTA_SIGNING_PASSWORD
#ANKIDEV=$ADEV make

cat > /tmp/build_rc.sh << EOF
cd ~/vicos-oelinux/poky && source build/conf/set_bb_env.sh
export ANKI_BUILD_VERSION=$INCREMENT
source ~/vicos-oelinux/poky/build/conf/set_anki_build_version.sh
setenv-8009-robot-image$PERForUSER
bitbake -c cleanall victor anki-version rampost machine-robot-image core-image-anki-initramfs
$ToDo
export OTA_MANIFEST_SIGNING_KEY=$OTA_SIGNING_PASSWORD
ANKIDEV=$ADEV make
exit 0
EOF

bash --rcfile /tmp/build_rc.sh -i

rm /tmp/build_rc.sh

echo
echo -e "\033[1;32mBuild complete! Your $PRODorOSKR ota is in $(pwd)/_build/$OTA_NAME\033[0m"
echo
