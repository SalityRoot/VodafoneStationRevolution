# xreadlink.m4 serial 3
dnl Copyright (C) 2002, 2003 Free Software Foundation, Inc.
dnl This file is free software, distributed under the terms of the GNU
dnl General Public License.  As a special exception to the GNU General
dnl Public License, this file may be distributed as part of a program
dnl that contains a configuration script generated by Autoconf, under
dnl the same distribution terms as the rest of that program.

AC_DEFUN([gl_XREADLINK],
[
  dnl Prerequisites of lib/xreadlink.c.
  AC_REQUIRE([gt_TYPE_SSIZE_T])
  AC_CHECK_HEADERS_ONCE(stdlib.h unistd.h)
])
