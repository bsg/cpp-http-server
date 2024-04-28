#ifndef HTTP_HPP
#define HTTP_HPP

#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <istream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <netdb.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#define KQUEUE
#define NEVENTS 64
#endif

// TODO case-insensitive header names
struct HttpHeader {
    std::map<std::string, std::string> fields;

    static HttpHeader parse(std::istringstream *stream) {
        HttpHeader header;

        for (std::string line; std::getline(*stream, line);) {
            if (line.starts_with("\r")) {
                break;
            }

            if (line.ends_with("\r")) { // TODO fail if no \r?
                line.pop_back();
            }

            int split_at = line.find(':');
            if (split_at) {
                std::string key = line.substr(0, split_at);
                while (isspace(line[++split_at]))
                    ;
                std::string value = line.substr(split_at, line.length());

                header.fields.insert({key, value});
            } else {
                // TODO
            }
        }

        return header;
    }

    std::optional<std::string> get(std::string key) {
        if (this->fields.count(key)) {
            return (this->fields)[key];
        } else {
            return {};
        }
    }

    void set(std::string key, std::string val) {
        this->fields.insert({key, val});
    }
};

enum class HttpMethod { GET, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE, PATCH };
static std::optional<HttpMethod> method_from_str(std::string s) {
    if (s == "GET") {
        return HttpMethod::GET;
    } else if (s == "HEAD") {
        return HttpMethod::HEAD;
    } else if (s == "POST") {
        return HttpMethod::POST;
    } else if (s == "PUT") {
        return HttpMethod::PUT;
    } else if (s == "DELETE") {
        return HttpMethod::DELETE;
    } else if (s == "CONNECT") {
        return HttpMethod::CONNECT;
    } else if (s == "OPTIONS") {
        return HttpMethod::OPTIONS;
    } else if (s == "TRACE") {
        return HttpMethod::TRACE;
    } else if (s == "PATCH") {
        return HttpMethod::PATCH;
    }
    return {};
}

struct HttpRequest {
    HttpMethod method;
    std::string method_str;
    std::string path;
    HttpHeader header;
    int content_length;
    char *content;

    static HttpRequest parse(char *p) {
        std::istringstream is(p);

        std::string line;
        std::getline(is, line);
        int split_at = line.find(" ");
        auto method_str = line.substr(0, split_at);
        line = line.substr(split_at + 1);
        split_at = line.find(" ");
        auto path = line.substr(0, split_at);

        HttpMethod method;
        if (auto m = method_from_str(method_str)) {
            method = *m;
        } else {
            // TODO
        }
        HttpHeader header = HttpHeader::parse(&is);

        char *content = p + is.tellg();

        int content_length = 0;
        if (auto l = header.get("Content-Length")) {
            content_length = std::stoi(*l);
        }

        return {.method = method,
                .method_str = method_str,
                .path = path,
                .header = header,
                .content_length = content_length,
                .content = content};
    }
};

struct HttpResponse {
    int status;
    HttpHeader header;
    size_t content_length;
    std::string content;
};

struct HttpResponseBuilder {
    HttpResponse response;

    HttpResponseBuilder *status(int status) {
        this->response.status = status;
        return this;
    }

    HttpResponseBuilder *header(std::string name, std::string value) {
        this->response.header.set(name, value);
        return this;
    }

    HttpResponseBuilder *body(std::string body) {
        auto len = body.length();
        this->response.content_length = len;
        this->response.content = body;
        return this;
    }

    HttpResponse build() {
        this->header("Content-Length", std::to_string(this->response.content_length));
        return this->response;
    }
};

struct Client {
    int fd;
    struct sockaddr addr;
    socklen_t socklen;
    std::vector<char> read_buffer;
    std::vector<char> write_buffer;
    size_t nwritten;

    Client() {
    }

    Client(int fd, struct sockaddr addr, socklen_t socklen) {
        this->fd = fd;
        this->addr = addr;
        this->socklen = socklen;
        this->nwritten = 0;
    }

    void write(std::string s) {
        this->write_buffer.reserve(this->write_buffer.size() + s.length());
        for (auto c : s) {
            this->write_buffer.push_back(c);
        }
    }

    int send(HttpResponse *response) {
        write("HTTP/1.1 ");
        write(std::to_string(response->status));
        switch (response->status) {
        case 200:
            write(" OK\r\n");
            break;
        case 400:
            write(" Bad Request\r\n");
            break;
        case 404:
            write(" Not Found\r\n");
            break;
        case 500:
            write(" Internal Server Error\r\n");
            break;
        // TODO handle all cases & maybe extract this out
        default:
            return -1;
        }

        for (auto const &[key, val] : response->header.fields) {
            write(key);
            write(": ");
            write(val);
            write("\r\n");
        }

        write("\r\n");

        write(response->content); // TODO respect content_length

        return 0;
    }
};

struct HttpServer {
  private:
    const char *m_host;
    const char *m_port;
    int m_backlog;
    int m_accept_fd;
    Client m_clients[1024]; // FIXME size
    void (*m_request_handler)(Client *, HttpRequest *);
    void (*m_connect_handler)(Client *);
    void (*m_disconnect_handler)(Client *);
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

        if (m_connect_handler) {
            m_connect_handler(&m_clients[client_fd]);
        }

        return 0;
    }

    int handle_close(Client *client) {
        if (m_disconnect_handler) {
            m_disconnect_handler(client);
        }

        if (close(client->fd) < 0) {
            perror("close");
            return -1;
        }

        return 0;
    }

    int handle_read(Client *client) {
        char buf[1024];
        size_t nread = recv(client->fd, buf, sizeof(buf), 0);
        client->read_buffer.insert(client->read_buffer.end(), buf, buf + nread);

        bool has_complete_header = false;
        for (int i = 0; i < 1024; i++) { // check for \r\n\r\n
            if (buf[i] == '\r' && i < 1024 - 3) {
                if (buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    has_complete_header = true;
                }
            }
        }

        if (has_complete_header) {
            HttpRequest request = HttpRequest::parse(client->read_buffer.data());

            m_request_handler(client, &request);

            struct kevent changelist;
            EV_SET(&changelist, client->fd, EVFILT_WRITE, EV_ENABLE, 0, 0, NULL);
            if (kevent(m_kq, &changelist, 1, NULL, 0, NULL) < 0) {
                perror("kevent after recv");
                return -1;
            }

            client->read_buffer.clear();
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
            client->write_buffer.clear();
            client->nwritten = 0;

            struct kevent changelist;
            EV_SET(&changelist, client->fd, EVFILT_WRITE, EV_DISABLE, 0, 0, NULL);
            if (kevent(m_kq, &changelist, 1, NULL, 0, NULL) < 0) {
                perror("kevent after recv");
                return -1;
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
        m_connect_handler = NULL;
        m_disconnect_handler = NULL;
    }

    int run() {
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

                else if (events[i].flags & EV_EOF) {
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

    void set_connect_handler(void (*hnd)(Client *)) {
        m_connect_handler = hnd;
    }

    void set_disconnect_handler(void (*hnd)(Client *)) {
        m_disconnect_handler = hnd;
    }
};

#endif