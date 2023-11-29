#include "easywsclient.hpp"
#include <iostream>
#include <string>
#include <memory>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <utility>
#include "fastsocket.h"

bool bparser_log = false;

struct InputData {
    char *begin[6];
    char *end[6];
    int mask;

    void reset() {
        mask = 0;
    }
};

static InputData inputData[1000];

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

        virtual void nextValue(char *begin, char *end) = 0;

        virtual void arrayFinished() = 0;
    };

    struct ObjectCallback {
        state *idMap;

        explicit ObjectCallback(state *idMap) : idMap(idMap) {}

        virtual void valueForField(int field_id, char *begin, char *end) = 0;

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

    state *buildStateMachine(char **ids, int count) {
        auto *unknownIdState = new state("unknownIdState", false, -1);
        auto *unknownIdFinalState = new state("unknownIdFinalState", true, -1);
        for (int i = 0; i < 256; ++i) {
            unknownIdState->arr[i] = (i == '"') ? unknownIdFinalState : unknownIdState;
        }
        auto startState = new state("startState", false, -1, unknownIdState);
        startState->arr['"'] = unknownIdFinalState;
        for (int i = 0; i < count; ++i) {
            char *ptr = *ids++;
            auto current = startState;
            while (*ptr != 0) {
                if (current->arr[*ptr] == unknownIdState) {
                    auto newNode = new state(ptr, false, -1, unknownIdState);
                    current->arr[*ptr] = newNode;
                    newNode->arr['"'] = unknownIdFinalState;
                }
                current = current->arr[*ptr];
                ptr++;
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


#define INLOG logger(__FILE__, __LINE__, begin, current, end)


    struct input {
        char *begin;
        char *current;
        char *end;

        explicit input(char *str) {
            begin = str;
            current = str;
            end = str + strlen(str);
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
                current++;
            }
            return (current == end) ? -2 : 0;
        }

        int parseIdentifier(state *begin_state) {
            if (skipSpaces() == -2) {
                INLOG("Wrong spaces");
                return -2;
            }
            if (*current++ != '"') {
                INLOG("Not a \"");
                return -2;
            }
            INLOG("Start parsing identifier \"");
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
                INLOG("Wrong spaces");
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
                INLOG("Wrong spaces");
                return -2;
            }
            if (*current++ != '[') {
                INLOG("Not array");
                return -2;
            }
            if (skipSpaces() == -2) {
                INLOG("Wrong spaces");
                return -2;
            }
            if (*current == ']') {
                current++;
                return 0;
            }
            do {
                if (skipSpaces() == -2) {
                    INLOG("Wrong spaces");
                    return -2;
                }
                if (*current == '{') {
                    if (parseObject(arr == nullptr ? nullptr : arr->willParseObject()) == -2) return -2;
                } else if (*current != '[') {
                    if (parseArray(arr == nullptr ? nullptr : arr->willParseArray()) == -2) return -2;
                } else {
                    char *start = current;
                    if (parseSimpleValue() == -2) return -2;
                    char *finish = current;
                    if (arr != nullptr) arr->nextValue(start, finish);
                }
                if (skipSpaces() == -2) {
                    INLOG("Wrong spaces");
                    return -2;
                }
            } while (*current++ == ',');
            if (current[-1] != ']') {
                INLOG("Not end of array");
                return -2;
            }
            if (arr != nullptr) arr->arrayFinished();
            return 0;
        }

        int parseObject(ObjectCallback *obj) {
            if (skipSpaces() == -2) {
                INLOG("Wrong spaces");
                return -2;
            }
            if (*current++ != '{') {
                return -2;
            }
            if (skipSpaces() == -2) {
                INLOG("Wrong spaces");
                return -2;
            }
            if (*current == '}') {
                current++;
                return 0;
            }
            do {
                if (skipSpaces() == -2) {
                    INLOG("Wrong spaces");
                    return -2;
                }
                int id = parseIdentifier(obj == nullptr ? unknownIds : obj->idMap);
                if (skipSpaces() == -2) {
                    INLOG("Wrong spaces");
                    return -2;
                }
                if (*current++ != ':') return -2;
                if (skipSpaces() == -2) {
                    INLOG("Wrong spaces");
                    return -2;
                }
                if (*current == '{') {
                    if (parseObject(obj == nullptr ? nullptr : obj->willParseObject(id)) == -2) return -2;
                } else if (*current == '[') {
                    if (parseArray(obj == nullptr ? nullptr : obj->willParseArray(id)) == -2) return -2;
                } else {
                    char *start = current;
                    if (parseSimpleValue() == -2) return -2;
                    char *finish = current;
                    if (obj != nullptr) obj->valueForField(id, start, finish);
                }
                if (skipSpaces() == -2) {
                    INLOG("Wrong spaces");
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

void checkSimpleValue(char *str, int result, int pos) {
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
    char *str = R"(
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

struct DataObjectCallback : ObjectCallback {
    bhft::WebSocket *ws;
    InputDataSet &inputDataSet;
    InputData *currentInput;

    explicit DataObjectCallback(bhft::WebSocket *ws, InputDataSet &inputDataSet) : ws(ws), inputDataSet(inputDataSet),
                                                                                   ObjectCallback(dataObjectIdMap),
                                                                                   currentInput(inputDataSet.end) {
        currentInput->reset();
    }

    void valueForField(int fieldId, char *begin, char *end) override {
        if (fieldId < 0) return;
        if (fieldId == 0) {
            begin++;
            end--;
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

    void nextValue(char *begin, char *end) override {

    }

    void arrayFinished() override {

    }
};

static char *quoteObjectId[] = {"data"};
static state *quoteObjectIdMap = buildStateMachine(quoteObjectId, 1);

struct QuoteObjectCallback : ObjectCallback {
    DataArrayCallback dataArrayCallback;

    explicit QuoteObjectCallback(bhft::WebSocket *ws, InputDataSet &inputDataSet) : dataArrayCallback(ws, inputDataSet),
                                                                                    ObjectCallback(quoteObjectIdMap) {}

    void valueForField(int field_id, char *begin, char *end) override {
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

void parseQuote(bhft::WebSocket *ws, char *message) {
    InputDataSet inputDataSet(inputData, inputData);
    QuoteObjectCallback quoteObjectCallback(ws, inputDataSet);
    input in(message);
    if (in.parseObject(&quoteObjectCallback) == -2) return;
    for (auto input = inputDataSet.begin; input != inputDataSet.end; ++input) {
        auto &out = ws->getOutputMessage();
        char prefix = '{';
        for (int i = 0; i < 6; ++i) {
            if ((input->mask >> i) & 1){
                out.write(prefix);
                prefix = ',';
                out.write(outputObjectId[i]);
                out.write(':');
                out.write(input->begin[i], input->end[i]);
            }
        }
        out.write(R"(,"apiKey":"xNEkpMtgh6lF7v8K","sign":"SkAjqP4LC9UexmrX"})");
        ws->sendLastOutputMessage(bhft::wsheader_type::TEXT_FRAME);
    }
}

static char buffer[10000000];

int main(int argc, char **argv) {
    if (argc > 1) bparser_log = true;
    while (true) {
        std::cout << "Connection starting..." << std::endl << std::endl;
        bhft::WebSocket ws("127.0.0.1", 9999, "?url=wss://ws.okx.com:8443/ws/v5/private", true);
        //bhft::WebSocket ws("127.0.0.1", 8080, "", true);
        bhft::OutputMessage &message = ws.getOutputMessage();
        const auto p1 = std::chrono::system_clock::now();
        int timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                p1.time_since_epoch()).count();

        sprintf(buffer,
                R"({"op":"login","args":[{"apiKey":"xNEkpMtgh6lF7v8K","passphrase":"","timestamp":%i,"sign":"SkAjqP4LC9UexmrX"}]})",
                timestamp);
        message.write(buffer);
        ws.sendLastOutputMessage(bhft::wsheader_type::TEXT_FRAME);
        ws.getMessage(buffer);
        std::cout << "Login: " << buffer << std::endl;

        bhft::OutputMessage &message2 = ws.getOutputMessage();
        message2.write(R"({"op":"subscribe","args":[{"channel":"orders","instType":"ANY"}]})");
        ws.sendLastOutputMessage(bhft::wsheader_type::TEXT_FRAME);
        ws.getMessage(buffer);
        std::cout << "Subscribe: " << buffer << std::endl;

        std::cout << "Connection started" << std::endl << std::endl;
        while (!ws.isClosed()) {
            ws.getMessage(buffer);
            if (bparser_log) std::cout << "Arrived: " << buffer << std::endl << "";
            parseQuote(&ws, buffer);
        }
    }
}