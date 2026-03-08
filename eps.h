#ifndef EPS_H
#define EPS_H
#include <stdint.h>
#include <string.h> // memcpy, memset

#define __USE_GNU // to get recvmmsg from sys/socket.h
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>

#ifndef EPS_GROUP_BASE // base addr for multicast groups
#define EPS_GROUP_BASE { 238,46,104,0 } // BE byte order
#endif
#ifndef EPS_PORT // port used for multicast traffic
#define EPS_PORT { 0xC5, 0x44 }; // 50500, BE byte order
#endif

#ifndef EPS_MAX_SUBS 
#define EPS_MAX_SUBS 128
#endif
#ifndef EPS_MMSGS // how many buffers to pass to recvmmsg
#define EPS_MMSGS 8
#endif
#ifndef eps_sub_SIZE // size of buffers passed to recvmmsg
#define eps_sub_SIZE 1472
#endif

// can be overriden with your own message groups
#ifndef EPS_GROUP
#define EPS_GROUP
enum EPS_GROUP {
  EPS_CMD = 0, // cmd streams from authoritative agents
  EPS_SIG, // signaling messages between agents
  EPS_TLM, // all other agent data streams, for logging and packing telemetry
  EPS_RIN, // radio inbound to network
  EPS_ROT, // radio outbound from network
  EPS_GROUPS,
};
#endif

typedef struct epoll_event eps_event;

// message identification:
// agent > msg > inst
// recommend using hashes to keep these values stable
typedef struct eps_id { 
  uint16_t agent;
  uint16_t msg;
  uint16_t inst;
  uint16_t seq;
} eps_id;

typedef struct eps_sub {
  eps_id id; // id filter for subscription, 0 fields are wildcards, seq ignored
  int64_t recv_ns; // timestamp
  uint8_t *data; // optional ptr to write msg data
  uint16_t size; // optional sizeof(*data), messages not this size will be dropped
} eps_sub;

struct eps {
  // params
  uint8_t sub_group[EPS_GROUPS];
  uint8_t mc_group[EPS_GROUPS][4]; // big-endian
  uint8_t mc_port[2]; // big endian
  int loopback;
  int ttl;  

  int mc_sock;

  struct sockaddr_in mc_dst;

  int subn;
  eps_sub subs[EPS_MAX_SUBS];

  struct mmsghdr mmsg_hdrs[EPS_MMSGS];
  struct iovec mmsg_vecs[EPS_MMSGS];
  uint8_t mmsg_bufs[EPS_MMSGS][eps_sub_SIZE];
  int mmsg_n;
  int mmsg_i;
  eps_sub mmsg_out;

  int epoll;
  int eventn;
  int eventi;
  eps_event events[8];

  int errn; // copy of errno that can be read after eps_poll or eps_next_msg

  const char *err;
};
struct eps eps;

void eps_add_sub(eps_sub sub) {
  if (eps.subn < EPS_MAX_SUBS) {
    eps.subs[eps.subn++] = sub;
  }
}

int eps_add_fd(int fd) {
  eps_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  return epoll_ctl(eps.epoll, EPOLL_CTL_ADD, fd, &ev);
}

int eps_add_timer(uint16_t ivl_ms) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (fd != -1) {
    struct itimerspec ts = {0};
    ts.it_interval.tv_sec  = ivl_ms / 1000;
    ts.it_interval.tv_nsec = (ivl_ms % 1000) * (1000*1000*1000);
    ts.it_value.tv_nsec = 1000;
    timerfd_settime(fd, 0, &ts, NULL);
    if (eps_add_fd(fd) != 0) {
      close(fd);
      fd = -1;
    }
  }
  return fd;
}

int64_t eps_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * (1000*1000*1000)) + ts.tv_nsec;
}

int eps_init() {
  if (!eps.mc_port[0]) {
    uint8_t port[2] = EPS_PORT;
    memcpy(eps.mc_port, port, sizeof(port));
  }
  if (!eps.mc_group[0][0]) {
    uint8_t group[4] = EPS_GROUP_BASE;
    memcpy(eps.mc_group[0], group, sizeof(group));
  }
  if ((int)eps.mc_group[0][3] + EPS_GROUPS > 0xFF ) {
    return 1; // TODO err
  }

  if ((eps.mc_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)) < 0) {
    return 1; // TODO err
  }

  int opt = 1;
  setsockopt(eps.mc_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  // setsockopt(eps.mc_sock, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

  opt = eps.ttl == 0? 1 : eps.loopback; // force loopback if ttl=0
  setsockopt(eps.mc_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &opt, sizeof(opt));
  setsockopt(eps.mc_sock, IPPROTO_IP, IP_MULTICAST_TTL, &eps.ttl, sizeof(eps.ttl));

  uint8_t localhost[4] = {127,0,0,1};
  // setsockopt(eps.mc_sock, IPPROTO_IP, IP_MULTICAST_IF, &localhost, sizeof(localhost));

  memset(&eps.mc_dst, 0, sizeof(eps.mc_dst));
  eps.mc_dst.sin_family = AF_INET;
  eps.mc_dst.sin_addr.s_addr = INADDR_ANY;
  memcpy(&eps.mc_dst.sin_port, eps.mc_port, sizeof(eps.mc_port));
  if (bind(eps.mc_sock, (struct sockaddr*) &eps.mc_dst, sizeof(eps.mc_dst)) < 0 ) {
    return 1; // TODO err
  }

  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq)); 
  mreq.imr_interface.s_addr = INADDR_ANY;
  uint8_t group[4];
  memcpy(group, eps.mc_group[0], sizeof(group));
  for (uint8_t i = 0; i < EPS_GROUPS; i++) {
    if (!eps.sub_group[i]) continue;
    if (i != 0) {
      memcpy(eps.mc_group[i], eps.mc_group[0], 4);
      eps.mc_group[i][3] = eps.mc_group[0][3]+i;
    }
    memcpy(&mreq.imr_multiaddr, eps.mc_group[i], 4);
    if (setsockopt(eps.mc_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
      return 1; // TODO err
    }

    printf("Joined %u.%u.%u.%u\n", 
      eps.mc_group[i][0], eps.mc_group[i][1], eps.mc_group[i][2], eps.mc_group[i][3]);
  }

  if ((eps.epoll = epoll_create1(0)) < 0) { return 1; /* TODO err; */ }
  if (eps_add_fd(eps.mc_sock) < 0) { return 1; /* TODO err */ }

  for (int i = 0; i < EPS_MMSGS; i++) {
    eps.mmsg_vecs[i].iov_base = eps.mmsg_bufs[i];
    eps.mmsg_vecs[i].iov_len  = sizeof(eps.mmsg_bufs[i]);

    // not reading msg addr or control, so just setting iov
    eps.mmsg_hdrs[i].msg_hdr.msg_iov    = &eps.mmsg_vecs[i];
    eps.mmsg_hdrs[i].msg_hdr.msg_iovlen = 1;
  }
}

int eps_poll() {
  eps.eventi = 0;
  eps.eventn = epoll_wait(eps.epoll, eps.events, sizeof(eps.events)/sizeof(*eps.events), -1);
  if (eps.eventn < 0) {
    if (errno != EINTR) {
      eps.errn = errno;
      eps.eventn = 0;
    }
  }
  return eps.eventn;
}

// return pointer to next msg or 0 if no more messages are available.
eps_sub *eps_next_msg() {
  eps_sub *out = 0;

  if (eps.mmsg_n == 0) {
    eps.mmsg_i = 0;
    eps.mmsg_n = recvmmsg(eps.mc_sock, eps.mmsg_hdrs, EPS_MMSGS, MSG_DONTWAIT, NULL);
    if (eps.mmsg_n < 0) {
      eps.mmsg_n = 0;
      eps.errn = errno;
    } else {
      eps.errn = 0;
    }
  }

  if (eps.mmsg_n > 0) {
    while (eps.mmsg_i < eps.mmsg_n) {
      int len = eps.mmsg_hdrs[eps.mmsg_i].msg_len;
      uint8_t *d = eps.mmsg_bufs[eps.mmsg_i++];
      eps_id id; 
      if (len < sizeof(id)) { continue; }
      memcpy(&id, d, sizeof(id));

      for (int i = 0; i < eps.subn; i++) {
        eps_sub sub = eps.subs[i];
        eps_id sd = sub.id;
        bool match = 1;
        match &= (sd.agent == 0) || (sd.agent == id.agent);
        match &= (sd.msg   == 0) || (sd.msg   == id.msg  );
        match &= (sd.inst  == 0) || (sd.inst  == id.inst );
        if (match) {
          out = &eps.mmsg_out;
          eps.mmsg_out.id = id;
          eps.mmsg_out.recv_ns = eps_ns();
          
          if (sub.data) {
            memcpy(sub.data, d, sub.size);
            eps.mmsg_out.data = sub.data;
            eps.mmsg_out.size = sub.size;
          } else {
            eps.mmsg_out.data = d;
            eps.mmsg_out.size = len;
          }
          break;
        }
      }
    }
    if (eps.mmsg_i == eps.mmsg_n) {
      eps.mmsg_n = 0;
    }
  }

  return out;
}

// return pointer to next event or 0 if no more messages are available.
eps_event* eps_next_event() {
  eps_event* out = 0;
  if (eps.eventi < eps.eventn) {
    out = &eps.events[eps.eventi++];
  }
  return out;
}

#endif//EPS_H
