SRCS = \
	http_parser.c \
	request.c \
	server.c

OBJS = $(subst .c,.o,$(SRCS))
HEADERS = \
	http_parser.h \
	server.h

ifeq (Windows_NT,$(OS))
CFLAGS = -D_WIN32_WINNT=0x0600
LIBS = -luv -lws2_32 -lpsapi -liphlpapi
TARGET = server.exe
else
CFLAGS =
LIBS = -luv
TARGET = server
endif

all : $(TARGET)

$(TARGET) : $(OBJS) $(HEADERS)
	gcc -o $@ $(OBJS) $(LIBS)

.c.o :
	gcc -c $(CFLAGS) -I. $< -o $@

clean :
	rm -f *.o $(TARGET)
