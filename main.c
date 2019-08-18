#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_ether.h>
#include <rte_byteorder.h>
#include "rte_rwlock.h"
#include "rte_cfgfile.h"


static struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static uint16_t nb_rxd = 1024;
static uint16_t nb_txd = 1024;
#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
#define MAX_PKT_BURST 32
#define MEMPOOL_CACHE_SIZE 256
struct lcore_queue_conf {
	unsigned n_rx_port;
	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
} __rte_cache_aligned;
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];
static int nb_flows;
#define MAX_NAME_LENGTH 100
#define MAX_IP_NUMBER_PER_FLOW 10
#define MAX_FLOW_NUMBER 10
struct flow_info {
	char name[MAX_NAME_LENGTH];
	unsigned int nb_ip;
	uint32_t ip[MAX_IP_NUMBER_PER_FLOW];
	unsigned long bytes;
};
struct flow_info flow_infos[MAX_FLOW_NUMBER];
static volatile bool force_quit;
struct rte_mempool* pktmbuf_pool = NULL;
// static struct ether_addr ethernet_port[RTE_MAX_ETHPORTS];
static unsigned long enabled_port_mask;
static char* file_name = NULL;
static unsigned int rx_queue_per_lcore = 1;
static rte_rwlock_t rwl[MAX_FLOW_NUMBER];
static uint64_t timer_period = 1; /* default period is 1 seconds */
static uint64_t tsc_period;
static void
signal_handler(int signal_number) {
	if (signal_number == SIGINT || signal_number == SIGTERM) {
		printf("\nSignal %d is received. Preparing to exit.\n",signal_number);
		force_quit = true;
	}
}

static int 
parse_portmask_args(const char* portmask) {
	char* end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}

static int 
parse_args(int argc, char** argv) {
	int ret, opt;
	int option_index;
	char** argvopt = argv;
	char* prgname = argv[0];
	const char short_options[] = "p:t:";
	const struct option long_options[] = {
		{"file", required_argument, NULL, 1},
		{NULL, 0, 0, 0}
	};

	while ((opt = getopt_long(argc, argvopt, short_options, 
		long_options, &option_index)) != -1){
		switch (opt) {
		case 'p':
			enabled_port_mask = parse_portmask_args(optarg);
			if (enabled_port_mask == 0) {
				fprintf(stderr, "Invalid portmask.\n");
				return -1;
			}
			break;
		case 't':
			timer_period = atoi(optarg);
			break;
		case 1:
			file_name = malloc(strlen(optarg));
			strcpy(file_name, optarg);
			break;
		case 0:
			break;
		default:
			return -1;
		}
	}
	ret = optind - 1;
	return ret;
}

/* todo */
static void
check_all_ports_link_status(uint32_t port_mask) {
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint16_t portid;
	uint8_t count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		RTE_ETH_FOREACH_DEV(portid) {
			if (force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf(
						"Port%d Link Up. Speed %u Mbps - %s\n",
						portid, link.link_speed,
						(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
						("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n", portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0) {
			printf(".");
			fflush(stdout);
			rte_delay_ms(CHECK_INTERVAL);
		}

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1)) {
			print_flag = 1;
			printf("done\n");
		}
	}
}

static inline size_t
get_vlan_offset(struct ether_hdr* eth_hdr, uint16_t* proto)
{
	size_t vlan_offset = 0;
	if (rte_cpu_to_be_16(ETHER_TYPE_VLAN) == *proto) {
		struct vlan_hdr* vlan_hdr = (struct vlan_hdr*)(eth_hdr + 1);
		vlan_offset = sizeof(struct vlan_hdr);
		*proto = vlan_hdr->eth_proto;
		if (rte_cpu_to_be_16(ETHER_TYPE_VLAN) == *proto) {
			vlan_hdr = vlan_hdr + 1;
			*proto = vlan_hdr->eth_proto;
			vlan_offset += sizeof(struct vlan_hdr);
		}
	}
	return vlan_offset;
}

/* accelerate the bytes */
static void
do_work(struct rte_mbuf* m) {
	struct ether_hdr* eth_hdr;
	struct ipv4_hdr* ipv4_hdr;
	uint16_t ether_type, offset=0;
	int i, j;

	eth_hdr = rte_pktmbuf_mtod(m,struct ether_hdr*);
	ether_type = eth_hdr->ether_type;
	offset = get_vlan_offset(eth_hdr, &ether_type);

	if (ether_type == rte_cpu_to_be_16(ETHER_TYPE_ARP)) {
		/* handle arp */
	}else if (eth_hdr->ether_type == rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
		ipv4_hdr = (struct ipv4_hdr*)((char*)(eth_hdr + 1)+offset);
		for (i = 0; i < MAX_FLOW_NUMBER; i++) {
			for (j = 0; j < flow_infos[i].nb_ip; j++) {
				if (ipv4_hdr->dst_addr == flow_infos[i].ip[j]) {
					rte_rwlock_write_lock(&rwl[i]);
					flow_infos[i].bytes += m->data_len;
					rte_rwlock_write_unlock(&rwl[i]);
					return;
				}
			}
		}
	}
}

/* display the rates of all flows */
static void 
display_statistics() {
	int i;
	unsigned long bit;
	for (i = 0; i < nb_flows; i++) {
		rte_rwlock_write_lock(&rwl[i]);
		bit = flow_infos[i].bytes * 8 / timer_period;
		flow_infos[i].bytes = 0;
		rte_rwlock_write_unlock(&rwl[i]);
		if ( bit > 1000000) {
			printf("flow_name:%s\trate:%.4fMbps,%ld bps \n", 
				flow_infos[i].name, bit*1.0/1000000, bit);
		}
		else if (bit > 1000) {
			printf("flow_name:%s\trate:%.4fKbps,%ld bps \n",
				flow_infos[i].name, bit * 1.0 / 1000, bit);
		}
		else {
			printf("flow_name:%s\trate:%ld bps \n", flow_infos[i].name, bit);
		}
	}
	printf("\n\n");
	fflush(stdout);
}
static int 
main_loop(__attribute__((unused)) void* dummy) {
	struct rte_mbuf* pkts_burst[MAX_PKT_BURST];
	struct rte_mbuf* m;
	struct lcore_queue_conf* qconf;
	int sent, i, j, nb_rx;
	unsigned lcore_id;
	unsigned portid;

	lcore_id = rte_lcore_id();
	qconf = &lcore_queue_conf[lcore_id];
	if (lcore_id == rte_get_master_lcore()) {
		uint64_t prev_tsc, cur_tsc;
		prev_tsc = rte_rdtsc();
		while (!force_quit) {
			cur_tsc = rte_rdtsc();
			if (unlikely((cur_tsc - prev_tsc) > tsc_period)) {
				prev_tsc = cur_tsc;
				display_statistics();
			}			
		}
	}
	else {
		if (qconf->n_rx_port == 0 ) {
			printf("lcore %u has nothing to do\n", lcore_id);
			return;
		}
		while (!force_quit) {
			for (i = 0; i < qconf->n_rx_port; i++) {
				portid = qconf->rx_port_list[i];
				nb_rx = rte_eth_rx_burst(portid, 0, pkts_burst, MAX_PKT_BURST);
				for (j = 0; j < nb_rx; j++) {
					m = pkts_burst[j];
					rte_prefetch0(rte_pktmbuf_mtod(m, void*));
					do_work(m);
					rte_pktmbuf_free(m);
				}
			}
		}
	}	
	return 0;
}

static int
flow_info_init() {
	int ret;
	printf("Configuring flow infos......\n");
	if (file_name == NULL) {
		uint8_t ip[4] = { 192,168,1,111 };
		strcpy(flow_infos[0].name, "flow 1");
		flow_infos[0].nb_ip = 1;
		flow_infos[0].ip[0] = ip[3] << 24 | ip[2] << 16 | ip[1] << 8 | ip[0];
		nb_flows = 1;
		ret = 0;
	}
	else {
		/* todo: read config file */
		struct rte_cfgfile* file = rte_cfgfile_load(file_name, 0);
		char entry_name[40];
		char section_name[40];
		int i,j;
		uint8_t ip[4];
		const char* entry;
		if (file == NULL) {
			printf("rte_cfgfile_load failed of %s", file_name);
			return -1;
		}
		entry = rte_cfgfile_get_entry(file, "globalinfo", "number of flows");
		if (unlikely(!entry)) {
			printf("rte_cfgfile_load failed at globalinfo : number of flows\n" );
			return -1;
		}
		nb_flows = atoi(entry);
		for (i = 0; i < nb_flows; i++) {
			sprintf(section_name, "flow%u", i);
			entry = rte_cfgfile_get_entry(file, section_name, "name");
			if (unlikely(!entry)) {
				printf("rte_cfgfile_load failed at %s: name\n", section_name);
				return -1;
			}
			strcpy(flow_infos[i].name, entry);
			entry = rte_cfgfile_get_entry(file, section_name, "number of ips");
			if (unlikely(!entry)) {
				printf("rte_cfgfile_load failed at %s: %s\n", section_name, entry_name);
				return -1;
			}
			
			flow_infos[i].nb_ip = atoi(entry);
			for (j = 0; j < flow_infos[i].nb_ip; j++) {
				sprintf(entry_name, "ip%u", j);
				entry = rte_cfgfile_get_entry(file, section_name, entry_name);
				if (unlikely(!entry)) {
					printf("rte_cfgfile_load failed at %s: %s\n", section_name, entry_name);
					return -1;
				}
				sscanf(entry, "%u.%u.%u.%u",&ip[0],&ip[1],&ip[2],&ip[3]);
				flow_infos[i].ip[j] = ip[3] << 24 | ip[2] << 16 | ip[1] << 8 | ip[0];
			}
			rte_cfgfile_close(file);
			free(file_name);
		}
		ret = 0;
	}
	printf("Configuring flow infos done.\n");
	return ret;
}

int 
main(int argc, char** argv) {

	int ret,i;
	uint32_t portid;
	int nb_lcores, rx_lcore_id;
	uint32_t nb_tx_queue = 0, nb_rx_queue = 1, lcore_id;
	unsigned int nb_mbufs;
	uint16_t nb_ports_available = 0, nb_ports;
	struct lcore_queue_conf* qconf;

	ret = rte_eal_init(argc, argv);
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments.\n");
	}
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	ret = parse_args(argc, argv); 
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "Invalid monitor arguments\n");
	}
	ret = flow_info_init();
	if (ret < 0) {
		rte_exit(EXIT_FAILURE, "flow info init fails.\n");
	}
	for (i = 0; i < nb_flows; i++) {
		rte_rwlock_init(&rwl[i]);
	}
	nb_lcores = rte_lcore_count();
	nb_ports = rte_eth_dev_count_avail();
	tsc_period = timer_period * rte_get_timer_hz();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
	rx_lcore_id = 1;
	qconf = NULL;

	/* assign lcore for ports */
	RTE_ETH_FOREACH_DEV(portid) {
		/* skip ports that are not enabled */
		if ((enabled_port_mask & (1 << portid)) == 0)
			continue;
		/* get the lcore_id for this port */
		while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
			lcore_queue_conf[rx_lcore_id].n_rx_port == rx_queue_per_lcore) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}

		if (qconf != &lcore_queue_conf[rx_lcore_id]) {
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[rx_lcore_id];
			nb_lcores++;
		}
		qconf->rx_port_list[qconf->n_rx_port] = portid;
		qconf->n_rx_port++;
		printf("Lcore %u: RX port %u\n", rx_lcore_id, portid);
	}
	nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
		nb_lcores * MEMPOOL_CACHE_SIZE), 8192U);
	pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());

	/* init each port */
	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_dev_info dev_info;

		if (enabled_port_mask & (1 << portid) == 0) { 
			printf("Skipping disabled port %d.\n", portid);
			continue;
		}
		nb_ports_available++;
		printf("Initializing port %d ... ", portid);
		fflush(stdout);

		rte_eth_dev_info_get(portid, &dev_info);
		if (dev_info.tx_offload_capa & DEV_TX_OFFLOAD_MBUF_FAST_FREE)
			local_port_conf.txmode.offloads |= DEV_TX_OFFLOAD_MBUF_FAST_FREE;

		ret = rte_eth_dev_configure(portid, nb_rx_queue, nb_tx_queue, &local_port_conf);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
				"Cannot configure device: err=%d, port=%d\n",
				ret, portid);
		}
		ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE,
				"Cannot adjust number of descriptors: err=%d, "
				"port=%d\n", ret, portid);
		}
		

		/* init one RX queue */
		fflush(stdout);
		rxq_conf = dev_info.default_rxconf;
		rxq_conf.offloads = local_port_conf.rxmode.offloads;
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
			rte_eth_dev_socket_id(portid),
			&rxq_conf,
			pktmbuf_pool);
		if (ret < 0) {
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
				ret, portid);
		}
	}
	printf("Ports initialization completes.\n\n");

	RTE_ETH_FOREACH_DEV(portid) {
		if (enabled_port_mask & (1 << portid) == 0) {
			continue;
		}
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				ret, portid);

		rte_eth_promiscuous_enable(portid);
		
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
			"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(enabled_port_mask);

	ret = 0;
	rte_eal_mp_remote_launch(main_loop, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	/* stop all available ports */
	RTE_ETH_FOREACH_DEV(portid) {
		if ((enabled_port_mask & (1 << portid)) == 0)
			continue;
		printf("Closing port %d...", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);
		printf(" Done\n");
	}
	printf("Bye...\n");

	return ret;
}