#ifndef HTTPPARSER_H
#define HTTPPARSER_H

#include <string>
#include <functional>
#include <cstring>

class HttpRequest {

    friend class HttpParser;

private:
    const static int MAX_HEADERS = 50;
    struct Header {
        std::string_view key, value;
    } headers[MAX_HEADERS];

public:
    std::string_view getHeader(std::string_view header) {
        for (Header *h = headers; (++h)->key.length(); ) {
            if (h->key.length() == header.length() && !strncmp(h->key.data(), header.data(), header.length())) {
                return h->value;
            }
        }
        return std::string_view(nullptr, 0);
    }

    std::string_view getUrl() {
        return headers->value;
    }

};

class HttpParser {

private:
    std::string fallback;
    int remainingStreamingBytes = 0;

    const size_t MAX_FALLBACK_SIZE = 1024 * 4;

    static unsigned int toUnsignedInteger(std::string_view str) {
        int unsignedIntegerValue = 0;
        for (unsigned char c : str) {
            unsignedIntegerValue = unsignedIntegerValue * 10 + (c - '0');
        }
        return unsignedIntegerValue;
    }

    static unsigned int getHeaders(char *postPaddedBuffer, char *end, struct HttpRequest::Header *headers) {
        char *preliminaryKey, *preliminaryValue, *start = postPaddedBuffer;

        for (unsigned int i = 0; i < HttpRequest::MAX_HEADERS; i++) {
            for (preliminaryKey = postPaddedBuffer; (*postPaddedBuffer != ':') & (*postPaddedBuffer > 32); *(postPaddedBuffer++) |= 32);
            if (*postPaddedBuffer == '\r') {
                if ((postPaddedBuffer != end) & (postPaddedBuffer[1] == '\n') & (i > 0)) {
                    headers->key = std::string_view(nullptr, 0);
                    return (postPaddedBuffer + 2) - start;
                } else {
                    return 0;
                }
            } else {
                headers->key = std::string_view(preliminaryKey, (size_t) (postPaddedBuffer - preliminaryKey));
                for (postPaddedBuffer++; (*postPaddedBuffer == ':' || *postPaddedBuffer < 33) && *postPaddedBuffer != '\r'; postPaddedBuffer++);
                preliminaryValue = postPaddedBuffer;
                postPaddedBuffer = (char *) memchr(postPaddedBuffer, '\r', end - postPaddedBuffer);
                if (postPaddedBuffer && postPaddedBuffer[1] == '\n') {
                    headers->value = std::string_view(preliminaryValue, (size_t) (postPaddedBuffer - preliminaryValue));
                    postPaddedBuffer += 2;
                    headers++;
                } else {
                    return 0;
                }
            }
        }
        return 0;
    }

    // the only caller of getHeaders
    template <int CONSUME_MINIMALLY>
    int fenceAndConsumePostPadded(char *data, int length, void *user, HttpRequest *req, std::function<void(void *, HttpRequest *)> &requestHandler, std::function<void(void *, std::string_view)> &dataHandler) {
        int consumedTotal = 0;
        data[length] = '\r';

        for (int consumed; length && (consumed = getHeaders(data, data + length, req->headers)); ) {
            data += consumed;
            length -= consumed;
            consumedTotal += consumed;

            req->headers->value = std::string_view(req->headers->value.data(), std::max<int>(0, req->headers->value.length() - 9));

            requestHandler(user, req);

            std::string_view contentLengthString = req->getHeader("content-length");
            if (contentLengthString.length()) {
                remainingStreamingBytes = toUnsignedInteger(contentLengthString);

                if (!CONSUME_MINIMALLY) {
                    int emittable = std::min(remainingStreamingBytes, length);
                    dataHandler(user, std::string_view(data, emittable));
                    remainingStreamingBytes -= emittable;

                    data += emittable;
                    length -= emittable;
                    consumedTotal += emittable;
                }
            }

            if (CONSUME_MINIMALLY) {
                break;
            }
        }
        return consumedTotal;
    }

public:

    // todo: what can we do with the socket inside the handlers? we need to check on return from any handler if we closed or terminated or upgraded the socket
    void consumePostPadded(char *data, int length, void *user, std::function<void(void *, HttpRequest *)> &&requestHandler, std::function<void(void *, std::string_view)> &&dataHandler, std::function<void(void *)> &&errorHandler) {

        HttpRequest req;

        if (remainingStreamingBytes) {
            if (remainingStreamingBytes >= length) {
                dataHandler(user, std::string_view(data, length));
                remainingStreamingBytes -= length;
                return;
            } else {
                dataHandler(user, std::string_view(data, remainingStreamingBytes));

                data += remainingStreamingBytes;
                length -= remainingStreamingBytes;

                remainingStreamingBytes = 0;
            }
        } else if (fallback.length()) {
            int had = fallback.length();

            int maxCopyDistance = std::min(MAX_FALLBACK_SIZE - fallback.length(), (size_t) length);

            fallback.reserve(maxCopyDistance + 32); // padding should be same as libus
            fallback.append(data, maxCopyDistance);

            int consumed = fenceAndConsumePostPadded<true>(fallback.data(), fallback.length(), user, &req, requestHandler, dataHandler);
            if (consumed) {

                fallback.clear();

                data += consumed - had;
                length -= consumed - had;

                // this is exactly the same as above!
                if (remainingStreamingBytes) {
                    if (remainingStreamingBytes >= length) {
                        dataHandler(user, std::string_view(data, length));
                        remainingStreamingBytes -= length;
                        return;
                    } else {
                        dataHandler(user, std::string_view(data, remainingStreamingBytes));

                        data += remainingStreamingBytes;
                        length -= remainingStreamingBytes;

                        remainingStreamingBytes = 0;
                    }
                }

            } else {
                if (fallback.length() == MAX_FALLBACK_SIZE) {
                    errorHandler(user);
                }
                return;
            }
        }

        int consumed = fenceAndConsumePostPadded<false>(data, length, user, &req, requestHandler, dataHandler);

        data += consumed;
        length -= consumed;

        if (length) {
            if (length < MAX_FALLBACK_SIZE) {
                fallback.append(data, length);
            } else {
                errorHandler(user);
            }
        }
    }
};

#endif // HTTPPARSER_H
