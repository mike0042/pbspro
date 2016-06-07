#include <pbs_config.h>   /* the master config generated by configure */

#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <netdb.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdlib.h>

#include "port_forwarding.h"

#define ERR_MSG_LEN 1024

#define PF_LOGGER(logfunc, msg) if(logfunc != NULL) { logfunc(msg); }

/* handy utility to handle forwarding socket connections to another host
 * pass in an initialized pfwdsock struct with sockets to listen on, a function
 * pointer to get a new socket for forwarding, and a hostname and port number to
 * pass to the function pointer, and it will do the rest. The caller probably
 * should fork first since this function is an infinite loop and never returns */

/* __attribute__((noreturn)) - how do I do this portably? */
int x11_reader_go =1;

extern int set_nodelay(int fd);

/**
 * @brief
 *      This function provides the port forwarding feature for forwarding the
 *      X data from mom to qsub and from qsub to the X server.
 *
 * @param socks[in] - Input structure which tracks the sockets that are active
 *                    and data read/written by peers.
 * @param connfunc[in] - Function pointer pointing to a function used for
 *                       either connecting the X server (if running in qsub) or
 *                       connecting qsub (if running in mom).
 * @param phost[in] - peer host that needs to be connected.
 * @param pport[in] - peer port number.
 * @param inter_read_sock[in] -  socket descriptor from where mom and qsub
 *                               readers read data.
 * @param readfunc[in] - function pointer pointing to the mom and qsub readers.
 * @param logfunc[in] - Function pointer for log function
 *
 * @return void
 */
void
port_forwarder(
	struct pfwdsock *socks,
	int (*connfunc)(char *, long),
	char *phost,
	int pport,
	int inter_read_sock,
	int (*readfunc)(int),
	void (*logfunc) (char *))
{
	fd_set rfdset, wfdset, efdset;
	int rc, maxsock = 0;
	struct sockaddr_in from;
	pbs_socklen_t fromlen;
	int n, n2, sock;
	fromlen = sizeof(from);
	char err_msg[LOG_BUF_SIZE];
	int readfunc_ret;
        /*
         * Make the sockets in the socks structure non blocking
         */
	for (n = 0; n < NUM_SOCKS; n++) {
		if (!(socks + n)->active || ((socks + n)->sock < 0))
			continue;
		if (set_nonblocking((socks + n)->sock) == -1) {
			close((socks + n)->sock);
			(socks + n)->active = 0;
			snprintf(err_msg, sizeof(err_msg),
				"set_nonblocking failed for socket=%d, errno=%d",
				(socks + n)->sock, errno);
			PF_LOGGER(logfunc, err_msg);
			continue;
		}
		if (set_nodelay((socks + n)->sock) == -1) {
			snprintf(err_msg, sizeof(err_msg),
				"set_nodelay failed for socket=%d, errno=%d",
				(socks + n)->sock, errno);
			PF_LOGGER(logfunc, err_msg);
		}
	}

	while (x11_reader_go) {
		FD_ZERO(&rfdset);
		FD_ZERO(&wfdset);
		FD_ZERO(&efdset);
		maxsock = inter_read_sock + 1;
		/*setting the sock fd in rfdset for qsub and mom readers to read data*/
		FD_SET(inter_read_sock, &rfdset);
		FD_SET(inter_read_sock, &efdset);
		for (n = 0; n < NUM_SOCKS; n++) {
			if (!(socks + n)->active || ((socks + n)->sock < 0))
				continue;

			if ((socks + n)->listening) {
				FD_SET((socks + n)->sock, &rfdset);
				maxsock = (socks + n)->sock > maxsock ?(socks + n)->sock : maxsock;
			} else{
				if ((socks + n)->bufavail < PF_BUF_SIZE) {
					FD_SET((socks + n)->sock, &rfdset);
					maxsock = (socks + n)->sock > maxsock ?(socks + n)->sock : maxsock;
				}
				if ((socks + ((socks + n)->peer))->bufavail -
					(socks + ((socks + n)->peer))->bufwritten > 0) {
					FD_SET((socks + n)->sock, &wfdset);
					maxsock = (socks + n)->sock > maxsock ?(socks + n)->sock : maxsock;
				}
			}

		}

		maxsock++;

		rc = select(maxsock, &rfdset, &wfdset, &efdset, NULL);
		if ((rc == -1) && (errno == EINTR))
			continue;
		if (rc < 0) {
			snprintf(err_msg, sizeof(err_msg),
				"port forwarding select() error");
			PF_LOGGER(logfunc, err_msg);
			return;
		}
		if (FD_ISSET(inter_read_sock, &efdset)) {
			snprintf(err_msg, sizeof(err_msg),
				"exception for socket=%d, errno=%d",
				inter_read_sock, errno);
			PF_LOGGER(logfunc, err_msg);
			close(inter_read_sock);
			return;
		}
		if (FD_ISSET(inter_read_sock, &rfdset)) {
			/*calling mom/qsub readers*/
			readfunc_ret = readfunc(inter_read_sock);
			if (readfunc_ret == -1) {
				snprintf(err_msg, sizeof(err_msg),
					"readfunc failed for socket:%d", inter_read_sock);
				PF_LOGGER(logfunc, err_msg);
			}
			if (readfunc_ret  < 0) {
				return;
			}
		}

		for (n = 0; n < NUM_SOCKS; n++) {
			if (!(socks + n)->active || ((socks + n)->sock < 0))
				continue;
			if (FD_ISSET((socks + n)->sock, &rfdset)) {
				if ((socks + n)->listening && (socks + n)->active) {
					int newsock = 0, peersock = 0;
					if ((sock = accept((socks + n)->sock, (struct sockaddr *)
						& from, &fromlen)) < 0) {
						if ((errno == EAGAIN) || (errno == EWOULDBLOCK)
							|| (errno == EINTR) || (errno == ECONNABORTED))
							continue;
						snprintf(err_msg, sizeof(err_msg),
							"closing the socket %d after accept call failure, errno=%d",
							(socks + n)->sock, errno);
						PF_LOGGER(logfunc, err_msg);
						close((socks + n)->sock);
						(socks + n)->active = 0;
						continue;
					}
                                        /*
                                         * Make the sock non blocking
                                         */
					if (set_nonblocking(sock) == -1) {
						snprintf(err_msg, sizeof(err_msg),
							"set_nonblocking failed for socket=%d, errno=%d",
							sock, errno);
						PF_LOGGER(logfunc, err_msg);
						close(sock);
						continue;
					}
					if (set_nodelay(sock) == -1) {
						snprintf(err_msg, sizeof(err_msg),
							"set_nodelay failed for socket=%d, errno=%d",
							sock, errno);
						PF_LOGGER(logfunc, err_msg);
					}

					newsock = peersock = 0;

					for (n2 = 0; n2 < NUM_SOCKS; n2++) {
						if ((socks + n2)->active || (((socks + n2)->peer != 0)
							&& (socks + ((socks + n2)->peer))->active))
							continue;
						if (newsock == 0)
							newsock = n2;
						else if (peersock == 0)
							peersock = n2;
						else
							break;
					}

					(socks + newsock)->sock = (socks + peersock)->remotesock
						= sock;
					(socks + newsock)->listening = (socks + peersock)->listening
						= 0;
					(socks + newsock)->active = (socks + peersock)->active = 1;
					(socks + peersock)->sock = connfunc(phost, pport);
                                        /*
                                         * Make sockets non-blocking
                                         */
					if (set_nonblocking((socks + peersock)->sock) == -1) {
						snprintf(err_msg, sizeof(err_msg),
							"set_nonblocking failed for socket=%d, errno=%d",
							(socks + peersock)->sock, errno);
						PF_LOGGER(logfunc, err_msg);
						close((socks + peersock)->sock);
						(socks + peersock)->active = 0;
						continue;
					}
					if (set_nodelay((socks + peersock)->sock) == -1) {
						snprintf(err_msg, sizeof(err_msg),
							"set_nodelay failed for socket=%d, errno=%d",
							(socks + peersock)->sock, errno);
						PF_LOGGER(logfunc, err_msg);
					}
					(socks + newsock)->bufwritten = (socks + peersock)->bufwritten = 0;
					(socks + newsock)->bufavail = (socks + peersock)->bufavail = 0;
					(socks + newsock)->buff[0] = (socks + peersock)->buff[0] = '\0';
					(socks + newsock)->peer = peersock;
					(socks + peersock)->peer = newsock;
				} else{
					/* non-listening socket to be read */
					rc = read(
						(socks + n)->sock,
						(socks + n)->buff + (socks + n)->bufavail,
						PF_BUF_SIZE - (socks + n)->bufavail);
					if (rc == -1) {
						if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINTR) || (errno == EINPROGRESS)) {
							continue;
						}
						shutdown((socks + n)->sock, SHUT_RDWR);
						close((socks + n)->sock);
						(socks + n)->active = 0;
						snprintf(err_msg, sizeof(err_msg),
							"closing the socket %d after read failure, errno=%d",
							(socks + n)->sock, errno);
						PF_LOGGER(logfunc, err_msg);
					} else if (rc == 0) {
						shutdown((socks + n)->sock, SHUT_RDWR);
						close((socks + n)->sock);
						(socks + n)->active = 0;
					} else{
						(socks + n)->bufavail += rc;
					}
				}
			} /* END if rfdset */
			if (FD_ISSET((socks + n)->sock, &wfdset)) {
				int peer = (socks + n)->peer;

				rc = write(
					(socks + n)->sock,
					(socks + peer)->buff + (socks + peer)->bufwritten,
					(socks + peer)->bufavail - (socks + peer)->bufwritten);

				if (rc == -1) {
					if ((errno == EWOULDBLOCK) || (errno == EAGAIN) || (errno == EINTR) || (errno == EINPROGRESS)) {
						continue;
					}
					shutdown((socks + n)->sock, SHUT_RDWR);
					close((socks + n)->sock);
					(socks + n)->active = 0;
					snprintf(err_msg, sizeof(err_msg),
						"closing the socket %d after write failure, errno=%d",
						(socks + n)->sock, errno);
					PF_LOGGER(logfunc, err_msg);
				} else if (rc == 0) {
					shutdown((socks + n)->sock, SHUT_RDWR);
					close((socks + n)->sock);
					(socks + n)->active = 0;
				} else{
					(socks + peer)->bufwritten += rc;
				}
			} /* END if wfdset */
			if (!(socks + n)->listening) {
				int peer = (socks + n)->peer;
				if ((socks + peer)->bufavail == (socks + peer)->bufwritten) {
					(socks + peer)->bufavail = (socks + peer)->bufwritten = 0;
				}
				if (!(socks + peer)->active && ((socks + peer)->bufwritten
					== (socks + peer)->bufavail)) {
					shutdown((socks + n)->sock, SHUT_RDWR);
					close((socks + n)->sock);
					(socks + n)->active = 0;
				}
			}

		} /* END foreach fd */


	} /* END while(x11_reader_go) */
}  /* END port_forwarder() */


/**
 * @brief
 *      This function returns a socket to the local X11 unix server.
 *
 * @param[in] dnr   Display number to which it has to connect to.
 *
 * @return	int
 * @retval	Socket fd connected to the local X11 unix server.	success
 * @retval  	-1  							Failure
 */
int
connect_local_xsocket(u_int dnr)
{
	int sock;
	struct sockaddr_un addr;

	if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "socket: %.100s", strerror(errno));
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), X_UNIX_PATH, dnr);

	if (connect(sock, (struct sockaddr *) & addr, sizeof(addr)) == 0)
		return sock;

	close(sock);
	fprintf(stderr, "connect %.100s: %.100s", addr.sun_path, strerror(errno));
	return (-1);
}

/**
 * @brief
 *      This function is called whenever there is a connection accepted by the
 *      port forwarder at qsub side. It will further send the data read by port
 *      forwarder to the x server listening on the display number set in the
 *      environment.
 * @param[in] display - The display number where X server is listening in
 *                      qsub.
 * @param[in] alsounused - This parameter is not used. its there just to
 *                         maintain consistency between function pointers used
 *                         by port_forwarder.
 * @return	int
 * @retval	socket number which is connected to Xserver.	success
 * @retval 	-1   						Failure
 */
int
x11_connect_display(
	char *display,
	long alsounused)
{
	int display_number, sock = 0;
	char buf[1024], *cp;
	struct addrinfo hints, *ai, *aitop;
	char strport[NI_MAXSERV];
	int gaierr;

	/*
	 * Now we decode the value of the DISPLAY variable and make a
	 * connection to the real X server.
	 */

	/*
	 * Check if it is a unix domain socket.  Unix domain displays are in
	 * one of the following formats: unix:d[.s], :d[.s], ::d[.s]
	 */
	if (strncmp(display, "unix:", 5) == 0 ||
		display[0] == ':') {
		/* Connect to the unix domain socket. */
		if (sscanf(strrchr(display, ':') + 1, "%d", &display_number) != 1) {
			fprintf(stderr, "Could not parse display number from DISPLAY: %.100s",
				display);
			return -1;
		}
		/* Create a socket. */
		sock = connect_local_xsocket(display_number);
		if (sock < 0)
			return -1;
		/* OK, we now have a connection to the display. */
		return sock;
	}

	/*
	 * Connect to an inet socket.  The DISPLAY value is supposedly
	 * hostname:d[.s], where hostname may also be numeric IP address.
	 */
	strncpy(buf, display, sizeof(buf));
	cp = strchr(buf, ':');
	if (!cp) {
		fprintf(stderr, "Could not find ':' in DISPLAY: %.100s", display);
		return -1;
	}

	*cp = 0;
	/* buf now contains the host name.  But first we parse the display number. */
	if (sscanf(cp + 1, "%d", &display_number) != 1) {
		fprintf(stderr, "Could not parse display number from DISPLAY: %.100s",
			display);
		return -1;
	}

	/* Look up the host address */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(strport, sizeof(strport), "%d", 6000 + display_number);
	if ((gaierr = getaddrinfo(buf, strport, &hints, &aitop)) != 0) {
		fprintf(stderr, "%100s: unknown host. (%s)", buf, gai_strerror(gaierr));
		return -1;
	}

	for (ai = aitop; ai; ai = ai->ai_next) {
		/* Create a socket. */
		sock = socket(ai->ai_family, SOCK_STREAM, 0);
		if (sock < 0) {
			fprintf(stderr, "socket: %.100s", strerror(errno));
			continue;
		}

		/* Connect it to the display. */
		if (connect(sock, ai->ai_addr, ai->ai_addrlen) < 0) {
			fprintf(stderr, "connect %.100s port %d: %.100s", buf,
				6000 + display_number, strerror(errno));
			close(sock);
			continue;
		}

		/* Success */
		break;
	}

	freeaddrinfo(aitop);
	if (!ai) {
		fprintf(stderr, "connect %.100s port %d: %.100s", buf, 6000 + display_number,
			strerror(errno));
		return -1;
	}

	set_nodelay(sock);
	return sock;
}
/**
 * @brief
 *      Set the given file descriptor to non blocking mode.
 *      Calling this on a socket causes all future read() and write() calls on
 *      that socket to do only as much as they can immediately, and return 
 *      without waiting.
 *      If no data can be read or written, they return -1 and set errno
 *      to EAGAIN or EWOULDBLOCK.
 *
 * @param[in] fd - file descriptor
 *
 * @return	int
 * @retval	1	success
 * @retval 	-1   	Failure
 */
int
set_nonblocking(int fd)
{
	int flags;

	if ((flags = fcntl(fd, F_GETFL, 0)) == -1) 
		flags = 0;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
		return -1;
	else
		return 1;
}