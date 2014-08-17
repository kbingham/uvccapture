CC=gcc
CPP=g++
APP_BINARY=uvccapture
VERSION = 0.3

#WARNINGS = -Wall


CFLAGS = -std=gnu99 -O2 -DLINUX -DVERSION=\"$(VERSION)\" $(WARNINGS)
CPPFLAGS = $(CFLAGS)

OBJECTS= uvccapture.o v4l2uvc.o


all:    uvccapture

clean:
	@echo "Cleaning up directory."
	rm -f *.a *.o $(APP_BINARY) core *~ log errlog

# Applications:
uvccapture: $(OBJECTS)
	$(CC)   $(OBJECTS) $(XPM_LIB) $(MATH_LIB) -o $(APP_BINARY)
