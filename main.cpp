#include <cstdio>
#include <iostream>
#include <istream>
#include <sstream>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <netdb.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#define KQUEUE
#define NEVENTS 256
#endif
#include <fcntl.h>

#include "http.hpp"

struct Client {
    int fd;
    struct sockaddr addr;
    socklen_t socklen;
    std::vector<char> *read_buffer;
    std::vector<char> write_buffer;
    size_t nwritten;

    Client() {
    }

    Client(int fd, struct sockaddr addr, socklen_t socklen) {
        this->fd = fd;
        this->addr = addr;
        this->socklen = socklen;
        this->read_buffer = new std::vector<char>;
        this->nwritten = 0;
    }

    void write(std::string s) {
        this->write_buffer.reserve(this->write_buffer.size() + s.length());
        for (auto c : s) {
            this->write_buffer.push_back(c);
        }
    }
};

struct HttpServer {
    const char *m_host;
    const char *m_port;
    int m_backlog;
    int m_accept_fd;
    Client m_clients[1024]; // FIXME size
    void (*m_request_handler)(Client *, HttpRequest *);
#ifdef KQUEUE
    int m_kq;
#endif

    int do_listen() {
        int status;
        struct addrinfo hints;
        struct addrinfo *servinfo;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        hints.ai_flags = AI_PASSIVE;

        status = getaddrinfo(m_host, m_port, &hints, &servinfo);
        if (status) {
            perror("getaddrinfo");
            return -1;
        }

        m_accept_fd = socket(servinfo->ai_family, servinfo->ai_socktype, servinfo->ai_protocol);
        if (m_accept_fd < 0) {
            perror("socket");
            return -1;
        }

        int reuse_addr = 1;
        status = setsockopt(m_accept_fd, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));
        if (status < 0) {
            perror("setsockopt");
            return -1;
        }

        status = bind(m_accept_fd, servinfo->ai_addr, servinfo->ai_addrlen);
        if (status < 0) {
            perror("bind");
            return -1;
        }

        status = listen(m_accept_fd, m_backlog);
        if (status < 0) {
            perror("listen");
            return -1;
        }

        return 0;
    }

    int handle_accept() {
        struct sockaddr client_addr;
        socklen_t client_len;

        int client_fd = accept(m_accept_fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_len);
        if (client_fd < 0) {
            perror("accept");
            return -1;
        }

        int flags = fcntl(client_fd, F_GETFL, 0);
        if (flags < 0) {
            perror("fnctl get on accept");
        }
        if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            perror("fnctl set on accept");
        }

#ifdef KQUEUE
        struct kevent changelist[2];
        EV_SET(&changelist[0], client_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
        EV_SET(&changelist[1], client_fd, EVFILT_WRITE, EV_ADD | EV_DISABLE, 0, 0, NULL);
        if (kevent(m_kq, changelist, 2, NULL, 0, NULL) < 0) {
            perror("kevent on connect");
            return -1;
        }
#endif

        m_clients[client_fd] = Client(client_fd, client_addr, client_len);

        printf("client connected fd: %d\n", client_fd);
        return 0;
    }

    int handle_close(Client *client) {
        if (close(client->fd) < 0) {
            perror("close");
            return -1;
        }

        printf("client disconnected fd: %d\n", client->fd);
        return 0;
    }

    int handle_read(Client *client) {
        char buf[1024];
        size_t nread = recv(client->fd, buf, sizeof(buf), 0);
        client->read_buffer->insert(client->read_buffer->end(), buf, buf + nread);

        bool has_complete_header = false;
        for (int i = 0; i < 1024; i++) { // check for \r\n\r\n
            if (buf[i] == '\r' && i < 1024 - 3) {
                if (buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    has_complete_header = true;
                }
            }
        }

        if (has_complete_header) {
            HttpRequest request = HttpRequest::parse(client->read_buffer->data());

            m_request_handler(client, &request);

            struct kevent changelist;
            EV_SET(&changelist, client->fd, EVFILT_WRITE, EV_ENABLE, 0, 0, NULL);
            if (kevent(m_kq, &changelist, 1, NULL, 0, NULL) < 0) {
                perror("kevent after recv");
                return -1;
            }
        }
        return 0;
    }

    int handle_write(Client *client) {
        int nwritten = send(client->fd, client->write_buffer.data() + client->nwritten, client->write_buffer.size(), 0);
        if (nwritten < 0) {
            perror("write");
            return -1;
        }

        if (nwritten == client->write_buffer.size()) {
            if (close(client->fd) < 0) {
                perror("close");
            }
        } else {
            client->nwritten = nwritten;
        }

        return 0;
    }

  public:
    HttpServer(const char *host, const char *port, void (*request_handler)(Client *, HttpRequest *)) {
        m_host = host;
        m_port = port;
        m_backlog = 1024;
        m_request_handler = request_handler;
    }

    int run() {
        printf("listening on %s:%s\n", m_host, m_port);
        do_listen();

#ifdef KQUEUE
        struct kevent changelist;
        m_kq = kqueue();
        EV_SET(&changelist, m_accept_fd, EVFILT_READ, EV_ADD, 0, 0, 0);

        if (kevent(m_kq, &changelist, 1, NULL, 0, NULL) < 0) {
            perror("kevent accept");
            return -1;
        }

        while (true) {
            struct kevent events[NEVENTS];
            int new_events = kevent(m_kq, NULL, 0, events, NEVENTS, NULL);
            if (new_events < 0) {
                perror("kevent poll");
                return -1;
            }

            for (int i = 0; i < new_events; i++) {
                int event_fd = events[i].ident;

                if (event_fd == m_accept_fd) {
                    handle_accept();
                }

                else if (events[i].flags == EV_EOF) {
                    handle_close(&m_clients[event_fd]);
                }

                else if (events[i].filter == EVFILT_READ) {
                    handle_read(&m_clients[event_fd]);
                }

                else if (events[i].filter == EVFILT_WRITE) {
                    handle_write(&m_clients[event_fd]);
                }
            }
#endif
        }
    }
};

void request_handler(Client *client, HttpRequest *request) {
    printf("METHOD: %s\n", request->method_str.c_str());
    printf("PATH: %s\n", request->path.c_str());
    if (request->content_length > 0) {
        write(STDOUT_FILENO, request->content, request->content_length);
        printf("\n\n");
    }
    client->write("HTTP/1.1 200 OK\r\n\r\n");
}

int main(int argc, char const *argv[]) {
    HttpServer httpServer("127.0.0.1", "8080", request_handler);
    int status = httpServer.run();
    if (status < 0) {
        perror("failed to start the server");
    }
    return 0;
}
