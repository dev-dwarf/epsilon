#ifndef EPS_H
#define EPS_H
#include <stdint.h>
#include <string.h> // memcpy, memset
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
#ifndef EPS_MSG_SIZE // size of buffers passed to recv
#define EPS_MSG_SIZE 1472
#endif

#ifndef EPS_GROUP
#define EPS_GROUP
enum EPS_GROUP {
  EPS_CMD = 0,
  EPS_SIG,
  EPS_TLM,
  EPS_RIN,
  EPS_ROT,
  EPS_GROUPS,
};
#endif

typedef struct epoll_event eps_event;

typedef struct eps_id { 
  uint16_t agent;
  uint16_t msg;
  uint16_t inst;
  uint16_t seq;
} eps_id;

typedef struct eps_msg {
  eps_id id;
  int64_t recv_ns;
  uint8_t *data;
  uint16_t size;
} eps_msg;

struct eps {
  bool sub_group[EPS_GROUPS];
  uint8_t mc_group[EPS_GROUPS][4];
  uint8_t mc_port[2];
  int loopback;
  int ttl;  

  int mc_sock;
  struct sockaddr_in mc_dst[EPS_GROUPS];
  int subn;
  eps_msg subs[EPS_MAX_SUBS];
  uint8_t msg_buf[EPS_MSG_SIZE];
  eps_msg msg_out;
  int epoll;
  int eventn;
  int eventi;
  eps_event events[8];
  int errn; // copy of errno that can be read after eps_poll or eps_next_msg
  const char *err;
};
struct eps eps;

void eps_add_sub(eps_msg sub) {
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
    ts.it_interval.tv_nsec = (ivl_ms % 1000) * ((int64_t)1e9);
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
    return ((int64_t)ts.tv_sec * ((int64_t)1e9)) + ts.tv_nsec;
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
  opt = eps.ttl == 0? 1 : eps.loopback; // force loopback if ttl=0 so that stuff happens
  setsockopt(eps.mc_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &opt, sizeof(opt));
  setsockopt(eps.mc_sock, IPPROTO_IP, IP_MULTICAST_TTL, &eps.ttl, sizeof(eps.ttl));
  {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = INADDR_ANY;
    memcpy(&dst.sin_port, eps.mc_port, sizeof(eps.mc_port));
    if (bind(eps.mc_sock, (struct sockaddr*) &dst, sizeof(dst)) < 0 ) {
      return 1; // TODO err
    }
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
    // TODO dont print by default
    printf("Joined %u.%u.%u.%u\n", 
      eps.mc_group[i][0], eps.mc_group[i][1], eps.mc_group[i][2], eps.mc_group[i][3]);
    
    eps.mc_dst[i].sin_family = AF_INET;
    memcpy(&eps.mc_dst[i].sin_port, eps.mc_port, sizeof(eps.mc_port));
    memcpy(&eps.mc_dst[i].sin_addr, eps.mc_group[i], sizeof(eps.mc_group[i]));
  }

  if ((eps.epoll = epoll_create1(0)) < 0) { return 1; /* TODO err; */ }
  if (eps_add_fd(eps.mc_sock) < 0) { return 1; /* TODO err */ }
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
eps_msg *eps_next_msg() {
  eps_msg *out = 0;

  int len = recv(eps.mc_sock, eps.msg_buf, sizeof(eps.msg_buf), 0);
  if (len < 0) {
    eps.errn = errno;
  } else if (len >= sizeof(eps_id)) {
    uint8_t *d = eps.msg_buf;
    eps_id id; memcpy(&id, d, sizeof(id));

    for (int i = 0; i < eps.subn; i++) {
      eps_msg sub = eps.subs[i];
      eps_id sd = sub.id;
      bool match = 1;
      match &= (sd.agent == 0) || (sd.agent == id.agent);
      match &= (sd.msg   == 0) || (sd.msg   == id.msg  );
      match &= (sd.inst  == 0) || (sd.inst  == id.inst );
      if (match) {
        out = &eps.msg_out;
        eps.msg_out.id = id;
        eps.msg_out.recv_ns = eps_ns();
        
        if (sub.data) {
          memcpy(sub.data, d, sub.size);
          eps.msg_out.data = sub.data;
          eps.msg_out.size = sub.size;
        } else {
          eps.msg_out.data = d;
          eps.msg_out.size = len;
        }
        break;
      }
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

int eps_send_ex(int *groups, int groupn, eps_msg *msg) {
  int out = 0;
  memcpy(msg->data, &msg->id, sizeof(msg->id));
  for (int i = 0; i < groupn; i++) {
    if (!(groups[i] < EPS_GROUPS)) {
      continue;
    }
    int r = sendto(eps.mc_sock, 
      msg->data, msg->size, MSG_DONTWAIT, 
      (struct sockaddr*) eps.mc_dst+groups[i], sizeof(eps.mc_dst)
    );
    if (r != msg->size) {
      out = 1;
      eps.errn = errno;
    }
  }
  msg->id.seq++;
  return out;
}
int eps_send(int group, eps_msg *msg) {
  return eps_send_ex(&group, 1, msg);
}

#endif//EPS_H
