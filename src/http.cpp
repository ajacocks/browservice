#include "http.hpp"

#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPRequestHandler.h>

class HTTPRequest::Impl {
public:
    Impl(
        Poco::Net::HTTPServerRequest& request,
        promise<function<void(Poco::Net::HTTPServerResponse&)>> responderPromise
    )
        : request_(request),
          responderPromise_(move(responderPromise)),
          responseSent_(false)
    {}
    ~Impl() {
        if(!responseSent_) {
            LOG(WARNING) << "HTTP response not provided, sending internal server error";
            sendTextResponse(
                500,
                "ERROR: Request handling failure\n",
                true
            );
        }
    }

    string method() const {
        CHECK(!responseSent_);
        return request_.getMethod();
    }
    string path() const {
        CHECK(!responseSent_);
        return request_.getURI();
    }

    void sendResponse(
        int status,
        string contentType,
        uint64_t contentLength,
        function<void(ostream&)> body,
        bool noCache
    ) {
        CHECK(!responseSent_);
        responseSent_ = true;
        responderPromise_.set_value(
            [
                status,
                contentType{move(contentType)},
                contentLength,
                body,
                noCache
            ](Poco::Net::HTTPServerResponse& response) {
                response.add("Content-Type", contentType);
                response.setContentLength64(contentLength);
                if(noCache) {
                    response.add("Cache-Control", "no-cache, no-store, must-revalidate");
                    response.add("Pragma", "no-cache");
                    response.add("Expires", "0");
                }
                body(response.send());
            }
        );
    }

    void sendTextResponse(int status, string text, bool noCache) {
        uint64_t contentLength = text.size();
        sendResponse(
            status,
            "text/plain; charset=UTF-8",
            contentLength,
            [text{move(text)}](ostream& out) {
                out << text;
            },
            noCache
        );
    }

private:
    Poco::Net::HTTPServerRequest& request_;
    promise<function<void(Poco::Net::HTTPServerResponse&)>> responderPromise_;
    bool responseSent_;
};

namespace http_ {

class HTTPRequestHandler : public Poco::Net::HTTPRequestHandler {
public:
    HTTPRequestHandler(weak_ptr<HTTPServerEventHandler> eventHandler)
        : eventHandler_(eventHandler)
    {}

    virtual void handleRequest(
        Poco::Net::HTTPServerRequest& request,
        Poco::Net::HTTPServerResponse& response
    ) override {
        promise<function<void(Poco::Net::HTTPServerResponse&)>> responderPromise;
        future<function<void(Poco::Net::HTTPServerResponse&)>> responderFuture =
            responderPromise.get_future();
        
        {
            shared_ptr<HTTPRequest> reqObj = HTTPRequest::create(
                make_unique<HTTPRequest::Impl>(request, move(responderPromise))
            );
            postTask(
                eventHandler_,
                &HTTPServerEventHandler::onHTTPServerRequest,
                reqObj
            );
        }

        std::function<void(Poco::Net::HTTPServerResponse&)> responder = responderFuture.get();
        responder(response);
    }

private:
    weak_ptr<HTTPServerEventHandler> eventHandler_;
};

class HTTPRequestHandlerFactory : public Poco::Net::HTTPRequestHandlerFactory {
public:
    HTTPRequestHandlerFactory(weak_ptr<HTTPServerEventHandler> eventHandler)
        : eventHandler_(eventHandler)
    {}

    virtual Poco::Net::HTTPRequestHandler* createRequestHandler(
        const Poco::Net::HTTPServerRequest& request
    ) override {
        return new HTTPRequestHandler(eventHandler_);
    }

private:
    weak_ptr<HTTPServerEventHandler> eventHandler_;
};

}
namespace { using namespace http_; }

HTTPRequest::HTTPRequest(CKey, unique_ptr<Impl> impl)
    : impl_(move(impl))
{}

string HTTPRequest::method() const {
    CEF_REQUIRE_UI_THREAD();
    return impl_->method();
}

string HTTPRequest::path() const {
    CEF_REQUIRE_UI_THREAD();
    return impl_->path();
}

void HTTPRequest::sendResponse(
    int status,
    string contentType,
    uint64_t contentLength,
    function<void(ostream&)> body,
    bool noCache
) {
    CEF_REQUIRE_UI_THREAD();
    impl_->sendResponse(status, contentType, contentLength, body, noCache);
}

void HTTPRequest::sendTextResponse(int status, string text, bool noCache) {
    CEF_REQUIRE_UI_THREAD();
    impl_->sendTextResponse(status, move(text), noCache);
}

class HTTPServer::Impl : public enable_shared_from_this<Impl> {
SHARED_ONLY_CLASS(Impl);
public:
    Impl(CKey,
        weak_ptr<HTTPServerEventHandler> eventHandler,
        const std::string& listenSockAddr
    )
        : eventHandler_(eventHandler),
          socketAddress_(listenSockAddr),
          serverSocket_(socketAddress_),
          httpServer_(
              new HTTPRequestHandlerFactory(eventHandler),
              threadPool_,
              serverSocket_,
              new Poco::Net::HTTPServerParams()
          ),
          shutdownStarted_(false),
          shutdownComplete_(false)
    {
        LOG(INFO) << "HTTP server listening to " << listenSockAddr;
        httpServer_.start();
    }
    ~Impl() {
        CHECK(shutdownComplete_);
    }

    void shutdown() {
        CEF_REQUIRE_UI_THREAD();
        if(shutdownStarted_) {
            return;
        }
        shutdownStarted_ = true;
        shared_ptr<Impl> self = shared_from_this();
        thread stopThread([self{move(self)}]() {
            self->httpServer_.stopAll(false);
            postTask([self{move(self)}]() {
                self->shutdownComplete_ = true;
                if(auto eventHandler = self->eventHandler_.lock()) {
                    eventHandler->onHTTPServerShutdownComplete();
                }
            });
        });
        stopThread.detach();
    }

    bool isShutdownComplete() {
        CEF_REQUIRE_UI_THREAD();
        return shutdownComplete_;
    }

private:
    weak_ptr<HTTPServerEventHandler> eventHandler_;
    Poco::ThreadPool threadPool_;
    Poco::Net::SocketAddress socketAddress_;
    Poco::Net::ServerSocket serverSocket_;
    Poco::Net::HTTPServer httpServer_;
    bool shutdownStarted_;
    bool shutdownComplete_;
};

HTTPServer::HTTPServer(CKey,
    weak_ptr<HTTPServerEventHandler> eventHandler,
    const std::string& listenSockAddr
) {
    CEF_REQUIRE_UI_THREAD();
    impl_ = Impl::create(eventHandler, listenSockAddr);
}

HTTPServer::~HTTPServer() {
    postTask(impl_, &Impl::shutdown);
}

void HTTPServer::shutdown() {
    impl_->shutdown();
}

bool HTTPServer::isShutdownComplete() {
    return impl_->isShutdownComplete();
}
