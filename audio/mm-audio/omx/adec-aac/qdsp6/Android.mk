ifeq ($(call is-board-platform-in-list,msm8660 msm8960 msm8974 msm8226 msm8610 copper apq8084 msm8916 msm8994 msm8992 msm8909 msm8996 msm8952 msm8937 thorium msm8953 msmgold msm8998 sdm660),true)
ifneq ($(BUILD_TINY_ANDROID),true)

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

# ---------------------------------------------------------------------------------
#                 Common definitons
# ---------------------------------------------------------------------------------

libOmxAacDec-def := -g -O3
libOmxAacDec-def += -DQC_MODIFIED
libOmxAacDec-def += -D_ANDROID_
libOmxAacDec-def += -D_ENABLE_QC_MSG_LOG_
libOmxAacDec-def += -DVERBOSE
libOmxAacDec-def += -D_DEBUG
libOmxAacDec-def += -DAUDIOV2
libOmxAacDec-def += -Wconversion
ifeq ($(call is-board-platform,msm8960),true)
libOmxAacDec-def += -DMSM_ALSA
endif
#ifeq ($(call is-board-platform-in-list,msm8960 msm8974 msm8226 msm8610 copper apq8084 msm8996 msm8998),true)
#libOmxAacDec-def += -DMSM_ALSA
#endif
ifeq ($(call is-board-platform-in-list,msm8610 apq8084 msm8996 msm8952 msm8937 thorium msm8953 msmgold msm8998 sdm660),true)
libOmxAacDec-def += -DQCOM_AUDIO_USE_SYSTEM_HEAP_ID
endif

# ---------------------------------------------------------------------------------
#             Make the Shared library (libOmxAacDec)
# ---------------------------------------------------------------------------------

include $(CLEAR_VARS)

libOmxAacDec-inc        := $(LOCAL_PATH)/inc
libOmxAacDec-inc        += $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr/include
libOmxAacDec-inc        += $(TARGET_OUT_HEADERS)/mm-core/omxcore
libOmxAacDec-inc        += $(TARGET_OUT_HEADERS)/common/inc

LOCAL_ADDITIONAL_DEPENDENCIES  := $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr

LOCAL_MODULE            := libOmxAacDec
LOCAL_MODULE_TAGS       := optional
LOCAL_CFLAGS            := $(libOmxAacDec-def)
LOCAL_C_INCLUDES        := $(libOmxAacDec-inc)
LOCAL_PRELINK_MODULE    := false
LOCAL_SHARED_LIBRARIES  := libutils liblog

LOCAL_SRC_FILES         := src/adec_svr.c
LOCAL_SRC_FILES         += src/omx_aac_adec.cpp

LOCAL_MODULE_OWNER := qcom
LOCAL_PROPRIETARY_MODULE := true

include $(BUILD_SHARED_LIBRARY)

endif #BUILD_TINY_ANDROID
endif #TARGET_BOARD_PLATFORM
# ---------------------------------------------------------------------------------
#                     END
# ---------------------------------------------------------------------------------

