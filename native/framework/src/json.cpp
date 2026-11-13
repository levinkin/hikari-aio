#include "json.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace hfw {

enum class parse_state : uint8_t { value, key, colon, after_value };

struct open_container {
    int32_t node;
    size_t pending_start;
};

struct text_frame {
    int32_t node;
    int32_t next;
};

struct parser {
    json_document& document;
    const uint8_t* bytes;
    size_t length;
    size_t position;
    std::vector<open_container> stack;
    std::vector<int32_t> pending;
    std::vector<int32_t> order;
    int32_t key;
    int32_t key_length;
};

static bool read_string(parser& state, int32_t& offset, int32_t& size)
{
    std::vector<char>& text = state.document.text;
    size_t start = text.size();
    state.position++;
    while (state.position < state.length) {
        uint8_t byte = state.bytes[state.position++];
        if (byte == '"') {
            offset = static_cast<int32_t>(start);
            size = static_cast<int32_t>(text.size() - start);
            text.push_back('\0');
            return true;
        }
        if (byte != '\\') {
            text.push_back(static_cast<char>(byte));
            continue;
        }
        if (state.position == state.length) {
            return false;
        }
        uint8_t escaped = state.bytes[state.position++];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            text.push_back(static_cast<char>(escaped));
            break;
        case 'b':
            text.push_back('\b');
            break;
        case 'f':
            text.push_back('\f');
            break;
        case 'n':
            text.push_back('\n');
            break;
        case 'r':
            text.push_back('\r');
            break;
        case 't':
            text.push_back('\t');
            break;
        case 'u':
            return false;
        default:
            break;
        }
    }
    return false;
}

static bool read_number(parser& state, float& result)
{
    bool negative = false;
    bool fraction = false;
    float value = 0.0f;
    float weight = 0.1f;
    for (;;) {
        if (state.position == state.length) {
            return false;
        }
        uint8_t byte = state.bytes[state.position];
        if (byte == '\n' || byte == ',') {
            break;
        }
        if (byte >= '0' && byte <= '9') {
            float digit = static_cast<float>(byte - '0');
            if (fraction) {
                value = std::fma(digit, weight, value);
                weight = weight * 0.1f;
            } else {
                value = std::fma(value, 10.0f, digit);
            }
        } else if (byte == '-') {
            negative = true;
        } else if (byte == '.') {
            if (fraction) {
                return false;
            }
            fraction = true;
        } else if (byte != '\r') {
            return false;
        }
        state.position++;
    }
    result = negative ? -value : value;
    return true;
}

static int32_t add_node(parser& state, json_kind kind)
{
    json_node node = {};
    node.kind = kind;
    node.text = -1;
    node.key = -1;
    node.first = -1;
    if (!state.stack.empty() && state.document.nodes[state.stack.back().node].kind == json_kind::object) {
        node.key = state.key;
        node.key_length = state.key_length;
    }
    int32_t index = static_cast<int32_t>(state.document.nodes.size());
    state.document.nodes.push_back(node);
    if (!state.stack.empty()) {
        state.pending.push_back(index);
    }
    return index;
}

static bool key_before(const json_document& document, const int32_t* children, int32_t left, int32_t right)
{
    const json_node& left_node = document.nodes[children[left]];
    const json_node& right_node = document.nodes[children[right]];
    int32_t shorter = std::min(left_node.key_length, right_node.key_length);
    int compared = std::memcmp(&document.text[left_node.key], &document.text[right_node.key], shorter);
    if (compared != 0) {
        return compared < 0;
    }
    if (left_node.key_length != right_node.key_length) {
        return left_node.key_length < right_node.key_length;
    }
    return left < right;
}

static bool key_equal(const json_document& document, int32_t left, int32_t right)
{
    const json_node& left_node = document.nodes[left];
    const json_node& right_node = document.nodes[right];
    return left_node.key_length == right_node.key_length
        && std::memcmp(&document.text[left_node.key], &document.text[right_node.key], left_node.key_length) == 0;
}

static void sift_down(const json_document& document, const int32_t* children, int32_t* order, size_t root, size_t end)
{
    for (;;) {
        size_t child = root * 2 + 1;
        if (child >= end) {
            return;
        }
        if (child + 1 < end && key_before(document, children, order[child], order[child + 1])) {
            child++;
        }
        if (!key_before(document, children, order[root], order[child])) {
            return;
        }
        std::swap(order[root], order[child]);
        root = child;
    }
}

static void merge_duplicate_keys(parser& state, size_t start)
{
    size_t count = state.pending.size() - start;
    state.order.resize(count);
    for (size_t i = 0; i < count; i++) {
        state.order[i] = static_cast<int32_t>(i);
    }
    const json_document& document = state.document;
    const int32_t* children = &state.pending[start];
    int32_t* order = state.order.data();
    for (size_t root = count / 2; root-- > 0;) {
        sift_down(document, children, order, root, count);
    }
    for (size_t end = count - 1; end > 0; end--) {
        std::swap(order[0], order[end]);
        sift_down(document, children, order, 0, end);
    }
    bool merged = false;
    size_t run = 0;
    for (size_t i = 1; i <= count; i++) {
        if (i < count && key_equal(document, children[order[run]], children[order[i]])) {
            continue;
        }
        if (i - run > 1) {
            state.pending[start + order[run]] = state.pending[start + order[i - 1]];
            for (size_t duplicate = run + 1; duplicate < i; duplicate++) {
                state.pending[start + order[duplicate]] = -1;
            }
            merged = true;
        }
        run = i;
    }
    if (merged) {
        state.pending.erase(std::remove(state.pending.begin() + start, state.pending.end(), -1), state.pending.end());
    }
}

static void close_container(parser& state)
{
    open_container top = state.stack.back();
    state.stack.pop_back();
    if (state.document.nodes[top.node].kind == json_kind::object && state.pending.size() - top.pending_start > 1) {
        merge_duplicate_keys(state, top.pending_start);
    }
    json_node& closed = state.document.nodes[top.node];
    closed.first = static_cast<int32_t>(state.document.children.size());
    closed.count = static_cast<int32_t>(state.pending.size() - top.pending_start);
    state.document.children.insert(state.document.children.end(), state.pending.begin() + top.pending_start, state.pending.end());
    state.pending.resize(top.pending_start);
}

static bool parse_value(parser& state, parse_state& next)
{
    while (state.position < state.length) {
        uint8_t byte = state.bytes[state.position];
        if (byte == '"' || byte == '-' || (byte >= '0' && byte <= '9') || byte == 'n' || byte == 't' || byte == 'f'
            || byte == '[' || byte == '{' || byte == ']' || byte == '}' || byte == ',' || byte == ':') {
            break;
        }
        state.position++;
    }
    if (state.position == state.length) {
        return false;
    }
    uint8_t byte = state.bytes[state.position];
    next = parse_state::after_value;
    if (byte == ']') {
        if (state.stack.empty() || state.document.nodes[state.stack.back().node].kind != json_kind::array) {
            return false;
        }
        state.position++;
        close_container(state);
        return true;
    }
    if (byte == '}' || byte == ',' || byte == ':') {
        return false;
    }
    if (byte == '{' || byte == '[') {
        json_kind kind = byte == '{' ? json_kind::object : json_kind::array;
        int32_t index = add_node(state, kind);
        state.stack.push_back({index, state.pending.size()});
        state.position++;
        next = kind == json_kind::object ? parse_state::key : parse_state::value;
        return true;
    }
    if (byte == '"') {
        int32_t offset = 0;
        int32_t size = 0;
        if (!read_string(state, offset, size)) {
            return false;
        }
        int32_t index = add_node(state, json_kind::string);
        state.document.nodes[index].text = offset;
        state.document.nodes[index].text_length = size;
        return true;
    }
    if (byte == 'n' || byte == 't' || byte == 'f') {
        size_t size = byte == 'f' ? 5 : 4;
        if (state.length - state.position < size) {
            return false;
        }
        state.position += size;
        int32_t index = add_node(state, byte == 'n' ? json_kind::null : json_kind::boolean);
        state.document.nodes[index].truth = byte == 't';
        return true;
    }
    float number = 0.0f;
    if (!read_number(state, number)) {
        return false;
    }
    int32_t index = add_node(state, json_kind::number);
    state.document.nodes[index].number = number;
    return true;
}

static bool parse_key(parser& state, parse_state& next)
{
    while (state.position < state.length) {
        uint8_t byte = state.bytes[state.position];
        if (byte == '"' || byte == '}' || byte == ':') {
            break;
        }
        state.position++;
    }
    if (state.position == state.length || state.bytes[state.position] == ':') {
        return false;
    }
    if (state.bytes[state.position] == '}') {
        state.position++;
        close_container(state);
        next = parse_state::after_value;
        return true;
    }
    next = parse_state::colon;
    return read_string(state, state.key, state.key_length);
}

static bool parse_colon(parser& state, parse_state& next)
{
    while (state.position < state.length) {
        uint8_t byte = state.bytes[state.position];
        if (byte == ':') {
            state.position++;
            next = parse_state::value;
            return true;
        }
        if (byte == '}') {
            return false;
        }
        state.position++;
    }
    return false;
}

static bool parse_after_value(parser& state, parse_state& next)
{
    while (state.position < state.length) {
        uint8_t byte = state.bytes[state.position];
        if (byte == ',') {
            state.position++;
            next = (!state.stack.empty() && state.document.nodes[state.stack.back().node].kind == json_kind::object) ? parse_state::key : parse_state::value;
            return true;
        }
        if (byte == '}' || byte == ']') {
            if (state.stack.empty() || state.document.nodes[state.stack.back().node].kind != (byte == '}' ? json_kind::object : json_kind::array)) {
                return false;
            }
            state.position++;
            close_container(state);
            next = parse_state::after_value;
            return true;
        }
        state.position++;
    }
    return false;
}

static bool parse_document(parser& state)
{
    parse_state step = parse_state::value;
    for (;;) {
        bool parsed = false;
        switch (step) {
        case parse_state::value:
            parsed = parse_value(state, step);
            break;
        case parse_state::key:
            parsed = parse_key(state, step);
            break;
        case parse_state::colon:
            parsed = parse_colon(state, step);
            break;
        case parse_state::after_value:
            parsed = parse_after_value(state, step);
            break;
        }
        if (!parsed) {
            return false;
        }
        if (step == parse_state::after_value && state.stack.empty()) {
            return true;
        }
    }
}

static void reserve_document(json_document& document, const uint8_t* bytes, size_t length)
{
    size_t nodes = 0;
    size_t text = 0;
    bool in_string = false;
    bool after_string = false;
    bool in_scalar = false;
    for (size_t position = 0; position < length; position++) {
        uint8_t byte = bytes[position];
        if (in_string) {
            if (byte == '"') {
                in_string = false;
                after_string = true;
            } else {
                position += byte == '\\';
                text++;
            }
            continue;
        }
        bool scalar = byte == '-' || byte == '.' || (byte >= '0' && byte <= '9');
        if (byte == '"') {
            in_string = true;
            nodes++;
            text++;
        } else if (byte == '[' || byte == '{' || byte == 'n' || byte == 't' || byte == 'f' || (scalar && !in_scalar)) {
            nodes++;
        } else if (byte == ':' && after_string) {
            nodes--;
        }
        in_scalar = scalar;
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r' && byte != '"') {
            after_string = false;
        }
    }
    document.nodes.reserve(nodes);
    document.children.reserve(nodes);
    document.text.reserve(text);
}

bool json_parse(json_document& document, const uint8_t* bytes, size_t length)
{
    document.nodes.clear();
    document.children.clear();
    document.text.clear();
    if (length > INT32_MAX) {
        return false;
    }
    reserve_document(document, bytes, length);
    parser state = {document, bytes, length, 0, {}, {}, {}, -1, 0};
    if (parse_document(state)) {
        return true;
    }
    document.nodes.clear();
    document.children.clear();
    document.text.clear();
    return false;
}

int32_t json_root(const json_document& document)
{
    return document.nodes.empty() ? -1 : 0;
}

int32_t json_find(const json_document& document, int32_t node, const char* key)
{
    if (node < 0 || document.nodes[node].kind != json_kind::object) {
        return -1;
    }
    size_t key_length = std::strlen(key);
    const json_node& parent = document.nodes[node];
    for (int32_t i = 0; i < parent.count; i++) {
        int32_t child = document.children[parent.first + i];
        const json_node& entry = document.nodes[child];
        if (static_cast<size_t>(entry.key_length) == key_length && std::memcmp(&document.text[entry.key], key, key_length) == 0) {
            return child;
        }
    }
    return -1;
}

int32_t json_at(const json_document& document, int32_t node, int32_t index)
{
    if (node < 0 || document.nodes[node].kind != json_kind::array || index < 0 || index >= document.nodes[node].count) {
        return -1;
    }
    return document.children[document.nodes[node].first + index];
}

int32_t json_size(const json_document& document, int32_t node)
{
    if (node < 0 || document.nodes[node].kind != json_kind::array) {
        return 0;
    }
    return document.nodes[node].count;
}

const char* json_key(const json_document& document, int32_t node)
{
    if (node < 0 || document.nodes[node].key < 0) {
        return "";
    }
    return &document.text[document.nodes[node].key];
}

float json_float(const json_document& document, int32_t node, float absent, float wrong_kind)
{
    if (node < 0 || document.nodes[node].kind == json_kind::null) {
        return absent;
    }
    if (document.nodes[node].kind != json_kind::number) {
        return wrong_kind;
    }
    return document.nodes[node].number;
}

int32_t json_int(const json_document& document, int32_t node, int32_t fallback)
{
    if (node < 0 || document.nodes[node].kind != json_kind::number) {
        return fallback;
    }
    float number = document.nodes[node].number;
    if (number >= 2147483647.0f) {
        return INT32_MAX;
    }
    if (number <= -2147483648.0f) {
        return INT32_MIN;
    }
    return static_cast<int32_t>(number);
}

bool json_bool(const json_document& document, int32_t node, bool fallback)
{
    if (node < 0 || document.nodes[node].kind != json_kind::boolean) {
        return fallback;
    }
    return document.nodes[node].truth;
}

static void append_scalar(const json_document& document, const json_node& node, std::string& output)
{
    if (node.kind == json_kind::string) {
        output.append(&document.text[node.text], node.text_length);
    } else if (node.kind == json_kind::number) {
        char buffer[64];
        int written = std::snprintf(buffer, sizeof buffer, "%f", static_cast<double>(node.number));
        output.append(buffer, std::min(written, static_cast<int>(sizeof buffer) - 1));
    } else if (node.kind == json_kind::boolean) {
        output.append(node.truth ? "true" : "false");
    }
}

bool json_text(const json_document& document, int32_t node, std::string& output)
{
    output.clear();
    if (node < 0 || document.nodes[node].kind == json_kind::null || document.nodes[node].kind == json_kind::object) {
        return false;
    }
    if (document.nodes[node].kind != json_kind::array) {
        append_scalar(document, document.nodes[node], output);
        return true;
    }
    std::vector<text_frame> stack;
    stack.push_back({node, 0});
    output.append("[\n");
    while (!stack.empty()) {
        text_frame& top = stack.back();
        const json_node& array = document.nodes[top.node];
        if (top.next == array.count) {
            stack.pop_back();
            output.append(stack.empty() ? "]\n" : "]\n\n");
            continue;
        }
        int32_t child = document.children[array.first + top.next];
        top.next++;
        output.push_back('\t');
        if (document.nodes[child].kind == json_kind::array) {
            output.append("[\n");
            stack.push_back({child, 0});
            continue;
        }
        append_scalar(document, document.nodes[child], output);
        output.push_back('\n');
    }
    return true;
}

}
