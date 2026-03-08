#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>

#define PORT 50500

int main(int argc, char *argv[]) {
    int sock = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (sock < 0) return 1;

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    {
        int loop = 1;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    }

    {
        int ttl = 1;
        if (argc > 2) {
            ttl = atoi(argv[2]);
        }
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }


    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    const char *groups[] = {
        "238.1.2.3",
        "238.4.5.6",
        "238.7.8.9"
    };

    for (int i = 0; i < 3; i++) {
        struct ip_mreq mreq = {};
        inet_pton(AF_INET, groups[i], &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;

        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) < 0) {
            printf("IP_ADD_MEMBERSHIP");
        } else {
            printf("Joined %s:%d\n", groups[i], PORT);
        }
    }

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT)
    };
    char outc[256];
    int outn = 0;

    char buf[2048];

    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    for (;;) {
        int r = poll(&pfd, 1, 1000);
        if (r < 0) { printf("ERROR!"); break; }
        if (pfd.revents & POLLIN) {
            char buf[2048];
            int n = recv(sock, buf, sizeof(buf)-1, 0);
            if (n > 0) {
                buf[n] = 0;
                printf("Received: %s\n", buf);
            }
        } else if (argc > 1) {
            inet_pton(AF_INET, argv[1], &dst.sin_addr);
            outn = snprintf(outc, sizeof(outc), "Hello from %s!", argv[1]);
            sendto(sock, outc, outn, 0, (struct sockaddr*)&dst, sizeof(dst));
            printf("Sent: %s\n", outc);
        }
    }

    close(sock);
    return 0;
}