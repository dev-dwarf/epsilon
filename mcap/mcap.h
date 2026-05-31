#ifndef MCAP_H
#define MCAP_H
#include <lf.h>
#include <string.h>
#include <unistd.h>

// MCAP format version 0 sequential writer, ros2idl/cdr, no chunking.
// Opcodes from mcap.dev/spec
#define MCAP_OP_HEADER         0x01
#define MCAP_OP_FOOTER         0x02
#define MCAP_OP_SCHEMA         0x03
#define MCAP_OP_CHANNEL        0x04
#define MCAP_OP_MESSAGE        0x05
#define MCAP_OP_STATISTICS     0x0B
#define MCAP_OP_SUMMARY_OFFSET 0x0E
#define MCAP_OP_DATA_END       0x0F

#ifndef MCAP_MAX_CHANNELS
#define MCAP_MAX_CHANNELS 64
#endif

typedef struct {
    int  fd;
    u16  next_schema_id;   // first schema = 1
    u16  next_channel_id;  // first channel = 0
    u64  bytes_written;
    u64  msg_count;
    u64  msg_start_time;   // init to (u64)-1
    u64  msg_end_time;
    u32  seq[MCAP_MAX_CHANNELS];
    u64  ch_msg_count[MCAP_MAX_CHANNELS];
} mcap_writer;

// internal helpers

static inline void mcap__raw(mcap_writer *w, const void *buf, u32 n) {
    write(w->fd, buf, n);
    w->bytes_written += n;
}
static inline void mcap__u8 (mcap_writer *w, u8  v) { mcap__raw(w, &v, 1); }
static inline void mcap__u16(mcap_writer *w, u16 v) { u8 b[2]={v,v>>8}; mcap__raw(w,b,2); }
static inline void mcap__u32(mcap_writer *w, u32 v) { u8 b[4]={v,v>>8,v>>16,v>>24}; mcap__raw(w,b,4); }
static inline void mcap__u64(mcap_writer *w, u64 v) {
    u8 b[8]={v,v>>8,v>>16,v>>24,v>>32,v>>40,v>>48,v>>56}; mcap__raw(w,b,8);
}
static inline void mcap__str(mcap_writer *w, const char *s) {
    u32 n=(u32)strlen(s); mcap__u32(w,n); mcap__raw(w,s,n);
}
static inline void mcap__rec(mcap_writer *w, u8 op, u64 len) {
    mcap__u8(w,op); mcap__u64(w,len);
}

static const u8 mcap__magic[8] = {0x89,'M','C','A','P',0x30,'\r','\n'};

// public API

static inline void mcap_open(mcap_writer *w, int fd) {
    *w = (mcap_writer){.fd=fd, .next_schema_id=1, .msg_start_time=(u64)-1};
    mcap__raw(w, mcap__magic, 8);
    // Header record: profile="ros2" library="ilon"  (content = 4+4+4+4 = 16 bytes)
    mcap__rec(w, MCAP_OP_HEADER, 16);
    mcap__str(w, "ros2");
    mcap__str(w, "ilon");
}

// Returns schema_id. idl_data is the IDL text (e.g. Example_Status_IDL), idl_len = sizeof-1.
static inline u16 mcap_schema(mcap_writer *w, const char *name,
                               const char *idl_data, u32 idl_len) {
    u16 id = w->next_schema_id++;
    u64 clen = 2 + (4+(u64)strlen(name)) + (4+7) + 4 + idl_len;
    mcap__rec(w, MCAP_OP_SCHEMA, clen);
    mcap__u16(w, id);
    mcap__str(w, name);
    mcap__str(w, "ros2idl");
    mcap__u32(w, idl_len);
    mcap__raw(w, idl_data, idl_len);
    return id;
}

// Returns channel_id. topic e.g. "/example0/status".
static inline u16 mcap_channel(mcap_writer *w, u16 schema_id, const char *topic) {
    u16 id = w->next_channel_id++;
    u64 clen = 2 + 2 + (4+(u64)strlen(topic)) + (4+3) + 4;
    mcap__rec(w, MCAP_OP_CHANNEL, clen);
    mcap__u16(w, id);
    mcap__u16(w, schema_id);
    mcap__str(w, topic);
    mcap__str(w, "cdr");
    mcap__u32(w, 0);
    return id;
}

// Write one message. cstruct points to an ilon-generated unpacked struct; struct_size = sizeof(struct).
// A 4-byte CDR LE header is prepended automatically.
static inline void mcap_message(mcap_writer *w, u16 channel_id, u64 log_time_ns,
                                 const void *cstruct, u32 struct_size) {
    static const u8 cdr_hdr[4] = {0x00, 0x01, 0x00, 0x00};
    // content: u16 ch_id + u32 seq + u64 log_time + u64 pub_time + 4 cdr_hdr + struct_size
    mcap__rec(w, MCAP_OP_MESSAGE, 2+4+8+8+4+(u64)struct_size);
    mcap__u16(w, channel_id);
    mcap__u32(w, w->seq[channel_id]++);
    mcap__u64(w, log_time_ns);
    mcap__u64(w, log_time_ns);
    mcap__raw(w, cdr_hdr, 4);
    mcap__raw(w, cstruct, struct_size);
    w->msg_count++;
    w->ch_msg_count[channel_id]++;
    if (log_time_ns < w->msg_start_time) w->msg_start_time = log_time_ns;
    if (log_time_ns > w->msg_end_time)   w->msg_end_time   = log_time_ns;
}

static inline void mcap_close(mcap_writer *w) {
    mcap__rec(w, MCAP_OP_DATA_END, 4);
    mcap__u32(w, 0);  // CRC disabled
    // Footer with no summary section — readers fall back to linear scan.
    // A post-processing pass can add a proper indexed summary later.
    mcap__rec(w, MCAP_OP_FOOTER, 20);
    mcap__u64(w, 0);  // summary_start = 0 (no summary)
    mcap__u64(w, 0);  // summary_offset_start = 0
    mcap__u32(w, 0);  // summary CRC disabled
    mcap__raw(w, mcap__magic, 8);
}

#endif // MCAP_H
