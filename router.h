struct forwarder
{
  pthread_t thread;
  uint8_t interface_id;
};

struct mac_entry
{
  uint32_t address;
  uint8_t interface;
  char mac[6];
};
