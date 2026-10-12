#include "yt/json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace json {

static const Value& nullValue() {
	static const Value v;
	return v;
}

static const std::string& emptyString() {
	static const std::string s;
	return s;
}

const std::string& Value::str() const { return type_ == Type::String ? str_ : emptyString(); }

double Value::num(double def) const {
	if (type_ == Type::Number) return num_;
	if (type_ == Type::String && !str_.empty()) {
		char* end = nullptr;
		double d = strtod(str_.c_str(), &end);
		if (end && end != str_.c_str()) return d;
	}
	if (type_ == Type::Bool) return b_ ? 1 : 0;
	return def;
}

long long Value::integer(long long def) const {
	if (type_ == Type::Number) return (long long)num_;
	if (type_ == Type::String && !str_.empty()) {
		char* end = nullptr;
		long long v = strtoll(str_.c_str(), &end, 10);
		if (end && end != str_.c_str()) return v;
	}
	return def;
}

bool Value::boolean(bool def) const {
	if (type_ == Type::Bool) return b_;
	if (type_ == Type::Number) return num_ != 0;
	return def;
}

const Value& Value::operator[](const char* key) const {
	if (type_ != Type::Object) return nullValue();
	for (const auto& kv : obj_)
		if (kv.first == key) return kv.second;
	return nullValue();
}

const Value& Value::operator[](size_t i) const {
	if (type_ != Type::Array || i >= arr_.size()) return nullValue();
	return arr_[i];
}

bool Value::has(const char* key) const {
	if (type_ != Type::Object) return false;
	for (const auto& kv : obj_)
		if (kv.first == key) return true;
	return false;
}

size_t Value::size() const {
	if (type_ == Type::Array) return arr_.size();
	if (type_ == Type::Object) return obj_.size();
	return 0;
}

Value& Value::set(const std::string& key, Value v) {
	if (type_ != Type::Object) {
		*this = object();
	}
	for (auto& kv : obj_) {
		if (kv.first == key) {
			kv.second = std::move(v);
			return kv.second;
		}
	}
	obj_.emplace_back(key, std::move(v));
	return obj_.back().second;
}

Value& Value::push(Value v) {
	if (type_ != Type::Array) *this = array();
	arr_.push_back(std::move(v));
	return arr_.back();
}

// ---------------------------------------------------------------- serializer

static void dumpString(const std::string& s, std::string& out) {
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				char buf[8];
				snprintf(buf, sizeof buf, "\\u%04x", c);
				out += buf;
			} else {
				out += (char)c;
			}
		}
	}
	out += '"';
}

static void dumpValue(const Value& v, std::string& out) {
	switch (v.type()) {
	case Type::Null: out += "null"; break;
	case Type::Bool: out += v.boolean() ? "true" : "false"; break;
	case Type::Number: {
		double d = v.num();
		char buf[32];
		if (std::isfinite(d) && d == (double)(long long)d && std::fabs(d) < 1e15)
			snprintf(buf, sizeof buf, "%lld", (long long)d);
		else if (std::isfinite(d))
			snprintf(buf, sizeof buf, "%.17g", d);
		else
			snprintf(buf, sizeof buf, "0");
		out += buf;
		break;
	}
	case Type::String: dumpString(v.str(), out); break;
	case Type::Array: {
		out += '[';
		bool first = true;
		for (const auto& e : v.items()) {
			if (!first) out += ',';
			first = false;
			dumpValue(e, out);
		}
		out += ']';
		break;
	}
	case Type::Object: {
		out += '{';
		bool first = true;
		for (const auto& kv : v.members()) {
			if (!first) out += ',';
			first = false;
			dumpString(kv.first, out);
			out += ':';
			dumpValue(kv.second, out);
		}
		out += '}';
		break;
	}
	}
}

std::string Value::dump() const {
	std::string out;
	dumpValue(*this, out);
	return out;
}

// ---------------------------------------------------------------- parser

class Parser {
public:
	Parser(const char* p, size_t n) : p_(p), end_(p + n) {}

	bool parseRoot(Value& out) {
		skipWs();
		if (!parseValue(out, 0)) return false;
		skipWs();
		return true;  // tolerate trailing garbage
	}

private:
	const char* p_;
	const char* end_;

	static constexpr int kMaxDepth = 512;

	void skipWs() {
		while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) p_++;
	}

	bool parseValue(Value& out, int depth) {
		if (depth > kMaxDepth || p_ >= end_) return false;
		switch (*p_) {
		case '{': return parseObject(out, depth);
		case '[': return parseArray(out, depth);
		case '"':
			out.type_ = Type::String;
			return parseString(out.str_);
		case 't':
			if (end_ - p_ >= 4 && !memcmp(p_, "true", 4)) {
				p_ += 4;
				out.type_ = Type::Bool;
				out.b_ = true;
				return true;
			}
			return false;
		case 'f':
			if (end_ - p_ >= 5 && !memcmp(p_, "false", 5)) {
				p_ += 5;
				out.type_ = Type::Bool;
				out.b_ = false;
				return true;
			}
			return false;
		case 'n':
			if (end_ - p_ >= 4 && !memcmp(p_, "null", 4)) {
				p_ += 4;
				out.type_ = Type::Null;
				return true;
			}
			return false;
		default: return parseNumber(out);
		}
	}

	bool parseObject(Value& out, int depth) {
		out.type_ = Type::Object;
		p_++;  // {
		skipWs();
		if (p_ < end_ && *p_ == '}') {
			p_++;
			return true;
		}
		while (p_ < end_) {
			skipWs();
			if (p_ >= end_ || *p_ != '"') return false;
			out.obj_.emplace_back();
			auto& kv = out.obj_.back();
			if (!parseString(kv.first)) return false;
			skipWs();
			if (p_ >= end_ || *p_ != ':') return false;
			p_++;
			skipWs();
			if (!parseValue(kv.second, depth + 1)) return false;
			skipWs();
			if (p_ >= end_) return false;
			if (*p_ == ',') {
				p_++;
				continue;
			}
			if (*p_ == '}') {
				p_++;
				return true;
			}
			return false;
		}
		return false;
	}

	bool parseArray(Value& out, int depth) {
		out.type_ = Type::Array;
		p_++;  // [
		skipWs();
		if (p_ < end_ && *p_ == ']') {
			p_++;
			return true;
		}
		while (p_ < end_) {
			skipWs();
			out.arr_.emplace_back();
			if (!parseValue(out.arr_.back(), depth + 1)) return false;
			skipWs();
			if (p_ >= end_) return false;
			if (*p_ == ',') {
				p_++;
				continue;
			}
			if (*p_ == ']') {
				p_++;
				return true;
			}
			return false;
		}
		return false;
	}

	static void appendUtf8(std::string& s, unsigned cp) {
		if (cp < 0x80) {
			s += (char)cp;
		} else if (cp < 0x800) {
			s += (char)(0xC0 | (cp >> 6));
			s += (char)(0x80 | (cp & 0x3F));
		} else if (cp < 0x10000) {
			s += (char)(0xE0 | (cp >> 12));
			s += (char)(0x80 | ((cp >> 6) & 0x3F));
			s += (char)(0x80 | (cp & 0x3F));
		} else {
			s += (char)(0xF0 | (cp >> 18));
			s += (char)(0x80 | ((cp >> 12) & 0x3F));
			s += (char)(0x80 | ((cp >> 6) & 0x3F));
			s += (char)(0x80 | (cp & 0x3F));
		}
	}

	bool hex4(unsigned& out) {
		if (end_ - p_ < 4) return false;
		out = 0;
		for (int i = 0; i < 4; i++) {
			char c = *p_++;
			out <<= 4;
			if (c >= '0' && c <= '9') out |= c - '0';
			else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
			else return false;
		}
		return true;
	}

	bool parseString(std::string& s) {
		p_++;  // opening quote
		// Fast path: find the closing quote with no escapes.
		const char* start = p_;
		while (p_ < end_ && *p_ != '"' && *p_ != '\\') p_++;
		s.assign(start, p_ - start);
		while (p_ < end_) {
			char c = *p_++;
			if (c == '"') return true;
			if (c != '\\') {
				s += c;
				continue;
			}
			if (p_ >= end_) return false;
			char e = *p_++;
			switch (e) {
			case '"': s += '"'; break;
			case '\\': s += '\\'; break;
			case '/': s += '/'; break;
			case 'b': s += '\b'; break;
			case 'f': s += '\f'; break;
			case 'n': s += '\n'; break;
			case 'r': s += '\r'; break;
			case 't': s += '\t'; break;
			case 'u': {
				unsigned cp;
				if (!hex4(cp)) return false;
				if (cp >= 0xD800 && cp <= 0xDBFF && end_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
					const char* save = p_;
					p_ += 2;
					unsigned lo;
					if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
						cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					} else {
						p_ = save;
					}
				}
				appendUtf8(s, cp);
				break;
			}
			default: s += e; break;
			}
		}
		return false;
	}

	bool parseNumber(Value& out) {
		const char* start = p_;
		while (p_ < end_ && (strchr("+-0123456789.eE", *p_) != nullptr) && *p_) p_++;
		if (p_ == start) return false;
		char buf[64];
		size_t n = (size_t)(p_ - start);
		if (n >= sizeof buf) n = sizeof buf - 1;
		memcpy(buf, start, n);
		buf[n] = 0;
		out.type_ = Type::Number;
		out.num_ = strtod(buf, nullptr);
		return true;
	}
};

bool parse(const char* data, size_t len, Value& out) {
	out = Value();
	Parser p(data, len);
	if (!p.parseRoot(out)) {
		out = Value();
		return false;
	}
	return true;
}

std::string text(const Value& v) {
	if (v.isString()) return v.str();
	if (v["simpleText"].isString()) return v["simpleText"].str();
	if (v["content"].isString()) return v["content"].str();
	const Value& runs = v["runs"];
	if (runs.isArray()) {
		std::string s;
		for (const auto& r : runs.items()) s += r["text"].str();
		return s;
	}
	return std::string();
}

}  // namespace json
