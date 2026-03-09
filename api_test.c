#include <stdio.h>
#include <stdlib.h>

#define EPS_IMPL
#include "eps.h"

int main(int argc, char *argv[]) {
  eps_init((eps_params){
    .sub_group[EPS_SIG] = 1,
    .ttl = 0,
    .loopback = 1,
  });

  eps_msg pub_msg[8] = {};
  int pub_fds[8] = {};
  int pub_n = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0) {
      char *p = argv[i+1];
      int n = strlen(p);
      if (n > 6) n = 6;
      memcpy(&pub_msg[pub_n].id, p, n);
      int rate = atoi(argv[i+2]);
      pub_fds[pub_n++] = eps_add_timer(rate);
      printf("publishing %s every %dms\n", p, rate);
      i += 2;
    }
    if (strcmp(argv[i], "-s") == 0) {
      int n = strlen(argv[i+1]);
      if (n > 6) n = 6;
      eps_msg sub = {0}; 
      memcpy(&sub.id, argv[i+1], n);
      eps_add_sub(sub);
      printf("subscribing to %s\n", argv[i+1]);
      i++;
    }
  }

  if (eps.subn == 0) {
   eps_add_sub((eps_msg){ });
   printf("subscribing to all\n");
  }

  eps_msg last;
  for (;;) {
    int n = eps_poll();

    for (eps_msg *msg; msg = eps_next_msg();) {
      char sub[7] = {0};
      memcpy((uint8_t*)sub, (uint8_t*) &msg->id, 6);
      printf("got %s %04d\n", sub, msg->id.seq);
      msg->recv_ns = 0;
      if (memcmp(&last, msg, sizeof(last)) == 0) {
        int x;
      }
    }

    for (eps_event* ev; ev = eps_next_event(); ) {
      for (int i = 0; i < pub_n; i++) {
        if (eps_ev_timer(ev, pub_fds[i])) {
          char buf[128] = {0};
          pub_msg[i].data = buf;
          pub_msg[i].size = sizeof(eps_id);
          eps_send(EPS_SIG, &pub_msg[i]);
          printf("sent %.*s %04d\n", 6, buf, pub_msg[i].id.seq-1);
        }
      }
    }

    if (eps.err[0]) {
      // handle errors
      EPS_BREAK;
    }
  }
}