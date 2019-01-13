/* -*- coding: utf-8 -*-
 *
 * January 12 2019, Christian Hopps <chopps@gmail.com>
 *
 */

#include <arpa/inet.h>
#include <err.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

void tfs_tunnel(int, int);

char progname[128];

struct frame {
	uint8_t flags[2];
	uint8_t proto[2];
	uint8_t data[0];
};

void
usage(void)
{
	fprintf(stderr, "usage: %s [-c|--connect server] [-p|--port service]\n",
		progname);
	exit(1);
}

int
tun_alloc(char *dev)
{
	struct ifreq ifr;
	int fd;

	if ((fd = open("/dev/net/tun", O_RDWR)) < 0)
		err(1, "open(/dev/net/tun)");

	memset(&ifr, 0, sizeof(ifr));
	ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
	strncpy(ifr.ifr_name, dev, IFNAMSIZ);

	if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0)
		err(1, "ioctl(TUNSETIFF)");

	strcpy(dev, ifr.ifr_name);

	return fd;
}

int
tfs_connect(const char *sname, const char *service)
{
	struct addrinfo hints = {.ai_socktype = SOCK_STREAM};
	struct addrinfo *result, *rp;
	int s;

	if (getaddrinfo(sname, service, &hints, &result))
		err(1, "client getaddrinfo");
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (s < 0)
			continue;
		if (connect(s, rp->ai_addr, rp->ai_addrlen) != -1)
			break;
	}
	if (rp == NULL)
		err(1, "client can't connect to %s:%s", sname, service);

	freeaddrinfo(result);
	return s;
}

int
tfs_accept(const char *sname, const char *service)
{
	struct addrinfo hints = {.ai_socktype = SOCK_STREAM};
	struct addrinfo *result, *rp;
	struct sockaddr_in caddr;
	socklen_t calen;
	int optval, s, ss;

	if (getaddrinfo(sname, service, &hints, &result))
		err(1, "server getaddrinfo");
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (s < 0)
			continue;

		optval = 1;
		setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,
			   sizeof(int));

		if (bind(s, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
	}
	if (rp == NULL)
		err(1, "server can't bind to %s:%s", sname, service);

	freeaddrinfo(result);

	if (listen(s, 5) < 0)
		err(1, "server listen");
	calen = sizeof(caddr);
	if ((ss = accept(s, (struct sockaddr *)&caddr, &calen)) < 0)
		err(1, "server accept");
	return ss;
}

int
main(int argc, char **argv)
{
	static struct option lopts[] = {
	    {"help", no_argument, 0, 'h'},
	    {"connect", required_argument, 0, 'c'},
	    {"dev", required_argument, 0, 'd'},
	    {"listen", required_argument, 0, 'l'},
	    {"port", required_argument, 0, 'p'},
	    {0, 0, 0, 0},
	};
	const char *listen = "::";
	const char *server = NULL;
	const char *sport = NULL;
	char devname[IFNAMSIZ + 1] = "vtun%d";
	int fd, s, opt, li;

	strncpy(progname, argv[0], sizeof(progname) - 1);

	while ((opt = getopt_long(argc, argv, "c:d:hl:p:", lopts, &li)) != -1) {
		switch (opt) {
		case 'c':
			/* connect */
			server = optarg;
			break;
		case 'd':
			/* dev */
			strncpy(devname, optarg, IFNAMSIZ - 1);
			break;
		case 'l':
			/* listen */
			listen = optarg;
			break;
		case 'h':
		case 'p':
			/* port */
			sport = optarg;
			break;
		case '?':
			usage();
			break;
		}
	}

	fd = tun_alloc(devname);
	printf("opened tun device: %s fd: %d\n", devname, fd);

	if (server == NULL) {
		s = tfs_accept(listen, sport);
		printf("accepted from client %d\n", s);
	} else {
		s = tfs_connect(server, sport);
		printf("connected to server %d\n", s);
	}

	tfs_tunnel(fd, s);

	sleep(10);

	return 0;
}

/* Local Variables: */
/* c-file-style: "bsd" */
/* c-c++-enable-clang-format-on-save: t */
/* End: */
