/****************************************************************************
 *
 * rg/pkg/include/os_includes.h
 * 
 * Copyright (C) Jungo LTD 2004
 * 
 * This program is free software; you can redistribute it
 * and/or modify it under the terms of the GNU General 
 * Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA 02111-1307, USA.
 *
 * Developed by Jungo LTD.
 * Residential Gateway Software Division
 * www.jungo.com
 * info@jungo.com
 */
/* SYNC: rg/pkg/include/os_includes.h <-> project/tools/util/os_includes.h */

/* IMPORTANT: Prior to including this file, define the needed sections. */

#include <rg_os.h>

#ifdef OS_INCLUDE_IOCTL
#ifndef OS_INCLUDE_IOCTL_INCLUDED
#define OS_INCLUDE_IOCTL_INCLUDED
#include <sys/ioctl.h>
#endif
#endif

#ifdef OS_INCLUDE_STD
#ifndef OS_INCLUDE_STD_INCLUDED
#define OS_INCLUDE_STD_INCLUDED
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#define OS_INCLUDE_STRING
#include <ctype.h>
#endif
#endif

#ifdef OS_INCLUDE_GETOPT
#include <unistd.h>
#include <getopt.h>
#endif

#ifdef OS_INCLUDE_STDDEF
#include <stddef.h>
#endif

#ifdef OS_INCLUDE_STDARG
#ifndef OS_INCLUDE_STDARG_INCLUDED
#define OS_INCLUDE_STDARG_INCLUDED
#include <stdarg.h>

/* Deal with pre-C99 compilers */
#ifndef va_copy
#ifdef __va_copy
#define va_copy(A, B) __va_copy(A, B)
#else
#warning "neither va_copy nor __va_copy is defined. Using simple copy."
#define va_copy(A, B) A = B
#endif
#endif

#endif
#endif

/* IO functions - open/read/close */
#ifdef OS_INCLUDE_IO
#ifndef OS_INCLUDE_IO_INCLUDED
#define OS_INCLUDE_IO_INCLUDED
#define OS_INCLUDE_TYPES
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#endif
#endif

#ifdef OS_INCLUDE_INET
#ifndef OS_INCLUDE_INET_INCLUDED
#define OS_INCLUDE_INET_INCLUDED
#include <netinet/in.h>
#include <arpa/inet.h>
#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 26
#endif
#if (defined(__GLIBC__) && defined(__GLIBC_MINOR__) && \
    (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 19)) && \
    !defined(__FAVOR_BSD))
#define uh_sport source
#define uh_dport dest
#define uh_ulen len
#define uh_sum check
#endif
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip6.h>
#endif
#endif

#ifdef OS_INCLUDE_SOCKET
#ifndef OS_INCLUDE_SOCKET_INCLUDED
#define OS_INCLUDE_SOCKET_INCLUDED
#include <sys/socket.h>
#include <fcntl.h>
#endif
#endif

#ifdef OS_INCLUDE_IF_ETHER
#ifndef OS_INCLUDE_IF_ETHER_INCLUDED
#define OS_INCLUDE_IF_ETHER_INCLUDED
#include <linux/if_ether.h>
#include <net/if_arp.h>
#endif
#endif

#ifdef OS_INCLUDE_IF_ATM
#ifndef OS_INCLUDE_IF_ATM_INCLUDED
#define OS_INCLUDE_IF_ATM_INCLUDED
#include <stdint.h>
#include <linux/atmdev.h>
typedef struct atm_qos atm_qos_t;
#endif
#endif

#ifdef OS_INCLUDE_TIME
#ifndef OS_INCLUDE_TIME_INCLUDED
#define OS_INCLUDE_TIME_INCLUDED
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#endif
#endif

#ifdef OS_INCLUDE_SELECT
#ifndef OS_INCLUDE_SELECT_INCLUDED
#define OS_INCLUDE_SELECT_INCLUDED
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif
#endif

#ifdef OS_INCLUDE_SIGNAL
#ifndef OS_INCLUDE_SIGNAL_INCLUDED
#define OS_INCLUDE_SIGNAL_INCLUDED
#include <sys/wait.h>
#include <signal.h>
#endif
#endif

#ifdef OS_INCLUDE_REBOOT
#ifndef OS_INCLUDE_REBOOT_INCLUDED
#define OS_INCLUDE_REBOOT_INCLUDED
#include <sys/reboot.h>
#include <linux/reboot.h>
#endif
#endif

#ifdef OS_INCLUDE_MOUNT
#ifndef OS_INCLUDE_MOUNT_INCLUDED
#define OS_INCLUDE_MOUNT_INCLUDED
#include <sys/mount.h>
#ifndef MNT_DETACH
#define MNT_DETACH 0x02
#endif
#endif
#endif

#ifdef OS_INCLUDE_TERMIO
#ifndef OS_INCLUDE_TERMIO_INCLUDED
#define OS_INCLUDE_TERMIO_INCLUDED
#include <termios.h>
#endif
#endif

#ifdef OS_INCLUDE_PROCESS
#ifndef OS_INCLUDE_PROCESS_INCLUDED
#define OS_INCLUDE_PROCESS_INCLUDED
#include <sys/types.h>
#include <wait.h>
#endif
#endif

#ifdef OS_INCLUDE_BOOTP
#ifndef OS_INCLUDE_BOOTP_INCLUDED
#define OS_INCLUDE_BOOTP_INCLUDED
#include <netinet/ip.h>
#endif
#endif

#ifdef OS_INCLUDE_STRING
#ifndef OS_INCLUDE_STRING_INCLUDED
#define OS_INCLUDE_STRING_INCLUDED
#include <string.h>
#endif
#endif

#ifdef OS_INCLUDE_NAMESER
#ifndef OS_INCLUDE_NAMESER_INCLUDED
#define OS_INCLUDE_NAMESER_INCLUDED
#include <arpa/nameser.h>
#define DNS_STATUS STATUS
#endif
#endif

#ifdef OS_INCLUDE_RESOLV
#ifndef OS_INCLUDE_RESOLV_INCLUDED
#define OS_INCLUDE_RESOLV_INCLUDED
#include <resolv.h>
#endif
#endif

#ifdef OS_INCLUDE_CDEFS
#ifndef OS_INCLUDE_CDEFS_INCLUDED
#define OS_INCLUDE_CDEFS_INCLUDED
#include <sys/cdefs.h>
#endif
#endif

#ifdef OS_INCLUDE_PARAM
#ifndef OS_INCLUDE_PARAM_INCLUDED
#define OS_INCLUDE_PARAM_INCLUDED
#include <sys/param.h>
#endif
#endif

#ifdef OS_INCLUDE_NETDB
#ifndef OS_INCLUDE_NETDB_INCLUDED
#define OS_INCLUDE_NETDB_INCLUDED
#include <netdb.h>
#endif
#endif

#ifdef OS_INCLUDE_UIO
#ifndef OS_INCLUDE_UIO_INCLUDED
#define OS_INCLUDE_UIO_INCLUDED
#include <sys/uio.h>
#endif
#endif

#ifdef OS_INCLUDE_TYPES
#ifndef OS_INCLUDE_TYPES_INCLUDED
#define OS_INCLUDE_TYPES_INCLUDED
#include <sys/types.h>
#endif
#endif

#ifdef OS_INCLUDE_NETIF
#ifndef OS_INCLUDE_NETIF_INCLUDED
#define OS_INCLUDE_NETIF_INCLUDED
#include <net/if.h>
#endif
#endif

#ifdef OS_INCLUDE_SOCKIOS
#ifndef OS_INCLUDE_SOCKIOS_INCLUDED
#define OS_INCLUDE_SOCKIOS_INCLUDED
#include <linux/sockios.h>
#endif
#endif

#ifdef OS_INCLUDE_TFTP
#ifndef OS_INCLUDE_TFTP_INCLUDED
#define OS_INCLUDE_TFTP_INCLUDED
#ifndef TFTP_0ACK
#define TFTP_0ACK 6
#endif
#include <arpa/tftp.h>
#define TFTP_SEGSIZE SEGSIZE
#define	TFTP_RRQ RRQ
#define	TFTP_WRQ WRQ
#define	TFTP_DATA DATA
#define	TFTP_ACK ACK
#define	TFTP_ERROR ERROR
#endif
#endif

#ifdef OS_INCLUDE_TELNET
#ifndef OS_INCLUDE_TELNET_INCLUDED
#define OS_INCLUDE_TELNET_INCLUDED
#include <arpa/telnet.h>
#endif
#endif

#ifdef OS_INCLUDE_MIB_LIB
#ifndef OS_INCLUDE_MIB_LIB_INCLUDE
#define OS_INCLUDE_MIB_LIB_INCLUDE
#endif
#endif

#ifdef OS_INCLUDE_ROUTE
#ifndef OS_INCLUDE_ROUTE_INCLUDE
#define OS_INCLUDE_ROUTE_INCLUDE
#include <net/route.h>
#endif
#endif

