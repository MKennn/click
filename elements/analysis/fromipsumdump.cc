// -*- mode: c++; c-basic-offset: 4 -*-
/*
 * fromipsumdump.{cc,hh} -- element reads packets from IP summary dump file
 * Eddie Kohler
 *
 * Copyright (c) 2001 International Computer Science Institute
 * Copyright (c) 2008 Meraki, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>

#include "fromipsumdump.hh"
#include <click/confparse.hh>
#include <click/router.hh>
#include <click/standard/scheduleinfo.hh>
#include <click/error.hh>
#include <click/glue.hh>
#include <click/straccum.hh>
#include <clicknet/ip.h>
#include <clicknet/udp.h>
#include <clicknet/tcp.h>
#include <clicknet/icmp.h>
#include <click/packet_anno.hh>
#include <click/nameinfo.hh>
#include <click/userutils.hh>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
CLICK_DECLS

#ifdef i386
# define GET4(p)	ntohl(*reinterpret_cast<const uint32_t *>((p)))
# define GET2(p)	ntohs(*reinterpret_cast<const uint16_t *>((p)))
#else
# define GET4(p)	(((p)[0]<<24) | ((p)[1]<<16) | ((p)[2]<<8) | (p)[3])
# define GET2(p)	(((p)[0]<<8) | (p)[1])
#endif
#define GET1(p)		((p)[0])

FromIPSummaryDump::FromIPSummaryDump()
    : _work_packet(0), _task(this), _timer(this)
{
    _ff.set_landmark_pattern("%f:%l");
}

FromIPSummaryDump::~FromIPSummaryDump()
{
}

void *
FromIPSummaryDump::cast(const char *n)
{
    if (strcmp(n, Notifier::EMPTY_NOTIFIER) == 0 && !output_is_push(0))
	return static_cast<Notifier *>(&_notifier);
    else
	return Element::cast(n);
}

int
FromIPSummaryDump::configure(Vector<String> &conf, ErrorHandler *errh)
{
    bool stop = false, active = true, zero = true, checksum = false, multipacket = false, timing = false;
    uint8_t default_proto = IP_PROTO_TCP;
    _sampling_prob = (1 << SAMPLING_SHIFT);
    String default_contents, default_flowid;
    
    if (cp_va_kparse(conf, this, errh,
		     "FILENAME", cpkP+cpkM, cpFilename, &_ff.filename(),
		     "STOP", 0, cpBool, &stop,
		     "ACTIVE", 0, cpBool, &active,
		     "ZERO", 0, cpBool, &zero,
		     "TIMING", 0, cpBool, &timing,
		     "CHECKSUM", 0, cpBool, &checksum,
		     "SAMPLE", 0, cpUnsignedReal2, SAMPLING_SHIFT, &_sampling_prob,
		     "PROTO", 0, cpByte, &default_proto,
		     "MULTIPACKET", 0, cpBool, &multipacket,
		     "DEFAULT_CONTENTS", 0, cpArgument, &default_contents,
		     "DEFAULT_FLOWID", 0, cpArgument, &default_flowid,
		     "CONTENTS", 0, cpArgument, &default_contents,
		     "FLOWID", 0, cpArgument, &default_flowid,
		     cpEnd) < 0)
	return -1;
    if (_sampling_prob > (1 << SAMPLING_SHIFT)) {
	errh->warning("SAMPLE probability reduced to 1");
	_sampling_prob = (1 << SAMPLING_SHIFT);
    } else if (_sampling_prob == 0)
	errh->warning("SAMPLE probability is 0; emitting no packets");

    _default_proto = default_proto;
    _stop = stop;
    _active = active;
    _zero = zero;
    _checksum = checksum;
    _timing = timing;
    _have_timing = false;
    _multipacket = multipacket;
    _have_flowid = _have_aggregate = _binary = false;
    if (default_contents)
	bang_data(default_contents, errh);
    if (default_flowid)
	bang_flowid(default_flowid, errh);
    return 0;
}

int
FromIPSummaryDump::read_binary(String &result, ErrorHandler *errh)
{
    assert(_binary);

    uint8_t record_storage[4];
    const uint8_t *record = _ff.get_unaligned(4, record_storage, errh);
    if (!record)
	return 0;
    int record_length = GET4(record) & 0x7FFFFFFFU;
    if (record_length < 4)
	return _ff.error(errh, "binary record too short");
    bool textual = (record[0] & 0x80 ? true : false);
    result = _ff.get_string(record_length - 4, errh);
    if (!result)
	return 0;
    if (textual) {
	const char *s = result.begin(), *e = result.end();
	while (e > s && e[-1] == 0)
	    e--;
	if (e != result.end())
	    result = result.substring(s, e);
    }
    _ff.set_lineno(_ff.lineno() + 1);
    return (textual ? 2 : 1);
}

int
FromIPSummaryDump::initialize(ErrorHandler *errh)
{
    // make sure notifier is initialized
    if (!output_is_push(0))
	_notifier.initialize(Notifier::EMPTY_NOTIFIER, router());
    _timer.initialize(router());
    
    if (_ff.initialize(errh) < 0)
	return -1;
    
    _minor_version = IPSummaryDump::MINOR_VERSION; // expected minor version
    String line;
    if (_ff.peek_line(line, errh, true) < 0)
	return -1;
    else if (line.substring(0, 14) == "!IPSummaryDump") {
	int major_version;
	if (sscanf(line.c_str() + 14, " %d.%d", &major_version, &_minor_version) == 2) {
	    if (major_version != IPSummaryDump::MAJOR_VERSION || _minor_version > IPSummaryDump::MINOR_VERSION) {
		_ff.warning(errh, "unexpected IPSummaryDump version %d.%d", major_version, _minor_version);
		_minor_version = IPSummaryDump::MINOR_VERSION;
	    }
	}
	(void) _ff.read_line(line, errh, true); // throw away line
    } else {
	// parse line again, warn if this doesn't look like a dump
	if (line.substring(0, 8) != "!creator" && line.substring(0, 5) != "!data" && line.substring(0, 9) != "!contents") {
	    if (!_fields.size() /* don't warn on DEFAULT_CONTENTS */)
		_ff.warning(errh, "missing banner line; is this an IP summary dump?");
	}
    }
    
    _format_complaint = false;
    if (output_is_push(0))
	ScheduleInfo::initialize_task(this, &_task, _active, errh);
    return 0;
}

void
FromIPSummaryDump::cleanup(CleanupStage)
{
    _ff.cleanup();
    if (_work_packet)
	_work_packet->kill();
    _work_packet = 0;
}

int
FromIPSummaryDump::sort_fields_compare(const void *ap, const void *bp,
				       void *user_data)
{
    int a = *reinterpret_cast<const int *>(ap);
    int b = *reinterpret_cast<const int *>(bp);
    FromIPSummaryDump *f = reinterpret_cast<FromIPSummaryDump *>(user_data);
    const IPSummaryDump::FieldReader *fa = f->_fields[a];
    const IPSummaryDump::FieldReader *fb = f->_fields[b];
    if (fa->order < fb->order)
	return -1;
    if (fa->order > fb->order)
	return 1;
    return (a < b ? -1 : (a == b ? 0 : 1));
}

void
FromIPSummaryDump::bang_data(const String &line, ErrorHandler *errh)
{
    Vector<String> words;
    cp_spacevec(line, words);

    _fields.clear();
    _field_order.clear();
    for (int i = 0; i < words.size(); i++) {
	String word = cp_unquote(words[i]);
	if (i == 0 && (word == "!data" || word == "!contents"))
	    continue;
	const IPSummaryDump::FieldReader *f = IPSummaryDump::FieldReader::find(word);
	if (!f) {
	    _ff.warning(errh, "unknown content type '%s'", word.c_str());
	    f = &IPSummaryDump::null_reader;
	} else if (!f->inject) {
	    _ff.warning(errh, "content type '%s' ignored on input", word.c_str());
	    f = &IPSummaryDump::null_reader;
	}
	_fields.push_back(f);
	_field_order.push_back(_fields.size() - 1);
    }

    if (_fields.size() == 0)
	_ff.error(errh, "no contents specified");

    click_qsort(_field_order.begin(), _fields.size(), sizeof(int),
		sort_fields_compare, this);
}

void
FromIPSummaryDump::bang_flowid(const String &line, ErrorHandler *errh)
{
    Vector<String> words;
    cp_spacevec(line, words);

    IPAddress src, dst;
    uint32_t sport = 0, dport = 0, proto = 0;
    if (words.size() < 5
	|| (!cp_ip_address(words[1], &src) && words[1] != "-")
	|| (!cp_integer(words[2], &sport) && words[2] != "-")
	|| (!cp_ip_address(words[3], &dst) && words[3] != "-")
	|| (!cp_integer(words[4], &dport) && words[4] != "-")
	|| sport > 65535 || dport > 65535) {
	_ff.error(errh, "bad !flowid specification");
	_have_flowid = false;
    } else {
	if (words.size() >= 6) {
	    if (cp_integer(words[5], &proto) && proto < 256)
		_default_proto = proto;
	    else if (words[5] == "T")
		_default_proto = IP_PROTO_TCP;
	    else if (words[5] == "U")
		_default_proto = IP_PROTO_UDP;
	    else if (words[5] == "I")
		_default_proto = IP_PROTO_ICMP;
	    else
		_ff.error(errh, "bad protocol in !flowid");
	}
	_given_flowid = IPFlowID(src, htons(sport), dst, htons(dport));
	_have_flowid = true;
    }
}

void
FromIPSummaryDump::bang_aggregate(const String &line, ErrorHandler *errh)
{
    Vector<String> words;
    cp_spacevec(line, words);

    if (words.size() != 2
	|| !cp_integer(words[1], &_aggregate)) {
	_ff.error(errh, "bad !aggregate specification");
	_have_aggregate = false;
    } else
	_have_aggregate = true;
}

void
FromIPSummaryDump::bang_binary(const String &line, ErrorHandler *errh)
{
    Vector<String> words;
    cp_spacevec(line, words);
    if (words.size() != 1)
	_ff.error(errh, "bad !binary specification");
    _binary = true;
    _ff.set_landmark_pattern("%f:record %l");
    _ff.set_lineno(1);
}

static void
set_checksums(WritablePacket *q, click_ip *iph)
{
    assert(iph == q->ip_header());
    
    iph->ip_sum = 0;
    iph->ip_sum = click_in_cksum((uint8_t *)iph, iph->ip_hl << 2);

    if (IP_ISFRAG(iph))
	/* nada */;
    else if (iph->ip_p == IP_PROTO_TCP) {
	click_tcp *tcph = q->tcp_header();
	tcph->th_sum = 0;
	unsigned csum = click_in_cksum((uint8_t *)tcph, q->transport_length());
	tcph->th_sum = click_in_cksum_pseudohdr(csum, iph, q->transport_length());
    } else if (iph->ip_p == IP_PROTO_UDP) {
	click_udp *udph = q->udp_header();
	udph->uh_sum = 0;
	unsigned csum = click_in_cksum((uint8_t *)udph, q->transport_length());
	udph->uh_sum = click_in_cksum_pseudohdr(csum, iph, q->transport_length());
    }
}

Packet *
FromIPSummaryDump::read_packet(ErrorHandler *errh)
{
    // read non-packet lines
    bool binary;
    String line;
    const char *data;
    const char *end;
    
    while (1) {
	if ((binary = _binary)) {
	    int result = read_binary(line, errh);
	    if (result <= 0)
		goto eof;
	    else
		binary = (result == 1);
	} else if (_ff.read_line(line, errh, true) <= 0) {
	  eof:
	    _ff.cleanup();
	    return 0;
	}

	data = line.begin();
	end = line.end();

	if (data == end)
	    /* do nothing */;
	else if (binary || (data[0] != '!' && data[0] != '#'))
	    /* real packet */
	    break;

	// parse bang lines
	if (data[0] == '!') {
	    if (data + 6 <= end && memcmp(data, "!data", 5) == 0 && isspace((unsigned char) data[5]))
		bang_data(line, errh);
	    else if (data + 8 <= end && memcmp(data, "!flowid", 7) == 0 && isspace((unsigned char) data[7]))
		bang_flowid(line, errh);
	    else if (data + 11 <= end && memcmp(data, "!aggregate", 10) == 0 && isspace((unsigned char) data[10]))
		bang_aggregate(line, errh);
	    else if (data + 8 <= end && memcmp(data, "!binary", 7) == 0 && isspace((unsigned char) data[7]))
		bang_binary(line, errh);
	    else if (data + 10 <= end && memcmp(data, "!contents", 9) == 0 && isspace((unsigned char) data[9]))
		bang_data(line, errh);
	}
    }

    // read packet data
    WritablePacket *q = Packet::make(14, (const unsigned char *) 0, 0, 1000);
    if (!q) {
	_ff.error(errh, strerror(ENOMEM));
	return 0;
    }
    if (_zero)
	memset(q->buffer(), 0, q->buffer_length());

    // prepare packet data
    StringAccum sa;
    IPSummaryDump::PacketOdesc d(this, q, _default_proto, (_have_flowid ? &_flowid : 0));
    int nfields = 0;

    // new code goes here
    if (_binary) {
	Vector<const unsigned char *> args;
	int nbytes;
	for (const IPSummaryDump::FieldReader * const *fp = _fields.begin(); fp != _fields.end(); ++fp) {
	    if (!(*fp)->inb)
		goto bad_field;
	    switch ((*fp)->type) {
	      case IPSummaryDump::B_0:
		nbytes = 0;
		goto got_nbytes;
	      case IPSummaryDump::B_1:
		nbytes = 1;
		goto got_nbytes;
	      case IPSummaryDump::B_2:
		nbytes = 2;
		goto got_nbytes;
	      case IPSummaryDump::B_4:
	      case IPSummaryDump::B_4NET:
		nbytes = 4;
		goto got_nbytes;
	      case IPSummaryDump::B_6PTR:
		nbytes = 6;
		goto got_nbytes;
	      case IPSummaryDump::B_8:
		nbytes = 8;
		goto got_nbytes;
	      case IPSummaryDump::B_16:
		nbytes = 16;
		goto got_nbytes;
	      got_nbytes:
		if (data + nbytes <= end) {
		    args.push_back((const unsigned char *) data);
		    data += nbytes;
		} else
		    goto bad_field;
		break;
	      case IPSummaryDump::B_SPECIAL:
		args.push_back((const unsigned char *) data);
		data = (const char *) (*fp)->inb(d, (const uint8_t *) data, (const uint8_t *) end, *fp);
		break;
	      bad_field:
	      default:
		args.push_back(0);
		data = end;
		break;
	    }
	}

	for (int *fip = _field_order.begin();
	     fip != _field_order.end() && d.p;
	     ++fip) {
	    const IPSummaryDump::FieldReader *f = _fields[*fip];
	    if (!args[*fip] || !f->inject)
		continue;
	    d.clear_values();
	    if (f->inb(d, args[*fip], (const uint8_t *) end, f)) {
		f->inject(d, f);
		nfields++;
	    }
	}
	
    } else {
	Vector<String> args;
	while (args.size() < _fields.size()) {
	    const char *original_data = data;
	    while (data < end)
		if (isspace((unsigned char) *data))
		    break;
		else if (*data == '\"')
		    data = cp_skip_double_quote(data, end);
		else
		    ++data;
	    args.push_back(line.substring(original_data, data));
	    while (data < end && isspace((unsigned char) *data))
		++data;
	}

	for (int *fip = _field_order.begin();
	     fip != _field_order.end() && d.p;
	     ++fip) {
	    const IPSummaryDump::FieldReader *f = _fields[*fip];
	    if (!args[*fip] || args[*fip].equals("-", 1) || !f->inject)
		continue;
	    d.clear_values();
	    if (f->ina(d, args[*fip], f)) {
		f->inject(d, f);
		nfields++;
	    }
	}
    }

    if (!nfields) {	// bad format
	if (!_format_complaint) {
	    // don't complain if the line was all blank
	    if (binary || (int) strspn(line.data(), " \t\n\r") != line.length()) {
		if (_fields.size() == 0)
		    _ff.error(errh, "no '!data' provided");
		else
		    _ff.error(errh, "packet parse error");
		_format_complaint = true;
	    }
	}
	if (d.p)
	    d.p->kill();
	d.p = 0;
    }

    // set source and destination ports even if no transport info on packet
    if (d.p && d.default_ip_flowid)
	if (d.make_ip(0))
	    d.make_transp();	// may fail

    if (d.p && d.is_ip) {
	// set IP length
	if (!d.p->ip_header()->ip_len) {
	    int len = d.p->network_length() + EXTRA_LENGTH_ANNO(d.p);
	    if (len > 0xFFFF)
		len = 0xFFFF;
	    d.p->ip_header()->ip_len = htons(len);
	}

	// set UDP length
	if (d.p->ip_header()->ip_p == IP_PROTO_UDP
	    && IP_FIRSTFRAG(d.p->ip_header())
	    && !d.p->udp_header()->uh_ulen) {
	    int len = htons(d.p->ip_header()->ip_len) - d.p->network_header_length();
	    d.p->udp_header()->uh_ulen = htons(len);
	}

	// set extra length annotation (post-IP length adjustment)
	SET_EXTRA_LENGTH_ANNO(d.p, ntohs(d.p->ip_header()->ip_len) - d.p->length());

	// set destination IP address annotation
	d.p->set_dst_ip_anno(d.p->ip_header()->ip_dst);

	// set checksum
	if (_checksum)
	    set_checksums(d.p, d.p->ip_header());
    }

    return d.p;

#if 0
    q->set_ip_header((click_ip *)q->data(), sizeof(click_ip));
    click_ip *iph = q->ip_header();
    iph->ip_v = 4;
    iph->ip_hl = sizeof(click_ip) >> 2;
    iph->ip_p = _default_proto;
    iph->ip_off = 0;
    
    StringAccum payload;
    String ip_opt;
    String tcp_opt;

    int ok = (binary ? 1 : 0);
    int ip_ok = 0;
    uint8_t ip_hl = 0;
    uint8_t tcp_off = 0;
    int16_t icmp_type = -1;
    uint32_t ip_len = 0;
    uint32_t payload_len = 0;
    bool have_payload_len = false;
    bool have_payload = false;
    
    for (int i = 0; data < end && i < _contents.size(); i++) {
	const unsigned char *original_data = data;
	const unsigned char *next;
	uint32_t u1 = 0, u2 = 0;

	// check binary case
	if (binary) {
	    switch (_contents[i]) {
	      case W_NONE:
		break;
	      case W_TIMESTAMP:
	      case W_FIRST_TIMESTAMP:
		u1 = GET4(data);
		u2 = GET4(data + 4) * 1000;
		data += 8;
		break;
	      case W_NTIMESTAMP:
	      case W_FIRST_NTIMESTAMP:
		u1 = GET4(data);
		u2 = GET4(data + 4);
		data += 8;
		break;
	      case W_TIMESTAMP_USEC1:
		u1 = GET4(data);
		u2 = GET4(data + 4);
		data += 8;
		break;
	      case W_TIMESTAMP_SEC:
	      case W_TIMESTAMP_USEC:
	      case W_IP_LEN:
	      case W_PAYLOAD_LEN:
	      case W_IP_CAPTURE_LEN:
	      case W_TCP_SEQ:
	      case W_TCP_ACK:
	      case W_COUNT:
	      case W_AGGREGATE:
	      case W_IP_SRC:
	      case W_IP_DST:
		u1 = GET4(data);
		data += 4;
		break;
	      case W_IP_ID:
	      case W_SPORT:
	      case W_DPORT:
	      case W_IP_FRAGOFF:
	      case W_TCP_WINDOW:
	      case W_TCP_URP:
		u1 = GET2(data);
		data += 2;
		break;
	      case W_IP_PROTO:
	      case W_TCP_FLAGS:
	      case W_LINK:
	      case W_IP_TOS:
	      case W_IP_TTL:
	      case W_IP_HL:
	      case W_TCP_OFF:
	      case W_ICMP_TYPE:
	      case W_ICMP_CODE:
		u1 = GET1(data);
		data++;
		break;
	      case W_IP_FRAG:
		// XXX less checking here
		if (*data == 'F')
		    u1 = htons(IP_MF);
		else if (*data == 'f')
		    u1 = htons(100); // random number
		data++;	// u1 already 0
		break;
	      case W_IP_OPT: {
		  const unsigned char *endopt = data + 1 + *data;
		  if (endopt <= end) {
		      ip_opt = line.substring((const char *) data + 1, (const char *) endopt);
		      data = endopt;
		  }
		  break;
	      }
	      case W_TCP_OPT:
	      case W_TCP_NTOPT:
	      case W_TCP_SACK: {
		  const unsigned char *endopt = data + 1 + *data;
		  if (endopt <= end) {
		      tcp_opt = line.substring((const char *) data + 1, (const char *) endopt);
		      data = endopt;
		  }
		  break;
	      }
	      case W_PAYLOAD_MD5: // ignore contents
		data += 16;
		break;
	    }
	    goto store_contents;
	}

	// otherwise, ascii
	// first, parse contents
	switch (_contents[i]) {

	  case W_NONE:
	    while (data < end && !isspace(*data))
		data++;
	    break;

	  case W_TIMESTAMP:
	  case W_NTIMESTAMP:
	  case W_FIRST_TIMESTAMP:
	  case W_FIRST_NTIMESTAMP:
	    next = cp_integer(data, end, 10, &u1);
	    if (next > data) {
		data = next;
		if (data + 1 < end && *data == '.') {
		    int digit = 0;
		    for (data++; digit < 9 && data < end && isdigit(*data); digit++, data++)
			u2 = (u2 * 10) + *data - '0';
		    for (; digit < 9; digit++)
			u2 = (u2 * 10);
		    for (; data < end && isdigit(*data); data++)
			/* nada */;
		}
	    }
	    break;
	    
	  case W_TIMESTAMP_SEC:
	  case W_TIMESTAMP_USEC:
	  case W_IP_LEN:
	  case W_PAYLOAD_LEN:
	  case W_IP_CAPTURE_LEN:
	  case W_IP_ID:
	  case W_SPORT:
	  case W_DPORT:
	  case W_TCP_SEQ:
	  case W_TCP_ACK:
	  case W_COUNT:
	  case W_AGGREGATE:
	  case W_TCP_WINDOW:
	  case W_TCP_URP:
	  case W_IP_TOS:
	  case W_IP_TTL:
	  case W_IP_HL:
	  case W_TCP_OFF:
	    data = cp_integer(data, end, 0, &u1);
	    break;

	  case W_ICMP_TYPE:
	    if (data != end && isdigit((unsigned char) *data))
		data = cp_integer(data, end, 0, &u1);
	    else {
		const unsigned char *first = data;
		while (data != end && !isspace((unsigned char) *data))
		    ++data;
		if (!NameInfo::query_int(NameInfo::T_ICMP_TYPE, this,
					 line.substring((const char *) first, (const char *) data), &u1))
		    data = first;
	    }
	    break;

	  case W_ICMP_CODE:
	    if (data != end && isdigit((unsigned char) *data))
		data = cp_integer(data, end, 0, &u1);
	    else if (icmp_type >= 0) {
		const unsigned char *first = data;
		while (data != end && !isspace((unsigned char) *data))
		    ++data;
		if (!NameInfo::query_int(NameInfo::T_ICMP_CODE + icmp_type, this,
					 line.substring((const char *) first, (const char *) data), &u1))
		    data = first;
	    }
	    break;

	  case W_TIMESTAMP_USEC1: {
#if HAVE_INT64_TYPES
	      uint64_t uu;
	      data = cp_integer(data, end, 0, &uu);
	      u1 = (uint32_t)(uu >> 32);
	      u2 = (uint32_t) uu;
#else
	      // silently truncate large numbers
	      data = cp_integer(data, end, 0, &u2);
#endif
	      break;
	  }
	    
	  case W_IP_SRC:
	  case W_IP_DST:
	    for (int j = 0; j < 4; j++) {
		const unsigned char *first = data;
		int x = 0;
		while (data < end && isdigit(*data) && x < 256)
		    (x = (x * 10) + *data - '0'), data++;
		if (x >= 256 || data == first || (j < 3 && (data >= end || *data != '.'))) {
		    data = original_data;
		    break;
		}
		u1 = (u1 << 8) + x;
		if (j < 3)
		    data++;
	    }
	    break;

	  case W_IP_PROTO:
	    if (*data == 'T') {
		u1 = IP_PROTO_TCP;
		data++;
	    } else if (*data == 'U') {
		u1 = IP_PROTO_UDP;
		data++;
	    } else if (*data == 'I') {
		u1 = IP_PROTO_ICMP;
		data++;
	    } else
		data = cp_integer(data, end, 0, &u1);
	    break;

	  case W_IP_FRAG:
	    if (*data == 'F') {
		u1 = htons(IP_MF);
		data++;
	    } else if (*data == 'f') {
		u1 = htons(100);	// random number
		data++;
	    } else if (*data == '.')
		data++;	// u1 already 0
	    break;

	  case W_IP_FRAGOFF:
	    next = cp_integer(data, end, 0, &u1);
	    if (_minor_version == 0) // old-style file
		u1 <<= 3;
	    if (next > data && (u1 & 7) == 0 && u1 < 65536) {
		u1 >>= 3;
		data = next;
		if (data < end && *data == '+') {
		    u1 |= IP_MF;
		    data++;
		}
	    }
	    break;

	  case W_TCP_FLAGS:
	    if (isdigit(*data))
		data = cp_integer(data, end, 0, &u1);
	    else if (*data == '.')
		data++;
	    else
		while (data < end && IPSummaryDump::tcp_flag_mapping[*data]) {
		    u1 |= 1 << (IPSummaryDump::tcp_flag_mapping[*data] - 1);
		    data++;
		}
	    break;

	  case W_IP_OPT:
	    if (*data == '.')
		data++;
	    else if (*data != '-')
		data = parse_ip_opt_ascii(data, end, &ip_opt, DO_IPOPT_ALL);
	    break;

	  case W_TCP_SACK:
	    if (*data == '.')
		data++;
	    else if (*data != '-')
		data = parse_tcp_opt_ascii(data, end, &tcp_opt, DO_TCPOPT_SACK);
	    break;
	    
	  case W_TCP_NTOPT:
	    if (*data == '.')
		data++;
	    else if (*data != '-')
		data = parse_tcp_opt_ascii(data, end, &tcp_opt, DO_TCPOPT_NTALL);
	    break;
	    
	  case W_TCP_OPT:
	    if (*data == '.')
		data++;
	    else if (*data != '-')
		data = parse_tcp_opt_ascii(data, end, &tcp_opt, DO_TCPOPT_ALL);
	    break;
	    
	  case W_LINK:
	    if (*data == '>' || *data == 'L') {
		u1 = 0;
		data++;
	    } else if (*data == '<' || *data == 'X' || *data == 'R') {
		u1 = 1;
		data++;
	    } else
		data = cp_integer(data, end, 0, &u1);
	    break;

	  case W_PAYLOAD:
	    if (*data == '\"') {
		payload.clear();
		const unsigned char *fdata = data + 1;
		for (data++; data < end && *data != '\"'; data++)
		    if (*data == '\\' && data < end - 1) {
			payload.append((const char *) fdata, (const char *) data);
			fdata = (const unsigned char *) cp_process_backslash((const char *) data, (const char *) end, payload);
			data = fdata - 1; // account for loop increment
		    }
		payload.append((const char *) fdata, (const char *) data);
		// bag payload if it didn't parse correctly
		if (data >= end || *data != '\"')
		    data = original_data;
		else {
		    have_payload = have_payload_len = true;
		    payload_len = payload.length();
		}
	    }
	    break;

	}

	// check whether we correctly parsed something
	{
	    bool this_ok = (data > original_data && (data >= end || isspace(*data)));
	    while (data < end && !isspace(*data))
		data++;
	    while (data < end && isspace(*data))
		data++;
	    if (!this_ok)
		continue;
	}

	// store contents
      store_contents:
	switch (_contents[i]) {

	  case W_TIMESTAMP:
	  case W_NTIMESTAMP:
	    if (u2 < 1000000000)
		q->timestamp_anno().set_nsec(u1, u2), ok++;
	    break;

	  case W_TIMESTAMP_SEC:
	    q->timestamp_anno().set_sec(u1), ok++;
	    break;

	  case W_TIMESTAMP_USEC:
	    if (u1 < 1000000)
		q->timestamp_anno().set_subsec(Timestamp::usec_to_subsec(u1)), ok++;
	    break;

	  case W_TIMESTAMP_USEC1:
	    if (u1 == 0 && u2 < 1000000)
		q->timestamp_anno().set_usec(0, u2), ok++;
	    else if (u1 == 0)
		q->timestamp_anno().set_usec(u2/1000000, u2%1000000), ok++;
#if HAVE_INT64_TYPES
	    else {
		uint64_t uu = ((uint64_t)u1 << 32) | u2;
		q->timestamp_anno().set_usec(uu/1000000, uu%1000000), ok++;
	    }
#endif
	    break;
	    
	  case W_IP_SRC:
	    iph->ip_src.s_addr = htonl(u1), ip_ok++;
	    break;

	  case W_IP_DST:
	    iph->ip_dst.s_addr = htonl(u1), ip_ok++;
	    break;

	  case W_IP_HL:
	    if (u1 >= sizeof(click_ip) && u1 <= 60 && (u1 & 3) == 0)
		ip_hl = u1, ip_ok++;
	    break;
	    
	  case W_IP_LEN:
	    ip_len = u1, ok++;
	    break;
	    
	  case W_PAYLOAD_LEN:
	    payload_len = u1, have_payload_len = true, ok++;
	    break;

	  case W_IP_CAPTURE_LEN:
	    /* XXX do nothing with this for now */
	    ok++;
	    break;
	    
	  case W_IP_PROTO:
	    if (u1 <= 255)
		iph->ip_p = u1, ip_ok++;
	    break;

	  case W_IP_TOS:
	    if (u1 <= 255)
		iph->ip_tos = u1, ip_ok++;
	    break;

	  case W_IP_TTL:
	    if (u1 <= 255)
		iph->ip_ttl = u1, ip_ok++;
	    break;

	  case W_IP_ID:
	    if (u1 <= 0xFFFF)
		iph->ip_id = htons(u1), ip_ok++;
	    break;

	  case W_IP_FRAG:
	    iph->ip_off = u1, ip_ok++;
	    break;

	  case W_IP_FRAGOFF:
	    if ((u1 & ~IP_MF) <= IP_OFFMASK)
		iph->ip_off = htons(u1), ip_ok++;
	    break;

	  case W_SPORT:
	    if (u1 <= 0xFFFF)
		q->udp_header()->uh_sport = htons(u1), ip_ok++;
	    break;

	  case W_DPORT:
	    if (u1 <= 0xFFFF)
		q->udp_header()->uh_dport = htons(u1), ip_ok++;
	    break;

	  case W_TCP_SEQ:
	    q->tcp_header()->th_seq = htonl(u1), ip_ok++;
	    break;

	  case W_TCP_ACK:
	    q->tcp_header()->th_ack = htonl(u1), ip_ok++;
	    break;

	  case W_TCP_FLAGS:
	    if (u1 <= 0xFF)
		q->tcp_header()->th_flags = u1, ip_ok++;
	    else if (u1 <= 0xFFF)
		// th_off will be set later
		*reinterpret_cast<uint16_t *>(q->transport_header() + 12) = htons(u1), ip_ok++;
	    break;

	  case W_TCP_OFF:
	    if (u1 >= sizeof(click_tcp) && u1 <= 60 && (u1 & 3) == 0)
		tcp_off = u1, ip_ok++;
	    break;

	  case W_TCP_WINDOW:
	    if (u1 <= 0xFFFF)
		q->tcp_header()->th_win = htons(u1), ip_ok++;
	    break;
	    
	  case W_TCP_URP:
	    if (u1 <= 0xFFFF)
		q->tcp_header()->th_urp = htons(u1), ip_ok++;
	    break;

	  case W_ICMP_TYPE:
	    if (u1 <= 255)
		q->icmp_header()->icmp_type = icmp_type = u1, ip_ok++;
	    break;

	  case W_ICMP_CODE:
	    if (u1 <= 255)
		q->icmp_header()->icmp_code = u1, ip_ok++;
	    break;

	  case W_COUNT:
	    if (u1)
		SET_EXTRA_PACKETS_ANNO(q, u1 - 1), ok++;
	    break;

	  case W_LINK:
	    SET_PAINT_ANNO(q, u1), ok++;
	    break;

	  case W_AGGREGATE:
	    SET_AGGREGATE_ANNO(q, u1), ok++;
	    break;

	  case W_FIRST_TIMESTAMP:
	  case W_FIRST_NTIMESTAMP:
	    if (u2 < 1000000000) {
		SET_FIRST_TIMESTAMP_ANNO(q, Timestamp::make_nsec(u1, u2));
		ok++;
	    }
	    break;

	}
    }

    if (!ok && !ip_ok) {	// bad format
	if (!_format_complaint) {
	    // don't complain if the line was all blank
	    if (binary || (int) strspn(line.data(), " \t\n\r") != line.length()) {
		if (_contents.size() == 0)
		    _ff.error(errh, "no '!data' provided");
		else
		    _ff.error(errh, "packet parse error");
		_format_complaint = true;
	    }
	}
	if (q)
	    q->kill();
	return 0;
    }
#endif

#if 0
    // append EOL IP options to fill out ip_hl
    if (sizeof(click_ip) + ip_opt.length() < ip_hl)
	ip_opt.append_fill(IPOPT_EOL, ip_hl - ip_opt.length() - sizeof(click_ip));

    // append IP options if any
    if (ip_opt) {
	if (!(q = handle_ip_opt(q, ip_opt)))
	    return 0;
	else		       // iph may have changed!! (don't use tcph etc.)
	    iph = q->ip_header();
    }
    
    // append EOL TCP options to fill out tcp_off
    if (sizeof(click_tcp) + tcp_opt.length() < tcp_off)
	tcp_opt.append_fill(TCPOPT_EOL, tcp_off - tcp_opt.length() - sizeof(click_tcp));

    // set TCP offset to a reasonable value; possibly reduce packet length
    if (iph->ip_p == IP_PROTO_TCP && IP_FIRSTFRAG(iph)) {
	if (!tcp_opt)
	    q->tcp_header()->th_off = sizeof(click_tcp) >> 2;
	else if (!(q = handle_tcp_opt(q, tcp_opt)))
	    return 0;
	else			// iph may have changed!!
	    iph = q->ip_header();
    } else if (iph->ip_p == IP_PROTO_UDP && IP_FIRSTFRAG(iph))
	q->take(sizeof(click_tcp) - sizeof(click_udp));
    else if (iph->ip_p == IP_PROTO_ICMP && IP_FIRSTFRAG(iph))
	q->take(sizeof(click_tcp) - click_icmp_hl(q->icmp_header()->icmp_type));
    else
	q->take(sizeof(click_tcp));

    // set IP length
    if (have_payload) {	// XXX what if ip_len indicates IP options?
	int old_length = q->length();
	iph->ip_len = ntohs(old_length + payload.length());
	if ((q = q->put(payload.length()))) {
	    memcpy(q->data() + old_length, payload.data(), payload.length());
	    // iph may have changed!!
	    iph = q->ip_header();
	}
    } else if (ip_len) {
	if (ip_len <= 0xFFFF)
	    iph->ip_len = ntohs(ip_len);
	else
	    iph->ip_len = ntohs(0xFFFF);
	SET_EXTRA_LENGTH_ANNO(q, ip_len - q->length());
    } else if (have_payload_len) {
	if (q->length() + payload_len <= 0xFFFF)
	    iph->ip_len = ntohs(q->length() + payload_len);
	else
	    iph->ip_len = ntohs(0xFFFF);
	SET_EXTRA_LENGTH_ANNO(q, payload_len);
    } else
	iph->ip_len = ntohs(q->length());

    // set UDP length (after IP length is available)
    if (iph->ip_p == IP_PROTO_UDP && IP_FIRSTFRAG(iph))
	q->udp_header()->uh_ulen = htons(ntohs(iph->ip_len) - (iph->ip_hl << 2));
    
    // set data from flow ID
    if (_use_flowid) {
	IPFlowID flowid = (PAINT_ANNO(q) & 1 ? _flowid.rev() : _flowid);
	if (flowid.saddr())
	    iph->ip_src = flowid.saddr();
	if (flowid.daddr())
	    iph->ip_dst = flowid.daddr();
	if (flowid.sport() && IP_FIRSTFRAG(iph))
	    q->tcp_header()->th_sport = flowid.sport();
	if (flowid.dport() && IP_FIRSTFRAG(iph))
	    q->tcp_header()->th_dport = flowid.dport();
	if (_use_aggregate)
	    SET_AGGREGATE_ANNO(q, _aggregate);
    } else if (!ip_ok)
	q->set_network_header(0, 0);

    // set destination IP address annotation
    q->set_dst_ip_anno(iph->ip_dst);

    // set checksum
    if (_checksum && ip_ok)
	set_checksums(q, iph);
#endif
    
    return q;
}

inline Packet *
set_packet_lengths(Packet *p, uint32_t extra_length)
{
    uint32_t length = p->length() + extra_length;
    if (htons(length) != p->ip_header()->ip_len) {
	if (WritablePacket *q = p->uniqueify()) {
	    click_ip *ip = q->ip_header();
	    ip->ip_len = htons(length);
	    if (ip->ip_p == IP_PROTO_UDP)
		q->udp_header()->uh_ulen = htons(length - (ip->ip_hl << 2));
	    return q;
	} else
	    return 0;
    } else
	return p;
}

Packet *
FromIPSummaryDump::handle_multipacket(Packet *p)
{
    assert(!_work_packet || _work_packet == p);
    
    if (!p || !EXTRA_PACKETS_ANNO(p)) {
	_work_packet = 0;
	return p;
    }

    uint32_t count = 1 + EXTRA_PACKETS_ANNO(p);

    // set up _multipacket variables on new packets (_work_packet == 0)
    if (!_work_packet) {
	assert(count > 1);
	// set length of all but the last packet
	_multipacket_length = (p->length() + EXTRA_LENGTH_ANNO(p)) / count;
	// beware if there isn't enough EXTRA_LENGTH to cover all the packets
	if (_multipacket_length < p->length()) {
	    _multipacket_length = p->length();
	    SET_EXTRA_LENGTH_ANNO(p, _multipacket_length * (count - 1));
	}
	// set timestamps
	_multipacket_end_timestamp = p->timestamp_anno();
	if (FIRST_TIMESTAMP_ANNO(p)) {
	    _multipacket_timestamp_delta = (p->timestamp_anno() - FIRST_TIMESTAMP_ANNO(p)) / (count - 1);
	    p->timestamp_anno() = FIRST_TIMESTAMP_ANNO(p);
	} else
	    _multipacket_timestamp_delta = Timestamp();
	// prepare IP lengths for _multipacket_extra_length
	_work_packet = set_packet_lengths(p, _multipacket_length - p->length());
	if (!_work_packet)
	    return 0;
    }

    // prepare packet to return
    if ((p = p->clone())) {
	SET_EXTRA_PACKETS_ANNO(p, 0);
	SET_EXTRA_LENGTH_ANNO(p, _multipacket_length - p->length());
    }

    // reduce weight of _work_packet 
    SET_EXTRA_PACKETS_ANNO(_work_packet, count - 2);
    SET_EXTRA_LENGTH_ANNO(_work_packet, EXTRA_LENGTH_ANNO(_work_packet) - _multipacket_length);
    if (count == 2) {
	_work_packet->timestamp_anno() = _multipacket_end_timestamp;
	_work_packet = set_packet_lengths(_work_packet, EXTRA_LENGTH_ANNO(_work_packet));
    } else
	_work_packet->timestamp_anno() += _multipacket_timestamp_delta;

    return p;
}

void
FromIPSummaryDump::run_timer(Timer *)
{
    if (_active) {
	if (output_is_pull(0))
	    _notifier.wake();
	else
	    _task.reschedule();
    }
}

bool
FromIPSummaryDump::check_timing(Packet *p)
{
    assert(!_work_packet || _work_packet == p);
    if (!_have_timing) {
	_timing_offset = Timestamp::now() - p->timestamp_anno();
	_have_timing = true;
    }
    Timestamp now = Timestamp::now();
    Timestamp t = p->timestamp_anno() + _timing_offset;
    if (now < t) {
	t -= Timer::adjustment();
	if (now < t) {
	    _timer.schedule_at(t);
	    if (output_is_pull(0))
		_notifier.sleep();
	} else {
	    if (output_is_push(0))
		_task.fast_reschedule();
	}
	_work_packet = p;
	return false;
    }
    _work_packet = 0;
    return true;
}

bool
FromIPSummaryDump::run_task(Task *)
{
    if (!_active)
	return false;
    Packet *p;

    while (1) {
	p = (_work_packet ? _work_packet : read_packet(0));
	if (!p && !_ff.initialized()) {
	    if (_stop)
		router()->please_stop_driver();
	    return false;
	} else if (!p)
	    break;
	if (p && _timing && !check_timing(p))
	    return false;
	if (_multipacket)
	    p = handle_multipacket(p);
	// check sampling probability
	if (_sampling_prob >= (1 << SAMPLING_SHIFT)
	    || (click_random() & ((1 << SAMPLING_SHIFT) - 1)) < _sampling_prob)
	    break;
	if (p)
	    p->kill();
    }

    if (p)
	output(0).push(p);
    _task.fast_reschedule();
    return true;
}

Packet *
FromIPSummaryDump::pull(int)
{
    if (!_active)
	return 0;
    Packet *p;

    while (1) {
	p = (_work_packet ? _work_packet : read_packet(0));
	if (!p && !_ff.initialized()) {
	    if (_stop)
		router()->please_stop_driver();
	    _notifier.sleep();
	    return 0;
	}
	if (p && _timing && !check_timing(p))
	    return 0;
	if (_multipacket)
	    p = handle_multipacket(p);
	// check sampling probability
	if (_sampling_prob >= (1 << SAMPLING_SHIFT)
	    || (click_random() & ((1 << SAMPLING_SHIFT) - 1)) < _sampling_prob)
	    break;
	if (p)
	    p->kill();
    }

    _notifier.wake();
    return p;
}


enum { H_SAMPLING_PROB, H_ACTIVE, H_ENCAP, H_STOP };

String
FromIPSummaryDump::read_handler(Element *e, void *thunk)
{
    FromIPSummaryDump *fd = static_cast<FromIPSummaryDump *>(e);
    switch ((intptr_t)thunk) {
      case H_SAMPLING_PROB:
	return cp_unparse_real2(fd->_sampling_prob, SAMPLING_SHIFT);
      case H_ACTIVE:
	return cp_unparse_bool(fd->_active);
      case H_ENCAP:
	return "IP";
      default:
	return "<error>";
    }
}

int
FromIPSummaryDump::write_handler(const String &s_in, Element *e, void *thunk, ErrorHandler *errh)
{
    FromIPSummaryDump *fd = static_cast<FromIPSummaryDump *>(e);
    String s = cp_uncomment(s_in);
    switch ((intptr_t)thunk) {
      case H_ACTIVE: {
	  bool active;
	  if (cp_bool(s, &active)) {
	      fd->_active = active;
	      if (fd->output_is_push(0) && active && !fd->_task.scheduled())
		  fd->_task.reschedule();
	      else if (!fd->output_is_push(0))
		  fd->_notifier.set_active(active, true);
	      return 0;
	  } else
	      return errh->error("'active' should be Boolean");
      }
      case H_STOP:
	fd->_active = false;
	fd->router()->please_stop_driver();
	return 0;
      default:
	return -EINVAL;
    }
}

void
FromIPSummaryDump::add_handlers()
{
    add_read_handler("sampling_prob", read_handler, H_SAMPLING_PROB);
    add_read_handler("active", read_handler, H_ACTIVE, Handler::CHECKBOX);
    add_write_handler("active", write_handler, H_ACTIVE);
    add_read_handler("encap", read_handler, H_ENCAP);
    add_write_handler("stop", write_handler, H_STOP, Handler::BUTTON);
    _ff.add_handlers(this);
    if (output_is_push(0))
	add_task_handlers(&_task);
}

ELEMENT_REQUIRES(userlevel FromFile IPSummaryDumpInfo ToIPSummaryDump)
EXPORT_ELEMENT(FromIPSummaryDump)
CLICK_ENDDECLS
