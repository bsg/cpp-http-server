#ifndef HTTP_HPP
#define HTTP_HPP

#include <map>
#include <optional>
#include <sstream>
#include <string>

// TODO case-insensitive header names
struct HttpHeader {
    std::map<std::string, std::string> *fields;

    static HttpHeader parse(std::istringstream *stream) {
        auto fields = new std::map<std::string, std::string>;

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

                fields->insert({key, value});
            } else {
                // TODO
            }
        }

        return {.fields = fields};
    }

    std::optional<std::string> get(std::string key) {
        if (fields->count(key)) {
            return (*fields)[key];
        } else {
            return {};
        }
    }
};

enum class HttpMethod { GET, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE, PATCH };
static std::optional<HttpMethod> method_from_str(std::string s) {
    if (s.compare("GET")) {
        return HttpMethod::GET;
    } else if (s.compare("HEAD")) {
        return HttpMethod::HEAD;
    } else if (s.compare("POST")) {
        return HttpMethod::POST;
    } else if (s.compare("PUT")) {
        return HttpMethod::PUT;
    } else if (s.compare("DELETE")) {
        return HttpMethod::DELETE;
    } else if (s.compare("CONNECT")) {
        return HttpMethod::CONNECT;
    } else if (s.compare("OPTIONS")) {
        return HttpMethod::OPTIONS;
    } else if (s.compare("TRACE")) {
        return HttpMethod::TRACE;
    } else if (s.compare("PATCH")) {
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
#endif