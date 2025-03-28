HOMEPAGE = "https://www.sourceware.org/lvm2/"
SECTION = "utils"
DESCRIPTION = "LVM2 is a set of utilities to manage logical volumes in Linux."
LICENSE = "GPLv2 & LGPLv2"
LIC_FILES_CHKSUM = "file://COPYING;md5=12713b4d9386533feeb07d6e4831765a \
                    file://COPYING.LIB;md5=fbc093901857fcd118f065f900982c24"

DEPENDS += "util-linux"

SRC_URI = "ftp://sources.redhat.com/pub/lvm2/old/LVM2.${PV}.tgz \
           file://lvm.conf \
           file://0001-implement-libc-specific-reopen_stream.patch \
           file://0002-Guard-use-of-mallinfo-with-__GLIBC__.patch \
           file://0003-include-fcntl.h-for-O_-defines-and-fcntl-signature.patch \
           file://0004-tweak-MODPROBE_CMD-for-cross-compile.patch \
           file://0001-Avoid-bashisms-in-init-scripts.patch \
           file://0005-do-not-build-manual.patch \
           file://0006-start-lvm2-monitor.service-after-tmp.mount.patch \
           "
S = "${WORKDIR}/LVM2.${PV}"

inherit autotools-brokensep pkgconfig systemd license

LVM2_PACKAGECONFIG = "dmeventd lvmetad"
LVM2_PACKAGECONFIG_append_class-target = " \
    ${@base_contains('DISTRO_FEATURES','selinux', 'selinux', '', d)} \
    udev \
"

# odirect is always enabled because there currently is a bug in
# lib/device/dev-io.c which prevents compiling without it. It is
# better to stick to configurations that were actually tested by
# upstream...
PACKAGECONFIG ??= "odirect ${LVM2_PACKAGECONFIG}"

PACKAGECONFIG[dmeventd] = "--enable-dmeventd,--disable-dmeventd"
PACKAGECONFIG[lvmetad] = "--enable-lvmetad,--disable-lvmetad"
PACKAGECONFIG[odirect] = "--enable-o_direct,--disable-o_direct"
PACKAGECONFIG[readline] = "--enable-readline,--disable-readline,readline"
PACKAGECONFIG[selinux] = "--enable-selinux,--disable-selinux,libselinux"
PACKAGECONFIG[thin-provisioning-tools] = "--with-thin=internal,--with-thin=none,thin-provisioning-tools"
PACKAGECONFIG[udev] = "--enable-udev_sync --enable-udev_rules --with-udevdir=${nonarch_base_libdir}/udev/rules.d,--disable-udev_sync --disable-udev_rules,udev"

# Unset user/group to unbreak install.
EXTRA_OECONF = "--with-user= \
                --with-group= \
                --enable-realtime \
                --enable-applib \
                --enable-cmdlib \
                --enable-pkgconfig \
                --with-usrlibdir=${libdir} \
                --with-systemdsystemunitdir=${systemd_system_unitdir} \
                --disable-thin_check_needs_check \
                --with-thin-check=${sbindir}/thin_check \
                --with-thin-dump=${sbindir}/thin_dump \
                --with-thin-repair=${sbindir}/thin_repair \
                --with-thin-restore=${sbindir}/thin_restore \
"

