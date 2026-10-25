CC = gcc
ifeq ($(OS),Windows_NT)
    CFLAGS = -O2 -march=x86-64 -mtune=generic -Wall -Wextra -fstack-protector-strong -D_FORTIFY_SOURCE=2 -shared
    LDFLAGS = -Wl,--dynamicbase,--nxcompat
    LIBS = -lws2_32 -lwinmm -lole32 -luuid -lbcrypt -lssp
    TARGET = audio_engine.dll
    SRC = audio_engine.c
    RM = del /Q $(TARGET) 2>nul || exit 0
else
    CFLAGS = -O2 -march=x86-64 -mtune=generic -Wall -Wextra -fPIC -fstack-protector-strong -D_FORTIFY_SOURCE=2 -shared
    LDFLAGS =
    LIBS = -lpulse-simple -lpulse -lasound -lcrypto -lpthread
    TARGET = audio_engine.so
    SRC = audio_engine_linux.c
    RM = rm -f $(TARGET)
endif

all: $(TARGET)

$(TARGET): $(SRC) c_crypto.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $(SRC) $(LIBS)

clean:
	$(RM)
