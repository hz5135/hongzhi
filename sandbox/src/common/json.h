#pragma once
#include <string>
#include <vector>
#include <utility>

namespace sbx {
namespace json {

enum class Type { Null, Bool, Number, String, Array, Object };

class Value {
public:
  Type type = Type::Null;
  bool boolValue = false;
  double num = 0;
  std::string str;
  std::vector<Value> arr;
  std::vector<std::pair<std::string, Value>> obj;

  static Value null() { return Value(); }
  static Value boolean(bool v) { Value x; x.type = Type::Bool; x.boolValue = v; return x; }
  static Value number(double v) { Value x; x.type = Type::Number; x.num = v; return x; }
  static Value text(const std::string& v) { Value x; x.type = Type::String; x.str = v; return x; }
  static Value array() { Value x; x.type = Type::Array; return x; }
  static Value object() { Value x; x.type = Type::Object; return x; }

  bool isNull() const { return type == Type::Null; }
  bool isBool() const { return type == Type::Bool; }
  bool isNumber() const { return type == Type::Number; }
  bool isString() const { return type == Type::String; }
  bool isArray() const { return type == Type::Array; }
  bool isObject() const { return type == Type::Object; }

  bool asBool() const { return boolValue; }
  double asNumber() const { return num; }
  long asInt() const { return static_cast<long>(num); }
  const std::string& asString() const { return str; }

  size_t size() const { return arr.size(); }
  const Value& at(size_t i) const { return arr.at(i); }
  Value& at(size_t i) { return arr.at(i); }

  bool has(const std::string& k) const {
    for (const auto& p : obj) if (p.first == k) return true;
    return false;
  }
  const Value& get(const std::string& k) const {
    static const Value kNull;
    for (const auto& p : obj) if (p.first == k) return p.second;
    return kNull;
  }
  void set(const std::string& k, Value v) {
    for (auto& p : obj) { if (p.first == k) { p.second = std::move(v); return; } }
    obj.emplace_back(k, std::move(v));
  }
  void push(Value v) { arr.push_back(std::move(v)); }

  const std::string& strOr(const std::string& def) const {
    if (isString()) return str;
    return def;
  }
  long intOr(long def) const {
    if (isNumber()) return asInt();
    return def;
  }
  bool boolOr(bool def) const {
    if (isBool()) return boolValue;
    return def;
  }
};

Value parse(const std::string& text);
std::string stringify(const Value& v);
std::string escape(const std::string& s);

} // namespace json
} // namespace sbx