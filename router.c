#include <fcntl.h>
#include <stdio.h>
#include <net/netmap_user.h>
#include <sys/poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include "trie.h"
#include "router.h"

#define NIC_MAX 4

// Set up variables for the required number of interfaces
char *nic_names[] = {"enp8s0f0","enp8s0f1","enp8s0f2","enp8s0f3"};
struct nmreq nmreqs[NIC_MAX];         // netmap request
struct netmap_if *nifps[NIC_MAX];     // netmap interface
struct netmap_ring *rxrings[NIC_MAX]; // RX ring
struct netmap_ring *txrings[NIC_MAX]; // TX ring
struct forwarder forwarders[NIC_MAX]; // Forwarder thread data
void *mems[NIC_MAX];                  // NIC memory
int fds[NIC_MAX];                     // netmap file descriptors
pthread_mutex_t locks[NIC_MAX];       // Mutex for NIC memory access

trie_t t4; // IPv4 forwarding table

void setup()
{
  // Initialize everything per NIC
  for(int n=0; n<NIC_MAX;n++) {
    fds[n] = open("/dev/netmap", O_RDWR);    // Open socket
    strcpy(nmreqs[n].nr_name, nic_names[n]); // Address NIC by name
    nmreqs[n].nr_version = NETMAP_API;       // netmap version
    nmreqs[n].nr_flags = NR_REG_ALL_NIC;     // Open all rings, hopefully there is only one anyway
    nmreqs[n].nr_ringid = NETMAP_NO_TX_POLL; // Don't transmit when polling for RX data
    ioctl(fds[n], NIOCREGIF, nmreqs+n);      // Open the interface for netmap mode
    mems[n] = mmap(NULL, nmreqs[n].nr_memsize, PROT_READ|PROT_WRITE, MAP_SHARED, fds[n], 0); // Map NIC memory
    nifps[n] = NETMAP_IF(mems[n], nmreqs[n].nr_offset); // Locate port memory
    rxrings[n] = NETMAP_RXRING(nifps[n], 0); // Set up pointer to RX ring
    txrings[n] = NETMAP_TXRING(nifps[n], 0); // Set up pointer to TX ring
    pthread_mutex_init(locks+n, NULL);       // Initialize mutex
    trie_init(&t4);                          // Initialize IPv4 forwarding table
  }
}

// Do forwarding for a single NIC's incoming frames
void *forward(void *c)
{
  struct forwarder *forwarder = c; // Configuration for this thread

  // Configuration for poll()
  struct pollfd pollfd;
  int fd = fds[forwarder->interface_id];
  pollfd.fd = fd;
  pollfd.events = POLLIN;

  // Pointers for this RX ring and a movable pointer for use with TX rings
  struct netmap_ring *rxring = rxrings[forwarder->interface_id];
  struct netmap_ring *txring;
  // Pointers for packet data
  char *rxbuf;
  char *txbuf;

  // Temporary variables for per-packet data
  uint32_t next_hop_ip = 0;
  uint8_t next_hop_interface = 1;
  uint16_t interfaces_touched;

  while (1) {
    // Sleep, this is not really necessary but reduces CPU usage by waiting for more packets to be processed at once
    usleep(10);
    // Check for received packets
    ioctl(fds[forwarder->interface_id], NIOCRXSYNC);
    // If no packets are waiting, use poll() to wait
    if(nm_ring_empty(rxring))
      poll(&pollfd, 1, -1);
    // No interfaces have been transmitted to yet
    interfaces_touched = 0;
    // Loop through all waiting packets
    while (!nm_ring_empty(rxring)) {
      // Find the RX packet data
      rxbuf = NETMAP_BUF(rxring, rxring->slot[rxring->cur].buf_idx);
      // Look up the destination in the IPv4 routing table
      trie_node_search(&t4, rxbuf+30, &next_hop_ip, &next_hop_interface);
      // Make a note that this interface needs to be flushed
      interfaces_touched |= (1<<next_hop_interface);

      // Find the TX ring
      txring = txrings[next_hop_interface];
      // Obtain mutex lock on TX interface
      pthread_mutex_lock(locks+next_hop_interface);
      // Check the TX interface isn't full
      if(txring->cur != txring->tail) {
        // Copy the packet length and data from RX to TX
        txring->slot[txring->cur].len = rxring->slot[rxring->cur].len;
        txbuf = NETMAP_BUF(txring, txring->slot[txring->cur].buf_idx);
        memcpy(txbuf, rxbuf, rxring->slot[rxring->cur].len);
        // Advance the TX ring pointer
        txring->head = txring->cur = nm_ring_next(txring, txring->cur);
      }
      // Release lock
      pthread_mutex_unlock(locks+next_hop_interface);
      // Advance the RX ring pointer
      rxring->head = rxring->cur = nm_ring_next(rxring, rxring->cur);
    }

    // Flush data to each interfaces if needed
    for(int n=0; n<NIC_MAX; n++) {
      if(interfaces_touched & (1<<n))
        ioctl(fds[n], NIOCTXSYNC);
    }
  }
}

int main(int argc, char **argv)
{
  // Hello world
  fprintf(stderr, "%s built %s %s\n", argv[0], __DATE__, __TIME__);

  // Initialize everything
  setup();

  // Populate some ficticious route data
  uint32_t ip_r;
  for(int n=0;n<1000000;n++) { // 1 Million IPs
    ip_r = htonl(167772160+n); // 10.0.0.0 + n
    // Insert into IPv4 routing table, next-hop local broadcast (IP 0) to interface 1
    trie_node_put(&t4, (char*)&ip_r, 32, 0, 1);
  }

  // Start all forwarders
  for(int n=0; n<NIC_MAX; n++) {
    forwarders[n].interface_id = n;
    pthread_create(&forwarders[n].thread, NULL, forward, forwarders+n);
  }

  // Join threads for completeness
  for(int n=0; n<NIC_MAX; n++) {
    pthread_join(forwarders[n].thread, NULL);
  }

  return(0);
}
