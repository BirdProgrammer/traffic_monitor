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

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = ETHER_MAX_LEN,
		.split_hdr_size = 0,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = NULL,
			.rss_hf = ETH_RSS_IP,
		},
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

static volatile bool force_quit;
struct rte_mempool* pktmbuf_pool = NULL;
// static struct ether_addr ethernet_port[RTE_MAX_ETHPORTS];
static unsigned long enabled_port_mask;
static char* file_name;
static unsigned int rx_queue_per_lcore = 1;
static const char short_options[] = "p:t:";
static const struct option long_options[] = {
	{"file", required_argument, NULL, 0},
	{NULL, 0, 0, 0}
};

static void
signal_handler(int signal_number) {
	if (signal_number == SIGINT || signal_number == SIGTERM) {
		printf("\nSignal %d is received. Preparing to exit.\n");
		force_quit == true;
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

	while ((opt = getopt_long(argc, argvopt, short_options, 
		long_options, &option_index) != -1)){
		switch (opt) {
		case 'p':
			enabled_port_mask = parse_portmask_args(optarg);
			if (enabled_port_mask == 0) {
				fprintf(stderr, "Invalid portmask.\n");
				return -1;
			}
			break;
		case 0:
			file_name = optarg;
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

/* accelerate the bytes */
static void
do_work(struct rte_mbuf* m) {
	printf("abc\n");
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
	if (qconf->n_rx_port == 0) {
		printf("lcore %u has nothing to do\n", lcore_id);
		return;
	}

	while (!force_quit) {
		if (lcore_id == rte_get_master_lcore()) {

		}
		else {
			for (i = 0; i < qconf->n_rx_port; i++) {
				portid = qconf->rx_port_list[i];
				nb_rx = rte_eth_rx_burst(portid, 0,
					pkts_burst, MAX_PKT_BURST);
				for (j = 0; j < nb_rx; j++) {
					m = pkts_burst[j];
					rte_prefetch0(rte_pktmbuf_mtod(m, void*));
					do_work(m);
				}
			}
		}
	}
}


int 
main(int argc, char** argv) {

	int ret;
	uint32_t portid;
	int nb_lcores, rx_lcore_id = 1;
	uint32_t nb_tx_queue=1, nb_rx_queue=1, lcore_id;
	unsigned int nb_mbufs;
	uint16_t nb_ports_available = 0, nb_ports;
	struct lcore_queue_conf* qconf = NULL;


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
	nb_lcores = rte_lcore_count();
	nb_ports = rte_eth_dev_count_avail();
	nb_mbufs = RTE_MAX(nb_ports * (nb_rxd + nb_txd + MAX_PKT_BURST +
		nb_lcores * MEMPOOL_CACHE_SIZE), 8192U);
	pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool", nb_mbufs,
		MEMPOOL_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
		rte_socket_id());

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

	RTE_ETH_FOREACH_DEV(portid) {
		struct rte_eth_conf local_port_conf = port_conf;
		struct rte_eth_rxconf rxq_conf;
		struct rte_eth_txconf txq_conf;
		struct rte_eth_dev_info dev_info;

		if (enabled_port_mask & (1 << portid) == 0) {
			printf("Skipping disabled port %d.\n", portid);
			continue;
		}

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
		
		/* init one TX queue on each port*/
		fflush(stdout);
		txq_conf = dev_info.default_txconf;
		txq_conf.offloads = local_port_conf.txmode.offloads;
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd, 
			rte_eth_dev_socket_id(portid), &txq_conf);
		//ret = rte_eth_tx_queue_setup(portid, 1, nb_txd, 0, txconf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE,
				"rte_eth_tx_queue_setup: err=%d, "
				"port=%d\n", ret, portid);
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
		printf("done: \n");

		rte_eth_promiscuous_enable(portid);
		nb_ports_available++;
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