SRCS = \
	http_parser.c \
	request.c \
	server.c

OBJS = $(subst .c,.o,$(SRCS))
HEADERS = \
	http_parser.h \
	server.h

ifeq (Windows_NT,$(OS))
CFLAGS = -Ilibuv/include -D_WIN32_WINNT=0x0600
LIBS = -Llibuv -luv -lws2_32 -lpsapi -liphlpapi
TARGET = http-server.exe
else
CFLAGS = -Ilibuv/include -O3 -g
LIBS = -Llibuv/.libs -luv -lpthread -lrt -g
TARGET = http-server
endif

all : $(TARGET)

$(TARGET) : $(OBJS) $(HEADERS)
	gcc -o $@ $(OBJS) $(LIBS)

.c.o :
	gcc -c $(CFLAGS) -I. $< -o $@

clean :
	rm -f *.o $(TARGET)
