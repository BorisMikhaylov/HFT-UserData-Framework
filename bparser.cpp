#include <iostream>
#include <string>
#include <stdio.h>

namespace bparser {

    struct state;
    struct objectCallback;

    struct arrayCallback {

        virtual arrayCallback *willParseArray() = 0;

        virtual objectCallback *willParseObject() = 0;

        virtual void nextValue(char *begin, char *end) = 0;

        virtual void arrayFinished() = 0;
    };

    struct objectCallback {
        state *idMap;

        explicit objectCallback(state *idMap) : idMap(idMap) {}

        virtual void valueForField(int field_id, char *begin, char *end) = 0;

        virtual objectCallback *willParseObject(int field_id) = 0;

        virtual arrayCallback *willParseArray(int field_id) = 0;

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

        explicit input(std::string &str) {
            begin = &*str.begin();
            current = &*str.begin();
            end = &*str.end();
        }

        void log(const std::string& message) {
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

        [[maybe_unused]] int parseArray(arrayCallback *arr) {
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

        int parseObject(objectCallback *obj) {
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