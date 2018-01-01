#define _GNU_SOURCE
#define NETMAP_WITH_LIBS
#include <fcntl.h>
#include <stdio.h>
#include <net/netmap_user.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/poll.h>
#include "trie.h"
#include "router.h"

#define NIC_COUNT 4
#define RING_COUNT 4

char *nic_names[] = {"enp8s0f0","enp8s0f1","enp8s0f2","enp8s0f3"};
trie_t t4; // IPv4 forwarding table

// This function runs in a thread. It handles a specific ring on *ALL* NICs
// For example thre first thread will handle enp8s0f0-0, enp8s0f1-0, enp8s0f2-0, enp8s0f3-0
// This allows me to do multi-threaded forwarding without any locking
void *thread_main(void * f) {
  struct forwarder *forwarder = f;
  int fds[NIC_COUNT];           // File descriptor for netmap socket
  struct nmreq reqs[NIC_COUNT]; // A struct for the netmap request
  void * mem;                   // Pointer to allocated memory area
  struct netmap_if *nifps[NIC_COUNT];     // Interfaces
  struct netmap_ring *rxrings[NIC_COUNT]; // RX rings
  struct netmap_ring *txrings[NIC_COUNT]; // TX rings
  memset(reqs, 0, sizeof(struct nmreq) * NIC_COUNT);

  for(int n=0; n<NIC_COUNT;n++) {
    fds[n] = open("/dev/netmap", O_RDWR);  // Open a generic netmap socket
    strcpy(reqs[n].nr_name, nic_names[n]); // Copy NIC name into request
    reqs[n].nr_version = NETMAP_API;       // Set version number
    reqs[n].nr_flags = NR_REG_ONE_NIC;     // We will be using a single hardware ring
    reqs[n].nr_ringid = forwarder->id;     // Select ring, disable TX on poll
    ioctl(fds[n], NIOCREGIF, &reqs[n]);    // Initialize port

    //printf("interface: %s\n", reqs[n].nr_name);
    //printf("memsize: %u\n", reqs[n].nr_memsize); // Check the allocated memory size
    printf("nr_arg2: %u\n", reqs[n].nr_arg2);    // Check the allocated memory area
  }

  // Map the memory
  mem = mmap(NULL, reqs[0].nr_memsize, PROT_READ|PROT_WRITE, MAP_SHARED, fds[0], 0);
  printf("mmap: %p\n", mem);

  struct pollfd pollfds[NIC_COUNT];
  for(int n=0; n<NIC_COUNT;n++) {
    nifps[n] = NETMAP_IF(mem, reqs[n].nr_offset); // Locate port memory
    rxrings[n] = NETMAP_RXRING(nifps[n], forwarder->id); // Set up pointer to RX ring
    txrings[n] = NETMAP_TXRING(nifps[n], forwarder->id); // Set up pointer to TX ring
    pollfds[n].fd = fds[n];
    pollfds[n].events = POLLIN;
  }

  // Pointers for the RX ring and TX ring
  struct netmap_ring *rxring;
  struct netmap_ring *txring;

  // Temporary variables for per-packet data
  uint32_t next_hop_ip = 0;
  uint8_t next_hop_interface = 1;

  // Pointers for packet data
  char *rxbuf;
  char *txbuf;

  unsigned int batch_total;
  unsigned int batch_count;

  while (1) {
    // Use poll to wait for one or more interfaces to have frames waiting
    poll(pollfds, NIC_COUNT, -1);

    // Simple way to monitor batch size
    if(batch_count == 100000) {
      printf("Thread %u Average batch: %u\n", forwarder->id, batch_total / batch_count);
      batch_total = 0;
      batch_count = 0;
    }
    batch_count++;

    // Loop over all NICs and check for waiting frames
    for(int n=0; n<NIC_COUNT; n++) {
      ioctl(fds[n], NIOCTXSYNC);
      rxring = rxrings[n];
      while (!nm_ring_empty(rxring)) {
        batch_total++;
        // Find the packet in the RX buffer
        rxbuf = NETMAP_BUF(rxring, rxring->slot[rxring->cur].buf_idx);
        // Assume the frame is an IPv4 packet and perform a lookup
        trie_node_search(&t4, rxbuf+30, &next_hop_ip, &next_hop_interface);
        // Fetch the appropriate TX ring
        txring = txrings[next_hop_interface];

        // Check for space in the TX ring
        if(nm_ring_space(txring)) {
          // Copy the packet length and data from RX to TX
          txring->slot[txring->cur].len = rxring->slot[rxring->cur].len;

          // Zero-copy buffer swap
          uint32_t pkt = rxring->slot[rxring->cur].buf_idx;
          rxring->slot[rxring->cur].buf_idx = txring->slot[txring->cur].buf_idx;
          txring->slot[txring->cur].buf_idx = pkt;

          // Find the buffer buffer in case we want to modify the frame (TTL etc)
          txbuf = NETMAP_BUF(txring, txring->slot[txring->cur].buf_idx);

          // Advance the TX ring pointer
          txring->head = txring->cur = nm_ring_next(txring, txring->cur);
        }
        // Advance the RX ring pointer
        rxring->head = rxring->cur = nm_ring_next(rxring, rxring->cur);
      }
    }
  }
}

int main(int argc, char **argv)
{
  // Hello world
  fprintf(stderr, "%s built %s %s\n", argv[0], __DATE__, __TIME__);

  // Populate IPv4 routing table
  trie_init(&t4);
  uint32_t ip_r;
  for(int n=0;n<1000000;n++) { // 1 Million IPs
    ip_r = htonl(167772160+n); // 10.0.0.0 + n
    // Insert into IPv4 routing table, next-hop local broadcast (IP 0) to interface 1
    trie_node_put(&t4, (char*)&ip_r, 32, 0, 1);
  }

  // A struct to hold thread information
  struct forwarder forwarder[RING_COUNT];
  // One thread is started per ring, our NICs have 4 rings enabled each so we start 4 threads
  for(int n=0; n<RING_COUNT; n++) {
    forwarder[n].id = n;
    pthread_create(&forwarder[n].thread, NULL, thread_main, &forwarder[n]);
    // Keep the threads on specified cores, mostly to keep things neat in top while testing
    cpu_set_t cpumask;
    CPU_ZERO(&cpumask);
    CPU_SET(n, &cpumask);
    pthread_setaffinity_np(forwarder[n].thread, sizeof(cpu_set_t), &cpumask);
  }

  // Join threads for completeness
  for(int n=0; n<RING_COUNT; n++) {
    pthread_join(forwarder[n].thread, NULL);
  }
  return(0);
}
