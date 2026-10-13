#include "json.h"
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace sbx {
namespace json {

namespace {

class Parser {
public:
  explicit Parser(const std::string& s) : s_(s) {}

  Value parse() {
    skipWs();
    Value v = parseValue();
    skipWs();
    return v;
  }

private:
  const std::string& s_;
  size_t i_ = 0;

  void skipWs() {
    while (i_ < s_.size() && isspace(static_cast<unsigned char>(s_[i_]))) i_++;
  }
  char peek() {
    if (i_ >= s_.size()) return '\0';
    return s_[i_];
  }
  char next() {
    if (i_ >= s_.size()) throw std::runtime_error("unexpected end of json");
    return s_[i_++];
  }
  bool expect(char ch) {
    if (peek() != ch) return false;
    i_++;
    return true;
  }
  void expectOrThrow(char ch) {
    if (!expect(ch)) throw std::runtime_error(std::string("expected '") + ch + "'");
  }

  Value parseValue() {
    char ch = peek();
    switch (ch) {
      case '{': return parseObject();
      case '[': return parseArray();
      case '"': return Value::text(parseString());
      case 't': parseLiteral("true"); return Value::boolean(true);
      case 'f': parseLiteral("false"); return Value::boolean(false);
      case 'n': parseLiteral("null"); return Value::null();
      default:
        if (ch == '-' || isdigit(static_cast<unsigned char>(ch))) return parseNumber();
        throw std::runtime_error("invalid json value");
    }
  }

  void parseLiteral(const char* lit) {
    size_t len = strlen(lit);
    if (s_.compare(i_, len, lit) != 0) throw std::runtime_error("invalid literal");
    i_ += len;
  }

  std::string parseString() {
    expectOrThrow('"');
    std::string out;
    while (true) {
      char ch = next();
      if (ch == '"') break;
      if (ch == '\\') {
        char e = next();
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            std::string hex = s_.substr(i_, 4);
            i_ += 4;
            out += static_cast<char>(static_cast<unsigned>(strtol(hex.c_str(), nullptr, 16)));
            break;
          }
          default: throw std::runtime_error("invalid escape");
        }
      } else {
        out += ch;
      }
    }
    return out;
  }

  Value parseNumber() {
    size_t start = i_;
    while (i_ < s_.size() &&
           (isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '-' || s_[i_] == '+' ||
            s_[i_] == '.' || s_[i_] == 'e' || s_[i_] == 'E')) {
      i_++;
    }
    return Value::number(strtod(s_.substr(start, i_ - start).c_str(), nullptr));
  }

  Value parseObject() {
    expectOrThrow('{');
    Value obj = Value::object();
    skipWs();
    if (expect('}')) return obj;
    while (true) {
      skipWs();
      if (peek() != '"') throw std::runtime_error("expected object key");
      std::string key = parseString();
      skipWs();
      expectOrThrow(':');
      skipWs();
      obj.set(key, parseValue());
      skipWs();
      if (expect('}')) break;
      expectOrThrow(',');
    }
    return obj;
  }

  Value parseArray() {
    expectOrThrow('[');
    Value arr = Value::array();
    skipWs();
    if (expect(']')) return arr;
    while (true) {
      skipWs();
      arr.push(parseValue());
      skipWs();
      if (expect(']')) break;
      expectOrThrow(',');
    }
    return arr;
  }
};

} // namespace

Value parse(const std::string& text) {
  return Parser(text).parse();
}

std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char ch : s) {
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (ch < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", ch);
          out += buf;
        } else {
          out += static_cast<char>(ch);
        }
    }
  }
  return out;
}

std::string stringify(const Value& v) {
  switch (v.type) {
    case Type::Null: return "null";
    case Type::Bool: return v.boolValue ? "true" : "false";
    case Type::Number: {
      if (v.num == static_cast<long long>(v.num)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v.num));
        return buf;
      }
      char buf[32];
      snprintf(buf, sizeof(buf), "%.10g", v.num);
      return buf;
    }
    case Type::String: return "\"" + escape(v.str) + "\"";
    case Type::Array: {
      std::string out = "[";
      for (size_t i = 0; i < v.arr.size(); i++) {
        if (i) out += ",";
        out += stringify(v.arr[i]);
      }
      return out + "]";
    }
    case Type::Object: {
      std::string out = "{";
      for (size_t i = 0; i < v.obj.size(); i++) {
        if (i) out += ",";
        out += "\"" + escape(v.obj[i].first) + "\":" + stringify(v.obj[i].second);
      }
      return out + "}";
    }
  }
  return "null";
}

} // namespace json
} // namespace sbx