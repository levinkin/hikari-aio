#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hfw {

enum class json_kind : uint8_t { null, boolean, number, string, array, object };

struct json_node {
    json_kind kind;
    bool truth;
    float number;
    int32_t text;
    int32_t text_length;
    int32_t key;
    int32_t key_length;
    int32_t first;
    int32_t count;
};

struct json_document {
    std::vector<json_node> nodes;
    std::vector<int32_t> children;
    std::vector<char> text;
};

bool json_parse(json_document& document, const uint8_t* bytes, size_t length);
int32_t json_root(const json_document& document);
int32_t json_find(const json_document& document, int32_t node, const char* key);
int32_t json_at(const json_document& document, int32_t node, int32_t index);
int32_t json_size(const json_document& document, int32_t node);
const char* json_key(const json_document& document, int32_t node);
float json_float(const json_document& document, int32_t node, float absent, float wrong_kind);
int32_t json_int(const json_document& document, int32_t node, int32_t fallback);
bool json_bool(const json_document& document, int32_t node, bool fallback);
bool json_text(const json_document& document, int32_t node, std::string& out);

}
