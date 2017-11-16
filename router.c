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

struct nmreq nmreqs[NIC_MAX];
struct netmap_if *nifps[NIC_MAX];
struct netmap_ring *rxrings[NIC_MAX];
struct netmap_ring *txrings[NIC_MAX];
struct forwarder forwarders[NIC_MAX];
void *mems[NIC_MAX];
char *nic_names[] = {"enp8s0f0","enp8s0f1","enp8s0f2","enp8s0f3"};
int fds[NIC_MAX];
pthread_mutex_t locks[NIC_MAX];

trie_t t4;

void setup()
{
  for(int n=0; n<NIC_MAX;n++) {
    fds[n] = open("/dev/netmap", O_RDWR);
    strcpy(nmreqs[n].nr_name, nic_names[n]);
    nmreqs[n].nr_version = NETMAP_API;
    nmreqs[n].nr_flags = NR_REG_ALL_NIC;
    nmreqs[n].nr_ringid = NETMAP_NO_TX_POLL;
    ioctl(fds[n], NIOCREGIF, nmreqs+n);
    mems[n] = mmap(NULL, nmreqs[n].nr_memsize, PROT_READ|PROT_WRITE, MAP_SHARED, fds[n], 0);
    nifps[n] = NETMAP_IF(mems[n], nmreqs[n].nr_offset);
    rxrings[n] = NETMAP_RXRING(nifps[n], 0);
    txrings[n] = NETMAP_TXRING(nifps[n], 0);
    pthread_mutex_init(locks+n, NULL);
    trie_init(&t4);
  }
}

void *forward(void *c)
{
  uint8_t dst_if;
  struct forwarder *forwarder = c;
  struct pollfd pollfd;
  int fd = fds[forwarder->interface_id];
  pollfd.fd = fd;
  pollfd.events = POLLIN;

  // Crude routing, choose egress interface depending on ingredd interface
  if(forwarder->interface_id == 0)
    dst_if = 2;
  if(forwarder->interface_id == 1)
    dst_if = 3;
  if(forwarder->interface_id == 2)
    dst_if = 0;
  if(forwarder->interface_id == 3)
    dst_if = 1;

  struct netmap_ring *rxring = rxrings[forwarder->interface_id];
  struct netmap_ring *txring = txrings[dst_if];
  char *rxbuf;
  char *txbuf;

  uint32_t next_hop_ip;
  uint8_t next_hop_interface;

  while (1) {
    usleep(1);
    poll(&pollfd, 1, -1);
    pthread_mutex_lock(locks+dst_if);
    while (!nm_ring_empty(rxring)) {
      rxbuf = NETMAP_BUF(rxring, rxring->slot[rxring->cur].buf_idx);
      trie_node_search(&t4, rxbuf+30, 32, &next_hop_ip, &next_hop_interface);

      if(txring->cur != txring->tail) {
        txring->slot[txring->cur].len = rxring->slot[rxring->cur].len;
        txbuf = NETMAP_BUF(txring, txring->slot[txring->cur].buf_idx);
        memcpy(txbuf, rxbuf, rxring->slot[rxring->cur].len);
        txring->head = txring->cur = nm_ring_next(txring, txring->cur);
      }
      rxring->head = rxring->cur = nm_ring_next(rxring, rxring->cur);
    }
    ioctl(fds[dst_if], NIOCTXSYNC);
    pthread_mutex_unlock(locks+dst_if);
  }
}

int main(int argc, char **argv)
{
  fprintf(stderr, "%s built %s %s\n", argv[0], __DATE__, __TIME__);
  setup();

  // Populate some ficticious route data
  uint32_t ip_r;
  for(int n=0;n<1000000;n++) {
    ip_r = htonl(167772160+n);
    trie_node_put(&t4, (char*)&ip_r, 32, 1, 1);
  }

  for(int n=0; n<NIC_MAX; n++) {
    forwarders[n].interface_id = n;
    pthread_create(&forwarders[n].thread, NULL, forward, forwarders+n);
  }

  for(int n=0; n<NIC_MAX; n++) {
    pthread_join(forwarders[n].thread, NULL);
  }

  return(0);
}
