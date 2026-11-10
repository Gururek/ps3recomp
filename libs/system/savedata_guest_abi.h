#ifndef PS3RECOMP_SAVEDATA_GUEST_ABI_H
#define PS3RECOMP_SAVEDATA_GUEST_ABI_H

#include <stdint.h>

/* Fixed 32-bit PS3 ABI layouts. Host C structs contain native pointers and
 * therefore cannot be used to decode callback memory in a 64-bit process. */
#define PS3_SAVEDATA_CBRESULT_SIZE       20u
#define PS3_SAVEDATA_STATGET_SIZE      1704u
#define PS3_SAVEDATA_STATSET_SIZE        12u
#define PS3_SAVEDATA_FILEGET_SIZE        68u
#define PS3_SAVEDATA_FILESET_SIZE        48u
#define PS3_SAVEDATA_SETBUF_SIZE         40u
#define PS3_SAVEDATA_FILESTAT_SIZE       56u
#define PS3_SAVEDATA_SETLIST_SIZE        16u
#define PS3_SAVEDATA_DIRLIST_SIZE        48u
#define PS3_SAVEDATA_LISTGET_SIZE        76u
#define PS3_SAVEDATA_FIXEDSET_SIZE       12u
#define PS3_SAVEDATA_DONEGET_SIZE       108u

#define PS3_SAVEDATA_SETLIST_SORT_TYPE     0u
#define PS3_SAVEDATA_SETLIST_SORT_ORDER    4u
#define PS3_SAVEDATA_SETLIST_PREFIX        8u
#define PS3_SAVEDATA_SETLIST_RESERVED     12u

#define PS3_SAVEDATA_SETBUF_DIR_LIST_MAX  0u
#define PS3_SAVEDATA_SETBUF_FILE_LIST_MAX 4u
#define PS3_SAVEDATA_SETBUF_RESERVED       8u
#define PS3_SAVEDATA_SETBUF_BUFFER_SIZE   32u
#define PS3_SAVEDATA_SETBUF_BUFFER        36u

#define PS3_SAVEDATA_LISTGET_DIR_NUM       0u
#define PS3_SAVEDATA_LISTGET_LIST_NUM      4u
#define PS3_SAVEDATA_LISTGET_LIST          8u

#define PS3_SAVEDATA_FIXEDSET_DIRNAME      0u
#define PS3_SAVEDATA_FIXEDSET_NEW_ICON     4u
#define PS3_SAVEDATA_FIXEDSET_OPTION       8u

#define PS3_SAVEDATA_DONEGET_RESULT        0u
#define PS3_SAVEDATA_DONEGET_DIRNAME       4u
#define PS3_SAVEDATA_DONEGET_SIZE_KB      36u
#define PS3_SAVEDATA_DONEGET_FREE_KB      40u

#define PS3_SAVEDATA_FILESET_OPERATION    0u
#define PS3_SAVEDATA_FILESET_TYPE         8u
#define PS3_SAVEDATA_FILESET_NAME        28u
#define PS3_SAVEDATA_FILESET_OFFSET      32u
#define PS3_SAVEDATA_FILESET_SIZE_BYTES  36u
#define PS3_SAVEDATA_FILESET_BUFFER_SIZE 40u
#define PS3_SAVEDATA_FILESET_BUFFER      44u

typedef struct Ps3SaveDataGuestFileRequest {
    uint32_t operation;
    uint32_t file_type;
    uint32_t name_ea;
    uint32_t offset;
    uint32_t size;
    uint32_t buffer_size;
    uint32_t buffer_ea;
} Ps3SaveDataGuestFileRequest;

typedef struct Ps3SaveDataGuestSetBuf {
    uint32_t dir_list_max;
    uint32_t file_list_max;
    uint32_t buffer_size;
    uint32_t buffer_ea;
} Ps3SaveDataGuestSetBuf;

static inline uint32_t ps3_savedata_read_be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline Ps3SaveDataGuestFileRequest
ps3_savedata_decode_file_request(const uint8_t raw[PS3_SAVEDATA_FILESET_SIZE])
{
    Ps3SaveDataGuestFileRequest request;
    request.operation = ps3_savedata_read_be32(raw + PS3_SAVEDATA_FILESET_OPERATION);
    request.file_type = ps3_savedata_read_be32(raw + PS3_SAVEDATA_FILESET_TYPE);
    request.name_ea = ps3_savedata_read_be32(raw + PS3_SAVEDATA_FILESET_NAME);
    request.offset = ps3_savedata_read_be32(raw + PS3_SAVEDATA_FILESET_OFFSET);
    request.size = ps3_savedata_read_be32(raw + PS3_SAVEDATA_FILESET_SIZE_BYTES);
    request.buffer_size = ps3_savedata_read_be32(raw + PS3_SAVEDATA_FILESET_BUFFER_SIZE);
    request.buffer_ea = ps3_savedata_read_be32(raw + PS3_SAVEDATA_FILESET_BUFFER);
    return request;
}

static inline Ps3SaveDataGuestSetBuf
ps3_savedata_decode_set_buf(const uint8_t raw[PS3_SAVEDATA_SETBUF_SIZE])
{
    Ps3SaveDataGuestSetBuf set_buf;
    set_buf.dir_list_max =
        ps3_savedata_read_be32(raw + PS3_SAVEDATA_SETBUF_DIR_LIST_MAX);
    set_buf.file_list_max =
        ps3_savedata_read_be32(raw + PS3_SAVEDATA_SETBUF_FILE_LIST_MAX);
    set_buf.buffer_size =
        ps3_savedata_read_be32(raw + PS3_SAVEDATA_SETBUF_BUFFER_SIZE);
    set_buf.buffer_ea =
        ps3_savedata_read_be32(raw + PS3_SAVEDATA_SETBUF_BUFFER);
    return set_buf;
}

#endif
