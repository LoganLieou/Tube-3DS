#include "net/http.hpp"

#include <curl/curl.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "util.hpp"

namespace net {

static std::atomic<bool> g_verifyTls{true};
static const char* kCaBundle = "romfs:/cacert.pem";

void globalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }
void globalExit() { curl_global_cleanup(); }
void setVerifyTls(bool verify) { g_verifyTls = verify; }

Client::Client() { h_ = curl_easy_init(); }

Client::~Client() {
	if (h_) curl_easy_cleanup(h_);
}

static size_t writeString(char* ptr, size_t size, size_t nmemb, void* user) {
	auto* s = static_cast<std::string*>(user);
	s->append(ptr, size * nmemb);
	return size * nmemb;
}

void Client::prepare(const std::string& url, const std::vector<std::string>& headers, curl_slist** list) {
	curl_easy_reset(h_);
	curl_easy_setopt(h_, CURLOPT_URL, url.c_str());
	errbuf_[0] = 0;
	curl_easy_setopt(h_, CURLOPT_ERRORBUFFER, errbuf_);
	curl_easy_setopt(h_, CURLOPT_NOSIGNAL, 1L);
	// Stream URLs are bound to the IP that requested them; keep every request on
	// IPv4 so the API call and the media fetches always come from the same address.
	curl_easy_setopt(h_, CURLOPT_IPRESOLVE, (long)CURL_IPRESOLVE_V4);
	curl_easy_setopt(h_, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(h_, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(h_, CURLOPT_CONNECTTIMEOUT, 15L);
	curl_easy_setopt(h_, CURLOPT_LOW_SPEED_LIMIT, 64L);
	curl_easy_setopt(h_, CURLOPT_LOW_SPEED_TIME, 20L);
	curl_easy_setopt(h_, CURLOPT_BUFFERSIZE, 64L * 1024);
	curl_easy_setopt(h_, CURLOPT_ACCEPT_ENCODING, "gzip");
	if (g_verifyTls) {
		curl_easy_setopt(h_, CURLOPT_CAINFO, kCaBundle);
		curl_easy_setopt(h_, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(h_, CURLOPT_SSL_VERIFYHOST, 2L);
	} else {
		curl_easy_setopt(h_, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(h_, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	*list = nullptr;
	for (const auto& h : headers) *list = curl_slist_append(*list, h.c_str());
	if (*list) curl_easy_setopt(h_, CURLOPT_HTTPHEADER, *list);
}

// Turns a curl failure into something a person can act on. Certificate date
// failures almost always mean the console's clock is wrong.
static std::string describeError(CURLcode rc, const char* detail) {
	std::string d = detail ? detail : "";
	if (rc == CURLE_PEER_FAILED_VERIFICATION || d.find("BADCERT") != std::string::npos) {
		if (d.find("BADCERT_FUTURE") != std::string::npos || d.find("BADCERT_EXPIRED") != std::string::npos) {
			time_t now = time(nullptr);
			char date[32] = "?";
			if (struct tm* t = gmtime(&now)) strftime(date, sizeof date, "%Y-%m-%d", t);
			return std::string("Your 3DS clock is wrong (it says ") + date +
			       "). Fix the date in System Settings";
		}
		if (d.find("BADCERT_NOT_TRUSTED") != std::string::npos)
			return "Untrusted certificate - this Wi-Fi may be intercepting HTTPS";
		if (d.find("BADCERT_CN_MISMATCH") != std::string::npos)
			return "Certificate name mismatch - this Wi-Fi may be redirecting traffic";
	}
	return d.empty() ? curl_easy_strerror(rc) : d;
}

static Response perform(CURL* h, curl_slist* list, const char* errbuf) {
	Response r;
	curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, writeString);
	curl_easy_setopt(h, CURLOPT_WRITEDATA, &r.body);
	CURLcode rc = curl_easy_perform(h);
	if (rc != CURLE_OK) {
		r.error = describeError(rc, errbuf);
		r.status = 0;
		util::logf("http error %d: %s (%s)", (int)rc, curl_easy_strerror(rc), errbuf);
	} else {
		curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &r.status);
		if (!r.ok()) r.error = "HTTP " + std::to_string(r.status);
	}
	if (list) curl_slist_free_all(list);
	return r;
}

static Response failed(const char* why) {
	Response r;
	r.error = why;
	return r;
}

Response Client::get(const std::string& url, const std::vector<std::string>& headers) {
	if (!h_) return failed("curl init failed");
	curl_slist* list;
	prepare(url, headers, &list);
	curl_easy_setopt(h_, CURLOPT_HTTPGET, 1L);
	return perform(h_, list, errbuf_);
}

Response Client::post(const std::string& url, const std::string& body, const std::vector<std::string>& headers) {
	if (!h_) return failed("curl init failed");
	curl_slist* list;
	prepare(url, headers, &list);
	curl_easy_setopt(h_, CURLOPT_POST, 1L);
	curl_easy_setopt(h_, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(h_, CURLOPT_POSTFIELDSIZE, (long)body.size());
	return perform(h_, list, errbuf_);
}

namespace {
struct RangeCtx {
	const std::function<bool()>* cancel;
	u64* total;
	std::vector<u8>* out;
	size_t cap;
};
}  // namespace

static size_t rangeWrite(char* ptr, size_t size, size_t nmemb, void* user) {
	auto* ctx = static_cast<RangeCtx*>(user);
	size_t n = size * nmemb;
	// A server that ignores Range would send the whole file; refuse to buffer it.
	if (ctx->out->size() + n > ctx->cap) return 0;
	ctx->out->insert(ctx->out->end(), (u8*)ptr, (u8*)ptr + n);
	return n;
}

static int rangeProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
	auto* ctx = static_cast<RangeCtx*>(user);
	return (ctx->cancel && *ctx->cancel && (*ctx->cancel)()) ? 1 : 0;
}

static size_t rangeHeader(char* buf, size_t size, size_t nitems, void* user) {
	auto* ctx = static_cast<RangeCtx*>(user);
	size_t n = size * nitems;
	static const char kKey[] = "content-range:";
	if (ctx->total && n > sizeof(kKey) && strncasecmp(buf, kKey, sizeof(kKey) - 1) == 0) {
		const char* slash = (const char*)memchr(buf, '/', n);
		if (slash && slash[1] != '*') *ctx->total = strtoull(slash + 1, nullptr, 10);
	}
	return n;
}

long Client::getRange(const std::string& url, u64 start, u64 end, std::vector<u8>& out,
                      const std::string& userAgent, const std::function<bool()>& shouldCancel,
                      std::string& error, u64* totalSize, std::string* effectiveUrl) {
	out.clear();
	if (!h_) {
		error = "curl init failed";
		return 0;
	}
	curl_slist* list;
	prepare(url, {}, &list);
	// Media is already compressed; don't ask for gzip.
	curl_easy_setopt(h_, CURLOPT_ACCEPT_ENCODING, nullptr);
	char range[64];
	snprintf(range, sizeof range, "%llu-%llu", (unsigned long long)start, (unsigned long long)end);
	curl_easy_setopt(h_, CURLOPT_RANGE, range);
	if (!userAgent.empty()) curl_easy_setopt(h_, CURLOPT_USERAGENT, userAgent.c_str());
	out.reserve((size_t)(end - start + 1));

	RangeCtx ctx{&shouldCancel, totalSize, &out, (size_t)(end - start + 1)};
	curl_easy_setopt(h_, CURLOPT_WRITEFUNCTION, rangeWrite);
	curl_easy_setopt(h_, CURLOPT_WRITEDATA, &ctx);
	curl_easy_setopt(h_, CURLOPT_HEADERFUNCTION, rangeHeader);
	curl_easy_setopt(h_, CURLOPT_HEADERDATA, &ctx);
	curl_easy_setopt(h_, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(h_, CURLOPT_XFERINFOFUNCTION, rangeProgress);
	curl_easy_setopt(h_, CURLOPT_XFERINFODATA, &ctx);

	CURLcode rc = curl_easy_perform(h_);
	long status = 0;
	if (rc == CURLE_OK) {
		curl_easy_getinfo(h_, CURLINFO_RESPONSE_CODE, &status);
		if (status != 206 && status != 200) error = "HTTP " + std::to_string(status);
		char* eff = nullptr;
		if (effectiveUrl && curl_easy_getinfo(h_, CURLINFO_EFFECTIVE_URL, &eff) == CURLE_OK && eff) *effectiveUrl = eff;
	} else {
		error = (rc == CURLE_ABORTED_BY_CALLBACK) ? "cancelled" : describeError(rc, errbuf_);
	}
	if (list) curl_slist_free_all(list);
	return status;
}

}  // namespace net

namespace net {

bool clockLooksWrong(std::string& dateOut) {
	// The build date is a floor: the console can't legitimately be earlier.
	static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
	const char* d = __DATE__;  // "Mmm dd yyyy"
	struct tm built = {};
	built.tm_mon = (int)((strstr(months, std::string(d, 3).c_str()) - months) / 3);
	built.tm_mday = atoi(d + 4);
	built.tm_year = atoi(d + 7) - 1900;
	time_t now = time(nullptr);
	char buf[32] = "?";
	if (struct tm* t = gmtime(&now)) strftime(buf, sizeof buf, "%Y-%m-%d", t);
	dateOut = buf;
	return now + 2 * 86400 < mktime(&built);
}

}  // namespace net
