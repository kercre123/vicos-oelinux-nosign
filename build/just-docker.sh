BUILD_ARGS_TERM="-it"
if [[ ${NO_TTY} == "1" ]]; then
        BUILD_ARGS_TERM="-t"
fi

docker build --build-arg UID=$(id -u $USER) --build-arg GID=$(id -g $USER) -t victor-builder build/
docker run ${BUILD_ARGS_TERM} \
    -v "$(pwd):/home/build/vicos-oelinux" \
    -v "$(pwd)/anki-deps:/home/build/.anki" \
    -v "$(pwd)/build/cache/ccache:/home/build/.ccache" \
    victor-builder bash -c "exec bash --rcfile <(echo \"cd ~/vicos-oelinux/poky && rm -rf build/tmp-glibc/deploy/images/apq8009-robot-robot-perf/* && source build/conf/set_bb_env.sh\")"
