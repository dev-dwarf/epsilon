#include <stdio.h>
#include <stdlib.h>

#include "eps.h"

int main(int argc, char *argv[]) {
  eps.sub_group[EPS_SIG] = 1;
  // eps.ttl = 1;
  // eps.loopback = 1;
  eps_init();

  eps_id pub_ids[8] = {};
  int pub_fds[8] = {};
  int pub_n = 0;

  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-p") == 0) {
      char *p = argv[i+1];
      int n = strlen(p);
      if (n > 6) n = 6;
      memcpy(&pub_ids[pub_n], p, n);
      int rate = atoi(argv[i+2]);
      pub_fds[pub_n++] = eps_add_timer(rate);
      printf("publishing %s every %dms\n", p, rate);
      i += 2;
    }
    if (strcmp(argv[i], "-s") == 0) {
      int n = strlen(argv[i+1]);
      if (n > 6) n = 6;
      eps_sub sub = {0}; 
      memcpy(&sub.id, argv[i+1], n);
      eps_add_sub(sub);
      printf("subscribing to %s\n", argv[i+1]);
      i++;
    }
  }

  if (eps.subn == 0) {
   eps_add_sub((eps_sub){ });
   printf("subscribing to all\n");
  }

  int64_t start_ns = eps_ns();
  for (;;) {
    int n = eps_poll();

    for (eps_sub *msg; msg = eps_next_msg();) {
      char sub[7] = {0};
      memcpy((uint8_t*)sub, (uint8_t*) &msg->id, 6);
      printf("%04d) got %s %04d\n", (int)((msg->recv_ns-start_ns)/(1000*1000*1000)), sub, msg->id.seq);
    }

    for (eps_event* ev; ev = eps_next_event(); ) {
      for (int i = 0; i < pub_n; i++) {
        if (ev->data.fd == pub_fds[i]) {
          uint64_t exp;
          ssize_t s = read(pub_fds[i], &exp, sizeof(exp));
          if (s == sizeof(exp)) {
            char buf[128] = {0};
            int n = 0;
            memcpy(buf, &pub_ids[i], sizeof(pub_ids[i]));
            n = sizeof(pub_ids[i]);

            sendto(eps.mc_sock, buf, n, 0, &eps.mc_dst, sizeof(eps.mc_dst));
            printf("sent %.*s %04d\n", 6, buf, pub_ids[i].seq);

            pub_ids[i].seq++;
          }
        }
      }
    }

    if (eps.errn) {
      // handle errors
    }
  }
}