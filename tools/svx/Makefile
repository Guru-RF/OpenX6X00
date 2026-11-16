# Host test of the SVXConnect app (APP > SVXConnect): `make` builds and runs them, nothing contacts a reflector.
# - svx_proto_test: src/svx_proto.c, what the app reads of its helper: the talkgroups and their priorities, who talks
#   on which, the recent overs, the selection and the lock, the transmission's events, the certificate.
# - svx_link_test: src/voice_link.c, the Simplex repeater Zello and SVXConnect share, with SVXConnect's name: the
#   cases of tools/zello/zello_link_test.c (only inside the amateur bands, both ways, the limits, the operator first,
#   no loop, the CW ID) and the words that carry the name.
# - svx_helper_test.py: svxconnect --stdio (tools/svxconnect/build.sh's native build, SVX_BIN= another) as the app
#   runs it, against SVXConnect's fake reflector on 127.0.0.1, its lines read by src/svx_proto.c (svx_proto_dump).
#   The first build clones SVXConnect-CLI from GitHub; later ones take it from $HOME/.cache/openx6x00/svxconnect.
SRC    := ../../src
CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O1 -g -I $(SRC)

all: test

svx_proto_test: svx_proto_test.c $(SRC)/svx_proto.c $(SRC)/svx_proto.h
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ svx_proto_test.c $(SRC)/svx_proto.c

svx_proto_dump: svx_proto_dump.c $(SRC)/svx_proto.c $(SRC)/svx_proto.h
	$(CC) $(CFLAGS) -o $@ svx_proto_dump.c $(SRC)/svx_proto.c

svx_link_test: svx_link_test.c ../zello/zello_link_test.c $(SRC)/voice_link.c $(SRC)/voice_link.h \
               $(SRC)/cfg/ham_bands.c
	$(CC) $(CFLAGS) -o $@ svx_link_test.c $(SRC)/voice_link.c $(SRC)/cfg/ham_bands.c -lm

test: svx_proto_test svx_link_test svx_proto_dump
	./svx_proto_test
	./svx_link_test
	python3 -I -B svx_helper_test.py

clean:
	rm -f svx_proto_test svx_proto_dump svx_link_test

.PHONY: all test clean
