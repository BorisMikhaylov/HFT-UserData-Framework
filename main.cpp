#include <iostream>
#include <string>
#include <memory>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <utility>
#include <map>
#include <thread>
#include <unistd.h>
#include <sstream>
#include <atomic>
#include <iomanip>
#include <ctime>
#include "fastsocket.h"

bool logEnabled = false;

struct TimeMeasurer {
    uint64_t currentTime;

    TimeMeasurer() {
        const auto p = std::chrono::system_clock::now();
        currentTime = std::chrono::duration_cast<std::chrono::microseconds>(
                p.time_since_epoch()).count();
    }

    void reset() {
        const auto p = std::chrono::system_clock::now();
        currentTime = std::chrono::duration_cast<std::chrono::microseconds>(
                p.time_since_epoch()).count();
    }

    uint64_t elapsedMicroSec() {
        const auto p = std::chrono::system_clock::now();
        uint64_t newTime = std::chrono::duration_cast<std::chrono::microseconds>(
                p.time_since_epoch()).count();
        return newTime - currentTime;
    }

    uint64_t elapsedMilliSec() {
        return elapsedMicroSec() / 1000;
    }
};

class SpinLock {
    const int UNLOCKED = 0;
    const int LOCKED = 1;

    std::atomic<int> m_value = 0;

public:
    void lock() {
        while (true) {
            int expected = UNLOCKED;
            if (m_value.compare_exchange_strong(expected, LOCKED))
                break;
        }
    }

    void unlock() {
        m_value.store(UNLOCKED);
    }
};

struct Mutex {
    SpinLock &spinLock;
    TimeMeasurer timeMeasurer;

    Mutex(SpinLock &spinLock) : spinLock(spinLock) {
        spinLock.lock();
    }

    virtual ~Mutex() {
        spinLock.unlock();
        if (logEnabled) std::cout << "Mutex ellapsed microsec:\t" << timeMeasurer.elapsedMicroSec() << std::endl;
    }
};

struct ThreadSync {
    static const int dataSize = 128;
    volatile uint64_t data[dataSize];
    volatile int count[dataSize];
    volatile int index;
    SpinLock locker;
    volatile bhft::socket_t socket[10];

    int getCount(uint64_t id) {
        for (int i = 0; i < dataSize; ++i) {
            if (data[i] == id) return count[i]++;
        }
        return 0;
    }

    void add(uint64_t id) {
        count[index] = 1;
        data[index++] = id;
        index %= dataSize;
    }
};

ThreadSync threadSync;

struct InputData {
    const char *begin[6];
    const char *end[6];
    int mask;

    void reset() {
        mask = 0;
    }

    uint64_t getId() {
        uint64_t id = 0;
        const char *ordBegin = begin[0];
        const char *ordEnd = end[0];
        while (ordBegin != ordEnd) {
            id *= 10;
            id += (*ordBegin++) - '0';
        }
        if (begin[4][1] == 'c') id *= 10;
        return id;
    }
};

struct InputDataSet {
    InputData *begin;
    InputData *end;

    InputDataSet(InputData *begin, InputData *anEnd) : begin(begin), end(anEnd) {}
};

namespace bparser {

    struct state;
    struct ObjectCallback;

    struct ArrayCallback {

        virtual ArrayCallback *willParseArray() = 0;

        virtual ObjectCallback *willParseObject() = 0;

        virtual void nextValue(const char *begin, const char *end) = 0;

        virtual void arrayFinished() = 0;
    };

    struct ObjectCallback {
        state *idMap;

        explicit ObjectCallback(state *idMap) : idMap(idMap) {}

        virtual void valueForField(int field_id, const char *begin, const char *end) = 0;

        virtual ObjectCallback *willParseObject(int field_id) = 0;

        virtual ArrayCallback *willParseArray(int field_id) = 0;

        virtual void objectFinished() = 0;
    };

    struct state {
        std::string name;
        bool isTerminal;
        int result;
        state *arr[256]{};

        state(
                std::string name,
                bool isTerminal,
                int result,
                state *defaultValue = nullptr)
                : name(std::move(name)),
                  isTerminal(
                          isTerminal),
                  result(result) {
            for (auto &a: arr) {
                a = defaultValue;
            }
        }
    };

    state *buildStateMachine(const char **ids, int count) {
        auto *unknownIdState = new state("unknownIdState", false, -1);
        auto *unknownIdFinalState = new state("unknownIdFinalState", true, -1);
        for (int i = 0; i < 256; ++i) {
            unknownIdState->arr[i] = (i == '"') ? unknownIdFinalState : unknownIdState;
        }
        auto startState = new state("startState", false, -1, unknownIdState);
        startState->arr['"'] = unknownIdFinalState;
        for (int i = 0; i < count; ++i) {
            const char *ptr = *ids++;
            auto current = startState;
            while (*ptr != 0) {
                if (current->arr[*ptr] == unknownIdState) {
                    auto newNode = new state(ptr, false, -1, unknownIdState);
                    current->arr[*ptr] = newNode;
                    newNode->arr['"'] = unknownIdFinalState;
                }
                current = current->arr[*ptr];
                ++ptr;
            }
            current->arr['"'] = new state("end of field", true, i);
        }
        return startState;
    }

    static state *unknownIds = buildStateMachine(nullptr, 0);

    struct logger {
        static bool enabled;
        char *file;
        int line;
        char *begin;
        char *current;
        char *end;

        logger(char *file, int line, char *begin, char *current, char *anEnd);


        void operator()(const char *format, ...) {
            if (!enabled) return;
            std::string str(begin, end);
            std::replace(str.begin(), str.end(), '\n', ' ');
            std::cout << str << "\n";
            std::cout << std::string(current - begin, ' ') << "^\n";

            char buf[1024];
            va_list args;
            va_start(args, format);
            vsprintf(buf, format, args);
            va_end(args);
            std::cout << "[" << file << ":" << line << "] " << buf << "\n";
        }

    };

    bool logger::enabled = false;

    logger::logger(char *file, int line, char *begin, char *current, char *anEnd) : file(file), line(line),
                                                                                    begin(begin), current(current),
                                                                                    end(anEnd) {}


//#define INLOG logger(__FILE__, __LINE__, begin, current, end)


    struct input {
        const char *begin;
        const char *current;
        const char *end;

        explicit input(bhft::Message &message) {
            begin = message.begin;
            current = message.begin;
            end = message.end;
        }

        void log(const std::string &message) {
            std::cout << message << ": " << *current;
        }

        static bool is_whitespace(char c) {
            return strchr(" \t\n\r", c) != nullptr;
        }

        static bool isEndOfValue(char c) {
            return strchr(", ]}\t\n\r", c) != nullptr;
        }

        int skipSpaces() {
            while (current != end && is_whitespace(*current)) {
                ++current;
            }
            return (current == end) ? -2 : 0;
        }

        int parseIdentifier(state *begin_state) {
            if (skipSpaces() == -2) {
                //INLOG("Wrong spaces");
                return -2;
            }
            if (*current++ != '"') {
                //INLOG("Not a \"");
                return -2;
            }
            //INLOG("Start parsing identifier \"");
            state *current_state = begin_state;
            while (current != end && !current_state->isTerminal) {
                current_state = current_state->arr[*current++];
            }
            if (current == end) {
                return -2;
            } else {
                return current_state->result;
            }
        }

        int parseSimpleValue() {
            if (skipSpaces() == -2) {
                //INLOG("Wrong spaces");
            }
            if (*current++ == '"') {
                bool prevIsSlash = false;
                while (current < end && (*current != '"' || prevIsSlash)) {
                    if (prevIsSlash) {
                        prevIsSlash = false;
                    } else if (*current == '\\') {
                        prevIsSlash = true;
                    }
                    ++current;
                }
                return current == end || *current++ != '"' ? -2 : 0;
            } else {
                while (current < end && !isEndOfValue(*current)) { ++current; }
                return current == end ? -2 : 0;
            }
        }

        [[maybe_unused]] int parseArray(ArrayCallback *arr) {
            if (skipSpaces() == -2) {
                //INLOG("Wrong spaces");
                return -2;
            }
            if (*current++ != '[') {
                //INLOG("Not array");
                return -2;
            }
            if (skipSpaces() == -2) {
                //INLOG("Wrong spaces");
                return -2;
            }
            if (*current == ']') {
                current++;
                return 0;
            }
            do {
                if (skipSpaces() == -2) {
                    //INLOG("Wrong spaces");
                    return -2;
                }
                if (*current == '{') {
                    if (parseObject(arr == nullptr ? nullptr : arr->willParseObject()) == -2) return -2;
                } else if (*current != '[') {
                    if (parseArray(arr == nullptr ? nullptr : arr->willParseArray()) == -2) return -2;
                } else {
                    const char *start = current;
                    if (parseSimpleValue() == -2) return -2;
                    const char *finish = current;
                    if (arr != nullptr) arr->nextValue(start, finish);
                }
                if (skipSpaces() == -2) {
                    //INLOG("Wrong spaces");
                    return -2;
                }
            } while (*current++ == ',');
            if (current[-1] != ']') {
                //INLOG("Not end of array");
                return -2;
            }
            if (arr != nullptr) arr->arrayFinished();
            return 0;
        }

        int parseObject(ObjectCallback *obj) {
            if (skipSpaces() == -2) {
                //INLOG("Wrong spaces");
                return -2;
            }
            if (*current++ != '{') {
                return -2;
            }
            if (skipSpaces() == -2) {
                //INLOG("Wrong spaces");
                return -2;
            }
            if (*current == '}') {
                ++current;
                return 0;
            }
            do {
                if (skipSpaces() == -2) {
                    //INLOG("Wrong spaces");
                    return -2;
                }
                int id = parseIdentifier(obj == nullptr ? unknownIds : obj->idMap);
                if (skipSpaces() == -2) {
                    //INLOG("Wrong spaces");
                    return -2;
                }
                if (*current++ != ':') return -2;
                if (skipSpaces() == -2) {
                    //INLOG("Wrong spaces");
                    return -2;
                }
                if (*current == '{') {
                    if (parseObject(obj == nullptr ? nullptr : obj->willParseObject(id)) == -2) return -2;
                } else if (*current == '[') {
                    if (parseArray(obj == nullptr ? nullptr : obj->willParseArray(id)) == -2) return -2;
                } else {
                    const char *start = current;
                    if (parseSimpleValue() == -2) return -2;
                    const char *finish = current;
                    if (obj != nullptr) obj->valueForField(id, start, finish);
                }
                if (skipSpaces() == -2) {
                    //INLOG("Wrong spaces");
                    return -2;
                }
            } while (*current++ == ',');
            if (current[-1] != '}') return -2;
            if (obj != nullptr) obj->objectFinished();
            return 0;
        }
    };
}

using namespace bparser;

void checkSimpleValue(bhft::Message &message, int result, int pos) {
    input in(message);
    int res = in.parseSimpleValue();
    std::cout << message.begin << ((result != res || in.begin + pos != in.current) ? ": error" : ": success")
              << "\n";

}

//void test_simple_value() {
//    checkSimpleValue(R"("asd",)", 0, 5);
//    checkSimpleValue(R"("as\td"])", 0, 7);
//    checkSimpleValue(R"("as\"d"])", 0, 7);
//    checkSimpleValue(R"("as\"d])", -2, 7);
//    checkSimpleValue(R"(as\td])", 0, 5);
//    checkSimpleValue(R"(as\td))", -2, 6);
//    checkSimpleValue(R"(astd\)])", 0, 6);
//}

void testIdentifier() {
    const char *ids[]{
            "id", "29835", "lqknlenq", "e34e5r6t7yuijkj"
    };
    state *startState = buildStateMachine(ids, 4);
    bhft::Message message((char *) R"(
 "lqknlenq"  )");
    input in(message);
    std::cout << in.parseIdentifier(startState);

}

static const char *dataObjectId[] = {"ordId",
                                     "side",
                                     "px",
                                     "sz",
                                     "state",
                                     "uTime"};
static const char *outputObjectId[] = {"\"orderId\"",
                                       "\"side\"",
                                       "\"price\"",
                                       "\"volume\"",
                                       "\"state\"",
                                       "\"uTime\""};

static state *dataObjectIdMap = buildStateMachine(dataObjectId, 6);

struct DataObjectCallback : ObjectCallback {
    bhft::WebSocket *ws;
    InputDataSet &inputDataSet;
    InputData *currentInput;

    explicit DataObjectCallback(bhft::WebSocket *ws, InputDataSet &inputDataSet) : ws(ws), inputDataSet(inputDataSet),
                                                                                   ObjectCallback(dataObjectIdMap),
                                                                                   currentInput(inputDataSet.end) {
        currentInput->reset();
    }

    uint64_t ctol(const char *str_begin, const char *str_end) {
        uint64_t ans = 0;
        ++str_begin;
        --str_end;
        while (str_begin != str_end) {
            ans *= 10;
            ans += (*str_begin++) - '0';
        }
        return ans;
    }

    void valueForField(int fieldId, const char *begin, const char *end) override {
        if (fieldId < 0) return;
        if (fieldId == 0) {
            ++begin;
            --end;
        }
        currentInput->begin[fieldId] = begin;
        currentInput->end[fieldId] = end;
        currentInput->mask |= 1 << fieldId;
    }

    ObjectCallback *willParseObject(int field_id) override {
        return nullptr;
    }

    ArrayCallback *willParseArray(int field_id) override {
        return nullptr;
    }

    void objectFinished() override {
        if (currentInput->mask != 0) { // TODO check all required fields
            currentInput = ++inputDataSet.end;
        }
        currentInput->reset();
    }
};


struct DataArrayCallback : ArrayCallback {
    DataObjectCallback dataObjectCallback;

    explicit DataArrayCallback(bhft::WebSocket *ws, InputDataSet &inputDataSet) : dataObjectCallback(ws,
                                                                                                     inputDataSet) {}

    ArrayCallback *willParseArray() override {
        return nullptr;
    }

    ObjectCallback *willParseObject() override {
        return &dataObjectCallback;
    }

    void nextValue(const char *begin, const char *end) override {

    }

    void arrayFinished() override {

    }
};

static const char *quoteObjectId[] = {"data"};
static state *quoteObjectIdMap = buildStateMachine(quoteObjectId, 1);

struct QuoteObjectCallback : ObjectCallback {
    DataArrayCallback dataArrayCallback;

    explicit QuoteObjectCallback(bhft::WebSocket *ws, InputDataSet &inputDataSet) : dataArrayCallback(ws, inputDataSet),
                                                                                    ObjectCallback(quoteObjectIdMap) {}

    void valueForField(int field_id, const char *begin, const char *end) override {
    }

    ObjectCallback *willParseObject(int fieldId) override {
        return nullptr;
    }

    ArrayCallback *willParseArray(int field_id) override {
        return field_id == 0 ? &dataArrayCallback : nullptr;
    }

    void objectFinished() override {

    }
};


static char buffer[10000000];

uint64_t getDelay(bhft::WebSocket &ws) {
    const int iterations = 100;
    uint64_t delay = 0;
    for (int i = 0; i < iterations; ++i) {
        auto pingMessage = ws.getOutputMessage();
        bhft::Message pongMessage(buffer);
        pingMessage.write("ewe");
        TimeMeasurer measurer;
        ws.sendLastOutputMessage(bhft::wsheader_type::PING);
        ws.getMessage(pongMessage, true);
        delay += measurer.elapsedMicroSec();
        //std::cout << "PING" << "\t" << timestampPong - timestampPing << std::endl;
    }
    return delay;
}

std::string getTimeAsString() {
    auto t = std::time(nullptr);
    auto tm = *std::localtime(&t);
    std::stringstream str;
    str << std::put_time(&tm, "%d-%m-%Y %H:%M:%S");
    return str.str();
}

struct HFTSocket {

    bhft::WebSocket ws;
    char buffer[4096];
    int id;

    explicit HFTSocket(int id, bool waitOnSocket) : ws("127.0.0.1", 9999, "?url=wss://ws.okx.com:8443/ws/v5/private",
                                                       true, waitOnSocket), id(id) {}

    bhft::status login() {
        bhft::OutputMessage &message = ws.getOutputMessage();
        const auto p1 = std::chrono::system_clock::now();
        int timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                p1.time_since_epoch()).count();

        sprintf(buffer,
                R"({"op":"login","args":[{"apiKey":"xNEkpMtgh6lF7v8K","passphrase":"","timestamp":%i,"sign":"SkAjqP4LC9UexmrX"}]})",
                timestamp);
        message.write(buffer);
        if (ws.sendLastOutputMessage(bhft::wsheader_type::TEXT_FRAME) == bhft::closed) {
            return bhft::closed;
        }
        bhft::Message inMessage1(buffer);
        if (ws.getMessage(inMessage1) == bhft::closed) {
            return bhft::closed;
        }
        auto start = std::chrono::system_clock::now();
        std::cout << id << "\tLogin:\t" << getTimeAsString() << "\t" << buffer << std::endl;
        return bhft::success;
    }

    bhft::status subscribe(std::string &subscribeMessage) {
        bhft::OutputMessage &message2 = ws.getOutputMessage();
        message2.write(subscribeMessage.c_str());
        if (ws.sendLastOutputMessage(bhft::wsheader_type::TEXT_FRAME) == bhft::closed) {
            return bhft::closed;
        }
        bhft::Message inMessage2(buffer);
        if (ws.getMessage(inMessage2) == bhft::closed) {
            return bhft::closed;
        }
        std::cout << id << "\tSubscribe:\t" << getTimeAsString() << "\t" << buffer << std::endl;
        return bhft::success;
    }

    bhft::status readMessage(InputDataSet &inputDataSet, bool returnOnNoData = false) {
        while (true) {
            bhft::Message inMessage(buffer + 1);
            auto stat = ws.getMessage(inMessage, returnOnNoData);
            if (stat != bhft::success) return stat;

            if (logEnabled) {
                struct timeval now;
                uint64_t timestamp;
                gettimeofday(&now, NULL);
                timestamp = (uint64_t) now.tv_sec * 1000000 + (uint64_t) now.tv_usec;
                std::stringstream str;
                str << id << "\t" << timestamp << "\tArrived: " << buffer + 1 << std::endl;
                std::cout << str.str();
            }
            if (inMessage.begin == inMessage.end) continue;
            if (*inMessage.begin != '{') *--inMessage.begin = '{';
            if (inMessage.end[-1] != '}') *inMessage.end++ = '}';
            QuoteObjectCallback quoteObjectCallback(&ws, inputDataSet);
            input in(inMessage);
            if (in.parseObject(&quoteObjectCallback) == -2) continue;
            return bhft::success;
        }
    }

    bhft::status writeMessage(const InputData &input) {
        auto &out = ws.getOutputMessage();
        char prefix = '{';
        for (int i = 0; i < 6; ++i) {
            if ((input.mask >> i) & 1) {
                out.write(prefix);
                prefix = ',';
                out.write(outputObjectId[i]);
                out.write(':');
                out.write(input.begin[i], input.end[i]);
            }
        }
        out.write(R"(,"apiKey":"xNEkpMtgh6lF7v8K","sign":"SkAjqP4LC9UexmrX"})");
        if (ws.sendLastOutputMessage(bhft::wsheader_type::TEXT_FRAME) == bhft::closed) {
            return bhft::closed;
        }
        if (logEnabled) {
            std::stringstream str;
            str << id << "\tSending message:\t" << std::string(input.begin[0], input.end[0]) << std::endl;
            std::cout << str.str();
        }
        return bhft::success;
    }
};

struct ReportOnExit {
    const char *message;
    int id;

    explicit ReportOnExit(const char *message, int id) : message(message), id(id) {}

    virtual ~ReportOnExit() {
        std::cout << id << "\t" << message;
    }

    void setMessage(const char *newMessage) {
        message = newMessage;
    }
};

void process(int threadId, int id, std::string &subscribeMessage, bool waitOnSocket, int maxFine) {
    ReportOnExit reporter("Closed by server\n", id);
    HFTSocket hftSocket(id, waitOnSocket);
    threadSync.socket[threadId] = hftSocket.ws.socket.socket;
    if (hftSocket.login() == bhft::closed) return;
    if (hftSocket.subscribe(subscribeMessage) == bhft::closed) return;
    InputData inputData[10];
    int fine = 0;
    while (true) {
        if (fine > maxFine) {
            return;
        }
        InputDataSet inputDataSet(inputData, inputData);
        TimeMeasurer timeMeasurer;
        auto stat = hftSocket.readMessage(inputDataSet, true);
        if (logEnabled) {
            auto ellapsed = timeMeasurer.elapsedMicroSec();
            if (ellapsed > 10000000) std::cout << "BIGBIGBIGBIGBIGBIG" << std::endl;
            else if (ellapsed > 10000) std::cout << "BIG" << std::endl;
            std::cout << "Read message ellapsed microsec:\t" << ellapsed << std::endl;
        }
        if (stat == bhft::closed) return;
        for (auto input = inputDataSet.begin; input != inputDataSet.end; ++input) {
            uint64_t inputId = input->getId();
            Mutex mutex(threadSync.locker);
            int cnt = threadSync.getCount(inputId);
            if (cnt > 0) {
                fine += (1 << (cnt - 1)) - 1;
                continue;
            }
            if (hftSocket.writeMessage(*input) == bhft::closed) {
                return;
            }
            threadSync.add(inputId);
        }
    }
}

void processLoop(int id, std::string &subscribeMessage, bool waitOnSocket, int maxFine) {
    int counter = (id + 1) * 10000;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(rand() % 1000));
        process(id, counter++, subscribeMessage, waitOnSocket, maxFine);
    }
}


int main(int argc, char **argv) {

    std::map<std::string, std::string> map;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        int index;
        if (!(index = arg.find('='))) return -1;
        map[arg.substr(0, index)] = arg.substr(index + 1);
    }


    uint64_t bestDelay = 10000000;
    if (map["log"] == "true") logEnabled = true;
    std::string channel = (map.find("channel") != map.end()) ? map["channel"] : "orders";
    std::string instType = (map.find("instType") != map.end()) ? map["instType"] : "ANY";
    std::string instId = (map.find("instId") != map.end()) ? map["instId"] : "";
    std::string instIdStr = (instId.empty()) ? "" : R"(,"instId":")" + instId + R"(")";
    int loginUpperBound = (map.find("loginLimit") == map.end()) ? 20000 : stoi(map["loginLimit"]);
    int logLevel = (map.find("logLevel") == map.end()) ? 1 : stoi(map["logLevel"]);
    bool waitOnSocket = map["wait"] == "true";

    std::string subscribeMessage =
            R"({"op":"subscribe","args":[{"channel":")" + channel + R"(","instType":")" + instType + R"(")" +
            instIdStr +
            R"(}]})";
    std::cout << "Subscribe message: \t" << subscribeMessage << std::endl;

    std::vector<std::thread> threads;
    for (int i = 0; i < logLevel; ++i) {
        sleep((rand() % 1000) / 100.0);
        threads.push_back(std::thread([&subscribeMessage, i, waitOnSocket]() {
            processLoop(i, subscribeMessage, waitOnSocket, i < 2 ? 1000000 : 500);
        }));
    }
    int i = 0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20000 + rand() % 10000));
        bhft::socket_t socket = threadSync.socket[(i++) % 2];
        closesocket(socket);
    }
    for (int i = 0; i < logLevel; ++i) {
        threads[i].join();
    }

}