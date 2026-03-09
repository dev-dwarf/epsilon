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

#ifndef EPS_BREAK
#if __GNUC__
#define EPS_BREAK do { __builtin_trap(); } while (0);
#else
#define EPS_BREAK do { *(volatile int *)0 = 0; } while (0);
#endif
#endif

#ifndef EPS_ERR
#include <stdio.h> // snprintf
#define EPS_ERR_SOFT(msg, ...) do { if (eps.err[0] == 0) {\
  snprintf(eps.err, sizeof(eps.err), "eps.h:%d: " msg "\n", __LINE__, ##__VA_ARGS__);\
}} while(0);
#define EPS_ERR_HARD(cond, msg, ...) do { if (!(cond)) {\
  EPS_ERR_SOFT(msg, ##__VA_ARGS__);\
  EPS_BREAK;\
}} while(0);
#define EPS_ERRNO_HARD(cond, msg, ...) EPS_ERR_HARD(cond, "" msg ": %s", ##__VA_ARGS__, strerror(errno));
#define EPS_ERRNO_SOFT(cond, msg, ...) do { if (!(cond)) {\
  EPS_ERR_SOFT("" msg ": %s", ##__VA_ARGS__, strerror(errno));\
}} while (0);
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

typedef struct eps_params {
  bool sub_group[EPS_GROUPS];
  uint8_t mc_group[EPS_GROUPS][4];
  uint8_t mc_port[2];
  int loopback;
  int ttl;  
} eps_params;

struct eps {
  eps_params params;
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

  char err[128];
};
extern struct eps eps;

void eps_init(eps_params params);
void eps_add_sub(eps_msg sub);
void eps_add_fd(int fd);
int eps_add_timer(uint16_t ivl_ms);
uint64_t eps_ev_timer(eps_event *ev, int fd);
int64_t eps_ns();
int eps_poll();
eps_msg *eps_next_msg();
eps_event* eps_next_event();
void eps_send_ex(int *groups, int groupn, eps_msg *msg);
void eps_send(int group, eps_msg *msg);

#ifdef  EPS_IMPL
struct eps eps;

void eps_add_sub(eps_msg sub) {
  if (eps.subn < EPS_MAX_SUBS) {
    eps.subs[eps.subn++] = sub;
  }
}

void eps_add_fd(int fd) {
  eps_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = fd;
  EPS_ERRNO_HARD(epoll_ctl(eps.epoll, EPOLL_CTL_ADD, fd, &ev) != -1, "");
}

int eps_add_timer(uint16_t ivl_ms) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  EPS_ERRNO_HARD(fd != -1, "failed to create timer");
  struct itimerspec ts = {0};
  ts.it_interval.tv_sec  = ivl_ms / 1000;
  ts.it_interval.tv_nsec = (ivl_ms % 1000) * ((int64_t)1e6);
  ts.it_value.tv_nsec = 1000;   
  EPS_ERRNO_HARD(timerfd_settime(fd, 0, &ts, NULL) != -1, "timerfd_settime");
  eps_add_fd(fd);
  return fd;
}
uint64_t eps_ev_timer(eps_event *ev, int fd) {
  if (ev->data.fd == fd) {
    uint64_t exp = 0;
    ssize_t s = read(fd, &exp, sizeof(exp));
    if (s == sizeof(exp)) {
      return exp;
    }
  }
  return 0;
}

int64_t eps_ns() {
    struct timespec ts;
    EPS_ERRNO_HARD(clock_gettime(CLOCK_MONOTONIC, &ts) != -1, "failed clock_gettime");
    return ((int64_t)ts.tv_sec * ((int64_t)1e9)) + ts.tv_nsec;
}

void eps_init(eps_params params) {
  if (!params.mc_port[0]) {
    uint8_t port[2] = EPS_PORT;
    memcpy(params.mc_port, port, sizeof(port));
  }
  if (!params.mc_group[0][0]) {
    uint8_t group[4] = EPS_GROUP_BASE;
    memcpy(params.mc_group[0], group, sizeof(group));
  }

  EPS_ERR_HARD(((int)params.mc_group[0][3]) + EPS_GROUPS <= 0xFF, 
    "mc_group[0][3] + EPS_GROUPS must not overflow");
  EPS_ERRNO_HARD((eps.mc_sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0)) != -1, 
    "failed to create multicast socket");

  eps.params = params;

  int opt = 1;
  EPS_ERRNO_HARD(setsockopt(eps.mc_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != -1, "failed SO_REUSEADDR");
  opt = params.ttl == 0? 1 : params.loopback; // force loopback if ttl=0 so that stuff happens
  EPS_ERRNO_HARD(setsockopt(eps.mc_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &opt, sizeof(opt)) != -1, "failed IP_MULTICAST_LOOP");
  EPS_ERRNO_HARD(setsockopt(eps.mc_sock, IPPROTO_IP, IP_MULTICAST_TTL, &params.ttl, sizeof(params.ttl)) != -1, "failed IP_MULTICAST_TTL");
  {
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = INADDR_ANY;
    memcpy(&dst.sin_port, params.mc_port, sizeof(params.mc_port));
    EPS_ERRNO_HARD(bind(eps.mc_sock, (struct sockaddr*) &dst, sizeof(dst)) != -1, 
      "failed to bind multicast socket");
  }

  struct ip_mreq mreq;
  memset(&mreq, 0, sizeof(mreq)); 
  mreq.imr_interface.s_addr = INADDR_ANY;
  uint8_t group[4];
  memcpy(group, params.mc_group[0], sizeof(group));
  for (uint8_t i = 0; i < EPS_GROUPS; i++) {
    if (!params.sub_group[i]) continue;
    if (i != 0) {
      memcpy(params.mc_group[i], params.mc_group[0], 4);
      params.mc_group[i][3] = params.mc_group[0][3]+i;
    }
    memcpy(&mreq.imr_multiaddr, params.mc_group[i], 4);

    EPS_ERRNO_HARD(
      setsockopt(eps.mc_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != -1, 
      "failed to join %u.%u.%u.%u",
      params.mc_group[i][0], params.mc_group[i][1], params.mc_group[i][2], params.mc_group[i][3]
    );

    eps.mc_dst[i].sin_family = AF_INET;
    memcpy(&eps.mc_dst[i].sin_port, params.mc_port, sizeof(params.mc_port));
    memcpy(&eps.mc_dst[i].sin_addr, params.mc_group[i], sizeof(params.mc_group[i]));
  }

  EPS_ERRNO_HARD((eps.epoll = epoll_create1(0)) != -1, "");
  eps_add_fd(eps.mc_sock);
}

int eps_poll() {
  eps.eventi = 0;
  eps.eventn = epoll_wait(eps.epoll, eps.events, sizeof(eps.events)/sizeof(*eps.events), -1);
  EPS_ERRNO_SOFT(eps.eventn >= 0, "epoll failed");
  return eps.eventn;
}

eps_msg *eps_next_msg() {
  eps_msg *out = 0;
  int len = recv(eps.mc_sock, eps.msg_buf, sizeof(eps.msg_buf), 0);
  if (len < 0) {
    EPS_ERRNO_SOFT(errno == EAGAIN || errno == EWOULDBLOCK, "eps_next_msg");
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

eps_event* eps_next_event() {
  eps_event* out = 0;
  if (eps.eventi < eps.eventn) {
    out = &eps.events[eps.eventi++];
  }
  return out;
}

void eps_send_ex(int *groups, int groupn, eps_msg *msg) {
  EPS_ERR_HARD(msg->size >= sizeof(msg->id), 
    "msg size too small for eps_id: %d", msg->size);
  memcpy(msg->data, &msg->id, sizeof(msg->id));
  for (int i = 0; i < groupn; i++) {
    if (!(groups[i] < EPS_GROUPS)) {
      continue;
    }
    int r = sendto(eps.mc_sock, 
      msg->data, msg->size, MSG_DONTWAIT, 
      (struct sockaddr*) eps.mc_dst+groups[i], sizeof(eps.mc_dst)
    );
    EPS_ERRNO_SOFT(r == msg->size || errno == EAGAIN || errno == EWOULDBLOCK, "eps_send_ex");
  }
  msg->id.seq++;
}
void eps_send(int group, eps_msg *msg) {
  eps_send_ex(&group, 1, msg);
}
#endif//EPS_IMPL
#endif//EPS_H
