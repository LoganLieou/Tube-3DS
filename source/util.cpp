#include "util.hpp"

#include <sys/stat.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace util {

Thread startThread(ThreadFunc fn, void* arg, size_t stack, int prio, int preferredCore) {
	Thread t = threadCreate(fn, arg, stack, prio, preferredCore, false);
	if (!t && preferredCore != -2) t = threadCreate(fn, arg, stack, prio, -2, false);
	if (!t) t = threadCreate(fn, arg, stack, prio, 0, false);
	if (!t) logf("threadCreate failed (prio %x core %d)", prio, preferredCore);
	return t;
}

bool isNew3DS() {
	static int cached = -1;
	if (cached < 0) {
		bool n3ds = false;
		APT_CheckNew3DS(&n3ds);
		cached = n3ds ? 1 : 0;
	}
	return cached == 1;
}

u64 nowMs() { return osGetTime(); }

void sleepMs(u32 ms) { svcSleepThread((s64)ms * 1000000LL); }

std::string formatTime(double seconds) {
	if (seconds < 0 || seconds != seconds) seconds = 0;
	long t = (long)seconds;
	long h = t / 3600, m = (t / 60) % 60, s = t % 60;
	char buf[32];
	if (h > 0)
		snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", h, m, s);
	else
		snprintf(buf, sizeof buf, "%ld:%02ld", m, s);
	return buf;
}

std::string urlEncode(const std::string& s) {
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(s.size() * 3);
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += (char)c;
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

std::string jsonEscape(const std::string& s) {
	std::string out;
	out.reserve(s.size() + 8);
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
	return out;
}

std::string trim(const std::string& s) {
	size_t a = 0, b = s.size();
	while (a < b && isspace((unsigned char)s[a])) a++;
	while (b > a && isspace((unsigned char)s[b - 1])) b--;
	return s.substr(a, b - a);
}

std::string toLower(std::string s) {
	for (auto& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

s64 parseRelativeAge(const std::string& text) {
	std::string s = toLower(text);
	// Find the first number in the string.
	size_t i = 0;
	while (i < s.size() && !isdigit((unsigned char)s[i])) i++;
	if (i == s.size()) {
		if (s.find("just now") != std::string::npos || s.find("live") != std::string::npos) return 0;
		return 0x7fffffffffffLL;
	}
	s64 n = 0;
	while (i < s.size() && isdigit((unsigned char)s[i])) n = n * 10 + (s[i++] - '0');
	while (i < s.size() && s[i] == ' ') i++;
	std::string unit = s.substr(i);
	s64 mul;
	if (unit.rfind("sec", 0) == 0 || unit.rfind("s ", 0) == 0) mul = 1;
	else if (unit.rfind("min", 0) == 0 || unit.rfind("m ", 0) == 0) mul = 60;
	else if (unit.rfind("hour", 0) == 0 || unit.rfind("h ", 0) == 0 || unit.rfind("hr", 0) == 0) mul = 3600;
	else if (unit.rfind("day", 0) == 0 || unit.rfind("d ", 0) == 0) mul = 86400;
	else if (unit.rfind("week", 0) == 0 || unit.rfind("w ", 0) == 0 || unit.rfind("wk", 0) == 0) mul = 604800;
	else if (unit.rfind("mo", 0) == 0) mul = 2592000;
	else if (unit.rfind("year", 0) == 0 || unit.rfind("y ", 0) == 0 || unit.rfind("yr", 0) == 0) mul = 31536000;
	else mul = 86400;
	return n * mul;
}

u32 utf8Next(const char*& p, const char* end) {
	unsigned char c = (unsigned char)*p++;
	if (c < 0x80) return c;
	int extra;
	u32 cp;
	if ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
	else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
	else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
	else return 0xFFFD;
	while (extra-- > 0) {
		if (p >= end || (((unsigned char)*p) & 0xC0) != 0x80) return 0xFFFD;
		cp = (cp << 6) | (((unsigned char)*p++) & 0x3F);
	}
	return cp;
}

bool readFile(const std::string& path, std::string& out) {
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return false;
	}
	out.resize((size_t)n);
	size_t got = n ? fread(&out[0], 1, (size_t)n, f) : 0;
	fclose(f);
	out.resize(got);
	return true;
}

bool writeFile(const std::string& path, const std::string& data) {
	std::string tmp = path + ".tmp";
	FILE* f = fopen(tmp.c_str(), "wb");
	if (!f) return false;
	bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	ok = (fclose(f) == 0) && ok;
	if (!ok) {
		remove(tmp.c_str());
		return false;
	}
	remove(path.c_str());
	return rename(tmp.c_str(), path.c_str()) == 0;
}

void mkdirs(const std::string& path) {
	std::string cur;
	for (size_t i = 0; i < path.size(); i++) {
		cur += path[i];
		if (path[i] == '/' && cur.size() > 1 && cur.back() == '/' && cur[cur.size() - 2] != ':')
			mkdir(cur.c_str(), 0777);
	}
	mkdir(path.c_str(), 0777);
}

const std::string& dataDir() {
	static const std::string dir = "sdmc:/3ds/Tube3DS";
	return dir;
}

static Mutex g_logLock;

void logf(const char* fmt, ...) {
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	LockGuard g(g_logLock);
#ifdef TUBE_HOST_TEST
	fprintf(stderr, "[log] %s\n", buf);
	return;
#endif
	static FILE* f = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		mkdirs(dataDir());
		f = fopen((dataDir() + "/log.txt").c_str(), "w");
	}
	if (f) {
		fprintf(f, "[%8llu] %s\n", (unsigned long long)osGetTime() % 100000000ULL, buf);
		fflush(f);
	}
}

}  // namespace util
