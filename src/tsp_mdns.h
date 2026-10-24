/*
 * tsp_mdns.h -- ".local" の名前を mDNS で引く (tsp_mdns.c)
 */
#ifndef TSP_MDNS_H
#define TSP_MDNS_H
#include "lwip/dns.h"

int tsp_mdns_is_local(const char *name);
err_t tsp_mdns_lookup(const char *name, dns_found_callback cb, void *arg);
/* dns_gethostbyname と同じ形。".local" なら mDNS、ほかは DNS */
err_t tsp_gethostbyname(const char *name, ip_addr_t *ip, dns_found_callback cb, void *arg);

#endif
