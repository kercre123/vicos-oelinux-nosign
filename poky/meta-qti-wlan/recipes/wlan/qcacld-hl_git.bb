inherit autotools-brokensep module qperf

DESCRIPTION = "Qualcomm Atheros WLAN CLD high latency driver"
LICENSE = "ISC"
LIC_FILES_CHKSUM = "file://${COREBASE}/meta/files/common-licenses/${LICENSE};md5=f3b90e78ea0cffb20bf5cca7947a896d"

# Targets - mdm9650 and sdx20: modulename = wlan_sdio.ko, chip name - qca9377
# Other targets : modulename = wlan.ko, chip name -

python __anonymous () {
     if d.getVar('BASEMACHINE', True) == 'mdm9650':
         d.setVar('WLAN_MODULE_NAME', 'wlan_sdio')
         d.setVar('CHIP_NAME', 'qca9377')
     elif d.getVar('BASEMACHINE', True) == 'sdx20':
         d.setVar('WLAN_MODULE_NAME', 'wlan_sdio')
         d.setVar('CHIP_NAME', 'qca9377')
         d.setVar('WLAN_MODULE_TARGET_NAME', 'wlan_sdio_cld20')
     else:
         d.setVar('WLAN_MODULE_NAME', 'wlan')
         d.setVar('CHIP_NAME', '')
}

FILES_${PN}     += "lib/firmware/wlan/*"
FILES_${PN}     += "${base_libdir}/modules/${KERNEL_VERSION}/extra/${WLAN_MODULE_NAME}.ko"
# The inherit of module.bbclass will automatically name module packages with
# kernel-module-" prefix as required by the oe-core build environment. Also it
# replaces '_' with '-' in the module name.
RPROVIDES_${PN} += "${@'kernel-module-${WLAN_MODULE_NAME}'.replace('_', '-')}"
PROVIDES_NAME   = "kernel-module-${WLAN_MODULE_NAME}"

do_unpack[deptask] = "do_populate_sysroot"
PR = "r0"

#This DEPENDS is to serialize kernel module builds
DEPENDS = "rtsp-alg"

FILESPATH =+ "${WORKSPACE}:"
SRC_URI = "file://wlan/qcacld-2.0/"

S = "${WORKDIR}/wlan/qcacld-2.0/"

# Append the chip name to firmware installation path
CHIP_NAME_APPEND = "${@base_conditional('CHIP_NAME', '', '', '/${CHIP_NAME}', d)}"
FIRMWARE_PATH = "${D}/lib/firmware/wlan/qca_cld${CHIP_NAME_APPEND}"

# Explicitly disable LL to enable HL as current WLAN driver is not having
# simultaneous support of HL and LL.
EXTRA_OEMAKE += "CONFIG_CLD_LL_CORE=n CONFIG_CNSS_PCI=n MODNAME=${WLAN_MODULE_NAME} CHIP_NAME=${CHIP_NAME}"

# The common header file, 'wlan_nlink_common.h' can be installed from other
# qcacld recipes too. To suppress the duplicate detection error, add it to
# SSTATE_DUPWHITELIST.
SSTATE_DUPWHITELIST += "${STAGING_DIR}/${MACHINE}${includedir}/qcacld/wlan_nlink_common.h"

do_install () {
    module_do_install

    install -d ${FIRMWARE_PATH}
    install -m 0644 ${S}/firmware_bin/WCNSS_cfg.dat ${FIRMWARE_PATH}/

    install -d ${D}${includedir}/qcacld/
    install -m 0644 ${S}/CORE/SVC/external/wlan_nlink_common.h ${D}${includedir}/qcacld/
}

do_install_append_sdx20 () {
    if [ -e ${D}/${base_libdir}/modules/${KERNEL_VERSION}/extra/${WLAN_MODULE_NAME}.ko ]; then
        mv ${D}/${base_libdir}/modules/${KERNEL_VERSION}/extra/${WLAN_MODULE_NAME}.ko ${D}/${base_libdir}/modules/${KERNEL_VERSION}/extra/${WLAN_MODULE_TARGET_NAME}.ko
    fi
}

do_module_signing() {
    if [ -f ${STAGING_KERNEL_BUILDDIR}/signing_key.priv ]; then
        if [ ${BASEMACHINE} == "apq8017" ]; then
            ${STAGING_KERNEL_DIR}/scripts/sign-file sha512 ${STAGING_KERNEL_BUILDDIR}/signing_key.priv ${STAGING_KERNEL_BUILDDIR}/signing_key.x509 ${PKGDEST}/${PROVIDES_NAME}/lib/modules/${KERNEL_VERSION}/extra/${WLAN_MODULE_NAME}.ko
        elif [ ${BASEMACHINE} == "sdx20" ]; then
            ${STAGING_KERNEL_DIR}/scripts/sign-file sha512 ${STAGING_KERNEL_BUILDDIR}/signing_key.priv ${STAGING_KERNEL_BUILDDIR}/signing_key.x509 ${PKGDEST}/${PROVIDES_NAME}/lib/modules/${KERNEL_VERSION}/extra/${WLAN_MODULE_TARGET_NAME}.ko
        else
            ${STAGING_KERNEL_DIR}/scripts/sign-file sha512 ${STAGING_KERNEL_BUILDDIR}/signing_key.priv ${STAGING_KERNEL_BUILDDIR}/signing_key.x509 ${PKGDEST}/${PN}/lib/modules/${KERNEL_VERSION}/extra/${WLAN_MODULE_NAME}.ko
        fi
    fi
}

addtask module_signing after do_package before do_package_write_ipk
