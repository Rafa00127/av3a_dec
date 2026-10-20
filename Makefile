# av3a_dec.dll (w64devkit / MinGW-w64)
#
#   make          -> av3a_dec.dll
#   make clean
#
# model.bin is not included; put it next to the DLL at runtime.

SRC := $(wildcard src/codec/libavs3_common/*.c) \
       $(wildcard src/codec/avs2p3Decoder/*.c) \
       $(wildcard src/codec/avs2p3Decoder/*/*.c) \
       $(wildcard src/codec/avs2p3Decoder/*/*/*.c) \
       $(wildcard src/codec/avs3Decoder/src/*.c) \
       src/av3a_dec.c
SRC := $(filter-out %/crc_16_temp.c,$(SRC))
SRC := $(filter-out %/bitstream_temp.c,$(SRC))

INC := -Isrc/codec/libavs3_common \
       -Isrc/codec/avs3Decoder/include \
       -Isrc/codec/avs2p3Decoder/header \
       -Isrc/codec/avs2p3Decoder/source \
       -Isrc/codec/avs2p3Decoder/threeD \
       -Isrc/codec/avs2p3Decoder/general/decode \
       -Isrc/codec/avs2p3Decoder/general/bwedec \
       -Isrc/codec/avs2p3Decoder/libAFsp/Include \
       -Isrc/codec/avs2p3Decoder/libAFsp/Include/libtsp

CFLAGS := -O3 -fcommon -std=c99 -w -Wno-error \
          -Wno-error=implicit-int \
          -Wno-error=implicit-function-declaration \
          -Wno-error=int-conversion \
          -Wno-error=incompatible-pointer-types \
          -Wno-error=return-mismatch \
          -Wno-error=declaration-missing-parameter-type

all: av3a_dec.dll

av3a_dec.dll: $(SRC)
	$(CC) $(CFLAGS) $(INC) -shared -o $@ $^ -lm

clean:
	rm -f av3a_dec.dll

.PHONY: all clean
