#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

// Minimal JSON DOM. Lookups on missing keys/indices return a shared null value,
// so deep paths like v["a"]["b"][0]["c"].str() never crash.
namespace json {

enum class Type : unsigned char { Null, Bool, Number, String, Array, Object };

class Value {
public:
	Value() = default;
	explicit Value(bool b) : type_(Type::Bool), b_(b) {}
	explicit Value(double n) : type_(Type::Number), num_(n) {}
	explicit Value(std::string s) : type_(Type::String), str_(std::move(s)) {}

	static Value array() { Value v; v.type_ = Type::Array; return v; }
	static Value object() { Value v; v.type_ = Type::Object; return v; }

	Type type() const { return type_; }
	bool isNull() const { return type_ == Type::Null; }
	bool isObject() const { return type_ == Type::Object; }
	bool isArray() const { return type_ == Type::Array; }
	bool isString() const { return type_ == Type::String; }
	bool isNumber() const { return type_ == Type::Number; }
	bool isBool() const { return type_ == Type::Bool; }

	const std::string& str() const;              // "" if not a string
	double num(double def = 0) const;            // also parses numeric strings
	long long integer(long long def = 0) const;
	bool boolean(bool def = false) const;

	const Value& operator[](const char* key) const;
	const Value& operator[](const std::string& key) const { return (*this)[key.c_str()]; }
	const Value& operator[](size_t i) const;
	const Value& operator[](int i) const { return (*this)[(size_t)i]; }
	bool has(const char* key) const;

	size_t size() const;  // array/object element count
	const std::vector<Value>& items() const { return arr_; }
	const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }

	// Mutation (used when building settings files).
	Value& set(const std::string& key, Value v);
	Value& push(Value v);

	std::string dump() const;

private:
	friend class Parser;
	Type type_ = Type::Null;
	bool b_ = false;
	double num_ = 0;
	std::string str_;
	std::vector<Value> arr_;
	std::vector<std::pair<std::string, Value>> obj_;
};

// Parses UTF-8 JSON. Returns false (and leaves out as null) on malformed input.
bool parse(const char* data, size_t len, Value& out);
inline bool parse(const std::string& s, Value& out) { return parse(s.data(), s.size(), out); }

// YouTube text helper: returns simpleText, the concatenation of runs[].text, or content.
std::string text(const Value& v);

}  // namespace json
