/*
 * Copyright (C) 1994-2020 Altair Engineering, Inc.
 * For more information, contact Altair at www.altair.com.
 *
 * This file is part of both the OpenPBS software ("OpenPBS")
 * and the PBS Professional ("PBS Pro") software.
 *
 * Open Source License Information:
 *
 * OpenPBS is free software. You can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * OpenPBS is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Commercial License Information:
 *
 * PBS Pro is commercially licensed software that shares a common core with
 * the OpenPBS software.  For a copy of the commercial license terms and
 * conditions, go to: (http://www.pbspro.com/agreement.html) or contact the
 * Altair Legal Department.
 *
 * Altair's dual-license business model allows companies, individuals, and
 * organizations to create proprietary derivative works of OpenPBS and
 * distribute them - whether embedded or bundled with other software -
 * under a commercial license agreement.
 *
 * Use of Altair's trademarks, including but not limited to "PBS™",
 * "OpenPBS®", "PBS Professional®", and "PBS Pro™" and Altair's logos is
 * subject to Altair's trademark licensing policies.
 */

#include <pbs_config.h>   /* the master config generated by configure */
#include <stdio.h>

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <netinet/tcp.h>
#include "portability.h"
#include "server_limits.h"
#include "libpbs.h"
#include "net_connect.h"
#include "pbs_error.h"
#include "libsec.h"
#include "pbs_internal.h"
#include "auth.h"


/**
 * @file	net_client.c
 */
static int conn_timeout = PBS_DIS_TCP_TIMEOUT_CONNECT; /* timeout for connect */

/**
 * @brief
 * 	engage_authentication - Use the security library interface to
 * 	engage the appropriate connection authentication.
 *
 * @param[in]      sd    socket descriptor to use in CS_* interface
 * @param[in]      addr  network address of the other party
 * @param[in]      port  associated port for the other party
 * @param[in]      authport_flags  authentication flags
 *
 * @return	int
 * @retval	 0  successful
 * @retval	-1 unsuccessful
 *
 * @par	Remark:	If the authentication fails, messages are logged to
 *              the server's log file and the connection's security
 *              information is closed out (freed).
 */

static int
engage_authentication(int sd, struct in_addr addr, int port, int authport_flags)
{
	int	ret;
	int mode;
	char ebuf[128];
#if !defined(WIN32)
	char	dst[INET_ADDRSTRLEN+1]; /* for inet_ntop */
#endif

	if (sd < 0) {
		cs_logerr(-1, __func__,	"Bad arguments, unable to authenticate.");
		return (-1);
	}

	mode = (authport_flags & B_SVR) ? CS_MODE_SERVER : CS_MODE_CLIENT;
	if (mode == CS_MODE_SERVER) {
		ret = CS_server_auth(sd);
		if (ret == CS_SUCCESS || ret == CS_AUTH_CHECK_PORT)
			return (0);
	} else if (mode == CS_MODE_CLIENT) {
		ret = CS_client_auth(sd);
		if (ret == CS_SUCCESS || ret == CS_AUTH_USE_IFF) {
			/*
				* For authentication via iff CS_client_auth
				* temporarily returning CS_AUTH_USE_IFF until such
				* time as iff becomes a part of CS_client_auth
				*/
			return (0);
		}
	}

#if defined(WIN32)
	/* inet_ntoa is thread-safe on windows */
	sprintf(ebuf,
		"Unable to authenticate with (%s:%d)",
		inet_ntoa(addr), port);
#else
	sprintf(ebuf,
		"Unable to authenticate with (%s:%d)",
		inet_ntop(AF_INET, (void *) &addr, dst,
		INET_ADDRSTRLEN), port);
#endif
	cs_logerr(-1, __func__, ebuf);

	if ((ret = CS_close_socket(sd)) != CS_SUCCESS) {
#if defined(WIN32)
		sprintf(ebuf, "Problem closing context (%s:%d)",
			inet_ntoa(addr), port);
#else
		sprintf(ebuf,
			"Problem closing context (%s:%d)",
			inet_ntop(AF_INET, (void *) &addr, dst,
			INET_ADDRSTRLEN), port);
#endif
		cs_logerr(-1, __func__, ebuf);
	}

	return (-1);
}

/*
 * @brief
 *      client_to_svr calls client_to_svr_extend to perform connection
 *      to server.
 *
 * @param[in]	hostaddr - address of host to which to connect (pbs_net_t)
 * @param[in]	port - port to which to connect
 * @param[in]	authport_flags  - flags or-ed together to describe
 *			authenication mode:
 *			BPRIV - use reserved local port
 *			BSVR  - Server mode if set, client mode if not
 * @returns	int
 * @retval	>=0 the socket obtained
 * @retval 	 PBS_NET_RC_FATAL (-1) if fatal error, just quit
 * @retval	 PBS_NET_RC_RETRY (-2) if temp error, should retry
 *
 */

int
client_to_svr(pbs_net_t hostaddr, unsigned int port, int authport_flags)
{
	return (client_to_svr_extend(hostaddr, port, authport_flags, NULL));
}

/**
 * @brief
 *	client_to_svr_extend - connect to a server
 *	Perform socket/tcp/ip stuff to connect to a server.
 *
 * @par Functionality
 *	Open a tcp connection to the specified address and port.
 *	Binds to a local socket, sets socket initially non-blocking,
 *	connects to remote system, resets sock blocking.
 *
 *	Note, the server's host address and port are was chosen as parameters
 *	rather than their names to possibly save extra look-ups.  It seems
 *	likely that the caller "might" make several calls to the same host or
 *	different hosts with the same port.  Let the caller keep the addresses
 *	around rather than look it up each time.
 *
 *	Special note: The reserved port mechanism is not needed when the
 *               the PBS authentication mechanism is not pbs_iff.  Being
 *               left in for minimal code change.  It should to be removed
 *               in a future version.
 *
 * @param[in]	hostaddr - address of host to which to connect (pbs_net_t)
 * @param[in]	port - port to which to connect
 * @param[in]	authport_flags  - flags or-ed together to describe
 *			authenication mode:
 *			BPRIV - use reserved local port
 *			BSVR  - Server mode if set, client mode if not
 * @param[in]   localaddr - host machine address to bind before connecting
 *                          to server.
 *
 * @returns	int
 * @retval	>=0 the socket obtained
 * @retval 	 PBS_NET_RC_FATAL (-1) if fatal error, just quit
 * @retval	 PBS_NET_RC_RETRY (-2) if temp error, should retry
 */

int
client_to_svr_extend(pbs_net_t hostaddr, unsigned int port, int authport_flags, char *localaddr)
{
	struct sockaddr_in	remote;
	int	sock;
	int	local_port;
	int	errn;
	int	rc;
#ifdef WIN32
	int	ret;
	int	non_block = 1;
	struct	linger      li;
	struct	timeval     tv;
	fd_set	            writeset;
#else
	struct pollfd	fds[1];
	pbs_socklen_t	len = sizeof(rc);
	int		oflag;
#endif


	/*	If local privilege port requested, bind to one	*/
	/*	Must be root privileged to do this		*/
	local_port = authport_flags & B_RESERVED;

	if (local_port) {
#ifdef	IP_PORTRANGE_LOW
		int			lport = IPPORT_RESERVED - 1;

		sock = rresvport(&lport);
		if (sock < 0) {
			if (errno == EAGAIN)
				return PBS_NET_RC_RETRY;
			else
				return PBS_NET_RC_FATAL;
		}
#else	/* IP_PORTRANGE_LOW */
		struct sockaddr_in	local;
		unsigned short		tryport;
		static unsigned short	start_port = 0;

		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0) {
			return PBS_NET_RC_FATAL;
		}

		if (start_port == 0) {	/* arbitrary start point */
			start_port = (getpid() %(IPPORT_RESERVED/2)) +
				IPPORT_RESERVED/2;
		}
		else if (--start_port < IPPORT_RESERVED/2)
			start_port = IPPORT_RESERVED - 1;
		tryport = start_port;

		memset(&local, 0, sizeof(local));
		local.sin_family = AF_INET;
		if (localaddr != NULL) {
			local.sin_addr.s_addr = inet_addr(localaddr);
			if (local.sin_addr.s_addr == INADDR_NONE) {
				perror("inet_addr failed");
				return (PBS_NET_RC_FATAL);
			}
		} else if (pbs_conf.pbs_public_host_name) {
			pbs_net_t public_addr;
			public_addr = get_hostaddr(pbs_conf.pbs_public_host_name);
			if (public_addr == (pbs_net_t)0) {
				return (PBS_NET_RC_FATAL);
			}
			local.sin_addr.s_addr = htonl(public_addr);
		}
		for (;;) {

			local.sin_port = htons(tryport);
			if (bind(sock, (struct sockaddr *)&local,
				sizeof(local)) == 0)
				break;
#ifdef WIN32
			errno = WSAGetLastError();
			if (errno != EADDRINUSE && errno != EADDRNOTAVAIL && errno != WSAEACCES) {
				closesocket(sock);
#else
			if (errno != EADDRINUSE && errno != EADDRNOTAVAIL) {
				close(sock);
#endif
				return PBS_NET_RC_FATAL;
			}
			else if (--tryport < (IPPORT_RESERVED/2)) {
				tryport = IPPORT_RESERVED - 1;
			}
			if (tryport == start_port) {
#ifdef WIN32
				closesocket(sock);
#else
				close(sock);
#endif
				return PBS_NET_RC_RETRY;
			}
		}
		/*
		 ** Ensure last tryport becomes start port on next call.
		 */
		start_port = tryport;
#endif	/* IP_PORTRANGE_LOW */
	}
	else {
		sock = socket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0) {
			return PBS_NET_RC_FATAL;
		}
	}

	remote.sin_addr.s_addr = htonl(hostaddr);

	remote.sin_port = htons((unsigned short)port);
	remote.sin_family = AF_INET;
#ifdef WIN32
	li.l_onoff = 1;
	li.l_linger = 5;

	setsockopt(sock, SOL_SOCKET, SO_LINGER, (char *)&li, sizeof(li));

	if (ioctlsocket(sock, FIONBIO, &non_block) == SOCKET_ERROR) {
		errno = WSAGetLastError();
		closesocket(sock);
		return (PBS_NET_RC_FATAL);
	}
#else
	oflag = fcntl(sock, F_GETFL);
	if (fcntl(sock, F_SETFL, (oflag | O_NONBLOCK)) == -1) {
		close(sock);
		return (PBS_NET_RC_FATAL);
	}
#endif

	if (connect(sock, (struct sockaddr *)&remote, sizeof(remote)) < 0) {

#ifdef WIN32
		errno = WSAGetLastError();
#endif
		/*
		 * Bacause of  threading, pbs_errno is actually a macro
		 * pointing to a variable within a tread context.  On certain
		 * platforms, the threading library resulted in errno being
		 * cleared after pbs_errno was set set from it, so save
		 * errno into a local variable first, then test it.
		 */
		errn = errno;
		pbs_errno = errn;
		switch (errn) {
#ifdef WIN32
			case WSAEINTR:
#else
			case EINTR:
#endif
			case EADDRINUSE:
			case ETIMEDOUT:
			case ECONNREFUSED:
#ifdef WIN32
				closesocket(sock);
#else
				close(sock);
#endif
				return (PBS_NET_RC_RETRY);

#ifdef WIN32
			case WSAEWOULDBLOCK:
				FD_ZERO(&writeset);
				FD_SET((unsigned int)sock, &writeset);
				tv.tv_sec = conn_timeout;	/* connect timeout */
				tv.tv_usec = 0;
				ret = select(1, NULL, &writeset, NULL, &tv);
				if (ret == SOCKET_ERROR) {
					errno = WSAGetLastError();
					errn = errno;
					pbs_errno = errn;
					closesocket(sock);
					return PBS_NET_RC_FATAL;
				} else if (ret == 0) {
					closesocket(sock);
					return PBS_NET_RC_RETRY;
				}
				break;
#else	/* UNIX */
			case EWOULDBLOCK:
			case EINPROGRESS:
				while (1) {
					fds[0].fd = sock;
					fds[0].events  = POLLOUT;
					fds[0].revents = 0;

					rc = poll(fds, (nfds_t)1, conn_timeout * 1000);
					if (rc == -1) {
						errn = errno;
						if ((errn != EAGAIN) && (errn != EINTR))
							break;
					} else
						break;	/* no error */
				}

				if (rc == 1) {
					/* socket may be connected and ready to write */
					rc = 0;
					if ((getsockopt(sock, SOL_SOCKET, SO_ERROR, &rc, &len) == -1) || (rc != 0)) {
						close(sock);
						return PBS_NET_RC_FATAL;
					}
					break;

				} else if (rc == 0) {
					/* socket not ready - not connected in time */
					close(sock);
					return PBS_NET_RC_RETRY;
				} else {
					/* socket not ready - error */
					close(sock);
					return PBS_NET_RC_FATAL;
				}
#endif	/* end UNIX */

			default:
#ifdef WIN32
				closesocket(sock);
#else
				close(sock);
#endif
				return (PBS_NET_RC_FATAL);
		}
	}

	/* reset socket to blocking */
#ifdef WIN32
	non_block = 0;
	if (ioctlsocket(sock, FIONBIO, &non_block) == SOCKET_ERROR) {
		errno = WSAGetLastError();
		closesocket(sock);
		return PBS_NET_RC_FATAL;
	}
#else	/* UNIX */
	if (fcntl(sock, F_SETFL, oflag) == -1) {
		close(sock);
		return (PBS_NET_RC_FATAL);
	}
#endif

	if (engage_authentication(sock,
		remote.sin_addr, port, authport_flags) != -1)
		return sock;

	/*authentication unsuccessful*/

#ifdef WIN32
	closesocket(sock);
#else
	close(sock);
#endif
	return (PBS_NET_RC_FATAL);
}

/**
 * @brief
 *      This function sets socket options to TCP_NODELAY
 * @param fd
 * @return 0 for SUCCESS
 *        -1 for FAILURE
 */
int
set_nodelay(int fd)
{
	int opt;
	pbs_socklen_t optlen;

	optlen = sizeof(opt);
	if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, &optlen) == -1)
		return 0;

	if (opt == 1)
		return 0;

	opt = 1;
	return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}
