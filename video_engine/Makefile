CC = gcc
ifeq ($(OS),Windows_NT)
    CFLAGS = -O2 -march=x86-64 -mtune=generic -Wall -Wextra -fstack-protector-strong -D_FORTIFY_SOURCE=2 -shared
    LDFLAGS = -Wl,--dynamicbase,--nxcompat
    LIBS = -lmfplat -lmfuuid -ld3d11 -ldxgi -lws2_32 -lgdi32 -lole32 -luuid -lbcrypt -lssp
    TARGET = video_engine.dll
    SRC = video_engine.c
    RM = del /Q $(TARGET) 2>nul || exit 0
else
    CFLAGS = -O2 -march=x86-64 -mtune=generic -Wall -Wextra -fPIC -fstack-protector-strong -D_FORTIFY_SOURCE=2 -shared
    LDFLAGS =
    LIBS = -lX11 -lXext -lcrypto -lpthread
    TARGET = video_engine.so
    SRC = video_engine_linux.c
    RM = rm -f $(TARGET)
endif

all: $(TARGET)

$(TARGET): $(SRC) ../audio_engine/c_crypto.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	$(RM)
