ACLOCAL_AMFLAGS = -I m4

AM_CFLAGS = -g -Wundef \
	    -Wstrict-prototypes \
	    -Wno-trigraphs

AM_CPPFLAGS = -D__packed__= \
	      -DFEATURE_LE_DIAG \
	      -I../core/lib/inc

h_sources =
requiredlibs =
library_includedir = $(pkgincludedir)
library_include_HEADERS = \
	card_application_toolkit_v02.h \
	common_v01.h \
	control_service_v01.h \
	network_access_service_v01.h \
	user_identity_module_v01.h \
	user_identity_module_remote_v01.h \
	voice_service_common_v02.h \
	voice_service_v01.h \
	voice_service_v02.h \
	network_access_service_common_v01.h \
	device_management_service_v01.h \
	lte_v01.h \
	user_identity_module_http_v01.h \
	radio_frequency_radiated_performance_enhancement_v01.h \
	phonebook_manager_service_v01.h \
	coexistence_manager_v01.h \
	circuit_switched_video_telephony_v01.h \
	wireless_data_service_common_v01.h \
	wireless_data_service_v01.h \
	wireless_messaging_service_v01.h \
	wireless_data_administrative_service_v01.h \
	over_the_top_v01.h \
	data_filter_service_v01.h \
	specific_absorption_rate_v01.h \
	data_system_determination_v01.h \
	data_port_mapper_v01.h \
	data_common_v01.h \
        quality_of_service_v01.h \
        lowi_service_v01.h \
	secure_filesystem_service_v01.h \
        persistent_device_configuration_v01.h \
	bandwidth_limits_management_v01.h \
        access_terminal_service_v01.h \
        subsystem_control_v02.h \
        authentication_service_v01.h \
        qti_wlan_scm_msgr_v01.h \
        ip_multimedia_subsystem_application_v01.h \
        ip_multimedia_subsystem_dcm_v01.h \
        ip_multimedia_subsystem_ims_common_v01.h \
        ip_multimedia_subsystem_presence_v01.h \
        ip_multimedia_subsystem_rtp_v01.h \
        ip_multimedia_subsystem_settings_v01.h \
	qmi_ims_vt_v01.h

c_sources = \
	common_v01.c \
	voice_service_common_v02.c \
	voice_service_v02.c \
	phonebook_manager_service_v01.c \
	network_access_service_v01.c \
	device_management_service_v01.c \
	radio_frequency_radiated_performance_enhancement_v01.c \
	lte_v01.c \
	coexistence_manager_v01.c \
	user_identity_module_http_v01.c \
	circuit_switched_video_telephony_v01.c \
	wireless_data_service_common_v01.c \
	wireless_data_service_v01.c \
	wireless_messaging_service_v01.c \
	wireless_data_administrative_service_v01.c \
	over_the_top_v01.c \
	data_filter_service_v01.c \
	network_access_service_common_v01.c \
	user_identity_module_v01.c \
	user_identity_module_remote_v01.c \
	card_application_toolkit_v02.c \
	control_service_v01.c \
	specific_absorption_rate_v01.c \
	data_system_determination_v01.c \
	data_port_mapper_v01.c \
	data_common_v01.c \
        quality_of_service_v01.c \
        lowi_service_v01.c \
	secure_filesystem_service_v01.c \
        persistent_device_configuration_v01.c \
	bandwidth_limits_management_v01.c \
        access_terminal_service_v01.c \
        subsystem_control_v02.c \
        authentication_service_v01.c \
        qti_wlan_scm_msgr_v01.c \
        ip_multimedia_subsystem_application_v01.c \
        ip_multimedia_subsystem_dcm_v01.c \
        ip_multimedia_subsystem_ims_common_v01.c \
        ip_multimedia_subsystem_presence_v01.c \
        ip_multimedia_subsystem_rtp_v01.c \
        ip_multimedia_subsystem_settings_v01.c \
	qmi_ims_vt_v01.c

lib_LTLIBRARIES = libqmiservices.la
libqmiservices_la_CC = @CC@
libqmiservices_la_SOURCES = $(c_sources) $(h_sources)
libqmiservices_la_CFLAGS = $(AM_CFLAGS) $(DIAG_CFLAGS) -fPIC
libqmiservices_la_CPPFLAGS = $(AM_CPPFLAGS)
libqmiservices_la_LDFLAGS = -shared -version-info 1:0:0
libqmiservices_la_LIBADD = $(requiredlibs)
