// Generated Code - DO NOT EDIT !!
// generated by 'emugen'

#ifndef GUARD_foo_decoder_context_t
#define GUARD_foo_decoder_context_t

#include "render-utils/IOStream.h"
#include "ChecksumCalculator.h"
#include "foo_server_context.h"


#include "emugl/common/logging.h"

struct foo_decoder_context_t : public foo_server_context_t {

	size_t decode(void *buf, size_t bufsize, IOStream *stream, ChecksumCalculator* checksumCalc);

};

#endif  // GUARD_foo_decoder_context_t
