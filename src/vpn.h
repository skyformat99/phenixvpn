/**
  vpn.h

  Copyright (C) 2015 clowwindy

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#ifndef VPN_H
#define VPN_H

#include <time.h>

#include "args.h"
#include "client.h"

typedef struct {
  int running;
  int tun;
  /* select() in winsock doesn't support file handler */
#ifndef TARGET_WIN32
  int control_pipe[2];
#else
  int control_fd;
  struct sockaddr control_addr;
  socklen_t control_addrlen;
  HANDLE cleanEvent;
#endif
  unsigned char *tun_buf;
  unsigned char *udp_buf;

	vpn_channel_t *channel;
	cli_ctx_t *cli_ctx;
  /* the address we currently use */
  struct sockaddr_storage remote_addr;
  struct sockaddr *remote_addrp;
  socklen_t remote_addrlen;
  int channel_id;

  shadowvpn_args_t *args;
} vpn_ctx_t;

/* return -1 on error. no need to destroy any resource */
int vpn_ctx_init(vpn_ctx_t *ctx, shadowvpn_args_t *args);

/* return -1 on error. no need to destroy any resource */
int vpn_run(vpn_ctx_t *ctx);

/* return -1 on error. no need to destroy any resource */
int vpn_stop(vpn_ctx_t *ctx);

/* these low level functions are exposed for Android jni */
#ifndef TARGET_WIN32
int vpn_tun_alloc(const char *dev);
#endif
#endif
