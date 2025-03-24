.PHONY: wired

all: libvector-gobot wired

go_deps:
	echo `/usr/local/go/bin/go version` && cd $(PWD) && /usr/local/go/bin/go mod download

libvector-gobot:
	cd vector-gobot && make GCC=${HOME}/.anki/vicos-sdk/dist/1.1.0-r04/prebuilt/bin/arm-oe-linux-gnueabi-clang GPP=${HOME}/.anki/vicos-sdk/dist/1.1.0-r04/prebuilt/bin/arm-oe-linux-gnueabi-clang++
	cp vector-gobot/build/libvector-gobot.so build/

wired: go_deps
	echo $(PWD)/vector-gobot/build
	CGO_LDFLAGS="-L$(PWD)/vector-gobot/build/ -Wl,-rpath-link,${HOME}/.anki/vicos-sdk/dist/1.1.0-r04/sysroot/lib -Wl,-rpath-link,${HOME}/.anki/vicos-sdk/dist/1.1.0-r04/sysroot/usr/lib -latomic" \
	CGO_CFLAGS="-I$(PWD)/vector-gobot/include" \
	CGO_ENABLED=1 GOARM=7 GOARCH=arm CC=/home/build/.anki/vicos-sdk/dist/1.1.0-r04/prebuilt/bin/arm-oe-linux-gnueabi-clang \
	CXX=/home/build/.anki/vicos-sdk/dist/1.1.0-r04/prebuilt/bin/arm-oe-linux-gnueabi-clang++ \
	 /usr/local/go/bin/go build -tags vicos -ldflags '-w -s' -o build/wired main.go
	upx build/wired


#vic-gateway: go_deps
#	CGO_ENABLED=1 GOARM=7 GOARCH=arm CC=/home/build/.anki/vicos-sdk/dist/1.1.0-r04/prebuilt/bin/arm-oe-linux-gnueabi-clang CXX=/home/build/.anki/vicos-sdk/dist/1.1.0-r04/prebuilt/bin/arm-oe-linux-gnueabi-clang++ PKG_CONFIG_PATH="$(PWD)/armlibs/lib/pkgconfig" CGO_CFLAGS="-I$(PWD)/armlibs/include -I$(PWD)/armlibs/include/opus -I$(PWD)/armlibs/include/ogg" CGO_CXXFLAGS="-stdlib=libc++ -std=c++11" CGO_LDFLAGS="-L$(PWD)/armlibs/lib -L$(PWD)/armlibs/lib/arm-linux-gnueabi/android" /usr/local/go/bin/go build -tags nolibopusfile,vicos -ldflags '-w -s -linkmode internal -extldflags "-static" -r /anki/lib' -o build/vic-gateway gateway/*.go
#
#	upx build/vic-gateway

