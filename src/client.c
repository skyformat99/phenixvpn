
#include "shadowvpn.h"

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>

//#include "uthash.h"
#include "portable_endian.h"

#ifndef TARGET_WIN32
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#else
//#include <winsock2.h>
typedef unsigned long in_addr_t;
//#define inet_ntop InetNtop
#endif

typedef struct iphdr ipv4_hdr_t;
typedef struct ipv6hdr ipv6_hdr_t;

cli_ctx_t * client_init(shadowvpn_args_t *args, int is_cli)
{
	int i;
	struct sockaddr addr;
	socklen_t addrlen;
	uint32_t netip;
	in_addr_t n_addr = inet_addr(args->net_ip);
	char pwd[50] = {0};
	if (n_addr == INADDR_NONE) {
		errf("Error: invalid net IP in config file: %s", args->net_ip);
		return NULL;
	}

	netip = ntohl((uint32_t)n_addr);

	cli_ctx_t *ctx = malloc(sizeof(cli_ctx_t));
  bzero(ctx, sizeof(cli_ctx_t));
	//init ctx
	ctx->concurrency = args->concurrency;
	ctx->channels = args->channels;
	ctx->type = is_cli?CLI_CLI:CLI_SRV;
	//add client
	if(ctx->type == CLI_CLI){
		//add a client,and set it to ctx
		sprintf(pwd, "%s%02d",args->password, netip);
		ctx->cli = client_add(ctx, htonl(netip), pwd);
		for(i = 0; i < args->concurrency; i++){
			//for client side add default remote addr(or addr list),
			//client will use these addresses to send data.
			channel_udp_addr(args->server, args->port + i, &addr, &addrlen);
			strategy_update_remote_addr_list(ctx->cli->strategy, 0, &addr, addrlen);
		}
	}else{
		//*.*.*.0 not used, *.*.*.1 reserved for srv
		//add a sets of clients, for reponse client
		//asume netip:*.*.*.1 so alloc(*.*.*.2~*.*.*.255)
	  for (i = 1; i < args->clients; i++) {
			sprintf(pwd, "%s%02d",args->password, netip + i);
			client_add(ctx, htonl(netip + i), pwd);
		}
	}

	return ctx;
}

cli_info_t * client_add(cli_ctx_t *ctx, uint32_t netip, const char *pwd)
{
	cli_info_t *cli = malloc(sizeof(cli_info_t));
	bzero(cli, sizeof(cli_info_t));

	cli->ctx = ctx;
	cli->strategy = strategy_init(ctx->concurrency, ctx->channels, ctx->type == CLI_CLI?STRATEGY_RND:STRATEGY_TIME);
	// assign IP based on tun IP and user tokens
	// for example:
	//		 tun IP is 10.7.0.1
	//		 client IPs will be 10.7.0.2, 10.7.0.3, 10.7.0.4, etc
	cli->output_tun_ip = netip;
	cli->pwd = strdup(pwd);
	cli->key = crypto_gen_key(pwd, strlen(pwd));
	struct in_addr in;
	in.s_addr = netip;
	logf("allocate output_tun_ip %s, pwd:%s", inet_ntoa(in), pwd);
	// add to hash: ctx->ip_to_clients[output_tun_ip] = client
	HASH_ADD(hh, ctx->ip_to_clients, output_tun_ip, 4, cli);

	return cli;
}

char *client_show_cli_ip(cli_info_t *cli, char *ip)
{
	struct in_addr in;
	in.s_addr = cli->output_tun_ip;

	strcpy(ip, inet_ntoa(in));
	return ip;
}

char *client_show_curr_ip(cli_ctx_t *ctx, char *ip){
	return client_show_cli_ip(ctx->cli, ip);
}

int get_client_by_netip(cli_ctx_t *ctx, uint32_t netip)
{
	struct in_addr in;
	in.s_addr = netip;
	ctx->cli = NULL;

	HASH_FIND(hh, ctx->ip_to_clients, &netip, 4, ctx->cli);
	if (ctx->cli == NULL) {
		logf("nat: client not found for given netip:%s", inet_ntoa(in));
		return -1;
	}

	//logf("nat: client found for given netip:%s", inet_ntoa(in));
	return 0;
}

int client_check_add(cli_ctx_t *ctx, uint32_t netip, const char *pwd)
{
	if(get_client_by_netip(ctx, netip) != 0){
		client_add(ctx, netip, pwd);
		return 0;
	}else{
		struct in_addr in;
		in.s_addr = netip;
		//TODO: client already exist.
		errf("client already exit for specified netip:%s", inet_ntoa(in));
		return -1;
	}
}

int client_remove(cli_ctx_t *ctx, uint32_t netip)
{
	if(get_client_by_netip(ctx, netip)){
		HASH_DELETE(hh, ctx->ip_to_clients, ctx->cli);
		free(ctx->cli->pwd);
		free(ctx->cli);
	}
	return 0;
}

int get_client_by_ipaddr(cli_ctx_t *ctx, struct sockaddr_storage *addr, socklen_t addrlen)
{
	char addr_c[INET6_ADDRSTRLEN] = {0};
	struct sockaddr_in *in_addr = (struct sockaddr_in *)addr;

	//inet_ntop(in_addr->sin_family, &in_addr->sin_addr.s_addr, s_addr, INET6_ADDRSTRLEN);

	if(in_addr->sin_family == AF_INET){
		strcpy(addr_c,inet_ntoa(in_addr->sin_addr));
		HASH_FIND(hh, ctx->ip_to_clients, &in_addr->sin_addr.s_addr, 4, ctx->cli);
	}else
		errf("nat: in_addr->sin_family not AF_INET:%d,", in_addr->sin_family);

	if (ctx->cli == NULL) {
		errf("nat: client not found for given addr:%s", addr_c);
		return -1;
	}
	return 0;

}

//get client according to ipv4 head addr(saddr/daddr)
int get_client_by_iphdr(cli_ctx_t *ctx, unsigned char *buf, size_t buflen, int is_saddr)
{
	ipv4_hdr_t *iphdr = (ipv4_hdr_t *)(buf);

	uint8_t iphdr_len;
	char sa_s[INET6_ADDRSTRLEN] ={0},da_s[INET6_ADDRSTRLEN] = {0};
	struct in_addr addr;

	addr.s_addr = iphdr->daddr;
	strcpy(da_s, inet_ntoa(addr));
	addr.s_addr = iphdr->saddr;
	strcpy(sa_s, inet_ntoa(addr));

	ctx->cli = NULL;
	if ((iphdr->version & 0xf) != 0x4) {
		// check header, currently IPv4 only
		// bypass IPv6
		//ipv6_hdr_t *ipv6hdr = (ipv6_hdr_t *)(buf);

		//inet_ntop(AF_INET6, &ipv6hdr->daddr, da_s, INET6_ADDRSTRLEN);
		//inet_ntop(AF_INET6, &ipv6hdr->saddr, sa_s, INET6_ADDRSTRLEN);
		//errf("%s ipv6 not support version:0x%x,is_saddr:%d, saddr:%s,daddr:%s", __func__, iphdr->version, is_saddr, sa_s, da_s);
		logf("%s ipv6 not support version:0x%x", __func__, iphdr->version);
		return 0;
	}

	iphdr_len = (iphdr->ihl & 0x0f) * 4;

	if(is_saddr)
		HASH_FIND(hh, ctx->ip_to_clients, &iphdr->saddr, 4, ctx->cli);
	else
		HASH_FIND(hh, ctx->ip_to_clients, &iphdr->daddr, 4, ctx->cli);

	if (ctx->cli == NULL) {
		errf("nat: client not found for given addr is_saddr:%d, saddr:%s,daddr:%s", is_saddr, sa_s, da_s);
		return -1;
	}
	return 0;
}

//get client according to ipv4 head daddr, used at down stream
int get_client_by_daddr(cli_ctx_t *ctx, unsigned char *buf, size_t buflen)
{
	return get_client_by_iphdr(ctx, buf, buflen, 0);
}

//get client according to ipv4 head saddr, used at up stream
int get_client_by_saddr(cli_ctx_t *ctx, unsigned char *buf, size_t buflen)
{
	return get_client_by_iphdr(ctx, buf, buflen, 1);
}

