#include "easywsclient.hpp"
#include <iostream>
#include <string>
#include <memory>
#include <deque>
#include <chrono>
#include <atomic>
#include <utility>
#include "bparser.cpp"
#include <stdio.h>

struct JsonOut {
    char buffer[10000]{};
    char *current;
    char prefix = '{';

    JsonOut() {
        current = buffer;
    }

    void addField(const char *name, const char *valueBegin, const char *valueEnd) {
        *current++ = prefix;
        prefix = ',';
        while (*name != 0) {
            *current++ = *name++;
        }
        *current++ = ':';
        while (valueBegin < valueEnd) {
            *current++ = *valueBegin++;
        }
    }

    void addField(const char *name, const char *value) {
        *current++ = prefix;
        prefix = ',';
        while (*name != 0) {
            *current++ = *name++;
        }
        *current++ = ':';
        while (*value != 0) {
            *current++ = *value++;
        }
    }

    void reset() {
        current = buffer;
        prefix = '{';
    }

    char *getStr() {
        return buffer;
    }

    void finishObject() {
        *current++ = '}';
        *current = 0;
    }
};


using namespace bparser;

void checkSimpleValue(std::string str, int result, int pos) {
    input in(str);
    int res = in.parseSimpleValue();
    std::cout << str << ((result != res || in.begin + pos != in.current) ? ": error" : ": success")
              << "\n";

}

void test_simple_value() {
    checkSimpleValue(R"("asd",)", 0, 5);
    checkSimpleValue(R"("as\td"])", 0, 7);
    checkSimpleValue(R"("as\"d"])", 0, 7);
    checkSimpleValue(R"("as\"d])", -2, 7);
    checkSimpleValue(R"(as\td])", 0, 5);
    checkSimpleValue(R"(as\td))", -2, 6);
    checkSimpleValue(R"(astd\)])", 0, 6);
}

void testIdentifier() {
    char *ids[]{
            "id", "29835", "lqknlenq", "e34e5r6t7yuijkj"
    };
    state *startState = buildStateMachine(ids, 4);
    std::string str = R"(
 "lqknlenq"  )";
    input in(str);
    std::cout << in.parseIdentifier(startState);

}

static char *dataObjectId[] = {"ordId",
                               "side",
                               "px",
                               "sz",
                               "state",
                               "uTime"};
static char *outputObjectId[] = {"\"orderId\"",
                                 "\"side\"",
                                 "\"price\"",
                                 "\"volume\"",
                                 "\"state\"",
                                 "\"uTime\""};

static state *dataObjectIdMap = buildStateMachine(dataObjectId, 6);

JsonOut jsonOutObject;

struct dataObjectCallback : objectCallback {
    easywsclient::WebSocket *ws;
    JsonOut &out;

    explicit dataObjectCallback(easywsclient::WebSocket *ws, JsonOut &out) : ws(ws), out(out),
                                                                             objectCallback(dataObjectIdMap) {}

    void valueForField(int fieldId, char *begin, char *end) override {
        if (fieldId < 0) return;
        if (fieldId == 0) {
            begin++;
            end--;
        }
        out.addField(outputObjectId[fieldId], begin, end);
    }

    objectCallback *willParseObject(int field_id) override {
        return nullptr;
    }

    arrayCallback *willParseArray(int field_id) override {
        return nullptr;
    }

    void objectFinished() override {
        out.addField("\"apiKey\"", "\"xNEkpMtgh6lF7v8K\"");
        out.addField("\"sign\"", "\"SkAjqP4LC9UexmrX\"");
        out.finishObject();
        ws->send(out.getStr());
        std::cout << "Sending: " << out.getStr() << std::endl << std::endl;
        out.reset();
    }
};


struct data_array_callback : arrayCallback {
    dataObjectCallback dataObjectCallback;

    explicit data_array_callback(easywsclient::WebSocket *ws, JsonOut &out) : dataObjectCallback(ws, out) {}

    arrayCallback *willParseArray() override {
        return nullptr;
    }

    objectCallback *willParseObject() override {
        return &dataObjectCallback;
    }

    void nextValue(char *begin, char *end) override {

    }

    void arrayFinished() override {

    }
};

static char *quoteObjectId[] = {"data"};
static state *quoteObjectIdMap = buildStateMachine(quoteObjectId, 1);

struct quoteObjectCallback : objectCallback {
    data_array_callback dataArrayCallback;

    explicit quoteObjectCallback(easywsclient::WebSocket *ws, JsonOut &out) : dataArrayCallback(ws, out),
                                                                              objectCallback(quoteObjectIdMap) {}

    void valueForField(int field_id, char *begin, char *end) override {
    }

    objectCallback *willParseObject(int fieldId) override {
        return nullptr;
    }

    arrayCallback *willParseArray(int field_id) override {
        return field_id == 0 ? &dataArrayCallback : nullptr;
    }

    void objectFinished() override {

    }
};

void parseQuote(easywsclient::WebSocket *ws) {
    std::string result;
    while (result.empty()) {
        ws->poll();
        ws->dispatch([&result](const std::string &message) {
            result = message;
        });
    }
    std::cout << "Read message: " << result << std::endl;
    jsonOutObject.reset();
    quoteObjectCallback quoteObjectCallback(ws, jsonOutObject);
    input in(result);
    in.parseObject(&quoteObjectCallback);
}

std::string readMessage(easywsclient::WebSocket *ws) {
    std::string result;
    while (result.empty()) {
        ws->poll();
        ws->dispatch([&result](const std::string &message) {
            result = message;
        });
    }
    std::cout << "Read message: " << result << std::endl;
    return result;
}

int main() {
    auto ws = easywsclient::WebSocket::from_url("ws://127.0.0.1:9999/?url=wss://ws.okx.com:8443/ws/v5/private");

    const auto p1 = std::chrono::system_clock::now();
    int timestamp = std::chrono::duration_cast<std::chrono::seconds>(
            p1.time_since_epoch()).count();

    char loginBuffer[1000];
    sprintf(loginBuffer,
            R"({"op":"login","args":[{"apiKey":"xNEkpMtgh6lF7v8K","passphrase":"","timestamp":%i,"sign":"SkAjqP4LC9UexmrX"}]})",
            timestamp);
    std::cout << loginBuffer << std::endl;
    std::string loginMessage = loginBuffer;
    ws->send(loginMessage);

    auto message = readMessage(ws);
    std::cout << "Received message: " << message << std::endl;
    if (message != "Login OK") {
        std::cout << "Login failed";
        exit(1);
    }

    ws->send(R"({"op":"subscribe","args":[{"channel":"orders","instType":"ANY"}]})");
    message = readMessage(ws);

    std::cout << "Received message: " << message << std::endl;
    if (message != "Subscribe OK") {
        std::cout << "Subscribe failed";
        exit(1);
    }

    while (true) {
        parseQuote(ws, readMessage(ws));
    }
    ws->close();
    delete ws;
}