TAG?=master
V?=5.2.x
P?=p1

ASRSRC=		/usr/src/lib/libc/asr
ASRFILES=	asr.c asr_debug.c asr_utils.c gethostnamadr_async.c	\
		res_send_async.c getaddrinfo_async.c getnameinfo_async.c \
		asr.h asr_private.h

T=/tmp/_build

all:

tarball:
	mkdir -p files
	rm -rf ${T}
	mkdir ${T}
	git archive --format=tar --prefix=opensmtpd-${V}/ ${TAG}:smtpd/ | \
	    (cd ${T} && tar xf -)
	for i in ${ASRFILES}; do cp ${ASRSRC}/$$i ${T}/opensmtpd-${V}/; done
	cat ${T}/opensmtpd-${V}/smtpd/Makefile |	\
	    sed 's/-I\/usr\/src\/lib\/libc\/asr//' |	\
	    sed 's/\/usr\/src\/lib\/libc\/asr//' > ${T}/_makefile
	cat ${T}/_makefile > ${T}/opensmtpd-${V}/smtpd/Makefile
	(cd ${T} && tar cfz ${.CURDIR}/files/opensmtpd-${V}.tar.gz opensmtpd-${V})

portable:
	mkdir -p files
	rm -rf ${T}
	mkdir ${T}
	git archive --format=tar --prefix=opensmtpd-${V}${P}/ portable | \
		(cd ${T} && tar xf -)
	rm -f ${T}/opensmtpd-${V}${P}/Makefile \
	      ${T}/opensmtpd-${V}${P}/smtpd/Makefile
	(cd ${T} && tar cfz ${.CURDIR}/files/opensmtpd-${V}${P}.tar.gz opensmtpd-${V}${P})
