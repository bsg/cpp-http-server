#include <cstdio>
#include <iostream>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <netdb.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#define KQUEUE
#endif

#include "http.hpp"

struct Client {
  int fd;
  struct sockaddr addr;
  socklen_t socklen;
  std::vector<char> *read_buffer; // TODO use stringstream instead?

  Client() {
  }

  Client(int fd, struct sockaddr addr, socklen_t socklen) {
    this->fd = fd;
    this->addr = addr;
    this->socklen = socklen;
    this->read_buffer = new std::vector<char>;
  }
};

class HttpServer final {
  const char *m_host;
  const char *m_port;
  int m_backlog;
  int m_accept_fd;
  Client m_clients[1024]; // FIXME size
  int m_kq;
  struct kevent m_change_event[4], m_event[4]; // FIXME

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

    EV_SET(m_change_event, client_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (kevent(m_kq, m_change_event, 1, NULL, 0, NULL) < 0) {
      perror("kevent filter on connect");
      return -1;
    }

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
      printf("METHOD: %s\n", request.method_str.c_str());
      printf("PATH: %s\n", request.path.c_str());
      if (request.content_length > 0) {
        write(STDOUT_FILENO, request.content, request.content_length);
        printf("\n\n");
      }
    }

    // TODO remove
    write(client->fd, "HTTP/1.1 200 OK\r\n\r\n", 19);
    close(client->fd);

    return 0;
  }

public:
  HttpServer(const char *host, const char *port) {
    m_host = host;
    m_port = port;
    m_backlog = 1024;
  }

  int run() {
    printf("listening on %s:%s\n", m_host, m_port);
    do_listen();

#ifdef KQUEUE
    m_kq = kqueue();
    EV_SET(m_change_event, m_accept_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, 0);

    if (kevent(m_kq, m_change_event, 1, NULL, 0, NULL) < 0) {
      perror("kevent accept");
      return -1;
    }

    while (true) {
      int new_events = kevent(m_kq, NULL, 0, m_event, 1, NULL);
      if (new_events < 0) {
        perror("kevent query");
        return -1;
      }

      for (int i = 0; i < new_events; i++) {
        int event_fd = m_event[i].ident;

        if (m_event[i].flags & EV_EOF) {
          handle_close(&m_clients[event_fd]);
        }

        else if (event_fd == m_accept_fd) {
          handle_accept();
        }

        else if (m_event[i].filter & EVFILT_READ) {
          handle_read(&m_clients[event_fd]);
        }
      }
#endif
    }
  }
};

int main(int argc, char const *argv[]) {
  HttpServer httpServer("127.0.0.1", "8080");
  int status = httpServer.run();
  if (status < 0) {
    perror("failed to start the server");
  }
  return 0;
}
