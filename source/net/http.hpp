#pragma once

#include <3ds.h>

#include <functional>
#include <string>
#include <vector>

typedef void CURL;
struct curl_slist;

namespace net {

void globalInit();
void globalExit();
void setVerifyTls(bool verify);
// True if the console's date is before this build was made (so HTTPS
// certificate checks will fail). dateOut receives the console's date.
bool clockLooksWrong(std::string& dateOut);

struct Response {
	long status = 0;
	std::string body;
	std::string error;
	bool ok() const { return status >= 200 && status < 300; }
};

// One Client per thread. A Client keeps its connections alive between requests.
class Client {
public:
	Client();
	~Client();
	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;

	Response get(const std::string& url, const std::vector<std::string>& headers = {});
	Response post(const std::string& url, const std::string& body,
	              const std::vector<std::string>& headers = {});

	// Downloads bytes [start, end] (inclusive) into out. shouldCancel is polled
	// during the transfer. totalSize receives the full resource size from
	// Content-Range when available; effectiveUrl the URL after redirects.
	// Returns the HTTP status (0 on network error).
	long getRange(const std::string& url, u64 start, u64 end, std::vector<u8>& out,
	              const std::string& userAgent, const std::function<bool()>& shouldCancel,
	              std::string& error, u64* totalSize = nullptr, std::string* effectiveUrl = nullptr);

private:
	CURL* h_ = nullptr;
	char errbuf_[256];  // CURL_ERROR_SIZE: curl's detailed reason for a failure
	void prepare(const std::string& url, const std::vector<std::string>& headers, curl_slist** list);
};

}  // namespace net
