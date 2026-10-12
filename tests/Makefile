# Host-side tests for the platform-independent parts of Tube3DS.
SRC := ../source
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wno-unused-function -Ihost_stub -I$(SRC) -DTUBE_HOST_TEST

all: host_api_test host_media_test
	./host_api_test
	./host_media_test

# Host FFmpeg (e.g. Homebrew's). On Apple Silicon with an x86_64 Homebrew, set
# HOST_ARCH=-arch x86_64.
FFMPEG_PREFIX ?= /usr/local
HOST_ARCH ?= $(if $(filter x86_64,$(shell lipo -archs $(FFMPEG_PREFIX)/lib/libavformat.dylib 2>/dev/null)),-arch x86_64,)

host_media_test: host_media_test.cpp $(SRC)/media/block_stream.cpp $(SRC)/yt/json.cpp $(SRC)/yt/innertube.cpp $(SRC)/net/http.cpp $(SRC)/util.cpp
	$(CXX) $(HOST_ARCH) $(CXXFLAGS) -I$(FFMPEG_PREFIX)/include $^ -L$(FFMPEG_PREFIX)/lib -lavformat -lavcodec -lavutil -lcurl -o $@

host_api_test: host_api_test.cpp $(SRC)/yt/json.cpp $(SRC)/yt/innertube.cpp $(SRC)/net/http.cpp $(SRC)/util.cpp
	$(CXX) $(CXXFLAGS) $^ -lcurl -o $@

clean:
	rm -f host_api_test host_media_test
.PHONY: all clean
