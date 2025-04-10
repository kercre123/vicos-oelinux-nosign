#                                               -*- Autoconf -*-

# configure.ac -- Autoconf script for ath6kl-utils.
#

# Process this file with autoconf to produce a configure script

# Requires autoconf tool later than 2.61
AC_PREREQ(2.61)
# Initialize the ath6kl package version 1.0.0
AC_INIT([ath6kl-utils],1.0.0)
# Does not strictly follow GNU Coding standards
AM_INIT_AUTOMAKE([foreign])
# Disables auto rebuilding of configure, Makefile.ins
AM_MAINTAINER_MODE
# defines some macros variable to be included by source
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])

# Checks for programs.
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_LIBTOOL
AC_PROG_AWK
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET

# Checks for libraries.
PKG_CHECK_MODULES([DIAG], [diag])
AC_SUBST([DIAG_CFLAGS])
AC_SUBST([DIAG_LIBS])

has_libnl_ver=0
# libnl-2 provides only libnl-2.0.pc file, so we check for separate libnl-genl-3.0.pc
# pkg-config file just for libnl-3.0 case.
#
PKG_CHECK_MODULES([LIBNL], [libnl-3.0 >= 3.0 libnl-genl-3.0 >= 3.0], [has_libnl_ver=3], [
   PKG_CHECK_MODULES([LIBNL], [libnl-2.0 >= 2.0], [has_libnl_ver=2], [
           PKG_CHECK_MODULES([LIBNL], [libnl-1], [has_libnl_ver=1], [has_libnl_ver=0])])])

if (test "$has_libnl_ver" -eq 0); then
        AC_MSG_ERROR(libnl and libnl-genl are required but were not found)
fi

if (test "$has_libnl_ver" -gt 1); then
        AC_DEFINE([HAVE_LIBNL20], [1], [Define if you have libnl-2.0 or higher])
fi

AC_SUBST([LIBNL_CFLAGS])
AC_SUBST([LIBNL_LIBS])

AC_ARG_WITH([glib],
      AC_HELP_STRING([--with-glib],
         [enable glib, building HLOS systems which use glib]))

if (test "x${with_glib}" = "xyes"); then
        AC_DEFINE(ENABLE_USEGLIB, 1, [Define if HLOS systems uses glib])
        PKG_CHECK_MODULES(GTHREAD, gthread-2.0 >= 2.16, dummy=yes,
                                AC_MSG_ERROR(GThread >= 2.16 is required))
        PKG_CHECK_MODULES(GLIB, glib-2.0 >= 2.16, dummy=yes,
                                AC_MSG_ERROR(GLib >= 2.16 is required))
        GLIB_CFLAGS="$GLIB_CFLAGS $GTHREAD_CFLAGS"
        GLIB_LIBS="$GLIB_LIBS $GTHREAD_LIBS"

        AC_SUBST(GLIB_CFLAGS)
        AC_SUBST(GLIB_LIBS)
fi
AM_CONDITIONAL(USE_GLIB, test "x${with_glib}" = "xyes")

AC_ARG_ENABLE(target,
  [AS_HELP_STRING([--enable-target=TARGET], [Specify the target product to build])],
  [TARGET=$enableval],
  [TARGET=none]
)
AM_CONDITIONAL([MDM], [test "x$TARGET" = "xmdm9607" -o "x$TARGET" = "xmdm9635" -o "x$TARGET" = "xmdm9640" -o "x$TARGET" = "xmdm9650" -o "x$TARGET" = "xapq8009" -o "x$TARGET" = "xapq8017" -o "x$TARGET" = "xapq8053" -o "x$TARGET" = "xapq8096" -o "x$TARGET" = "xsdx20" -o "x$TARGET" = "xsdxhedgehog"])

# Checks for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL
AC_HEADER_STDC
AC_C_INLINE
AC_TYPE_INT64_T
AC_TYPE_PID_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_ERROR_AT_LINE
AC_FUNC_FORK
AC_FUNC_MALLOC

AC_CONFIG_FILES([ \
        Makefile \
        libtcmd/Makefile \
        libathtestcmd/Makefile \
        libathtestcmd/libtlvutil/Makefile \
        myftm/Makefile \
        ath6kl-utils.pc
        ])
AC_OUTPUT

