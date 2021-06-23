#include "server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>

int g_epfd = -1;      /* epoll file descriptor. */
int g_listen_fd = -1; /* listen socket's file descriptor. */

int init_server(const char *ip, int port) {
    int reuse = 1;
    struct epoll_event ee;
    struct sockaddr_in sa;

    /* create socket. */
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        LOG_SYS_ERR("create socket failed!");
        return -1;
    }
    LOG("create listen socket, fd: %d.", g_listen_fd);

    /* bind. */
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr =
        (ip == NULL || !strlen(ip)) ? htonl(INADDR_ANY) : inet_addr(ip);

    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    bind(g_listen_fd, (struct sockaddr *)&sa, sizeof(sa));

    /* listen */
    if (listen(g_listen_fd, BACKLOG) == -1) {
        LOG_SYS_ERR("listen failed!");
        close(g_listen_fd);
        return -1;
    }

    /* set nonblock */
    if (set_nonblocking(g_listen_fd) < 0) {
        return -1;
    }

    LOG("set socket nonblocking. fd: %d.", g_listen_fd);

    /* epoll */
    g_epfd = epoll_create(1024);
    if (g_epfd < 0) {
        LOG_SYS_ERR("epoll create failed!");
        close(g_listen_fd);
        return -1;
    }

    LOG("epoll_ctl add event: <EPOLLIN>, fd: %d.", g_listen_fd);

    ee.data.fd = g_listen_fd;
    ee.events = EPOLLIN;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen_fd, &ee) < 0) {
        LOG_SYS_ERR("epoll_ctl add event: <EPOLLIN> failed! fd: %d.",
                    g_listen_fd);
        close(g_listen_fd);
        return -1;
    }

    LOG("server start now, ip: %s, port: %d.",
        (ip == NULL || !strlen(ip)) ? "127.0.0.1" : ip, port);
    return 1;
}

void run_server() {
    int i, fd;
    client_t *c;
    struct epoll_event *ees;

    memset(g_clients, 0, MAX_CLIENT_CNT);
    ees = (struct epoll_event *)calloc(EVENTS_SIZE, sizeof(struct epoll_event));

    while (1) {
        int n = epoll_wait(g_epfd, ees, EVENTS_SIZE, 5 * 1000);
        for (i = 0; i < n; i++) {
            fd = ees[i].data.fd;
            if (fd == g_listen_fd) {
                fd = accept_data(g_listen_fd);
                if (fd < 0) {
                    continue;
                }
                /* create a new client. */
                add_client(fd, EPOLLIN);
            } else {
                if (!(ees[i].events & (EPOLLIN | EPOLLOUT))) {
                    LOG_SYS_ERR("invalid event! fd: %d, events: %d",
                                fd, ees[i].events);
                    del_client(fd);
                    continue;
                }

                c = get_client(fd);
                if (c == NULL) {
                    LOG("invalid client, fd: %d.", fd);
                    continue;
                }

                if (ees[i].events & EPOLLIN) {
                    /* read data from network. */
                    int ret = read_data(fd);
                    if (ret == 0) {
                        LOG("client is closed! fd: %d.", fd);
                        del_client(fd);
                        continue;
                    } else if (ret < 0) {
                        if (errno != EAGAIN && errno != EINTR) {
                            LOG_SYS_ERR("read from fd: %d failed!", c->fd);
                            del_client(fd);
                            continue;
                        }
                    }
                }

                /* handle read buffer, response to client. */
                handle_data(fd);

                if (ees[i].events & EPOLLOUT) {
                    if (write_data(fd) < 0) {
                        if (errno != EAGAIN && errno != EINTR) {
                            LOG_SYS_ERR("write data failed! fd: %d.", fd);
                            del_client(fd);
                            continue;
                        }
                    }
                }

                /* check written buffer, then epoll_ctl EPOLLOUT. */
                handle_write_events(fd);
            }
        }
    }
}

int handle_data(int fd) {
    client_t *c = get_client(fd);

    /* copy read buffer to write buffer, and then clear read buffer. */
    if (c->rlen > 0) {
        memcpy(c->wbuf + c->wlen, c->rbuf, c->rlen);
        c->wlen += c->rlen;
        memset(c->rbuf, 0, BUFFER_LEN);
        c->rlen = 0;
    }

    if (write_data(fd) < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            LOG_SYS_ERR("write data failed! fd: %d.", fd);
            del_client(fd);
            return -1;
        }
    }

    return 1;
}

int set_nonblocking(int fd) {
    int val = fcntl(fd, F_GETFL);
    val |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, val) < 0) {
        LOG_SYS_ERR("set non block failed! fd: %d.", fd);
        return -1;
    }
    return 0;
}

client_t *add_client(int fd, int events) {
    client_t *c;

    if (get_client(fd) != NULL) {
        LOG("old client exists, add failed! fd: %d.", fd);
        return NULL;
    }

    c = (client_t *)calloc(1, sizeof(client_t));
    c->fd = fd;
    c->events = events;
    g_clients[fd] = c;
    LOG("add client done, fd: %d.", fd);
    return c;
}

client_t *get_client(int fd) {
    if (fd <= 0 || fd >= MAX_CLIENT_CNT) {
        return NULL;
    }
    return g_clients[fd];
}

int del_client(int fd) {
    client_t *c = get_client(fd);
    if (c == NULL) {
        LOG("invalid client, fd: %d.", fd);
        return -1;
    }

    if (c->events & (EPOLLIN | EPOLLOUT)) {
        LOG("epoll_ctl <delete events>, fd: %d.", fd);
        if (epoll_ctl(g_epfd, EPOLL_CTL_DEL, fd, NULL) < 0) {
            LOG_SYS_ERR("epoll_ctl <delete events> failed! fd: %d.", fd);
        }
    }

    close(fd);
    free(c);
    g_clients[fd] = NULL;
    LOG("remove client, fd: %d.", fd);
    return 0;
}

int accept_data(int listen_fd) {
    int fd, port;
    char ip[32];

    struct epoll_event ee;
    struct sockaddr addr;
    struct sockaddr_in *sai;
    socklen_t addr_len = sizeof(addr);

    /* get new client. */
    fd = accept(listen_fd, &addr, &addr_len);
    if (fd < 0) {
        if (errno == EINTR || errno == EAGAIN) {
            LOG("accept next time!");
        } else {
            LOG_SYS_ERR("accept failed!");
        }
        return -1;
    }

    /* get client's connection info. */
    sai = (struct sockaddr_in *)&addr;
    inet_ntop(AF_INET, (void *)&(sai->sin_addr), ip, 32);
    port = ntohs(sai->sin_port);
    LOG("accept new client, fd: %d, ip: %s, port: %d", fd, ip, port);

    LOG("set socket nonblocking. fd: %d.", fd);
    if (set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }

    /* epoll ctrl client's events. */
    LOG("epoll_ctl add event: <EPOLLIN>, fd: %d.", fd);
    ee.data.fd = fd;
    ee.events = EPOLLIN;
    if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, fd, &ee) < 0) {
        LOG_SYS_ERR("epoll_ctl add event: <EPOLLIN> failed! fd: %d.", fd);
        close(fd);
        return -1;
    }

    return fd;
}

/* read data from network. */
int read_data(int fd) {
    client_t *c;
    int rlen;
    char rbuf[1024];
    memset(rbuf, 0, 1024);

    c = get_client(fd);
    if (c == NULL) {
        return -1;
    }

    while (1) {
        rlen = read(c->fd, rbuf, 1024);
        if (rlen == 0) {
            return 0;
        } else if (rlen < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                // LOG("for async, try to read next time! fd: %d.\n", c->fd);
            } else {
                LOG_SYS_ERR("read data failed! fd: %d.", fd);
            }
            return -1;
        } else {
            /* save data to read buffer. */
            memcpy(c->rbuf + c->rlen, rbuf, rlen);
            c->rlen += rlen;
            LOG("fd: %d, read len: %d, buffer: %s", c->fd, rlen, rbuf);
        }
    }

    return 1;
}

int write_data(int fd) {
    int len, wlen;
    client_t *c;

    c = get_client(fd);
    if (c == NULL) {
        LOG("invalid client, fd: %d.", fd);
        return -1;
    }

    if (c->wlen == 0) {
        return 0;
    }

    /* limit write len for EPOLLOUT test. */
    len = c->wlen < 8 ? c->wlen : 8;

    /* write data to client. */
    wlen = write(c->fd, c->wbuf, len);
    if (wlen >= 0) {
        /* truncate data. */
        memcpy(c->wbuf, c->wbuf + wlen, c->wlen - wlen);
        c->wlen -= wlen;
        LOG("write to client. fd: %d, write len: %d, left: %d.",
            c->fd, wlen, c->wlen);
        return wlen;
    } else {
        if (errno == EAGAIN || errno == EINTR) {
            LOG("try to write next time! fd: %d.", c->fd);
        } else {
            LOG_SYS_ERR("write data failed! fd: %d.", c->fd);
        }
        return -1;
    }
}

int handle_write_events(int fd) {
    int op;
    client_t *c;
    struct epoll_event ee;

    c = g_clients[fd];
    if (c == NULL) {
        LOG("invalid client, fd: %d.", fd);
        return -1;
    }

    if (c->wlen > 0) {
        if (!(c->events & EPOLLOUT)) {
            LOG("epoll_ctl add event: <EPOLLOUT>, fd: %d.", fd);

            /* write next time, then control EPOLLOUT. */
            ee.data.fd = c->fd;
            ee.events = c->events;
            ee.events |= EPOLLOUT;
            op = (!c->events) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
            if (epoll_ctl(g_epfd, op, c->fd, &ee) < 0) {
                LOG_SYS_ERR("epoll_ctl add event: <EPOLLOUT> failed! fd: %d.",
                            c->fd);
                return -1;
            }
            c->events = ee.events;
            return 1;
        }
    } else {
        if (c->events & EPOLLOUT) {
            LOG("epoll_ctl delete event: <EPOLLOUT>, fd: %d.", fd);

            /* no data to write, then delete EPOLLOUT.*/
            ee.data.fd = c->fd;
            ee.events = c->events;
            ee.events &= ~EPOLLOUT;
            op = (!c->events) ? EPOLL_CTL_DEL : EPOLL_CTL_MOD;
            if (epoll_ctl(g_epfd, op, c->fd, &ee) < 0) {
                LOG_SYS_ERR("epoll_ctl delete event: <EPOLLOUT> failed! fd: %d.",
                            c->fd);
                return -1;
            }
            c->events = ee.events;
            return 1;
        }
    }

    return 1;
}