DESCRIPTION = "Silly wire daemon for training wakeword and setting performance profile"
LICENSE = "Anki-Inc.-Proprietary"                                                                   
LIC_FILES_CHKSUM = "file://${COREBASE}/meta-qti-bsp/files/anki-licenses/\                           
Anki-Inc.-Proprietary;md5=4b03b8ffef1b70b13d869dbce43e8f09"

SERVICE_FILE = "wired.service"

SRC_URI = "file://${SERVICE_FILE}"

inherit systemd

do_install_append () {
   if ${@bb.utils.contains('DISTRO_FEATURES', 'systemd', 'true', 'false', d)}; then
       install -d ${D}${systemd_unitdir}/system/
       install -m 0644 ${WORKDIR}/${SERVICE_FILE} -D ${D}${systemd_unitdir}/system/${SERVICE_FILE}
   fi
}

FILES_${PN} += "${systemd_unitdir}/system/"
SYSTEMD_SERVICE_${PN} = "${SERVICE_FILE}"

inherit externalsrc

EXTERNALSRC = "${WORKSPACE}/anki/wired"

do_compile() {
    cd "${EXTERNALSRC}"
    make
}

# wired is packaged with upx, so Yocto can't figure out
# what libs it wants, so Yocto chooses to get rid of
# libvector-gobot.so. we must skip QA and stripping

do_strip() {
    :
}

do_package_qa() {
    :
}

do_install () {
    mkdir -p ${D}/usr/bin
    mkdir -p ${D}/usr/lib
    mkdir -p ${D}/etc/wired
    cp ${WORKSPACE}/anki/wired/build/wired ${D}/usr/bin/
    cp ${WORKSPACE}/anki/wired/build/libvector-gobot.so ${D}/usr/lib/
    cp -r ${WORKSPACE}/anki/wired/webroot ${D}/etc/wired/
}

FILES_${PN} += "usr/bin/wired"
FILES_${PN} += "/usr/lib/libvector-gobot.so"
FILES_${PN} += "etc/wired/webroot"

FILES_${PN}-dev = ""
INSANE_SKIP_${PN} += "dev-so"


INSANE_SKIP_${PN} += "ldflags"
#INSANE_SKIP_${PN} = "${@oe.utils.get_valid_qa_checks(d)}"
INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_PACKAGE_DEBUG_SPLIT = '1'
