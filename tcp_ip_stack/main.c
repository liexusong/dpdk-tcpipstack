/*-
 *   BSD LICENSE
 * 
 *   Copyright(c) 2010-2014 Intel Corporation. All rights reserved.
 *   All rights reserved.
 * 
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 * 
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 * 
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

// this file is used as a sample to init dpdk, user are expected to write their own such file.

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

#include <rte_common.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_tailq.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>

#include <assert.h>
#include "config.h"
#include "main.h"
#include "etherin.h"
#include "ether.h"
#include "tcp_tcb.h"
#include "etherout.h"
#include "logger.h"
#include "socket_tester.h"
#include "socket_interface.h"
#include "cli_server.h"
#include "timer.h"
#include "counters.h"
#include "debug.h"

#define RTE_LOGTYPE_L2FWD RTE_LOGTYPE_USER1

#define MBUF_SIZE (MBUF_BUFFER_LEN + sizeof(struct rte_mbuf) + RTE_PKTMBUF_HEADROOM)
#define NB_MBUF   8192 * 3

/*
 * RX and TX Prefetch, Host, and Write-back threshold values should be
 * carefully set for optimal performance. Consult the network
 * controller's datasheet and supporting DPDK documentation for guidance
 * on how these parameters should be set.
 */
#define RX_PTHRESH 8 /**< Default values of RX prefetch threshold reg. */
#define RX_HTHRESH 8 /**< Default values of RX host threshold reg. */
#define RX_WTHRESH 4 /**< Default values of RX write-back threshold reg. */

/*
 * These default values are optimized for use with the Intel(R) 82599 10 GbE
 * Controller and the DPDK ixgbe PMD. Consider using other values for other
 * network controllers and/or network drivers.
 */
#define TX_PTHRESH 36 /**< Default values of TX prefetch threshold reg. */
#define TX_HTHRESH 0  /**< Default values of TX host threshold reg. */
#define TX_WTHRESH 0  /**< Default values of TX write-back threshold reg. */

#define MAX_PKT_BURST 32
#define BURST_TX_DRAIN_US 100 /* TX drain every ~100us */

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* mask of enabled ports */
static uint32_t l2fwd_enabled_port_mask = 0;

/* list of enabled ports */
static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];

static unsigned int l2fwd_rx_queue_per_lcore = 1;

struct mbuf_table {
	unsigned len;
	struct rte_mbuf *m_table[MAX_PKT_BURST];
};

#define MAX_RX_QUEUE_PER_LCORE 16
#define MAX_TX_QUEUE_PER_PORT 16
struct lcore_queue_conf {
	unsigned n_rx_port;
	unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
	struct mbuf_table tx_mbufs[RTE_MAX_ETHPORTS];

} __rte_cache_aligned;
struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 0, /**< IP checksum offload disabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static const struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {
		.pthresh = RX_PTHRESH,
		.hthresh = RX_HTHRESH,
		.wthresh = RX_WTHRESH,
	},
};

static const struct rte_eth_txconf tx_conf = {
	.tx_thresh = {
		.pthresh = TX_PTHRESH,
		.hthresh = TX_HTHRESH,
		.wthresh = TX_WTHRESH,
	},
	.tx_free_thresh = 0, /* Use PMD default values */
	.tx_rs_thresh = 0, /* Use PMD default values */
	/*
	* As the example won't handle mult-segments and offload cases,
	* set the flag by default.
	*/
	.txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS | ETH_TXQ_FLAGS_NOOFFLOADS,
};

struct rte_mempool * l2fwd_pktmbuf_pool = NULL;

/* Per-port statistics struct */
struct l2fwd_port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
} __rte_cache_aligned;
struct l2fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];

/* A tsc-based timer responsible for triggering statistics printout */
#define TIMER_MILLISECOND 2000000ULL /* around 1ms at 2 Ghz */
#define MAX_TIMER_PERIOD 86400 /* 1 day max */
static int64_t timer_period = 10 * TIMER_MILLISECOND * 1000; /* default period is 10 seconds */


struct rte_mbuf *get_mbuf(void)
{
   struct rte_mbuf *mbuf = NULL;   
   mbuf = rte_pktmbuf_alloc(l2fwd_pktmbuf_pool);
   mbuf->udata64 = 0;
   static int counter_id = -1;
   if(counter_id == -1) {
      counter_id = create_counter("mbuf_allocated");
   }
   counter_inc(counter_id, 1);
   return mbuf;
}

int
send_packet_out(struct rte_mbuf *mbuf, int port, int free_mbuf)
{
   static int count = 0;
   count ++;
   
   static struct rte_mbuf *last = NULL;
   if(last== mbuf) {
      logger(LOG_ETHER, LOG_LEVEL_NORMAL, "Sending same buf again.\n");
   }
   last = mbuf;
   struct rte_mbuf **mbuf_arr = malloc(sizeof(struct rte_mbuf*) * 2);
   if(mbuf_arr == NULL) {
      logger(LOG_ETHER, LOG_LEVEL_NORMAL, "mbuf array is null\n");
   }
   //logger(LOG_ETHER, LOG_LEVEL_NORMAL, "sending port from %d\n", port);
   // this buf will be released only when we will receive the ack of this.
   struct rte_mbuf *new_mbuf = mbuf;
   // let tx delete this mbuf, we have taken copy of it.
   if( 0 && (free_mbuf == 1) ) {
       uint16_t last_ref = rte_mbuf_refcnt_read (mbuf);
     //  rte_mbuf_refcnt_set (mbuf, last_ref + 1);
       new_mbuf = rte_pktmbuf_clone(mbuf, l2fwd_pktmbuf_pool); 
       logger(LOG_ETHER, LOG_LEVEL_NORMAL, "updating the reference of mbuf  %p from %u to %u\n", new_mbuf, last_ref, rte_mbuf_refcnt_read (mbuf));
       if(new_mbuf == NULL) {
          logger(LOG_ETHER, LOG_LEVEL_NORMAL, "failed to clone mbuf, will try later.\n");
          return 0;
       }
   }
   mbuf_arr[0] = new_mbuf;
   char buf[1024];
    DumpMbufInBuf(mbuf, buf);
          logger(LOG_ETHER, LOG_LEVEL_NORMAL, "packet sent to wire  %p  with %s\n", mbuf, buf);
   int total_packets_sent = rte_eth_tx_burst(PORT_CONFIGURE, 0, &mbuf_arr[0], 1);
   (void) port;
   //int total_packets_sent = rte_eth_tx_burst(port, 0, &mbuf_arr[0], 1);
   return total_packets_sent;
}

/* main processing loop */
static void
l2fwd_main_loop(void)
{
	struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
	unsigned lcore_id;
	unsigned i,  portid, nb_rx;
	struct lcore_queue_conf *qconf;

	lcore_id = rte_lcore_id();

	qconf = &lcore_queue_conf[lcore_id];
   printf("Call for %d\n.", lcore_id);
	if ((lcore_id == 1)) {  // core 1 dedicaated for my socket example.
      printf("using core 1 for server socket listner\n");
		RTE_LOG(INFO, L2FWD, "lcore %u has nothing to do\n", lcore_id);
      uint8_t ip[4];
      const char *IpString = IP_INTERFACE_1;
      int i = 0;
      int k = 0;
      int val = 0;
      for(i=0;i<4;i++) {
         val = 0;
         while((IpString[k] != '.') && (IpString[k] != '\0')) {
            val = val * 10 + IpString[k] - 48;
            printf (" %u ", val);
            k++;
         }
         k++;
         ip[i] = val;
         printf ("Ip index %u %d\n", i, ip[i]);
      }

 //     ip[0] = 192;
  //    ip[1] = 168;
  //    ip[2] = 78;
      init_counters();
      if(1) { // test accept otherwise test connect
  //       ip[3] = 2;
         init_socket_example(23, ip); // this call is blocking call. 
      }
      else {
  //       ip[3] = 3;
         init_socket_example_connect(23, ip); // this call is blocking call. 
      }
      //init_socket_example(24, ip); // this call is blocking call. 
	//	return;
	}
   if(lcore_id == 3) {
      // core 3 for cli. nneed to create multiple threads here for mgmt tasks.
      printf("using core 3 for cli\n");// what a waste of resource. :)
      cli_server_init();
   }
   if(lcore_id != 2) {  // doing all on core 2
      printf("Ignoring core id %d\n", lcore_id);
      return;
   }

	RTE_LOG(INFO, L2FWD, "entering main loop on lcore %u\n", lcore_id);

	for (i = 0; i < qconf->n_rx_port; i++) {

//		portid = qconf->rx_port_list[i];
	//	RTE_LOG(INFO, L2FWD, " -- lcoreid=%u portid=%u\n", lcore_id,
	//		portid);
	}

   portid = PORT_CONFIGURE;//ports ++;

	while (1) {

//		cur_tsc = rte_rdtsc();
	   unsigned int i;	
//      if(portid) 
//         portid = 0;
//      else 
//         portid = 1;

		nb_rx = rte_eth_rx_burst((uint8_t) portid, 0,
		//nb_rx = rte_eth_rx_burst((uint8_t) 1, 0,
						 pkts_burst, MAX_PKT_BURST);
  //  logger(LOG_ETHER, LOG_LEVEL_NORMAL, " packets re %d port %d\n", nb_rx, portid);
			if(nb_rx) {
            for(i=0;i<nb_rx;i++) {
//               pkts_burst[i].user_data = malloc(sizeof(int));
               ether_in(pkts_burst[i]);
	            //rte_eth_tx_burst(portid, 0, &pkts_burst[i], 1);
	         }
         }
         CheckEtherOutRing();
         check_socket_out_queue();
         DoTimer();
   }

}
#include <rte_spinlock.h>
rte_spinlock_t *lock;

static int
l2fwd_launch_one_lcore(__attribute__((unused)) void *dummy)
{
//   static done = 1;

//   rte_spinlock_lock(lock);
//   if(1 || done) {
	   l2fwd_main_loop();
//      done = 0;
//   }
//   rte_spinlock_unlock(lock);
	return 0;
}

/* display usage */
static void
l2fwd_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK [-q NQ]\n"
	       "  -p PORTMASK: hexadecimal bitmask of ports to configure\n"
	       "  -q NQ: number of queue (=ports) per lcore (default is 1)\n"
		   "  -T PERIOD: statistics will be refreshed each PERIOD seconds (0 to disable, 10 default, 86400 maximum)\n",
	       prgname);
}

static int
l2fwd_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	if (pm == 0)
		return -1;

	return pm;
}

static unsigned int
l2fwd_parse_nqueue(const char *q_arg)
{
	char *end = NULL;
	unsigned long n;

	/* parse hexadecimal string */
	n = strtoul(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return 0;
	if (n == 0)
		return 0;
	if (n >= MAX_RX_QUEUE_PER_LCORE)
		return 0;

	return n;
}

static int
l2fwd_parse_timer_period(const char *q_arg)
{
	char *end = NULL;
	int n;

	/* parse number string */
	n = strtol(q_arg, &end, 10);
	if ((q_arg[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;
	if (n >= MAX_TIMER_PERIOD)
		return -1;

	return n;
}

/* Parse the argument given in the command line of the application */
static int
l2fwd_parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{NULL, 0, 0, 0}
	};

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "p:q:T:",
				  lgopts, &option_index)) != EOF) {

		switch (opt) {
		/* portmask */
		case 'p':
			l2fwd_enabled_port_mask = l2fwd_parse_portmask(optarg);
			if (l2fwd_enabled_port_mask == 0) {
				printf("invalid portmask\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		/* nqueue */
		case 'q':
			l2fwd_rx_queue_per_lcore = l2fwd_parse_nqueue(optarg);
			if (l2fwd_rx_queue_per_lcore == 0) {
				printf("invalid queue number\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		/* timer period */
		case 'T':
			timer_period = l2fwd_parse_timer_period(optarg) * 1000 * TIMER_MILLISECOND;
			if (timer_period < 0) {
				printf("invalid timer period\n");
				l2fwd_usage(prgname);
				return -1;
			}
			break;

		/* long options */
		case 0:
			l2fwd_usage(prgname);
			return -1;

		default:
			l2fwd_usage(prgname);
			return -1;
		}
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

/* Check the link status of all ports in up to 9s, and print them finally */
static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	printf("\nChecking link status");
	fflush(stdout);
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					printf("Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
				(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
					("full-duplex") : ("half-duplex\n"));
				else
					printf("Port %d Link Down\n",
						(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == 0) {
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


int GetTotalInterfaces(void)
{
	return rte_eth_dev_count();
}
int
MAIN(int argc, char **argv)
{
   lock = malloc(sizeof(rte_spinlock_t));
   rte_spinlock_init(lock);
	struct lcore_queue_conf *qconf;
	struct rte_eth_dev_info dev_info;
	int ret;
	uint8_t nb_ports;
	uint8_t nb_ports_available;
	uint8_t portid, last_port;
	unsigned rx_lcore_id;
	unsigned nb_ports_in_mask = 0;


	/* init EAL */
	printf ("Launching cores\n");
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	printf ("Launching cores2\n");
	argc -= ret;
	argv += ret;

	/* parse application arguments (after the EAL ones) */
	ret = l2fwd_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid L2FWD arguments\n");

	/* create the mbuf pool */
	l2fwd_pktmbuf_pool =
		rte_mempool_create("mbuf_pool", NB_MBUF,
				   MBUF_SIZE, 32,
				   sizeof(struct rte_pktmbuf_pool_private),
				   rte_pktmbuf_pool_init, NULL,
				   rte_pktmbuf_init, NULL,
				   rte_socket_id(), 0);
	if (l2fwd_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	/* init driver(s) */
/*
	if (rte_pmd_init_all() < 0)
		rte_exit(EXIT_FAILURE, "Cannot init pmd\n");
*/
//	if (rte_eal_pci_probe() < 0)
//		rte_exit(EXIT_FAILURE, "Cannot probe PCI\n");
	printf ("Launching cores2\n");
	argc -= ret;

	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0)
		rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");

	if (nb_ports > RTE_MAX_ETHPORTS)
		nb_ports = RTE_MAX_ETHPORTS;

   if(nb_ports > TOTAL_PORTS) {
      nb_ports = TOTAL_PORTS;
      printf("Total ports configured is less than available.\n");
   }
   if(nb_ports < TOTAL_PORTS) {
      printf("Ports configured is less than available. Giving UP\n");
      assert(0);
      return -1;
   }
   
	printf ("Launching cores\n");

	/*
	 * Each logical core is assigned a dedicated TX queue on each port.
	 */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		if (nb_ports_in_mask % 2) {
			l2fwd_dst_ports[portid] = last_port;
			l2fwd_dst_ports[last_port] = portid;
		}
		else
			last_port = portid;

		nb_ports_in_mask++;

		rte_eth_dev_info_get(portid, &dev_info);
	}
	if (nb_ports_in_mask % 2) {
		printf("Notice: odd number of ports in portmask.\n");
		l2fwd_dst_ports[last_port] = last_port;
	}

	rx_lcore_id = 0;
	qconf = NULL;

	/* Initialize the port/queue configuration of each logical core */
	for (portid = 0; portid < nb_ports; portid++) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0)
			continue;

		/* get the lcore_id for this port */
		while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
		       lcore_queue_conf[rx_lcore_id].n_rx_port ==
		       l2fwd_rx_queue_per_lcore) {
			rx_lcore_id++;
			if (rx_lcore_id >= RTE_MAX_LCORE)
				rte_exit(EXIT_FAILURE, "Not enough cores\n");
		}

		if (qconf != &lcore_queue_conf[rx_lcore_id])
			/* Assigned a new logical core in the loop above. */
			qconf = &lcore_queue_conf[rx_lcore_id];

		qconf->rx_port_list[qconf->n_rx_port] = portid;
		qconf->n_rx_port++;
		printf("Lcore %u: RX port %u\n", rx_lcore_id, (unsigned) portid);
	}

	nb_ports_available = nb_ports;

   printf("ports = %d", nb_ports);
	/* Initialise each port */
	for (portid = 0; portid < nb_ports; portid++) {
//	for (portid = 0; portid < 1; portid++) {
		/* skip ports that are not enabled */
		if ((l2fwd_enabled_port_mask & (1 << portid)) == 0) {
			printf("Skipping disabled port %u\n", (unsigned) portid);
			nb_ports_available--;
			continue;
		}
		/* init port */
		printf("Initializing port %u... ", (unsigned) portid);
		fflush(stdout);
		ret = rte_eth_dev_configure(portid, 1, 1, &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
				  ret, (unsigned) portid);

		rte_eth_macaddr_get(portid,&l2fwd_ports_eth_addr[portid]);

		/* init one RX queue */
		fflush(stdout);
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
					     rte_eth_dev_socket_id(portid), &rx_conf,
					     l2fwd_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
				  ret, (unsigned) portid);

		/* init one TX queue on each port */
		fflush(stdout);
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid), &tx_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
				ret, (unsigned) portid);

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
				  ret, (unsigned) portid);

		printf("done: \n");

		rte_eth_promiscuous_enable(portid);

		printf("Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n\n",
				(unsigned) portid,
				l2fwd_ports_eth_addr[portid].addr_bytes[0],
				l2fwd_ports_eth_addr[portid].addr_bytes[1],
				l2fwd_ports_eth_addr[portid].addr_bytes[2],
				l2fwd_ports_eth_addr[portid].addr_bytes[3],
				l2fwd_ports_eth_addr[portid].addr_bytes[4],
				l2fwd_ports_eth_addr[portid].addr_bytes[5]);
      SetInterfaceHW(l2fwd_ports_eth_addr[portid].addr_bytes, portid);

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));
	}

	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE,
			"All available ports are disabled. Please set portmask.\n");
	}

	check_all_ports_link_status(nb_ports, l2fwd_enabled_port_mask);
   struct Interface *IfList[1];
   struct Interface temp;
   int i;
   for(i=0;i<6;i++) {
   //   temp.HwAddress[i] = 0x01;
   }
   temp.InterfaceNumber = 1;
      const char *IpString = IP_INTERFACE_1;
  //    int i = 0;
      int k = 0;
      int val = 0;
      for(i=0;i<4;i++) {
         val = 0;
         while((IpString[k] != '.') && (IpString[k] != '\0')) {
            val = val * 10 + IpString[k] - 48;
            printf (" %u ", val);
            k++;
         }
         k++;
         temp.IP[i] = val;
         printf ("Ip index %u %d\n", i, temp.IP[i]);
      }
//   temp.IP[0] = 192;
//   temp.IP[1] = 168;
//   temp.IP[2] = 78;
//   temp.IP[3] = 2;
   IfList[0] = &temp;
   InitLogger();
   InitEtherInterface();
   InitTcpTcb();
   //InitInterface(IfList, 1);
   AddInterface(IfList[0]);

#if 0
	/* launch per-lcore init on every lcore */
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0)
			return -1;
	}
#endif
	rte_eal_mp_remote_launch(l2fwd_launch_one_lcore, NULL, SKIP_MASTER);
   printf("Main exiting.\n");

    rte_eal_mp_wait_lcore ();
	return 0;
}

